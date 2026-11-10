# Usage statistics

Publication draft. This text accompanies the source implementation; it has not
been published and no collection service has been deployed by this change.

## Your choice

Usage statistics are off until you choose **Share statistics**. Declining does
not affect your ability to use the viewer, and we do not ask again after you
decline. You can change your choice at the login screen or in Preferences >
Privacy > Usage statistics. Adding a new category of information requires your
consent again.

While sharing is enabled, the viewer collects small statistical summaries and
queues an initial summary after about two minutes of a settled login, followed
by summaries about once an hour. It does not send a record of each frame or each
action. You can inspect the report's literal JSON in **See what is sent**,
including before enabling statistics.

## What reports contain

Reports describe your viewer version, operating-system family and coarse version,
hardware models and bucketed capacities, screen characteristics, selected graphics
settings and feature switches, and aggregate performance, memory, loading, and
session-outcome statistics. Only explicitly allowed fields and hardware-model
formats are accepted. Unsupported measurements are marked unavailable rather than
invented. The field guide and schema specify the exact fields for this version.

Reports do not contain your avatar or account name, account identifier, inventory,
chat, in-world location, region names, URLs you visit, filenames, filesystem paths,
hardware serial numbers, MAC addresses, or anything you type. There is no persistent
installation identifier. Each report has a fresh random identifier used only to
recognize retries. Similar hardware profiles can still make reports resemble one
another; the absence of an installation identifier does not prevent that correlation.

## Sending and storage

Production reports are sent over verified HTTPS to the viewer team's configured
service on slipstreamviewer.org. A network connection necessarily exposes its IP
address to the receiving network infrastructure. IP addresses are not report fields
and must not be retained in application or proxy request logs. The ingest service
uses temporary salted address hashes only to enforce rate limits. Infrastructure
and tunnel-provider handling must be reviewed before this draft is published.

Raw reports expire after 90 days. Random report identifiers used for retry
deduplication expire after seven days and are removed from stored raw report
payloads. Aggregate totals can remain after the raw reports expire. Public statistics
must suppress rare groups rather than publish individual hardware profiles. Reports
are not sold or shared as individual records with grid operators or other parties.

The local queue holds at most eight reports of at most 16 KiB each, for at most
30 days. Network failures do not create an unlimited backlog or a retry storm.

## Turning sharing off

Turning sharing off stops collection immediately, discards the current summary,
and requests deletion of queued reports. A report already being sent may still
arrive. You can also delete queued reports without changing your consent choice.

Because reports have no account or installation identifier, we cannot locate all
of a person's earlier reports for an individual deletion request. Those reports
expire under the retention policy above. Your choice is saved immediately, even
if you subsequently cancel the Preferences window.
