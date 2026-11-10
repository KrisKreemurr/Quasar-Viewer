# Usage-statistics ingest

This is source for an optional service, not a deployment. Nothing here installs,
starts, or changes a service on another machine. The viewer requires explicit
consent before collecting or sending reports. Server deployment and publication of
the privacy statement require a separate owner decision.

The implementation uses Python's standard library and SQLite. Its canonical
schema is `doc/telemetry/schema_v1.json`; the client embeds that same schema.
Run the offline checks from this directory:

```sh
python3 -I -m unittest discover -s . -p test_ingest.py
```

For an explicitly requested local integration test, start the service on loopback
with a private disposable database. Do not use a production database for tests.

```sh
python3 -I ingest.py --host 127.0.0.1 --port 8791 --database /absolute/private/test.sqlite
```

The directory must already exist and be writable only by the service account.
The executable sets a restrictive creation mask. Set the viewer's non-persistent
`SLTelemetryURL` to `http://127.0.0.1:8791/api/telemetry/v1` for this local test.
Enabling the viewer's usage-statistics consent is still required. Restore the URL
after the test. Test/custom builds do not send to the production endpoint.

## Before a deployment

- Run the service as a dedicated unprivileged account, bound to loopback, behind
  the owner's HTTPS proxy. Keep the database and its WAL files out of the web
  root and out of general-purpose log collectors.
- Disable access logging for ingest and health routes. Do not log request bodies,
  headers, IP addresses, query strings, or rejected field values. The application
  suppresses the standard HTTP access/error tracebacks; the proxy, tunnel,
  supervisor, and monitoring configuration need their own review.
- Buffer requests at the proxy, set a small body limit, enforce request/header
  timeouts, and restrict accepted methods and paths. The application has a 16 KiB
  body limit, 16 handler slots, and socket timeouts. Do not expose this development
  HTTP server directly to the Internet.
- If the proxy supplies `X-Real-IP`, configure
  `SLIPSTREAM_TELEMETRY_TRUSTED_PROXIES` with its exact peer IP addresses. The proxy
  must replace this header with its verified client address, never append or pass
  through an arbitrary client header. Untrusted peers cannot select their rate
  limit identity. Addresses are used only for ephemeral salted rate-limit hashes,
  not stored in reports or SQLite.
- Review any upstream tunnel provider's handling of network metadata. The
  application's no-IP-storage policy does not configure third-party infrastructure.
- `SLIPSTREAM_TELEMETRY_HMAC_KEY` must match the viewer build's key. The shipped
  client-side key is a public abuse deterrent, not authentication and not proof of
  genuine viewer data. Keep rate limits and capacity bounds even with valid HMACs.
- Set OS memory, CPU, file-size, and disk-quota limits for the service. Alert on
  counts/status only. Never include rejected payloads in an alert.
- Publish the reviewed privacy text and field guide before enabling a production
  endpoint. No report links an account or install to a deletion handle.

## Storage and retention

Raw reports expire after 90 days. Per-report random retry IDs are removed from raw
payloads and retained separately for seven days to deduplicate retries. The server
adds its own received day; client timestamps are not stored. Daily totals remain
after raw expiry. The database uses secure deletion and WAL checkpointing.

Any backup must follow the same raw-data expiry policy; restoring an old backup
must run retention cleanup before serving it. Do not export raw profiles as public
statistics. Public aggregates require suppression of rare groups as described in
the design's reporting policy.

No production listener, server, proxy, tunnel, scheduled job, or deployment has
been configured by this change.
