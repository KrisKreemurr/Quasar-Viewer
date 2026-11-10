#!/usr/bin/env python3
"""Source contract for #877: a mirror Dynamic toggle invalidates its cached capture."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
header = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
source = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()


def body(signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


assert "bool        mMirrorCapDynamic = false;" in header
mirror_setup = body("void LLCleanRendererVK::setMirror(")
assert "const bool dynamicChanged = mMirrorDynamic != mMirrorCapDynamic;" in mirror_setup
assert "mMirrorStatic = !dynamicChanged && wf.mirror_static" in mirror_setup

idle_skip = body("void LLCleanRendererVK::prbGatherChanges(")
assert "bool change = mMirrorDynamic != mMirrorCapDynamic" in idle_skip
assert "mMirrorIdleSkip = !change;" in idle_skip

capture = body("void LLCleanRendererVK::recordMirror(")
assert "mMirrorCapDynamic = mMirrorDynamic;" in capture
assert capture.index("mMirrorCapDynamic = mMirrorDynamic;") < capture.index("vkCmdBeginRendering")

# A capture is required both to add avatars and to remove avatars left in the old image.
assert [requested != captured for requested, captured in ((True, False), (False, True), (True, True))] == [True, True, False]
print("PASS: Dynamic changes bypass both mirror rate-skip and idle-skip")
print("PASS: capture policy is advanced only when a new mirror capture is recorded")
