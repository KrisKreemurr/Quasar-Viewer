#!/usr/bin/env python3
"""Fixture for the RT reflection resolve on a rough receiver with a small bright emitter (the owner's
"dancing reflections"). A model of clean_deferred_light.comp's path, not the shader itself:

  floor: W x H receiver pixels, each with a static normal-map tilt (planks) + a per-frame jitter term
         (the jittered pixel centre samples the normal map elsewhere);
  rays:  one per lattice pixel (spacing L) per frame -- MIRROR (reflect about the shading normal) or
         STOCHASTIC (a GGX lobe sample: the mirror direction + a lobe-wide offset, R2 over frames);
  hit:   the small emitter (radius RE rad around its direction; radiance EM) else the env (ENV);
  resolve: the 3x3 + lattice + disc taps with the shader's Gaussian weights (sB from the lobe footprint);
  temporal: the shader's EMA (alpha 1/12 at rough 0.6, 1/5 mirror), its clamp box (mu +- (1.25 sigma +
         0.02 max)) and anti-lag (OLD), or the NEW rule (see temporal()).

Metric: on a static camera / static emitter, per-pixel temporal standard deviation of the resolved
image over the last frames (mean over the streak region and the 99th percentile), plus the bias of
the temporal mean against the reference glossy reflection (many lobe samples per pixel)."""
import numpy as np, sys

W = H = 96
L = 2                  # lattice spacing (density 1/4)
ROUGH = float(sys.argv[1]) if len(sys.argv) > 1 else 0.35
ALPHA = ROUGH * ROUGH
RE = 0.035             # emitter angular radius (rad)
EM = 40.0              # emitter radiance
ENV = 0.05
TILT = 0.06            # static normal-map tilt sigma (rad of normal)
JIT = 0.02             # per-frame jitter of the sampled normal (rad)
FRAMES = 160
LOBE = 2.0 * ALPHA     # reflected-lobe angular sigma (rad) of the GGX lobe (approx)
rng = np.random.default_rng(7)

ys, xs = np.mgrid[0:H, 0:W]
# the flat floor's mirror direction per pixel as a 2D angle (the virtual image is a linear map)
base = np.stack([(xs - W / 2) * 0.004, (ys - H / 2) * 0.004], -1)
tilt = rng.normal(0.0, TILT, (H, W, 2))
emitter = np.array([0.0, 0.03])

T_EM, T_ENV = 1.5, 4.0   # hit distances: the emitter (on the avatar), the wall behind it

def hit_radiance(d):
    r = np.linalg.norm(d - emitter, axis=-1)
    return np.where(r < RE, EM, ENV)

def hit_dist(d):
    r = np.linalg.norm(d - emitter, axis=-1)
    return np.where(r < RE, T_EM, T_ENV)

def rays(frame, stochastic):
    nj = rng.normal(0.0, JIT, (H, W, 2))
    d = base + 2.0 * (tilt + nj)            # a mirror turns twice the normal's tilt
    if stochastic:
        k = frame & 0xFFFF
        u = (np.stack([np.full((H, W), 0.7548776662 * k), np.full((H, W), 0.5698402910 * k)], -1)
             + rot) % 1.0
        # a lobe-shaped 2D offset (Box-Muller of the R2 point, the pixel's rotation)
        rr = np.sqrt(-2.0 * np.log(np.maximum(1.0 - u[..., 0], 1e-6))) * LOBE
        d = d + np.stack([rr * np.cos(6.283185 * u[..., 1]), rr * np.sin(6.283185 * u[..., 1])], -1)
    c = hit_radiance(d)
    lat = ((xs % L) == 0) & ((ys % L) == 0)
    return np.where(lat, c, np.nan), np.where(lat, hit_dist(d), np.nan)

rot = rng.random((H, W, 2))

def resolve(rt, td=None, distw=False):
    # taps: every lattice pixel within the lobe footprint radius rB (sB = 0.6 rB), the shader's Gaussian
    rB = 5.0
    sB = max(0.6 * rB, 0.6 * L)
    acc = np.zeros((H, W)); acc2 = np.zeros((H, W)); wsum = np.zeros((H, W))
    R = int(np.ceil(rB))
    if distw:
        # tMean: the own ray's distance, else the nearest lattice pixel's (the shader's 3 x 3 mean)
        tm = td[(ys // L) * L, (xs // L) * L]
    for dy in range(-R, R + 1):
        for dx in range(-R, R + 1):
            if dx * dx + dy * dy > rB * rB + 2:
                continue
            w = np.exp(-(dx * dx + dy * dy) / (2 * sB * sB))
            sh = np.roll(np.roll(rt, -dy, 0), -dx, 1)
            ok = ~np.isnan(sh)
            if distw:   # rtrHitDistW: exp(-((t - tMean) / (0.35 tMean))^2)
                tq = np.roll(np.roll(td, -dy, 0), -dx, 1)
                w = w * np.where(ok, np.exp(-((tq - tm) / (0.35 * tm)) ** 2), 0.0)
            acc += np.where(ok, w * sh, 0.0); acc2 += np.where(ok, w * sh * sh, 0.0); wsum += np.where(ok, w, 0.0)
    mu = acc / np.maximum(wsum, 1e-9)
    sg = np.sqrt(np.maximum(acc2 / np.maximum(wsum, 1e-9) - mu * mu, 0.0))
    return mu, sg

def temporal(hist, res, mu, sg, new):
    if isinstance(new, tuple):   # (EMA of n frames; loose box: k x the frame's own estimate + 3 sigma above)
        if hist is None: return res
        n, k = new
        a0 = 1.0 / n
        sigma = 1.25 * sg + 0.02 * np.maximum(mu, res) + 1e-4
        if k > 0:
            hist = np.clip(hist, np.minimum(mu, res) - 3.0 * sigma, k * np.maximum(mu, res) + 3.0 * sigma)
        return hist + (res - hist) * a0
    a0 = 1.0 / (5.0 + (12.0 - 5.0) * np.clip((ROUGH - 0.05) / 0.55, 0, 1) ** 2 * (3 - 2 * np.clip((ROUGH - 0.05) / 0.55, 0, 1)))
    sigma = 1.25 * sg + 0.02 * np.maximum(mu, res) + 1e-4
    if hist is None:
        return res
    if not new:
        lo = np.minimum(mu, res) - sigma; hi = np.maximum(mu, res) + sigma
        hc = np.clip(hist, lo, hi)
        off = np.abs(hist - hc) / (sigma + 1e-4)
        a = a0 + (1.0 - a0) * np.clip(off - 0.25, 0, 1)
        return hc + (res - hc) * a
    if new == 3:   # F: the old box, no anti-lag, and the history may DROP only at the EMA rate
        lo = np.minimum(mu, res) - sigma; hi = np.maximum(mu, res) + sigma
        hc = np.maximum(np.clip(hist, lo, hi), hist * (1.0 - a0))
        return hc + (res - hc) * a0
    # NEW (stochastic lobe): no anti-lag; the box keeps only gross outliers (a disocclusion, a moved
    # bright image): 3 sigma below, 3 sigma + the frame's own estimate above
    lo = np.minimum(mu, res) - 3.0 * sigma; hi = np.maximum(mu, res) * 2.0 + 3.0 * sigma
    hc = np.clip(hist, lo, hi)
    return hc + (res - hc) * a0

def run(stochastic, new, distw):
    hist = None; out = []
    for f in range(FRAMES):
        rt, td = rays(f, stochastic)
        mu, sg = resolve(rt, td, distw)
        hist = temporal(hist, mu, mu, sg, new)
        out.append(hist.copy())
    seq = np.array(out[FRAMES // 2:])
    return seq

# reference: the glossy reflection (lobe-convolved emitter) per pixel, many samples
def reference():
    # the expected lobe sample at every lattice pixel (many samples), through the same spatial filter:
    # the glossy image the estimator should converge to
    n = 512
    acc = np.zeros((H, W))
    for i in range(n):
        d = base + 2.0 * tilt + rng.normal(0, LOBE, (H, W, 2)) + rng.normal(0.0, 2 * JIT, (H, W, 2))
        acc += hit_radiance(d)
    e = acc / n
    lat = ((xs % L) == 0) & ((ys % L) == 0)
    mu, _ = resolve(np.where(lat, e, np.nan))
    return mu

t = min(max((ROUGH - 0.25) / 0.35, 0.0), 1.0)
SHIP_N = 8.0 + (20.0 - 8.0) * t * t * (3 - 2 * t)   # rtrTemporalAlphaStoch
ref = reference()
streak = ref > ENV + 0.2 * (ref.max() - ENV)
print(f"ref max {ref.max():.2f} mean {ref.mean():.3f}")
print(f"fixture: {W}x{H} px, lattice {L}, rough {ROUGH}, emitter {EM} r {RE} rad, {FRAMES} frames; streak px {streak.sum()}")
for name, st, nw, dw in [("BEFORE: mirror rays, hit-dist weight, old temporal", False, False, True),
                         ("lobe rays, hit-dist weight, old temporal", True, False, True),
                         ("lobe rays, hit-dist weight, shipped temporal", True, (SHIP_N, 8), True),
                         ("AFTER: lobe rays, no hit-dist weight, shipped temporal", True, (SHIP_N, 8), False)]:
    seq = run(st, nw, dw)
    sd = seq.std(0); mean = seq.mean(0)
    rel = sd[streak] / np.maximum(ref[streak], 1e-3)
    bias = (mean[streak].mean() - ref[streak].mean()) / ref[streak].mean()
    # blobs: frame-to-frame change of the image (mean abs diff over the floor), relative to the streak level
    dfr = np.abs(np.diff(seq, axis=0)).mean() / ref[streak].mean()
    print(f"  {name:30s} temporal sd/ref (streak) mean {rel.mean():.3f} p99 {np.percentile(rel, 99):.3f} | "
          f"frame-to-frame |d| {dfr:.4f} | bias {bias:+.3f} | max px {seq.max():.1f}")
