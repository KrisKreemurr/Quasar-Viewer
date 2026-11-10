#!/usr/bin/env python3
"""Generate isolated test inputs and a proposed plan; never invoke a compiler."""
import hashlib
import json
import subprocess
from pathlib import Path

root=Path(__file__).resolve().parents[3]
head=subprocess.check_output(['/usr/bin/git','rev-parse','HEAD'],cwd=root,text=True).strip()
if subprocess.check_output(['/usr/bin/git','status','--porcelain','--untracked-files=no'],cwd=root,text=True).strip():
    raise SystemExit('tracked source must be clean before generating a pinned plan')
message=root/'indra/llmessage'
out=root/'.validation/ownerloop'
stage=out/'source'; build=out/'build'; temp=out/'tmp'; cache=out/'cache'
for path in (stage,build,temp,cache): path.mkdir(parents=True,exist_ok=True)
copies=['lltransportownerloop.cpp','lltransportownerloop.h','lltransportownerapi.h','lltransportreceivepump.h']
for name in copies: (stage/name).write_bytes((message/name).read_bytes())
# Extracted fixtures use fake boundaries, not the viewer precompiled header.
# Production retains its real linden_common.h; only the isolated staging tree
# supplies this minimal header for the same first-include spelling.
(stage/'linden_common.h').write_text('#pragma once\n')
(stage/'llpacketring.h').write_bytes((message/'tests/lltransportownerloop_fake_ring.h').read_bytes())
schema=(message/'lltransportschema.cpp').read_text()
(stage/'transport_schema_freeze.inc').write_text(schema[schema.index('LLTransportFreezeStatus freezeTransportSchema('):])
builder=(message/'lltemplatemessagebuilder.cpp').read_text()
start=builder.index('static S32 zero_code(')
end=builder.index('void LLTemplateMessageBuilder::compressMessage(',start)
(stage/'legacy_zero_code.inc').write_text(builder[start:end])
inputs=sorted(message.glob('lltransport*.h'))+[message/'lltransportownerloop.cpp',message/'lltransportcircuitstate.cpp',
    message/'lltransportdecoder.cpp',message/'lltransportschema.cpp',message/'lltemplatemessagebuilder.cpp']
inputs+=sorted((message/'tests').glob('lltransportownerloop_*'))
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
manifest={str(p.relative_to(root)):sha(p) for p in inputs}
manifest.update({str(p.relative_to(root)):sha(p) for p in stage.iterdir() if p.is_file()})
mp=out/'manifest.json'; mp.write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
flags=['-std=c++20','-O2','-Wall','-Wextra','-Werror','-UNDEBUG','-pthread','-I'+str(stage),'-I'+str(message)]
sources=[message/'lltransportcircuitstate.cpp',message/'lltransportdecoder.cpp',stage/'lltransportownerloop.cpp',
         message/'tests/lltransportownerloop_command_test.cpp',message/'tests/lltransportownerloop_wire_test.cpp',
         message/'tests/lltransportownerloop_action_test.cpp',message/'tests/lltransportownerloop_watchdog_test.cpp']
sources.append(message/'tests/lltransportownerloop_piggy_test.cpp')
sources.append(message/'tests/lltransportownerloop_startping_test.cpp')
objects=[build/x for x in ['state.o','decoder.o','owner.o','command.o','wire.o','action.o','watchdog.o','piggy.o','startping.o']]
commands=[['/usr/bin/c++',*flags,'-c',str(src),'-o',str(obj)] for src,obj in zip(sources,objects)]
commands += [['/usr/bin/c++','-pthread',*map(str,objects[:4]),'-o',str(build/'command-test')],
             ['/usr/bin/c++','-pthread',str(objects[0]),str(objects[1]),str(objects[4]),'-o',str(build/'wire-test')],
             ['/usr/bin/c++','-pthread',str(objects[0]),str(objects[1]),str(objects[2]),str(objects[5]),'-o',str(build/'action-test')],
             ['/usr/bin/c++','-pthread',str(objects[0]),str(objects[1]),str(objects[2]),str(objects[6]),'-o',str(build/'watchdog-test')],
             ['/usr/bin/c++','-pthread',str(objects[0]),str(objects[1]),str(objects[2]),str(objects[7]),'-o',str(build/'piggy-test')],
             ['/usr/bin/c++','-pthread',str(objects[0]),str(objects[1]),str(objects[2]),str(objects[8]),'-o',str(build/'startping-test')],
             [str(build/'command-test')],[str(build/'wire-test')],[str(build/'action-test')],[str(build/'watchdog-test')],[str(build/'piggy-test')],[str(build/'startping-test')]]
plan={'head':head,'cwd':str(root),'environment':{'TMPDIR':str(temp),'TMP':str(temp),'TEMP':str(temp),'XDG_CACHE_HOME':str(cache)},
      'preserve_HOME':True,'manifest':str(mp),'manifest_sha256':sha(mp),'commands':commands,
      'timeout_seconds_per_command':30,'executed':False,'requirements':['verify all inputs before and after',
      'serial execution, core dumps disabled, owned process group timeout','retain full stdout/stderr and stop on first failure']}
pp=out/'proposed-plan.json'; pp.write_text(json.dumps(plan,indent=2)+'\n')
print('manifest',sha(mp),'plan',sha(pp),'path',pp)
