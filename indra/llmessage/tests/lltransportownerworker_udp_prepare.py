#!/usr/bin/env python3
"""Stage the real owner, worker, native wait and UDP fixture; do not compile."""
import hashlib
import json
import subprocess
from pathlib import Path

root=Path(__file__).resolve().parents[3]
head=subprocess.check_output(['/usr/bin/git','rev-parse','HEAD'],cwd=root,text=True).strip()
if subprocess.check_output(['/usr/bin/git','status','--porcelain','--untracked-files=no'],cwd=root,text=True).strip():
    raise SystemExit('tracked source must be clean')
message=root/'indra/llmessage'; out=root/'.validation/worker-udp'
stage=out/'source'; build=out/'build'; temp=out/'tmp'; cache=out/'cache'
for path in (stage,build,temp,cache): path.mkdir(parents=True,exist_ok=True)
headers=sorted(message.glob('lltransport*.h'))
names=['lltransportcircuitstate.cpp','lltransportdecoder.cpp','lltransportownerloop.cpp',
       'lltransportownerworker.cpp','lltransportownerwait.cpp']
sources=[message/name for name in names]
for path in headers+sources: (stage/path.name).write_bytes(path.read_bytes())
ring=message/'tests/lltransportowner_udp_ring.h'; (stage/'llpacketring.h').write_bytes(ring.read_bytes())
(stage/'linden_common.h').write_text('#pragma once\n')
schema=(message/'lltransportschema.cpp').read_text()
(stage/'transport_schema_freeze.inc').write_text(schema[schema.index('LLTransportFreezeStatus freezeTransportSchema('):])
test=message/'tests/lltransportownerworker_udp_test.cpp'
inputs=headers+sources+[ring,test,message/'lltransportschema.cpp',Path(__file__).resolve()]
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
manifest={str(p.relative_to(root)):sha(p) for p in inputs}
manifest.update({str(p.relative_to(root)):sha(p) for p in stage.iterdir() if p.is_file()})
mp=out/'manifest.json'; mp.write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
flags=['-std=c++20','-O2','-Wall','-Wextra','-Werror','-UNDEBUG','-pthread','-DLL_LINUX=1','-DLL_WINDOWS=0','-DLL_TRANSPORT_PUMP_TESTING','-I'+str(stage)]
objects=[build/(p.stem+'.o') for p in sources]+[build/'worker-udp-test.o']
commands=[['/usr/bin/c++',*flags,'-c',str(stage/p.name),'-o',str(o)] for p,o in zip(sources,objects)]
commands.append(['/usr/bin/c++',*flags,'-c',str(test),'-o',str(objects[-1])])
exe=build/'worker-udp-test'; commands.append(['/usr/bin/c++','-pthread',*map(str,objects),'-o',str(exe)])
commands.append([str(exe)])
plan={'head':head,'cwd':str(root),'environment':{'TMPDIR':str(temp),'TMP':str(temp),'TEMP':str(temp),'XDG_CACHE_HOME':str(cache)},
      'preserve_HOME':True,'manifest':str(mp),'manifest_sha256':sha(mp),'commands':commands,
      'timeout_seconds_per_command':20,'executed':False,'requirements':['serial, core dumps disabled, owned process group timeout',
      'all staged inputs verified before and after','local UDP only; fake packet ring and templates; not a viewer or Windows runtime test']}
pp=out/'plan.json'; pp.write_text(json.dumps(plan,indent=2)+'\n')
print('head',head,'manifest',sha(mp),'plan',sha(pp),'path',pp)
