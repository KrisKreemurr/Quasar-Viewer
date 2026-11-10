"""Regression guard for crawling noise in mixed local light beside moving casters (radiance mode).

RenderVKShadowCasterHistory 2 resets a texel's local-light history when its visibility evidence (MOT w, the
producer's num / den) and its same-surface 3x3 mean both leave the history's visibility. That evidence is ONE
or TWO RIS picks: 0 or 1 per pick wherever one light reaches the receiver and another is blocked. The reused
reservoir pick is shared by the spatial-reuse neighbourhood, so the 3x3 mean flipped with it, and a 0.1
tolerance read every flip as a moving caster: on every moving-caster-tagged texel (an animating avatar and
its shadows) the history was cut to two frames and the binary picks crawled through. The fix:
  * the evidence counts only spatially independent samples (pdnEvidenceWeight; MOT bit 19 marks exact lists);
  * a change must exceed the 3x3 mean's sampling noise (dnCasterSamplingNoise: Bhatia-Davis p (1 - p),
    pooled with a young prior) -- K = 3.5 in one frame, or 2.2 in the same direction in two consecutive frames
    (dnCasterPending / dnCasterConfirmed: two pending bits in W's low mantissa);
  * the prior accumulates the 3x3 mean at 10 bits (the 5-bit code lost every 32-frame step: it drifted);
  * a young history still deviating keeps the two-frame cap (dnCasterFollow).

The fixture compiles those production GLSL helpers (clean_dn_temporal.comp, clean_pdn_common.glsl,
clean_history_time.glsl) into C++ and replays the temporal policy on a 24 x 24 texel receiver lit by two equal
point lights + a faint wall-blocked third (3 candidates > K = 2: the RIS picks, not the exact list):
  (a) one light visible, one blocked, every texel moving-caster tagged (an animating receiver), at 60, 30 and
      144 Hz: <= 0.1 reset per texel-second, mean = the visible light alone, and at 60 Hz a frame-to-frame rms
      <= 2 x a detector-free converged history's;
  (b) a lit floor; a caster's shadow edge sweeps in blocking one light, then away (0.5 and 2 texels / frame);
  (c) the mixed floor of (a); an edge sweeps in blocking the visible light (-> 0), then away.
(b) / (c) bound the bias behind the moving edge (the 40a5446bba trailing) over 64 runs -- interior (full 3x3)
<= 0.2, the grid's edge rows (6 same-surface neighbours: a silhouette) <= 0.6 -- and a no-detector control
must trail, so the bound means something. Old code (no pdnEvidenceWeight / dnCasterSamplingNoise) replays with
its own evidence and test, and fails (a): 55 % resets, rms 42 x.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "caster-noise"
STAGE = "source"
DESCRIPTION = "mixed local light beside moving casters: no pick-noise history resets, no trailing edge"
GUARDS = ("Crawling noise on animating avatars / moving-caster shadows where one local light is visible and "
          "another blocked: the caster-change detector reading one-pick visibility noise as a moving caster; "
          "and the opposite failure, a moving caster's shadow trailing behind it.")


def _function(src, name):
    m = re.search(r"(?:bool|float|uint)\s+" + name + r"\([^)]*\)\s*\{", src)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


_FIXTURE = r"""
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using uint = unsigned int;
using std::abs; using std::isinf; using std::sqrt; using std::exp; using std::log;
static float min(float a, float b) { return std::min(a, b); }
static float max(float a, float b) { return std::max(a, b); }
static float clamp(float x, float a, float b) { return std::clamp(x, a, b); }
static float mix(float a, float b, float t) { return a + (b - a) * t; }
static float testStep = 1.0f;   // (frame interval / the 60 Hz reference: the (a) replay's cadence)
static float htStep() { return testStep; }
HELPERS
// The temporal pass's caster test as clean_dn_temporal.comp main() composes the helpers (edgeD / the
// pending bits / the restart); the OLD composition when the source has none of them.
static bool detect(float own, float mean, float prior, float sig, float n, float len, uint prevPend, uint& pend, bool& follow)
{
    follow = false;
#ifdef NEW_POLICY
    const float noise = dnCasterSamplingNoise(prior, mean, n, n, len);
    const float cur = dnCasterCurrent(own, mean, false);   // (every texel's evidence a stochastic pick)
    pend = dnCasterPending(cur, mean, prior, sig, noise);
    const bool changed = dnCasterVisibilityChange(cur, mean, prior, sig, noise) || dnCasterConfirmed(pend, prevPend);
    follow = dnCasterFollow(pend, len);
    if (changed) pend = 0u;
    return changed;
#else
    (void)n; (void)len; (void)prevPend; pend = 0u;
    return dnCasterVisibilityChange(own, mean, prior, sig);
#endif
}
static float restart(float own, float mean, float n)
{
#ifdef NEW_POLICY
    return dnCasterEvidence(own, mean, n);
#else
    (void)mean; (void)n; return own;
#endif
}
static float evidence(float own, float mean, float n) { return restart(own, mean, n); }
static float evidenceWeight(bool reused, uint ke, uint j)
{
#ifdef NEW_POLICY
    return pdnEvidenceWeight(reused, ke, j);
#else
    (void)reused; (void)ke; (void)j; return 1.0f;   // (the old evidence: every sample's num / den)
#endif
}

static const int N = 24, BLOCK = 4;
static uint rng = 0x9E3779B9u;
static float rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return float(rng >> 8) * (1.0f / 16777216.0f); }
// the history meta's evidence prior: its stored code (10 bits; old: 5) and accumulation rate (old: the colour's)
static float storePrior(float v)
{
#ifdef NEW_POLICY
    return pdnMetaVisibility(pdnMetaVisibilityCode(v));
#else
    return pdnVisibilityValue(pdnVisibilityCode(v));
#endif
}
static float evidenceAlpha(float age, float colourAlpha)
{
#ifdef NEW_POLICY
    (void)colourAlpha; return dnCasterEvidenceAlpha(age);
#else
    (void)age; return colourAlpha;
#endif
}
// the two point lights + a faint candidate behind a wall (3 candidates > K = 2: the RIS picks, not the exact list)
static const float T[3] = {1.0f, 1.0f, 0.05f};
static int pick() { const float u = rnd() * (T[0] + T[1] + T[2]); return u < T[0] ? 0 : (u < T[0] + T[1] ? 1 : 2); }

struct Px { float acc = 0, age = 0, vis = -1, ev = -1, raw = 0; uint pend = 0; bool init = false; };
// One frame of the radiance producer (K = 2: the reused reservoir, shared by its BLOCK x BLOCK spatial-reuse
// neighbourhood, + the fresh pick, per texel) and the temporal pass (every texel moving-caster tagged:
// casterAffected -- an animating receiver, an avatar's shadow). vis(x, y, light): 1 visible, 0 blocked.
template <class Vis>
static int frame(std::vector<Px>& g, Vis vis, bool detector)
{
    const float W = T[0] + T[1] + T[2];
    int res[N / BLOCK][N / BLOCK];
    for (auto& r : res) for (int& v : r) v = pick();
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        Px& p = g[y * N + x];
        const int li[2] = {res[y / BLOCK][x / BLOCK], pick()};
        float raw = 0, en = 0, ed = 0;
        for (uint j = 0; j < 2; ++j)
        {
            const float v = vis(x, y, li[j]), c = W * 0.5f;   // target x weight W / (Ke p)
            raw += c * v;
            const float ew = evidenceWeight(true, 2u, j);
            en += c * v * ew; ed += c * ew;
        }
        p.raw = raw;
        p.ev = ed > 1e-8f ? pdnVisibilityValue(pdnVisibilityCode(en / ed)) : -1.0f;
    }
    int fired = 0;
    std::vector<Px> out = g;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
    {
        const Px& p = g[y * N + x];
        Px& o = out[y * N + x];
        if (!p.init) { o.acc = p.raw; o.age = 1; o.vis = storePrior(p.ev); o.init = true; continue; }
        float s = 0, s2 = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
            const float e = g[qy * N + qx].ev;
            if (e < 0) continue;
            s += e; s2 += e * e; n += 1;
        }
        const float mean = n > 0 ? s / n : 0, sig = n > 0 ? sqrt(max(s2 / n - mean * mean, 0.0f)) : 0;
        uint pend = 0u;
        bool follow = false;
        const bool changed = detector && n > 0 && detect(p.ev, mean, p.vis, sig, n, p.age, p.pend, pend, follow);
        float hist = p.acc, len = p.age;
        if (changed) { len = 0; hist = p.raw; ++fired; }
        const float cap = dnCasterShouldCap(detector ? 2u : 0u, true, changed || follow) ? dnCasterHistoryCap(32.0f, htStep()) : 32.0f;
        const float age = htAdvance(len, cap);
        const float a = len > 0 ? htAlpha(1.0f / max(age, 1.0f)) : 1.0f;
        o.acc = mix(hist, p.raw, a);
        o.age = age;
        o.pend = pend;
        o.vis = storePrior(dnCasterVisibilityAcc(p.vis, changed ? restart(p.ev, mean, n) : evidence(p.ev, mean, n),
                                                 changed ? 1.0f : evidenceAlpha(p.age, a)));
    }
    g.swap(out);
    return fired;
}

int main()
{
    int bad = 0;
    // (a) A visible, B blocked, C blocked: the truth is A alone (1). Detector vs no detector (the control's
    // frame-to-frame rms is what a converged 32-frame history of these picks gives).
    for (float fps : {60.0f, 30.0f, 144.0f})
    {
    testStep = 60.0f / fps;
    double rmsC = 0;
    for (int det = 0; det <= 1; ++det)
    {
        std::vector<Px> g(N * N);
        auto mixed = [](int, int, int l) { return l == 0 ? 1.0f : 0.0f; };
        for (int f = 0; f < 128; ++f) frame(g, mixed, det != 0);
        double fires = 0, sum = 0, d2 = 0, cnt = 0, pv = 0, pv2 = 0;
        std::vector<float> prev(N * N);
        for (int i = 0; i < N * N; ++i) prev[i] = g[i].acc;
        for (int f = 0; f < 2048; ++f)
        {
            fires += frame(g, mixed, det != 0);
            for (int i = 0; i < N * N; ++i)
            {
                sum += g[i].acc; d2 += (g[i].acc - prev[i]) * (g[i].acc - prev[i]); prev[i] = g[i].acc; cnt += 1;
                pv += g[i].vis; pv2 += g[i].vis * g[i].vis;
            }
        }
        const double rate = fires / cnt, mean = sum / cnt, rms = sqrt(d2 / cnt), pm = pv / cnt, ps = sqrt(max(float(pv2 / cnt - pm * pm), 0.0f));
        if (!det) { rmsC = rms; continue; }
        std::printf("(a) %g Hz: one light visible, one blocked, moving-caster tagged: resets %.5f / texel-frame, mean %.3f "
                    "(the visible light alone: 1), frame-to-frame rms %.4f (no detector %.4f); evidence prior %.3f +- %.3f\n",
                    fps, rate, mean, rms, rmsC, pm, ps);
        // (resets per texel-SECOND: the per-frame test sees the same evidence at any rate, so its false rate
        // per second grows with the frame rate -- bounded at 0.1 / s, 144 Hz included)
        if (rate * fps > 0.1) { std::printf("FAIL (a): pick noise resets the history (%.3f / texel-second > 0.1)\n", rate * fps); bad = 1; }
        if (abs(mean - 1.0) > 0.03) { std::printf("FAIL (a): mean %.3f != the visible light alone (1)\n", mean); bad = 1; }
        if (fps == 60.0f && rms > 2.0 * rmsC) { std::printf("FAIL (a): frame-to-frame rms %.4f > 2 x converged %.4f (crawling)\n", rms, rmsC); bad = 1; }
    }
    }
    testStep = 1.0f;
    // (b) / (c): a caster's shadow edge sweeps in from x = 0 (b: blocking light B on a lit floor, c: blocking the
    // visible light A on the mixed floor of (a)), then leaves the same way, at 0.5 and 2 texels per frame. The
    // lag: the signed mean error (the bias toward the old state) of the texels whose 3x3 the edge left >= 2
    // frames ago, relative to the change. INTERIOR = full 3x3 (the pass / fail number); EDGE ROWS = 6
    // same-surface neighbours (a silhouette's / thin surface's lower test power), reported and bounded looser.
    for (int scen = 0; scen < 2; ++scen)
        for (float speed : {0.5f, 2.0f})
            for (int det = 1; det >= 0; --det)
            {
                const int blocked = scen == 0 ? 1 : 0, F = int(float(N + 4) / speed) + 1, RUNS = 64;
                rng = 0x9E3779B9u + uint(scen * 4 + int(speed));   // (each case its own fixed seed)
                const float before = scen == 0 ? 2.0f : 1.0f, after = scen == 0 ? 1.0f : 0.0f;
                // per (phase, frame): summed error and count over RUNS independent runs (a single run's young
                // histories make the bias of a few columns a noisy number)
                std::vector<double> eI(2 * F, 0.0), nI(2 * F, 0.0), eE(2 * F, 0.0), nE(2 * F, 0.0);
                for (int run = 0; run < RUNS; ++run)
                {
                    std::vector<Px> g(N * N);
                    for (int f = 0; f < 192; ++f)
                        frame(g, [&](int, int, int l) { return scen == 0 ? (l < 2 ? 1.0f : 0.0f) : (l == 0 ? 1.0f : 0.0f); }, det != 0);
                    for (int phase = 0; phase < 2; ++phase)
                        for (int f = 0; f < F; ++f)
                        {
                            const float edge = speed * float(f + 1);   // texels x < edge: shadowed (phase 0) / freed (1)
                            auto vis = [&](int x, int, int l) {
                                if (l == 2 || (scen == 1 && l == 1)) return 0.0f;
                                const bool in = phase == 0 ? float(x) < edge : float(x) >= edge;
                                return (in && l == blocked) ? 0.0f : 1.0f;
                            };
                            frame(g, vis, det != 0);
                            const int k = phase * F + f;
                            for (int y = 0; y < N; ++y) for (int x = 1; x < N - 1; ++x)
                            {
                                if (float(x) + 1.0f + 2.0f * speed >= edge) continue;   // the 3x3 left >= 2 frames ago
                                const float e = g[y * N + x].acc - (phase == 0 ? after : before);
                                if (y == 0 || y == N - 1) { eE[k] += e; nE[k] += 1; } else { eI[k] += e; nI[k] += 1; }
                            }
                        }
                }
                double worstI = 0, worstE = 0;
                for (int k = 0; k < 2 * F; ++k)
                {
                    if (nI[k] > 0) worstI = std::max(worstI, abs(eI[k]) / nI[k] / abs(before - after));
                    if (nE[k] > 0) worstE = std::max(worstE, abs(eE[k]) / nE[k] / abs(before - after));
                }
                const char c = scen == 0 ? 'b' : 'c';
                std::printf("(%c) %s, %.1f texel / frame: worst relative bias behind the edge: interior %.3f, edge rows %.3f\n",
                            c, det ? "detector" : "no detector (control: must trail)", speed, worstI, worstE);
                if (det && worstI > 0.2) { std::printf("FAIL (%c): the moving caster's shadow trails (interior)\n", c); bad = 1; }
                if (det && worstE > 0.6) { std::printf("FAIL (%c): the moving caster's shadow trails (edge rows)\n", c); bad = 1; }
                if (!det && worstI < 0.3) { std::printf("FAIL (%c): the control does not trail: the lag case tests nothing\n", c); bad = 1; }
            }
    return bad;
}
"""


def _wiring(read):
    """The producer / temporal wiring the replay composes but cannot see: [] when it holds."""
    missing = []
    c = read("indra/vkclean/shaders/clean_deferred_light.comp")
    t = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    if not re.search(r"const float ew = wgt \* pdnEvidenceWeight\(reused, sel\.Ke, si - s0\);", c) \
            or "evN += dt * vis * ew; evD += dt * ew;" not in c or "pdnSampleVisibility(evN, evD) << 9u" not in c:
        missing.append("clean_deferred_light.comp: MOT w's evidence is not the independent samples' (pdnEvidenceWeight)")
    if "(sel.exact ? PDN_MOT_EVIDENCE_EXACT : 0u)" not in c:
        missing.append("clean_deferred_light.comp: MOT w does not mark exact (deterministic) evidence")
    if "visStoch += qExact ? 0.0 : 1.0" not in t \
            or "dnCasterSamplingNoise(receiverHistory.visibility, visMean, visStoch, visN, receiverHistory.ld)" not in t:
        missing.append("clean_dn_temporal.comp: the caster test does not take the 3x3's sampling noise")
    if not re.search(r"\|\| dnCasterConfirmed\(pendV, receiverHistory\.pending\)\)", t) \
            or "((casterChangeD ? 0u : pendV) << PDN_META_PEND_SHIFT)" not in t:
        missing.append("clean_dn_temporal.comp: the pending deviation is not stored / confirmed across frames")
    if "dnCasterEvidence(ownVisibility, visMean, visStoch)," not in t or "pdnMetaVisibilityCode(visibility)" not in t \
            or "dnCasterEvidenceAlpha(receiverHistory.ld)" not in t:
        missing.append("clean_dn_temporal.comp: the 10-bit evidence prior does not accumulate dnCasterEvidence")
    if "casterChangeD || follow" not in t or "dnCasterPolicy() == 2u ? edgeV" not in t:
        missing.append("clean_dn_temporal.comp: the caster test's result does not reach both channels' cap")
    return missing


def assemble(read):
    """(fixture C++ source, new policy?) from the production helpers; None when one is missing."""
    temporal = read("indra/vkclean/shaders/clean_dn_temporal.comp")
    common = read("indra/vkclean/shaders/clean_pdn_common.glsl")
    htime = read("indra/vkclean/shaders/clean_history_time.glsl")
    new = _function(temporal, "dnCasterSamplingNoise") is not None and _function(common, "pdnEvidenceWeight") is not None
    names = [(temporal, n) for n in ("dnCasterChange", "dnCasterVisibilityChange", "dnCasterVisibilityAcc",
                                     "dnCasterShouldCap", "dnCasterHistoryCap")]
    names += [(common, n) for n in ("pdnVisibilityCode", "pdnVisibilityValue")]
    names += [(htime, n) for n in ("htAlpha", "htAdvance")]
    if new:
        names += [(temporal, n) for n in ("dnCasterEvidenceAlpha", "dnCasterSamplingNoise", "dnCasterPending",
                                          "dnCasterConfirmed", "dnCasterFollow", "dnCasterCurrent", "dnCasterEvidence")]
        names += [(common, n) for n in ("pdnEvidenceWeight", "pdnMetaVisibilityCode", "pdnMetaVisibility")]
    parts = re.findall(r"#define DN_CASTER_\w+ [^\n]+", temporal)
    for src, n in names:
        fn = _function(src, n)
        if fn is None:
            return None, n
        parts.append("static " + fn)
    helpers = re.sub(r"(?<![\w.])(\d+\.\d*(?:[eE][-+]?\d+)?)(?![\w.f])", r"\1f", "\n".join(parts))
    return _FIXTURE.replace("HELPERS", helpers), new


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    source, new = assemble(read)
    if source is None:
        result.fail("production helper %s not found (moved or renamed?): the fixture covers nothing" % new)
        return
    missing = _wiring(read) if new else []
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.fail("\n".join(missing)) if missing else result.warn("no C++ compiler: the replay not run")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "caster_noise.cpp")
        with open(src, "w") as o:
            o.write(source)
        exe = os.path.join(tmp, "t")
        c = subprocess.run([cxx, "-std=c++17", "-O2", "-w"] + (["-DNEW_POLICY"] if new else []) + [src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("the fixture does not compile against the production helpers:\n" + c.stderr[:3000])
            return
        r = subprocess.run([exe], capture_output=True, text=True)
    out = r.stdout.strip()
    if r.returncode or missing:
        result.fail("\n".join(([out] if out else []) + missing
                              + ([] if new else ["(old policy: no pdnEvidenceWeight / dnCasterSamplingNoise)"])))
        return
    result.summary = out.splitlines()[0] if out else "ok"
