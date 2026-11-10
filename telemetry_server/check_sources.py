#!/usr/bin/env python3
# Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
"""Serial -fsyntax-only checks using existing Ninja flags; never builds a viewer.

No configure, dependency download, PCH generation, object, link or package step.
For an isolated backport overlay, pass --source-root pointing at its own headers.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

SOURCES = [
    'newview/sltelemetry.cpp', 'newview/sltelemetryreport.cpp',
    'newview/llfloatersltelemetry.cpp', 'newview/llappviewer.cpp',
    'newview/llstartup.cpp', 'newview/llfloaterpreference.cpp',
    'newview/llfloaterreportbug.cpp', 'newview/llviewerfloaterreg.cpp',
    'newview/slupdatechecker.cpp', 'newview/sldatausage.cpp',
    'newview/llcrossingtrace.cpp', 'newview/llmemdiag.cpp',
    'llcommon/llmemorygovernor.cpp', 'llcorehttp/_httpoprequest.cpp',
    'llcorehttp/_httpservice.cpp', 'llcorehttp/httpoptions.cpp',
]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--logs', type=Path, required=True)
    parser.add_argument('--legacy-app-warnings', action='store_true', help='Only for baseline-verified warnings in existing app/startup hooks')
    parser.add_argument('sources', nargs='*', default=SOURCES)
    args = parser.parse_args()
    build, root = args.build.resolve(), args.source_root.resolve()
    ninja = (build / 'build.ninja').read_text()
    args.logs.mkdir(parents=True, exist_ok=True)
    for source in args.sources:
        reference = 'newview/llfloaterreportbug.cpp' if source.startswith('newview/') else 'llcorehttp/_httpoprequest.cpp'
        pattern = r'^build [^:]+: [^\n]* (/[^\n]*/indra/)' + re.escape(reference) + r'[^\n]*\n((?:  [^\n]*\n)+)'
        match = re.search(pattern, ninja, re.MULTILINE)
        if not match:
            raise SystemExit('Existing compiler flags unavailable: ' + reference)
        values = {}
        for line in match.group(2).splitlines():
            key, unused, value = line.strip().partition(' = ')
            values[key] = value
        flags = shlex.split(values['DEFINES'] + ' ' + values['INCLUDES'] + ' ' + values['FLAGS'])
        command = ['/usr/bin/c++']
        flags = iter(flags)
        for flag in flags:
            if flag == '-include':
                next(flags)
                continue
            if flag.startswith(('-fpch', '-Winvalid-pch', '-O')):
                continue
            command.append(flag.replace(match.group(1), str(root / 'indra') + '/'))
        command += ['-O1', '-fsyntax-only', '-Werror']
        if args.legacy_app_warnings and source == 'newview/llappviewer.cpp':
            command += ['-Wno-error=deprecated-declarations', '-Wno-error=class-memaccess', '-Wno-error=misleading-indentation']
        if args.legacy_app_warnings and source == 'newview/llstartup.cpp':
            command += ['-Wno-error=deprecated-declarations']
        command.append(str(root / 'indra' / source))
        log = args.logs / (source.replace('/', '_') + '.log')
        print('START', source, flush=True)
        with log.open('w') as output:
            result = subprocess.run(command, cwd=build, stdout=output, stderr=subprocess.STDOUT)
        print('PASS' if not result.returncode else 'FAIL', source, str(log), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)

if __name__ == '__main__':
    main()
