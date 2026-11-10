"""Regression guard for the rigged pre-skin capacity (indra/newview/vkgfx/llcleanrigsizing.h).

Compiles scripts/check/test_rig_sizing.cpp with the host C++ compiler and runs it: a frame-by-frame
simulation of buildRiggedFrame's scan / request, rigPrestep's grow and the cull's allocation (cache
arena, spill, deterministic drop, lagged readback) around the real target and capacity code. With
the VRAM ladder calm no rigged face may stay dropped past the frame it arrived; the old
demand-following policy must still reproduce the lagged drops (the model sees the bug); every
capacity stays inside the rigged budget; faces return once pressure ends.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "rig-sizing"
STAGE = "source"
DESCRIPTION = "rigged capacity follows demand while calm (llcleanrigsizing.h fixtures)"
GUARDS = ("Rigged mesh parts missing on a calm GPU (#VKRigDrop rigged_capacity with pressure 0): the "
          "skinned-vertex / work capacity lagging the demand by the readback.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: rigged sizing fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_rig_sizing.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rig_sizing")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "calm: no late drop; old policy reproduces; budget bound; faces return"
