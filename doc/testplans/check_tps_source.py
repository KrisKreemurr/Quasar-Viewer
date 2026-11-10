#!/usr/bin/env python3
"""Serial actual-TU syntax checks using existing Ninja flags; no viewer build."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--logs', required=True, type=Path)
parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
args = parser.parse_args()
ninja = (args.build / 'build.ninja').read_text()
args.logs.mkdir(parents=True, exist_ok=True)
for name in ('sltpscamera.cpp', 'llagent.cpp', 'llviewermessage.cpp'):
    match = re.search(r'^build [^\n]+: [^\n]* (/[^\n]*/indra/)newview/' +
                      re.escape(name) + r'[^\n]*\n((?:  [^\n]*\n)+)', ninja, re.M)
    if not match:
        raise SystemExit('No existing compiler command: ' + name)
    values = dict(line.strip().split(' = ', 1) for line in match.group(2).splitlines())
    source_flags = iter(shlex.split(values['DEFINES'] + ' ' + values['INCLUDES'] + ' ' + values['FLAGS']))
    command = ['/usr/bin/c++']
    for flag in source_flags:
        if flag == '-include':
            next(source_flags)
            continue
        if flag.startswith(('-fpch', '-Winvalid-pch', '-O')):
            continue
        command.append(flag.replace(match.group(1), str(args.source_root.resolve() / 'indra') + '/'))
    command += ['-O1', '-fsyntax-only', '-Werror', str(args.source_root.resolve() / 'indra/newview' / name)]
    print('START', name, flush=True)
    started = time.monotonic()
    with (args.logs / (name + '.log')).open('w') as log:
        log.write(shlex.join(command) + '\n')
        log.flush()
        result = subprocess.run(command, cwd=args.build, stdout=log, stderr=subprocess.STDOUT)
    print('PASS' if result.returncode == 0 else 'FAIL', name,
          round(time.monotonic() - started, 2), 's', flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
