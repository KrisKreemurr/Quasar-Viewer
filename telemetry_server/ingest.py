#!/usr/bin/env python3
# Copyright (C) 2026 Slipstream Viewer contributors. Author: SkyLine.
# SPDX-License-Identifier: LGPL-2.1-only
"""Offline-ready usage statistics ingest. No request/access/error logging."""
import argparse
import datetime as dt
import hashlib
import hmac
import http.server
import json
import ipaddress
import math
import os
from pathlib import Path
import re
import secrets
import sqlite3
import threading
import time
import zlib

SCHEMA = json.loads((Path(__file__).resolve().parents[1] / 'doc/telemetry/schema_v1.json').read_text())
BODY_LIMIT = 16384
# Public deterrent, not authentication. Must match the viewer build override.
DEFAULT_KEY = 'b70fc0951b274a982a47df583e17f6291653fa408810547c'

class Invalid(ValueError):
    pass

def validate(value, rule=SCHEMA):
    if 'enum' in rule and value not in rule['enum']:
        raise Invalid('enum')
    kind = rule['type']
    if kind == 'object':
        if type(value) is not dict or set(value) - rule['properties'].keys() or set(rule['required']) - value.keys():
            raise Invalid('fields')
        for field, child in value.items():
            validate(child, rule['properties'][field])
    elif kind == 'array':
        if type(value) is not list or not rule['minItems'] <= len(value) <= rule['maxItems']:
            raise Invalid('array')
        for item in value:
            validate(item, rule['items'])
    elif kind == 'integer':
        if type(value) is not int or not rule['minimum'] <= value <= rule['maximum']:
            raise Invalid('integer')
    elif kind == 'number':
        if type(value) not in (int, float) or not math.isfinite(value) or not rule['minimum'] <= value <= rule['maximum'] or abs(value/rule['multipleOf']-round(value/rule['multipleOf'])) > 1e-6:
            raise Invalid('number')
    elif kind == 'boolean':
        if type(value) is not bool:
            raise Invalid('boolean')
    elif kind == 'string':
        if type(value) is not str:
            raise Invalid('string')
        if 'enum' in rule:
            if value not in rule['enum']:
                raise Invalid('enum')
        elif not rule['minLength'] <= len(value) <= rule['maxLength'] or re.fullmatch(rule['pattern'], value, flags=re.ASCII) is None:
            raise Invalid('string')
    else:
        raise Invalid('schema')

def no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise Invalid('duplicate_field')
        result[key] = value
    return result

def decode(body):
    if not 0 < len(body) <= BODY_LIMIT:
        raise Invalid('size')
    try:
        data = json.loads(body, object_pairs_hook=no_duplicates,
                          parse_constant=lambda value: (_ for _ in ()).throw(Invalid('number')))
        validate(data)
        for category in ('steady','loading','background','vr'):
            sample=data['perf'][category]
            previous, total = -1, 0
            for index, count in sample['hist']:
                if not previous < index <= 65 or not count:
                    raise Invalid('histogram')
                previous, total = index, total + count
            if total != sample['frames'] or any(sample[field] > total for field in ('over_50ms', 'over_100ms', 'over_250ms', 'over_1s', 'x2_median')):
                raise Invalid('histogram')
        missing = data['availability']['settings_missing']
        names = set(SCHEMA['properties']['settings']['properties'])
        if len(missing) != len(set(missing)) or set(missing) != names - data['settings'].keys():
            raise Invalid('settings_availability')
        return data
    except (ValueError, UnicodeError, RecursionError, OverflowError) as error:
        raise Invalid('payload') from error

def day_now():
    return dt.datetime.now(dt.timezone.utc).date()

class Store:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA secure_delete=ON')
        self.db.execute('PRAGMA wal_autocheckpoint=64')
        self.db.execute('PRAGMA journal_size_limit=1048576')
        self.db.executescript('''
          CREATE TABLE IF NOT EXISTS reports(received_day TEXT NOT NULL, schema INTEGER NOT NULL,
            survey INTEGER NOT NULL, channel TEXT NOT NULL, version TEXT NOT NULL, payload BLOB NOT NULL);
          CREATE INDEX IF NOT EXISTS report_day ON reports(received_day);
          CREATE TABLE IF NOT EXISTS report_ids(id TEXT PRIMARY KEY, received_day TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS daily_totals(day TEXT NOT NULL, channel TEXT NOT NULL,
            version TEXT NOT NULL, reports INTEGER NOT NULL, survey INTEGER NOT NULL,
            PRIMARY KEY(day,channel,version));
        ''')
        self.cleanup()

    def _cleanup(self, today):
        self.db.execute('DELETE FROM reports WHERE received_day <= ?', ((today - dt.timedelta(days=90)).isoformat(),))
        self.db.execute('DELETE FROM report_ids WHERE received_day <= ?', ((today - dt.timedelta(days=7)).isoformat(),))

    def cleanup(self, today=None):
        with self.lock, self.db:
            self._cleanup(today or day_now())
            # WAL gets checkpointed, preventing indefinite retention in the WAL.
        with self.lock:
            self.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')

    def accept(self, data, body, today=None):
        today = today or day_now()
        # report_id is kept ONLY in the 7-day dedupe table, not in raw payloads.
        stored = dict(data)
        stored.pop('report_id')
        payload = zlib.compress(json.dumps(stored, separators=(',', ':')).encode())
        with self.lock, self.db:
            self._cleanup(today)
            if self.db.execute('SELECT 1 FROM report_ids WHERE id=?', (data['report_id'],)).fetchone():
                return False
            # Hard bounds protect disk use even if the public deterrent is extracted.
            if self.db.execute('SELECT count(*) FROM reports').fetchone()[0] >= 2_000_000 or self.db.execute('SELECT count(*) FROM report_ids').fetchone()[0] >= 500_000:
                raise Invalid('capacity')
            self.db.execute('INSERT INTO report_ids VALUES (?,?)', (data['report_id'], today.isoformat()))
            viewer = data['viewer']
            self.db.execute('INSERT INTO reports VALUES (?,?,?,?,?,?)', (today.isoformat(), data['schema'], int(data['survey']), viewer['channel'], viewer['version'], payload))
            self.db.execute('''INSERT INTO daily_totals VALUES (?,?,?,?,?) ON CONFLICT(day,channel,version)
                DO UPDATE SET reports=reports+1,survey=survey+excluded.survey''',
                (today.isoformat(), viewer['channel'], viewer['version'], 1, int(data['survey'])))
            return True

class Limiter:
    """Bounded in-memory daily salted IP hashes. Never forwarded headers."""
    def __init__(self):
        self.lock = threading.Lock()
        self.day = None
        self.hour = None
        self.salt = b''
        self.clients = {}
        self.total = 0

    def allow(self, address, now=None):
        now = time.time() if now is None else now
        with self.lock:
            day, hour = int(now // 86400), int(now // 3600)
            if day != self.day:
                self.day, self.salt = day, secrets.token_bytes(32)
                self.clients.clear()
            if hour != self.hour:
                self.hour, self.total = hour, 0
                self.clients.clear()
            digest = hmac.digest(self.salt, address.encode('ascii'), 'sha256')
            count = self.clients.get(digest, 0)
            if count >= 30 or self.total >= 20_000 or (digest not in self.clients and len(self.clients) >= 20_000):
                return False
            self.clients[digest] = count + 1
            self.total += 1
            return True

class Service(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 16
    def __init__(self, address, store, key, trusted_proxies=()):
        self.store, self.key, self.limiter = store, key.encode(), Limiter()
        self.trusted_proxies = frozenset(ipaddress.ip_address(peer) for peer in trusted_proxies)
        self.slots = threading.BoundedSemaphore(16)
        self.last_cleanup = time.monotonic()
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        pass  # Base class prints client address + traceback; explicitly suppressed.

    def service_actions(self):
        if time.monotonic() - self.last_cleanup >= 3600:
            self.store.cleanup()
            self.last_cleanup = time.monotonic()

def rate_address(peer, headers, trusted_proxies):
    # Exact configured peers only. Never parse X-Forwarded-For lists, store or log IPs.
    address = ipaddress.ip_address(peer)
    values = headers.get_all('X-Real-IP', [])
    if address in trusted_proxies and len(values) == 1 and len(values[0]) <= 45:
        try:
            forwarded = ipaddress.ip_address(values[0])
            if str(forwarded) == values[0]:
                address = forwarded
        except ValueError:
            pass
    return str(address)

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = 'UsageStatistics'
    sys_version = ''
    timeout = 15
    def setup(self):
        super().setup()
        self.connection.settimeout(15)
    def log_message(self, *args):
        pass
    def log_request(self, *args):
        pass
    def log_error(self, *args):
        pass
    def reply(self, code, value):
        body = json.dumps(value, separators=(',', ':')).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        if code == 429:
            self.send_header('Retry-After', '3600')
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
    def do_GET(self):
        self.reply(200, {'ok': True}) if self.path == '/api/telemetry/health' else self.reply(404, {'error': 'path'})
    def do_POST(self):
        if self.path != '/api/telemetry/v1':
            return self.reply(404, {'error': 'path'})
        if not self.server.limiter.allow(rate_address(self.client_address[0], self.headers, self.server.trusted_proxies)):
            return self.reply(429, {'error': 'rate'})
        try:
            if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) != 1:
                raise Invalid('framing')
            length = int(self.headers['Content-Length'])
            if not 0 < length <= BODY_LIMIT:
                return self.reply(413, {'error': 'size'})
            if self.headers.get('Content-Type') != 'application/json':
                raise Invalid('content_type')
            stamp = self.headers.get('X-Slipstream-Timestamp', '')
            signature = self.headers.get('X-Slipstream-Signature', '')
            if not re.fullmatch(r'[0-9]{10,11}', stamp) or abs(time.time() - int(stamp)) > 600:
                raise Invalid('signature')
            body = self.rfile.read(length)
            if len(body) != length:
                raise Invalid('framing')
            expected = hmac.new(self.server.key, stamp.encode() + b'\n' + body, hashlib.sha256).hexdigest()
            if not hmac.compare_digest(signature, expected):
                raise Invalid('signature')
            data = decode(body)
            accepted = self.server.store.accept(data, body)
            self.reply(202 if accepted else 200, {'ok': True} if accepted else {'ok': True, 'duplicate': True})
        except (ValueError, UnicodeError):
            self.reply(400, {'error': 'invalid'})
        except sqlite3.Error:
            self.reply(503, {'error': 'storage'})

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--database', required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8791)
    args = parser.parse_args()
    os.umask(0o077)
    trusted = tuple(filter(None, os.environ.get('SLIPSTREAM_TELEMETRY_TRUSTED_PROXIES', '').split(',')))
    service = Service((args.host, args.port), Store(args.database), os.environ.get('SLIPSTREAM_TELEMETRY_HMAC_KEY', DEFAULT_KEY), trusted)
    service.serve_forever()

if __name__ == '__main__':
    main()
