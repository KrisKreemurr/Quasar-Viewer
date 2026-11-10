"""Regression guard for point lights lighting the next room through its wall: the own-fixture clip.

SL point lights sit inside their fixtures, so a shadow ray whose committed caster is the light's FIXTURE (the
light's own prim at any size since #1439 / f51dd5896a, or a small instance whose box holds the light's centre --
clean_vis_walk.glsl rtFixtureBox) is traced again. The old rule traced it again only up to where it ENTERS the
fixture's box and called the rest lit: every OTHER instance inside that box (a wall, a closed door) never
shadowed, and a receiver inside the box (entry 0) was lit outright. A lamp prim whose box reached through a wall
lit the next room through the wall and its closed door, diffuse and specular, as if it had no shadow at all
(the pool-room report, 2026-11-07). The fix (rtFixNext, one rule for rtVisTraceK, rtVisQueued and
clean_shadow_core.glsl shadowSampleRays): the part before the box as before, then the part inside it walked
NoOpaque with only the fixture's own candidates passed over (rt_fix_skip).

The fixture compiles the production rtVisTraceK (+ rtFixNext when present) into C++ against a mock walk along
one ray -- TerminateOnFirstHit commits ANY hit, so every commit order of the scene's hits is replayed -- and
requires, in every order:
  * lamp alone, nested shade + bulb, receiver inside a large own prim with nothing else: LIT (the lamp's own
    geometry never shadows it -- #1439's stove);
  * a wall before the lamp's box: BLOCKED;
  * a wall INSIDE the lamp's box between the receiver and the light: BLOCKED (old: lit when the shade was found
    first);
  * a receiver INSIDE a large own prim's box behind a wall and a closed door: BLOCKED (old: lit, entry 0);
  * a wall inside the outer box of a nested lamp: BLOCKED.
Source wiring: the walk runs NoOpaque and passes fixture candidates over under rt_fix_skip, never re-clips a
skipped walk at a fixture, and the three re-trace sites all take rtFixNext.
"""
import itertools
import os
import re
import shutil
import subprocess
import tempfile

NAME = "fixture-occluders"
STAGE = "source"
DESCRIPTION = "a point light's own-fixture re-trace keeps every other occluder in the fixture's box (no light through walls)"
GUARDS = ("Point lights lighting the next room through a wall and a closed door (diffuse + specular, as if "
          "unshadowed): the own-fixture rule clipping the shadow ray at the fixture box's entry (#1439 own prim "
          "at any size; pool room 2026-11-07).")


def _function(src, ret, name):
    m = re.search(ret + r"\s+" + name + r"\([^)]*\)\s*\{", src)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


def _cxx(fn):
    fn = re.sub(r"\binout\s+(\w+)\s+(\w+)", r"\1& \2", fn)
    fn = fn.replace("[[dont_unroll]]", "")
    return "static " + fn


# Scenes along one ray from the receiver (t = 0) toward a light at t = 5 (the walk's tFull 4.98). Each instance:
# (name, fixture?, box entry along the ray (its slab entry: may be < 0 -- the receiver inside), [surface hits t]).
_SCENES = [
    ("lamp alone", True, [("shade", True, 4.7, [4.8]), ("bulb", True, 4.9, [4.95])]),
    ("wall before the lamp's box", False, [("wall", False, 0.0, [2.0]), ("shade", True, 4.7, [4.8])]),
    ("wall inside the lamp's box", False, [("lamp", True, 3.6, [3.7, 4.8]), ("wall", False, 0.0, [4.0])]),
    ("receiver inside a big own prim, wall + closed door", False,
     [("own prim", True, -2.0, [1.5, 4.9]), ("wall", False, 0.0, [1.0, 1.1]), ("door", False, 0.0, [1.05])]),
    ("receiver inside a big own prim, nothing else", True, [("own prim", True, -2.0, [1.5, 4.9])]),
    ("nested shade + bulb", True, [("shade", True, 4.5, [4.6]), ("bulb", True, 4.9, [4.92])]),
    ("wall inside a nested lamp's outer box", False,
     [("shade", True, 4.5, [4.6]), ("bulb", True, 4.9, [4.92]), ("wall", False, 0.0, [4.55])]),
]

_HARNESS = r"""
#include <algorithm>
#include <cstdio>
#include <vector>
using uint = unsigned int;
struct vec3 {
    float r, g, b;
    vec3() : r(0), g(0), b(0) {}
    explicit vec3(float v) : r(v), g(v), b(v) {}
};
static vec3 operator*(vec3 a, vec3 b) { vec3 c; c.r = a.r * b.r; c.g = a.g * b.g; c.b = a.b * b.b; return c; }
static vec3& operator*=(vec3& a, vec3 b) { a = a * b; return a; }
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static float rt_fix_t = -1.0f;
static bool rt_fix_skip = false;
struct Hit { int inst; float t; };
struct Inst { bool fixture; float entry; };
static std::vector<Inst> g_inst;
static std::vector<Hit> g_order;   // the traversal order: TerminateOnFirstHit commits the first hit found in it
static int g_walks = 0;
// The walk (clean_vis_walk.glsl rtVisWalkFP) on the mock scene: a walk under rt_fix_skip passes every fixture
// candidate over (NoOpaque); otherwise the first hit found commits -- a fixture's sets rt_fix_t (rtWalkBlocked:
// its box entry, >= 0). Either way a committed caster returns 0, no hit 1.
static vec3 rtVisWalkF(vec3, vec3, float tmin, float tmax, uint, uint, uint)
{
    ++g_walks;
    for (const Hit& h : g_order)
    {
        if (h.t < tmin || h.t > tmax) continue;
        const Inst& in = g_inst[h.inst];
        if (in.fixture && rt_fix_skip) continue;
        if (in.fixture) rt_fix_t = std::max(in.entry, 0.0f);
        return vec3(0.0f);
    }
    return vec3(1.0f);
}
HELPERS
int main()
{
    int bad = 0, orders = 0, maxWalks = 0;
SCENES
    std::printf("%d scenes x every commit order (%d orders), <= %d walks per ray\n", SCENE_COUNT, orders, maxWalks);
    return bad ? 1 : 0;
}
"""


def _scene_code(scenes):
    out = []
    for name, lit, insts in scenes:
        hits = [(i, t) for i, (_, _, _, ts) in enumerate(insts) for t in ts]
        out.append("    g_inst = {%s};" % ", ".join("{%s, %.4ff}" % ("true" if f else "false", e)
                                                   for (_, f, e, _) in insts))
        for perm in itertools.permutations(hits):
            out.append("    g_order = {%s}; rt_fix_t = -1.0f; rt_fix_skip = false; g_walks = 0;"
                       % ", ".join("{%d, %.4ff}" % (i, t) for i, t in perm))
            out.append("    { const vec3 w = rtVisTraceK(vec3(0.0f), vec3(0.0f), 4.98f, 0u, 0u, 0u); ++orders;"
                       " maxWalks = std::max(maxWalks, g_walks);"
                       " if ((w.r > 0.5f) != %s || rt_fix_skip) { if (bad++ < 12) std::printf(\"FAIL %s: %s, order %s\\n\"); } }"
                       % ("true" if lit else "false", name, "expected lit, got blocked" if lit else
                          "expected blocked (the light reaches through), got lit",
                          " ".join("%s@%g" % (insts[i][0], t) for i, t in perm)))
    return "\n".join(out)


def _wiring(read):
    missing = []
    walk = read("indra/vkclean/shaders/clean_vis_walk.glsl")
    core = read("indra/vkclean/shaders/clean_shadow_core.glsl")
    fp = _function(walk, "vec3", "rtVisWalkFP") or ""
    if not re.search(r"const bool fixSkip = SH_FIX_ON && rt_fix_skip && !fol_walk;\s*if \(fixSkip\) flags \|= gl_RayFlagsNoOpaqueEXT;", fp):
        missing.append("rtVisWalkFP: a fixture-skip walk (rt_fix_skip) does not run NoOpaque")
    if "!fixSkip" not in fp or "if (rtFixtureBox(id, rq, false, sa, sb)) continue;" not in fp:
        missing.append("rtVisWalkFP: the fixture-skip walk does not pass the fixture's candidates over (or keeps the opaque-first pre-ray)")
    if "SH_FIX_ON && !rt_fix_skip && !trans_walk && rtFixtureBox(fid, q, true, fa, fb)" not in walk:
        missing.append("rtWalkBlocked: a fixture-skip walk can still be re-clipped at a fixture's box")
    q = _function(walk, "vec3", "rtVisQueued") or ""
    if "rtFixNext(stage, t0, tmc, tmax, acc, vis)" not in q or "rt_fix_skip = stage == 2u" not in q:
        missing.append("rtVisQueued (the visibility queue's walk) does not take rtFixNext's steps")
    rays = _function(core, "vec3", "shadowSampleRays") or ""
    if "rtFixNext(fst, tm0, tmc, tFull, facc, w)" not in rays or "rt_fix_skip = SHR_FR(st) == 2u;" not in rays \
            or "rtVisWalkFP(o, d, tm0, tmc," not in rays:
        missing.append("clean_shadow_core.glsl shadowSampleRays (pass 15) does not take rtFixNext's steps")
    return missing


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    walk = read("indra/vkclean/shaders/clean_vis_walk.glsl")
    trace = _function(walk, "vec3", "rtVisTraceK")
    if trace is None:
        result.fail("clean_vis_walk.glsl rtVisTraceK not found (moved or renamed?): the fixture covers nothing")
        return
    step = _function(walk, "bool", "rtFixNext")
    helpers = ([_cxx(step)] if step else []) + [_cxx(trace)]
    missing = _wiring(read) if step else ["clean_vis_walk.glsl has no rtFixNext: the re-trace clips the ray at the "
                                          "fixture's box entry (other occluders in the box never shadow)"]
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.fail("\n".join(missing)) if missing else result.warn("no C++ compiler: the replay not run")
        return
    source = (_HARNESS.replace("HELPERS", "\n".join(helpers))
              .replace("SCENES", _scene_code(_SCENES)).replace("SCENE_COUNT", str(len(_SCENES))))
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "fixture_occluders.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-w", src, "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("the fixture does not compile against the production walk:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out else []) + missing))
        return
    result.summary = out.splitlines()[-1] if out else "ok"
