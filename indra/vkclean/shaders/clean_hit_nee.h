// vkclean -- HIT LIGHT SAMPLING (sealed-room leak): how a ray-traced hit (a GI bounce) shadows its local
// lights with a fixed number of shadow rays. Shared by clean_rt_shade.glsl (rtHitLocalLights) and the
// CPU fixture scripts/check/test_hit_nee.cpp (scripts/check/checks/hit_nee.py), so the logic the check
// runs is the logic the GPU runs.
//
// THE LEAK IT REPLACES: the hit shadow-ray budget traced the K BRIGHTEST shadowable shares at the hit and
// added EVERY OTHER light that reached it UNSHADOWED. With K = 1 (RenderVKGIHitShadowRays' default) and a
// house full of lamps, a GI ray inside a closed room hit a wall lit by the next room's lamp at full
// strength (only the room's own brightest light was tested): light through solid walls, at any distance
// from the wall the lamp's radius reaches, carried on by the radiance cache's multi-bounce.
//
// THE ESTIMATOR (next-event estimation, Pharr / Jakob / Humphreys PBRT 14.2; Talbot et al. 2005 RIS with
// the exact target): every shadowable share s_i at the hit enters K independent weighted reservoirs
// (streaming, with replacement) with weight w_i = lum(s_i) > 0; reservoir k keeps share s_k with
// probability w_k / W, W = sum w_i. Each pick takes ONE shadow ray v_k and contributes
//     s_k x W / (K w_k) x v_k
// -- an unbiased estimate of sum s_i v_i with no unshadowed remainder: a light every ray finds blocked
// adds exactly 0. Every pick's luminance is W / K (no firefly above the total), the cost is K rays per
// hit whatever the light count, and the denoisers integrate the noise.
//
// hnAdd / hnFinish keep the budget's slot layout (score, light, share; a valid pick prefix), and hnFinish
// leaves each slot's share SCALED by W / (K w_k): the consumers that subtract share x (1 - vis) per traced
// slot from the walk's sum (the inline loop, the visibility queue's resolve vqHitLocal) need no change.
#ifndef CLEAN_HIT_NEE_H
#define CLEAN_HIT_NEE_H
#define HN_MAXK 4
#ifdef __cplusplus
// (the fixture's vec3 shim and uint come from the includer)
#define HN_FN inline
#define inout
#else
#define HN_FN
#endif
// PCG hash (O'Neill; the generic one used across the shaders)
HN_FN uint hnHash(uint v)
{
    const uint st = v * 747796405u + 2891336453u;
    const uint w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return (w >> 22u) ^ w;
}
// the uniform [0, 1) of candidate i in reservoir k (seed: the hit and the frame)
HN_FN float hnRand(uint seed, uint i, uint k)
{
    return float(hnHash(seed ^ hnHash(i * 4u + k + 0x5BD1E995u)) >> 8u) * (1.0f / 16777216.0f);
}
HN_FN float hnLum(vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
// Empties the K slots.
HN_FN void hnInit(inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    for (int j = 0; j < HN_MAXK; ++j) { score[j] = 0.0f; light[j] = 0u; share[j] = vec3(0.0f); }
}
// Candidate i (unique per walk step): light li with its SHADOWABLE share sh. Returns the new weight sum W.
// A share without visible light (lum <= HN_MIN_W) is no candidate: it adds nothing, shadowed or not. (The
// floor keeps W / (K w) finite: the uniform is 0 once in 2^24 draws, and 0 x W < w picks ANY w -- a share
// at the cutoff fade, ~1e-38, scaled by W / w overflowed to inf and put a NaN in the hit's light.)
#define HN_MIN_W 1e-20f
HN_FN float hnAdd(float wsum, uint K, uint seed, uint i, uint li, vec3 sh,
                  inout float score[HN_MAXK], inout uint light[HN_MAXK], inout vec3 share[HN_MAXK])
{
    const float w = hnLum(sh);
    if (!(w > HN_MIN_W)) return wsum;
    const float W = wsum + w;
    for (int j = 0; j < HN_MAXK; ++j)
        if (uint(j) < K && hnRand(seed, i, uint(j)) * W < w)
        {
            score[j] = w; light[j] = li; share[j] = sh;
        }
    return W;
}
// After the walk: each pick's share scaled to its estimate (W / (K w_k)), its score = W / K. Returns the
// sum of the scaled shares -- what the walk adds to its total before the K rays remove the blocked parts.
HN_FN vec3 hnFinish(float wsum, uint K, inout float score[HN_MAXK], inout vec3 share[HN_MAXK])
{
    vec3 s = vec3(0.0f);
    for (int j = 0; j < HN_MAXK; ++j)
    {
        if (uint(j) >= K || !(score[j] > 0.0f)) { score[j] = 0.0f; continue; }
        share[j] = share[j] * (wsum / (float(K) * score[j]));
        score[j] = wsum / float(K);
        s = s + share[j];
    }
    return s;
}
// EXACT SLOTS (the forward alpha-blend layers' light, clean_draw.frag CLEAN_BLEND_HIT): a blended layer has no
// pixel history and no denoiser, so the estimator's noise above shows as it is. Most of a receiver's light comes
// from one or two lamps; the M STRONGEST shadowable shares therefore take a shadow ray each and enter the sum
// exactly (no estimator noise for the lights that dominate), and only the REST are candidates of the K picks.
// Unbiased: every share is either exact (its own ray) or a reservoir candidate (W / (K w) above), never both,
// never neither -- hnExactOffer hands back the one share that leaves the exact set at each step (the offered one
// when it is not among the M strongest so far, else the one it displaced) and the caller adds THAT to the
// reservoirs; reservoir sampling does not depend on the order the candidates arrive in. Cost: M + K shadow rays.
#define HN_MAXM 4
#ifdef __cplusplus
#define HN_REF(T) T&
#else
#define HN_REF(T) inout T
#endif
// Empties the M exact slots.
HN_FN void hnExactInit(inout float escore[HN_MAXM], inout uint elight[HN_MAXM], inout vec3 eshare[HN_MAXM],
                       inout uint eid[HN_MAXM])
{
    for (int j = 0; j < HN_MAXM; ++j) { escore[j] = 0.0f; elight[j] = 0u; eshare[j] = vec3(0.0f); eid[j] = 0u; }
}
// Offers candidate (id, li, sh) to the M exact slots (descending by luminance). Returns true with the share that
// leaves the exact set in (id, li, sh) -- the caller hands it to hnAdd -- or false when nothing is left over (an
// empty slot took it, or a share without visible light: no candidate at all, as in hnAdd).
HN_FN bool hnExactOffer(uint M, inout float escore[HN_MAXM], inout uint elight[HN_MAXM], inout vec3 eshare[HN_MAXM],
                        inout uint eid[HN_MAXM], HN_REF(uint) id, HN_REF(uint) li, HN_REF(vec3) sh)
{
    float sc = hnLum(sh);
    if (!(sc > HN_MIN_W)) return false;
    for (int j = 0; j < HN_MAXM; ++j)
        if (uint(j) < M && sc > escore[j])
        {
            const float ts = escore[j]; const uint ti = eid[j]; const uint tl = elight[j]; const vec3 tv = eshare[j];
            escore[j] = sc; eid[j] = id; elight[j] = li; eshare[j] = sh;
            sc = ts; id = ti; li = tl; sh = tv;
        }
    return sc > 0.0f;
}
// After the walk: the sum of the exact shares (each removed again by its own ray where blocked).
HN_FN vec3 hnExactSum(uint M, float escore[HN_MAXM], vec3 eshare[HN_MAXM])
{
    vec3 s = vec3(0.0f);
    for (int j = 0; j < HN_MAXM; ++j)
        if (uint(j) < M && escore[j] > 0.0f) s = s + eshare[j];
    return s;
}
#undef HN_REF
#ifdef __cplusplus
#undef inout
#endif
#endif
