// vkclean -- A RAY-TRACED RECEIVER'S LOCAL LIGHTS (rtHitLocalLights; the probe-capture specialization of
// clean_draw_common.glsl localLights + the main-view hooks listed in clean_rt_shade.glsl: the light list, the
// hit shadow-ray budget and its sampled estimator, clean_hit_nee.h). Included by clean_rt_shade.glsl (the RT
// reflection / mirror / GI hits and the ray-traced probe capture) and by the forward alpha-blend layers'
// own light (clean_draw.frag CLEAN_BLEND_HIT, clean_draw_common.glsl blendHitLocal), which are shaded the way a
// hit is: one walk, one estimator.
#ifndef CLEAN_RT_HIT_LIGHTS_GLSL
#define CLEAN_RT_HIT_LIGHTS_GLSL
// FORWARD HOOKS (the blend layers only; every hit kernel leaves them undefined, so its walk is the text it was):
//   RT_SHADE_LIGHT_MASK(P, words) -> bool: fills words[RT_SHADE_LIGHT_MASK_WORDS] with a 256-bit light mask holding
//                                    every light that can reach P (the forward froxel cell, lossless) and returns
//                                    true, or false = walk RT_SHADE_LIGHT_CLUSTER(P) as without it. (Not with
//                                    RT_SHADE_REGIR.)
//   RT_SHADE_EXACT_M              -> (with the sampled budget) the M strongest shadowable shares take a ray each and
//                                    enter the sum exactly; only the rest are candidates of the K picks
//                                    (clean_hit_nee.h EXACT SLOTS). Rays: M + K.
//   RT_SHADE_GLARE                -> a float lvalue: + LL materialF's alpha-blend glare of each light's legacy
//                                    specular, unshadowed (as the forward walk's point lights always were), and
//                                    that specular clamped to [0, 1] as the forward alpha path does
#ifndef RT_SHADE_STAT
#define RT_SHADE_STAT(slot, n)
#endif
#ifndef RT_SHADE_LIGHT_NOTE
#define RT_SHADE_LIGHT_NOTE(li)
#endif
#if defined(RT_SHADE_PROJ_K) && defined(RT_SHADE_PROJ_RAY)
#define RT_SHADE_PROJ_BUDGETED 1
#ifndef RT_SHADE_PROJ_MAXK
#define RT_SHADE_PROJ_MAXK 4
#endif
#else
#define RT_SHADE_PROJ_BUDGETED 0
#endif
// RT_SHADE_STOCH_SEED (uint, the frame's seed; with the budget) -> the budget SAMPLES its K rays among every
// shadowable share in proportion to its light (clean_hit_nee.h: next-event estimation) instead of tracing
// the K brightest and leaving the rest UNSHADOWED -- no light reaches the hit without a shadow test. With
// RT_SHADE_POINT_BUDGET on, the shadowable shares are every point light, every ray-traced projector record
// and every projector WITHOUT a record (pass 15's extended set; RT_SHADE_PROJ_RAY traces it from its apex).
// The GI trace defines it (the sealed-room leak); the reflections keep the brightest-K rule.
#if RT_SHADE_PROJ_BUDGETED && defined(RT_SHADE_STOCH_SEED)
#define RT_SHADE_STOCH 1
#include "clean_hit_nee.h"
#if RT_SHADE_PROJ_MAXK != HN_MAXK
#error "clean_hit_nee.h's slot count must be the budget's"
#endif
#else
#define RT_SHADE_STOCH 0
#endif

// Projector shadow-map visibility at filter quality 0 (clean_draw_common.glsl
// projShadowMap, the capture path) + the atlas translucency layer.
vec3 rtProjShadowMapQ0(Light L, uint idx, vec3 P, vec3 N)
{
    ProjShadow s = pshadow[idx];
    if (s.params.w < 0.5) return vec3(1.0);
    vec3  tl    = L.proj_origin.xyz - P;
    float d     = max(dot(-tl, L.proj_axis.xyz), 0.0);
    vec3  l     = normalize(tl);
    float texel = s.params.x * d;
    float k     = 1.0 - clamp(dot(N, l), 0.0, 1.0);
    vec3  Po    = P + N * (texel * s.texel.z * (0.5 + k)) + l * (texel * s.texel.w);
    vec4  c     = s.vp * vec4(Po - ssr.cam_origin.xyz, 1.0);   // camera-relative (#555)
    if (c.w <= 0.0) return vec3(1.0);
    vec3  ndc   = c.xyz / c.w;
    if (ndc.z >= 1.0 || ndc.z <= 0.0) return vec3(1.0);
    vec2  uv    = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(1.0);
    vec2  t     = s.texel.xy;
    vec2  lo    = s.rect.xy + t * 0.5, hi = s.rect.xy + s.rect.zw - t * 0.5;
    vec2  a     = s.rect.xy + uv * s.rect.zw;
    const float vis = textureLod(uProjShadowAtlas, vec3(clamp(a, lo, hi), ndc.z), 0.0);
    if (vis > 0.001 && ssr.render_opts.w > 0.5)
    {
        const vec2 t2 = t * 2.0;
        const vec2 tc = clamp(a, s.rect.xy + t2, s.rect.xy + s.rect.zw - t2);
        const float front = textureLod(uProjTransDepth, vec3(tc, ndc.z), 0.0);
        if (front < 0.999) return vis * mix(textureLod(uProjTransColor, tc, 0.0).rgb, vec3(1.0), front);
    }
    return vec3(vis);
}

// clean_draw_common.glsl localLights, probe-capture specialization (list = the capture's
// light list: 0 scheduled job, 1 realtime probe).
// (#1039 M2) RT_SHADE_BUDGET_OUT(projK, kScore, kLight, kShare): defined, the walk ends with every share
// UNSHADOWED -- returns that sum, hands the hook the budget's picks (the K brightest shadowable shares,
// descending, score 0 = an empty slot: the valid picks are a prefix; not called when the frame has no light)
// and traces no ray; the visibility queue's producer queues the picks' rays (clean_deferred_light.comp
// vqHitRequests).
vec3 rtHitLocalLights(vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, float metal, float rough, LegacySpec ls, uint list)
{
    const uint count = uint(linfo.cam_pos.w);
    if (count == 0u) return vec3(0.0);
#ifdef RT_SHADE_REGIR
    // (#869) the light grid's picks (weighted) instead of the walk, where the hook takes the hit
    uint  rgL[REGIR_PICKS_MAX];
    float rgW[REGIR_PICKS_MAX];
    const uint rgN = RT_SHADE_REGIR(P, N, rgL, rgW);
    const bool useGrid = rgN != REGIR_NO_CELL;
#endif
#ifdef RT_SHADE_LIGHT_CLUSTER
    uint c = 0u;
    bool walkAll = false;
    uint n = 0u;
#ifdef RT_SHADE_REGIR
    if (useGrid) n = rgN;
    else
#endif
    {
        c = RT_SHADE_LIGHT_CLUSTER(P);
        walkAll = c == RT_SHADE_ALL_LIGHTS;
        n = walkAll ? min(count, 256u) : clusterLen(cluster_count[c]);
    }
#else
    const uint c = CLUSTER_COUNT + list;
    uint n = clusterLen(cluster_count[c]);
#ifdef RT_SHADE_REGIR
    if (useGrid) n = rgN;
#endif
#endif
#ifdef RT_SHADE_LIGHT_MASK
#ifdef RT_SHADE_REGIR
#error "RT_SHADE_LIGHT_MASK walks a light mask instead of the cluster list: not with the light grid's picks"
#endif
    // (the forward walk) the mask's lights in index order: n = their count, li = the next set bit
    uint lmW[RT_SHADE_LIGHT_MASK_WORDS];
    const bool lmOn = RT_SHADE_LIGHT_MASK(P, lmW);
    uint lmI = 0u;
    if (lmOn)
    {
        n = 0u;
        for (uint w = 0u; w < RT_SHADE_LIGHT_MASK_WORDS; ++w) n += uint(bitCount(lmW[w]));
    }
#endif
#ifdef RT_SHADE_BUDGET_OUT
    // (#1039 register pass, the visibility queue's PRODUCER only -- the walk that traces no ray) one set of
    // surface terms live across the walk, per model: PBR the BRDF's diffuse / specular colours and roughness,
    // legacy its base colour, specular colour and glossiness, selected once (the same values each branch read
    // before). The kernels that trace the budget's rays inline keep the form below unchanged: the selected
    // form hung the fused RT LIGHT (DeferRtSplitLight) on NVIDIA 615.78 (see the commit message).
    const vec3  diffuseColor  = pbr ? base * (1.0 - 0.04) * (1.0 - metal) : base;   // | base (legacy)
    const vec3  specularColor = pbr ? mix(vec3(0.04), base, metal) : ls.spec;      // | ls.spec (legacy)
    const float rghGloss      = pbr ? rough : ls.gloss;                             // rough | ls.gloss
#define RTL_ROUGH rghGloss
#define RTL_BASE  diffuseColor
#define RTL_LSPEC specularColor
#define RTL_GLOSS rghGloss
#else
    const vec3 diffuseColor  = base * (1.0 - 0.04) * (1.0 - metal);
    const vec3 specularColor = mix(vec3(0.04), base, metal);
#define RTL_ROUGH rough
#define RTL_BASE  base
#define RTL_LSPEC ls.spec
#define RTL_GLOSS ls.gloss
#endif
    vec3 sum = vec3(0.0);
#if RT_SHADE_PROJ_BUDGETED
    // HIT SHADOW-RAY BUDGET (see RT_SHADE_PROJ_K): the K brightest shadowable projector shares,
    // descending (score 0 = empty slot).
    const uint projK = min(uint(RT_SHADE_PROJ_K), uint(RT_SHADE_PROJ_MAXK));
    float kScore[RT_SHADE_PROJ_MAXK];
    uint  kLight[RT_SHADE_PROJ_MAXK];
    vec3  kShare[RT_SHADE_PROJ_MAXK];
    [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j) { kScore[j] = 0.0; kLight[j] = 0u; kShare[j] = vec3(0.0); }
#endif
#ifdef RT_SHADE_EXACT_M
#if !RT_SHADE_STOCH
#error "RT_SHADE_EXACT_M splits the sampled budget (RT_SHADE_STOCH_SEED + RT_SHADE_PROJ_K + RT_SHADE_PROJ_RAY)"
#endif
    // (clean_hit_nee.h EXACT SLOTS) the M strongest shares, each with its own ray
    const uint exM = min(uint(RT_SHADE_EXACT_M), uint(HN_MAXM));
    float exScore[HN_MAXM];
    uint  exLight[HN_MAXM];
    vec3  exShare[HN_MAXM];
    uint  exId[HN_MAXM];
    hnExactInit(exScore, exLight, exShare, exId);
    // a candidate share: offered to the exact slots, and what leaves them goes to the K reservoirs
#define RT_HN_CANDIDATE(cid0, cli0, csh0) { uint cid = (cid0); uint cli = (cli0); vec3 csh = (csh0); \
        if (hnExactOffer(exM, exScore, exLight, exShare, exId, cid, cli, csh)) \
            hnW = hnAdd(hnW, projK, hnSeed, cid, cli, csh, kScore, kLight, kShare); }
#endif
#if RT_SHADE_STOCH
    // (clean_hit_nee.h) the reservoirs' weight sum and the hit's seed: the frame's, hashed with the hit point
    // (the producer and the fused path shade the same record: the same picks)
    float hnW = 0.0;
    const uint hnSeed = hnHash(floatBitsToUint(P.x) ^ hnHash(floatBitsToUint(P.y) ^ hnHash(floatBitsToUint(P.z)
                               ^ uint(RT_SHADE_STOCH_SEED))));
#endif
    for (uint i = 0u; i < n; ++i)
    {
        uint li;
        float lw = 1.0;   // the light's estimator weight (1: exact; the grid's RIS weight)
#ifdef RT_SHADE_REGIR
        if (useGrid) { li = rgL[i]; lw = rgW[i]; }
        else
#endif
#ifdef RT_SHADE_LIGHT_MASK
        if (lmOn)
        {
            while (lmW[lmI] == 0u) ++lmI;   // (n counts the set bits: one is left while i < n)
            li = lmI * 32u + uint(findLSB(lmW[lmI]));
            lmW[lmI] &= lmW[lmI] - 1u;
        }
        else
#endif
        {
#ifdef RT_SHADE_LIGHT_CLUSTER
            li = walkAll ? i : cluster_index[c * CLUSTER_CAP + i];
#else
            li = cluster_index[c * CLUSTER_CAP + i];
#endif
        }
        if (li >= count) continue;   // (never: a list entry past this frame's lights)
        // THE LIGHT REACH (#1043, clean_light_eval.glsl): range, attenuation and the cutoff fade -- the
        // main view's lights end where the reflected / bounced ones do
        vec3 lv;
        float d2;
        if (!lightInRange(lights[li].pos_radius, P, lv, d2)) continue;
        Light L = lights[li];
        float dist;
        const float atten = lightReachAt(L.pos_radius, L.color_falloff, d2, dist);
        if (atten <= 0.0) continue;
        RT_SHADE_STAT(0, 1u);
        RT_SHADE_LIGHT_NOTE(li);
        if (L.proj_origin.w < 0.5)
        {   // POINT (multiPointLightF)
            const vec3 l = lv / max(dist, 1e-4);
            vec3 pt = vec3(0.0);
            if (pbr)
            {
                float nl; vec3 df, sp;
                llPunctual(diffuseColor, specularColor, RTL_ROUGH, N, V, l, nl, df, sp);
                pt = atten * L.color_falloff.rgb * 3.25 * clamp(nl * (df + sp), vec3(0.0), vec3(10.0));
            }
            else
            {
                const float nl = dot(N, l);
                if (nl > 0.0)
                {
                    pt = L.color_falloff.rgb * (nl * atten) * RTL_BASE;
                    if (RTL_GLOSS > 0.0)   // legacy Blinn-Phong (#470, multiPointLightF)
                    {
                        float slit;
                        const float scol = legacySpecular(N, V, l, RTL_GLOSS, slit);
#ifdef RT_SHADE_GLARE
                        const vec3 gsp = clamp((slit * atten * scol) * L.color_falloff.rgb * RTL_LSPEC, vec3(0.0), vec3(1.0));
                        RT_SHADE_GLARE = max(RT_SHADE_GLARE, gsp.r) + max(max(gsp.r, gsp.g), gsp.b);   // (LL materialF)
                        pt += gsp;
#else
                        pt += (slit * atten * scol) * L.color_falloff.rgb * RTL_LSPEC;
#endif
                    }
                }
            }
#if RT_SHADE_STOCH && defined(RT_SHADE_POINT_BUDGET)
            // (clean_hit_nee.h) a sampling candidate: its light reaches the sum only through a traced pick
            if (RT_SHADE_POINT_BUDGET && projK > 0u)
            {
#ifdef RT_SHADE_EXACT_M
                RT_HN_CANDIDATE(i, li, lw * pt)
                continue;
#endif
                hnW = hnAdd(hnW, projK, hnSeed, i, li, lw * pt, kScore, kLight, kShare);
                continue;
            }
#endif
            sum += lw * pt;
#if RT_SHADE_PROJ_BUDGETED && defined(RT_SHADE_POINT_BUDGET) && !RT_SHADE_STOCH
            // (RT_SHADE_POINT_BUDGET, the GI trace) the point light's share competes for the hit
            // shadow-ray budget like a ray-traced projector record (RT_SHADE_PROJ_RAY traces toward
            // its centre, stopping at its fixture sphere); without the hook point lights stay
            // unshadowed (the capture rule)
            if (RT_SHADE_POINT_BUDGET && projK > 0u)
            {
                vec3  psh = lw * pt;
                float sc = dot(psh, vec3(0.2126, 0.7152, 0.0722));
                uint  sl = li;
                [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j)
                {
                    if (uint(j) < projK && sc > kScore[j])
                    {
                        const float ts = kScore[j]; const uint tl = kLight[j]; const vec3 tv = kShare[j];
                        kScore[j] = sc; kLight[j] = sl; kShare[j] = psh;
                        sc = ts; sl = tl; psh = tv;
                    }
                }
            }
#endif
        }
        else
        {   // PROJECTOR (spotLightF)
            vec2 ptc;
            if (!lightProjFrustum(L.proj_mat, P, ptc)) continue;
            const float l_dist = -dot(lv, L.proj_axis.xyz);
            const vec3  l  = normalize(L.proj_origin.xyz - P);
            const float nl_raw = dot(N, l);
            const float nl = clamp(nl_raw, 0.000001, 1.0);
            vec3 dlit = projDiffuse(L, l_dist, ptc.xy);
            // Capture rule (projectorVisibility): shadow-map records sample the atlas,
            // ray-traced records stay unshadowed.
            const bool legacySpec = !pbr && RTL_GLOSS > 0.0;
            const bool legacyEnv  = !pbr && ls.env > 0.0;
            vec3 vis = vec3(1.0);
            bool rayRec = false;   // (budgeted: a ray-traced record whose shadow is decided after the walk)
            // SHADOWED AMBIANCE (RenderVKProjAmbianceShadowedMode; localLight's rule): a shadowed
            // record's ambiance takes its visibility -- the soft max-LOD texel widens the test, and
            // a hit turned away from the apex (the legacy ambiance's only receivers) is shadowed
            // by its own surface under the back-face self-shadow rule (0, no ray).
#if RT_SHADE_STOCH && defined(RT_SHADE_POINT_BUDGET)
            // (clean_hit_nee.h) a projector WITHOUT a shadow record is traced too (pass 15's extended set:
            // the main view shadows it; here it lit every hit it reached unshadowed, ambiance and all)
            const bool extRay = RT_SHADE_POINT_BUDGET && projK > 0u && L.tex.z == 0u;
#else
            const bool extRay = false;
#endif
            const bool ambSh = L.proj_params.w > 0.0 && projAmbShadowed()
                            && ((L.tex.z != 0u && pshadow[L.tex.z - 1u].params.w > 0.5) || extRay);
            const bool ambBack = ambSh && !(nl_raw > 0.0);
            const bool ambSelf = ambBack && rtBackfaceSelfShadow();
#if RT_SHADE_STOCH
            // (clean_hit_nee.h) a traced share costs no ray of its own: no brightness gate (a hundred dim
            // lights under it summed to a visible leak) -- every lit share of a ray-traced light is a candidate
            const bool rayable = extRay || (L.tex.z != 0u && pshadow[L.tex.z - 1u].params.w > 0.5
                                            && pshadow[L.tex.z - 1u].params.z > 1.5);
#else
            const bool rayable = false;
#endif
            if (!ambSelf && (ambBack || (nl_raw > 0.0 && (rayable || ambSh || legacyEnv || max(max(dlit.r, dlit.g), dlit.b) * atten * (legacySpec ? 16.0 : 1.0) > 0.002)))
                && (L.tex.z != 0u || extRay))
            {
                const uint idx = L.tex.z - 1u;
                if (extRay)
                {
#if RT_SHADE_STOCH
                    RT_SHADE_STAT(1, 1u);
                    rayRec = true;
#endif
                }
                else if (pshadow[idx].params.w > 0.5 && pshadow[idx].params.z <= 1.5)
                    vis = rtProjShadowMapQ0(L, idx, P, N);
#ifdef RT_SHADE_PROJ_RAY
                else if (pshadow[idx].params.w > 0.5)
                {
                    RT_SHADE_STAT(1, 1u);
#if RT_SHADE_PROJ_BUDGETED
                    rayRec = true;
#else
                    RT_SHADE_STAT(2, 1u);
                    vis = RT_SHADE_PROJ_RAY(L, P, N);
#endif
                }
#endif
            }
            if (ambSelf) vis = vec3(0.0);   // (its own surface: no ray)
            dlit *= vis;
            // sh = the record's SHADOWABLE share (through dlit / vis); the ambiance takes the
            // shadow only when ambSh (a budgeted ray record: into sh unshadowed, the budget's ray
            // removes its blocked part; else x vis), otherwise straight into sum.
            const vec3 ambV = ambSh ? (rayRec ? vec3(1.0) : vis) : vec3(1.0);
            vec3 sh = vec3(0.0);
            const float amb_da = (nl * 0.5 + 0.5) * L.proj_params.w;
            if (pbr)
            {
                float nlp; vec3 df, sp;
                llPunctual(diffuseColor, specularColor, RTL_ROUGH, N, V, l, nlp, df, sp);
                const vec3 brdf = clamp(nlp * (df + sp), vec3(0.0), vec3(10.0));
                sh += atten * dlit * 3.25 * brdf;
                const vec3 at = projAmbiance(L, amb_da, atten, 0.0, nl, ptc.xy) * 3.25 * brdf * ambV;
                if (ambSh) sh += at; else sum += lw * at;
            }
            else
            {
                const float lit = nl * atten;
                sh += dlit * lit * RTL_BASE;
                const vec3 at = RTL_BASE * projAmbiance(L, amb_da, atten, lit, nl, ptc.xy) * max(dot(-l, N), 0.0) * ambV;
                if (ambSh) sh += at; else sum += lw * at;
                if (legacySpec)   // legacy Blinn-Phong (#470, spotLightF)
                {
                    float slit;
                    const float scol = legacySpecular(N, V, l, RTL_GLOSS, slit);
#ifdef RT_SHADE_GLARE
                    const vec3 gsp = clamp(dlit * (slit * atten * scol) * RTL_LSPEC, vec3(0.0), vec3(1.0));
                    RT_SHADE_GLARE = max(RT_SHADE_GLARE, gsp.r) + max(max(gsp.r, gsp.g), gsp.b);   // (LL materialF)
                    sh += gsp;
#else
                    sh += clamp(dlit * (slit * atten * scol) * RTL_LSPEC, vec3(0.0), vec3(1.0));
#endif
                }
                if (legacyEnv && nl_raw > 0.0)   // projector image reflection (spotLightF)
                    sh += projEnvReflect(L, P, N, V, RTL_GLOSS) * vis * ls.env;
            }
            sh *= lw;   // (the weighted share: what the sum holds and the budget ranks / removes)
#if RT_SHADE_STOCH
            if (rayRec && projK > 0u)
            {
#ifdef RT_SHADE_EXACT_M
                RT_HN_CANDIDATE(i, li, sh)
                continue;
#endif
                hnW = hnAdd(hnW, projK, hnSeed, i, li, sh, kScore, kLight, kShare);
                continue;
            }
#endif
            sum += sh;
#if RT_SHADE_PROJ_BUDGETED && !RT_SHADE_STOCH
            if (rayRec && projK > 0u)
            {   // keep the K brightest shares (descending insertion; the dimmest drops off the end)
                float sc = dot(sh, vec3(0.2126, 0.7152, 0.0722));
                uint  sl = li;
                [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j)
                {
                    if (uint(j) < projK && sc > kScore[j])
                    {
                        const float ts = kScore[j]; const uint tl = kLight[j]; const vec3 tv = kShare[j];
                        kScore[j] = sc; kLight[j] = sl; kShare[j] = sh;
                        sc = ts; sl = tl; sh = tv;
                    }
                }
            }
#endif
        }
    }
#if RT_SHADE_STOCH
    // the picks' estimates (every share scaled to W / (K w)); the K rays below / the queue remove their blocked parts
    sum += hnFinish(hnW, projK, kScore, kShare);
#endif
#ifdef RT_SHADE_EXACT_M
#undef RT_HN_CANDIDATE
    // the exact shares, then ONE ray loop over the exact slots and the picks (one inlined shadow walk)
    sum += hnExactSum(exM, exScore, exShare);
    for (uint j = 0u; j < exM + projK; ++j)
    {
        const bool ex = j < exM;
        const uint q = ex ? j : j - exM;
        if (!((ex ? exScore[q] : kScore[q]) > 0.0)) continue;
        const vec3 vis = RT_SHADE_PROJ_RAY(lights[ex ? exLight[q] : kLight[q]], P, N);
        sum -= (ex ? exShare[q] : kShare[q]) * (vec3(1.0) - vis);
    }
    sum = max(sum, vec3(0.0));
#elif RT_SHADE_PROJ_BUDGETED
#ifdef RT_SHADE_BUDGET_OUT
    RT_SHADE_BUDGET_OUT(projK, kScore, kLight, kShare);   // (#1039 M2: the producer queues the rays)
#else
    // The budget's rays, after the walk (one call site: one inlined shadow walk).
    for (uint j = 0u; j < projK; ++j)
    {
        if (kScore[j] <= 0.0) break;
        RT_SHADE_STAT(2, 1u);
        const vec3 vis = RT_SHADE_PROJ_RAY(lights[kLight[j]], P, N);
        sum -= kShare[j] * (vec3(1.0) - vis);
    }
    sum = max(sum, vec3(0.0));
#endif
#endif
#undef RTL_ROUGH
#undef RTL_BASE
#undef RTL_LSPEC
#undef RTL_GLOSS
    return sum;
}
#endif
