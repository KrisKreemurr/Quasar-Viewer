"""Regression guard for the sea state's pure math (#1020; indra/newview/slseamath.h).

Compiles scripts/check/test_sea_math.cpp with the host C++ compiler and runs it: the sea-floor
reduction (a thin pillar is ignored, a mesh floor is found, nothing found keeps the terrain) and
the shore breakers' strength (a 20 m river laps, a lee shore gets none, a windward beach keeps
full breakers).
"""
import os
import shutil
import subprocess
import tempfile

NAME = "sea-math"
STAGE = "source"
DESCRIPTION = "sea floor reduction and shore breaker fixtures (slseamath.h)"
GUARDS = ("Waves spawning in a river's centre and running out to both banks; posts making shallows; "
          "mesh sea floors ignored.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: sea math fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_sea_math.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_sea_math")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "floor reduction + river / lee / windward breakers"
