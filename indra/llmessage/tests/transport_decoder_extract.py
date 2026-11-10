#!/usr/bin/env python3
"""Generate an actual-reader-loop differential fixture; does not compile it."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--root', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
reader = (a.root / 'indra/llmessage/lltemplatemessagereader.cpp').read_text()
start = reader.index('    // loop through the template building the data structure as we go')
end = reader.index('    if (mCurrentRMessageData->mMemberBlocks.empty()', start)
schema = (a.root / 'indra/llmessage/lltransportschema.cpp').read_text()
schema_start = schema.index('LLTransportFreezeStatus freezeTransportSchema(')
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'transport_legacy_loop.inc').write_text(reader[start:end])
(a.output / 'transport_schema_freeze.inc').write_text(schema[schema_start:])
