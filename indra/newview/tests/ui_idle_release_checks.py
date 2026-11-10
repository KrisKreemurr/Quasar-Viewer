#!/usr/bin/env python3
"""#1405: the UI idle sweep keeps the skin sprites and glyph pages widgets share.

Extracts the production keepThroughIdleSweep() from llrendervkbackend.cpp into a
small native fixture and checks which cache entries the idle sweep may release,
then checks that tickCache()'s idle release actually consults it.

Usage:
  python3 <source>/indra/newview/tests/ui_idle_release_checks.py \
      --source-root <source> --out <scratch-dir> --run
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess


def extract(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"missing production function: {marker}")
    brace = source.find("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise RuntimeError(f"unterminated production function: {marker}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(args.source_root)
    out = pathlib.Path(args.out)
    backend_path = root / "indra/newview/vkgfx/llrendervkbackend.cpp"
    backend = backend_path.read_text()

    helper = extract(backend, "static bool keepThroughIdleSweep(")
    constant_line = next((l for l in backend.splitlines()
                          if "kIdleKeepSpriteBytes" in l and "constexpr" in l), None)
    if constant_line is None:
        raise RuntimeError("missing kIdleKeepSpriteBytes")

    # The idle release in tickCache() must consult the helper (while there is no
    # VRAM pressure), or the helper is dead code.
    tick = extract(backend, "void LLRenderVKBackend::tickCache()")
    idle_release = tick[tick.find("if (now >= mNextIdleSweep)"):]
    idle_release = idle_release[:idle_release.find("sCacheBytesPublished")]
    if "keepThroughIdleSweep(t.kind, t.bytes)" not in idle_release:
        raise RuntimeError("tickCache() idle release does not consult keepThroughIdleSweep()")

    cpp = '''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using U8=uint8_t; using U64=uint64_t;
struct LLTextureData { enum { VKUI_UNTAGGED = 0, VKUI_FONT, VKUI_FONT_COLOR, VKUI_SPRITE, VKUI_ICON,
    VKUI_THUMB, VKUI_MAP, VKUI_MEDIA, VKUI_AVATAR, VKUI_WORLD, VKUI_KIND_COUNT }; };
CONSTANT
HELPER
int main(){
 using T=LLTextureData;
 // Skin sprites (window backgrounds, focus frames, button states) stay.
 assert(keepThroughIdleSweep(T::VKUI_SPRITE, 64ull << 10));
 assert(keepThroughIdleSweep(T::VKUI_SPRITE, 256ull << 10));
 // A large skin image (a login background) is still released while idle.
 assert(!keepThroughIdleSweep(T::VKUI_SPRITE, (256ull << 10) + 1));
 // Glyph atlas pages, grayscale and colour, stay.
 assert(keepThroughIdleSweep(T::VKUI_FONT, 256ull << 10));
 assert(keepThroughIdleSweep(T::VKUI_FONT_COLOR, 1ull << 20));
 // Everything the idle release is for still goes: thumbnails, map tiles, icons,
 // media, and anything untagged.
 for (U8 k : {(U8)T::VKUI_UNTAGGED, (U8)T::VKUI_ICON, (U8)T::VKUI_THUMB, (U8)T::VKUI_MAP,
              (U8)T::VKUI_MEDIA, (U8)T::VKUI_AVATAR, (U8)T::VKUI_WORLD})
  assert(!keepThroughIdleSweep(k, 4ull << 10));
 std::puts("UI idle release: PASS (small sprites and glyph pages kept, large and other kinds released)");
}
'''
    cpp = cpp.replace("CONSTANT", constant_line.strip()).replace("HELPER", helper)
    out.mkdir(parents=True, exist_ok=True)
    generated = out / "ui_idle_release_checks.cpp"
    generated.write_text(cpp)
    print(generated)
    if args.run:
        exe = out / "ui_idle_release_checks"
        result = subprocess.run(["c++", "-std=c++20", "-O0", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                                 str(generated), "-o", str(exe)], cwd=out, timeout=60)
        if result.returncode:
            return result.returncode
        return subprocess.run([str(exe)], cwd=out, timeout=10).returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
