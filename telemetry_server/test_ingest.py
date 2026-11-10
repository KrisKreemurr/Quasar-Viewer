# Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
import contextlib
import copy
import datetime as dt
from email.message import Message
import io
import ipaddress
import json
import hashlib
import hmac
import time
from types import SimpleNamespace
from pathlib import Path
import tempfile
import unittest
import zlib

import ingest
ROOT = Path(__file__).resolve().parents[1]

class IngestTests(unittest.TestCase):
    def setUp(self):
        self.good = json.loads((ROOT / 'doc/telemetry/fixtures/valid.json').read_text())
    def body(self, value):
        return json.dumps(value, separators=(',', ':')).encode()
    def test_valid(self):
        self.assertEqual(ingest.decode(self.body(self.good)), self.good)
    def test_unknown_nested_fields(self):
        def visit(value):
            if type(value) is dict:
                value['ATTACKER_KEY_John_Smith'] = 'user@example.com'
                with self.assertRaises(ingest.Invalid):
                    ingest.decode(self.body(self.good))
                del value['ATTACKER_KEY_John_Smith']
                for child in value.values():
                    visit(child)
            elif type(value) is list:
                for child in value:
                    visit(child)
        visit(self.good)
    def test_hardware_fixtures(self):
        fixtures = json.loads((ROOT / 'doc/telemetry/fixtures/models.json').read_text())
        for category, cases in fixtures.items():
            field = 'model' if category == 'cpu' else 'name'
            for allowed in cases['valid']:
                self.good[category][field] = allowed
                ingest.decode(self.body(self.good))
            for forbidden in cases['invalid'] + ['x'*65, '/home/name', 'C:\\Users\\name']:
                self.good[category][field] = forbidden
                with self.assertRaises(ingest.Invalid):
                    ingest.decode(self.body(self.good))
            self.good[category][field] = 'other'
    def test_numeric_types_and_histograms(self):
        for field, value in [('schema', True), ('schema', 1.0), ('consent', 2), ('survey', 1)]:
            data = copy.deepcopy(self.good); data[field] = value
            with self.assertRaises(ingest.Invalid):
                ingest.decode(self.body(data))
        for hist in [[[66,1]], [[1,0]], [[2,1],[1,1]], [[1,1],[1,1]], [[1,2,3]]]:
            data = copy.deepcopy(self.good); data['perf']['steady']['hist'] = hist
            with self.assertRaises(ingest.Invalid):
                ingest.decode(self.body(data))
    def test_duplicate_fields_and_size_depth(self):
        for body in [b'{"schema":1,"schema":1}', b'x'*16385, b'['*10000+b']'*10000, b'{"schema":NaN}']:
            with self.assertRaises(ingest.Invalid):
                ingest.decode(body)
    def test_storage_dedupe_and_retention(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = ingest.Store(str(Path(tmp)/'test.db'))
            today = dt.date(2026,10,31)
            self.assertTrue(store.accept(self.good, self.body(self.good), today))
            self.assertFalse(store.accept(self.good, self.body(self.good), today))
            stored = json.loads(zlib.decompress(store.db.execute('SELECT payload FROM reports').fetchone()[0]))
            self.assertNotIn('report_id', stored)
            self.assertNotIn('timestamp', stored)
            self.assertEqual(store.db.execute('PRAGMA secure_delete').fetchone()[0], 1)
            store.cleanup(today + dt.timedelta(days=7))
            self.assertEqual(store.db.execute('SELECT count(*) FROM report_ids').fetchone()[0], 0)
            self.assertEqual(store.db.execute('SELECT count(*) FROM reports').fetchone()[0], 1)
            store.cleanup(today + dt.timedelta(days=90))
            self.assertEqual(store.db.execute('SELECT count(*) FROM reports').fetchone()[0], 0)
            self.assertEqual(store.db.execute('SELECT reports FROM daily_totals').fetchone()[0], 1)
            self.assertEqual(set(row[1] for row in store.db.execute('PRAGMA table_info(reports)')), {'received_day','schema','survey','channel','version','payload'})
            store.db.close()
    def test_bounded_rate_limiter(self):
        limiter = ingest.Limiter()
        for unused in range(30): self.assertTrue(limiter.allow('192.0.2.1', 10000))
        self.assertFalse(limiter.allow('192.0.2.1', 10000))
        self.assertTrue(limiter.allow('192.0.2.2', 10000))
        self.assertTrue(limiter.allow('192.0.2.1', 20000))
        self.assertTrue(all(type(key) is bytes and len(key)==32 for key in limiter.clients))
        previous = limiter.salt; limiter.allow('192.0.2.1', 100000)
        self.assertNotEqual(previous, limiter.salt)
    def test_trusted_proxy_clients_and_untrusted_spoof(self):
        trusted = frozenset([ipaddress.ip_address('127.0.0.1')])
        limiter = ingest.Limiter()
        for address in ('192.0.2.1','192.0.2.2'):
            headers = Message(); headers['X-Real-IP'] = address
            key = ingest.rate_address('127.0.0.1',headers,trusted)
            self.assertEqual(key,address)
            for unused in range(30): self.assertTrue(limiter.allow(key,10000))
            self.assertFalse(limiter.allow(key,10000))
            self.assertEqual(ingest.rate_address('192.0.2.99',headers,trusted),'192.0.2.99')
        for invalid in ('John Smith','192.0.2.1,192.0.2.2','2001:0DB8::1','x'*1000):
            headers=Message();headers['X-Real-IP']=invalid
            self.assertEqual(ingest.rate_address('127.0.0.1',headers,trusted),'127.0.0.1')
    def test_signed_ingest_replay_dedupe_and_refusal_without_listener(self):
        store=ingest.Store(':memory:')
        service=SimpleNamespace(store=store,key=ingest.DEFAULT_KEY.encode(),limiter=ingest.Limiter(),trusted_proxies=frozenset())
        body=self.body(self.good)
        def request(payload, age=0, signature=None):
            handler=object.__new__(ingest.Handler)
            handler.server=service;handler.path='/api/telemetry/v1';handler.client_address=('192.0.2.1',1234)
            handler.headers=Message();handler.headers['Content-Type']='application/json';handler.headers['Content-Length']=str(len(payload))
            stamp=str(int(time.time())-age)
            handler.headers['X-Slipstream-Timestamp']=stamp
            handler.headers['X-Slipstream-Signature']=signature or hmac.new(service.key,stamp.encode()+b'\n'+payload,hashlib.sha256).hexdigest()
            handler.rfile=io.BytesIO(payload)
            replies=[];handler.reply=lambda status,value:replies.append((status,value))
            handler.do_POST();return replies[0]
        self.assertEqual(request(body)[0],202)
        self.assertEqual(request(body),(200,{'ok':True,'duplicate':True}))
        self.assertEqual(request(body,age=601)[0],400)
        self.assertEqual(request(body,signature='0'*64)[0],400)
        data=copy.deepcopy(self.good);data['os']['attacker-key']='user@example.com'
        self.assertEqual(request(self.body(data)),(400,{'error':'invalid'}))
        self.assertEqual(request(b'x'*16385)[0],413)
        contents=' '.join(str(row) for table in ('reports','report_ids','daily_totals') for row in store.db.execute('SELECT * FROM '+table))
        self.assertNotIn('192.0.2.1',contents)
        self.assertNotIn('user@example.com',contents)
        store.db.close()
    def test_generated_schema_and_setting_types(self):
        schema=json.loads((ROOT/'doc/telemetry/schema_v1.json').read_text())
        embedded=(ROOT/'indra/newview/sltelemetryschema.h').read_text().split('R"sljson(',1)[1].split(')sljson"',1)[0]
        self.assertEqual(json.loads(embedded),schema)
        import xml.etree.ElementTree as ET
        items=iter(ET.parse(ROOT/'indra/newview/app_settings/settings.xml').getroot().find('map'))
        controls={}
        for key,value in zip(items,items):
            pairs=iter(value);controls[key.text]={a.text:b.text for a,b in zip(pairs,pairs)}
        for name,rule in schema['properties']['settings']['properties'].items():
            if name in controls:
                self.assertEqual(rule['type']=='boolean',controls[name]['Type']=='Boolean',name)
        for name in ('RenderVKSunShadows','RenderVKProjectorShadows'):
            self.assertEqual(schema['properties']['settings']['properties'][name]['enum'],[0,1,2])
    def test_logging_suppressed(self):
        sink=io.StringIO()
        with contextlib.redirect_stderr(sink),contextlib.redirect_stdout(sink):
            ingest.Handler.log_message(None,'%s','secret')
            ingest.Handler.log_request(None,'secret')
            ingest.Handler.log_error(None,'secret')
            ingest.Service.handle_error(None,None,('192.0.2.1',1234))
        self.assertEqual(sink.getvalue(),'')

if __name__ == '__main__': unittest.main()
