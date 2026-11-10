#!/usr/bin/env python3
# Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
"""Build only the offline settings fixture with actual llcontrol.cpp and existing libraries."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args();build=a.build.resolve();root=Path(__file__).resolve().parents[2]
s=(build/'build.ninja').read_text()
m=re.search(r'^build [^\n]+/llcontrol.cpp.o: [^\n]* (/[^\n]*/indra/)llxml/llcontrol.cpp[^\n]*\n((?:  [^\n]*\n)+)',s,re.M)
if not m: raise SystemExit('Existing control compiler flags unavailable')
v=dict(line.strip().split(' = ',1) for line in m.group(2).splitlines())
flags=iter(shlex.split(v['DEFINES']+' '+v['INCLUDES']+' '+v['FLAGS']));cmd=['/usr/bin/c++']
for flag in flags:
    if flag=='-include': next(flags);continue
    if flag.startswith(('-fpch','-Winvalid-pch','-O','-g')) or flag=='-DNDEBUG': continue
    cmd.append(flag.replace(m.group(1),str(root/'indra')+'/'))
cmd+=['-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I'+str(root/'indra/llxml'),str(root/'indra/newview/tests/sltelemetrysettings_test.cpp'),str(root/'indra/llxml/llcontrol.cpp'),'-Wl,--gc-sections','-Wl,--start-group']
for lib in ['llxml','llfilesystem','llcachestore','llmath','llmeshoptimizer','llcommon']:cmd.append(str(build/lib/('lib'+lib+'.a')))
for lib in ['apr-1','aprutil-1','expat']:cmd.append(str(build/'packages/lib/release'/('lib'+lib+'.a')))
for lib in ['boost_fiber','boost_context','boost_filesystem','boost_thread','boost_program_options','boost_url','z','ssl','crypto']:cmd.append(str(build/'packages/lib/sse/release'/('lib'+lib+'.a')))
cmd+=['-Wl,--end-group','-ldl','-pthread','-lrt','-o',str(a.output.resolve())]
print(shlex.join(cmd),flush=True)
subprocess.run(cmd,cwd=a.output.resolve().parent,check=True)
subprocess.run([str(a.output.resolve())],check=True)
print('PASS actual LLControlGroup consent0/-1/+1 save/reload and open/write/replace failure fixtures',flush=True)
