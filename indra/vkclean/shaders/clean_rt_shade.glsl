// vkclean -- RAY-TRACED HIT SHADING: lights an RtSurface (clean_rt_surface.glsl) the way
// clean_draw.frag / clean_water.frag shade a REFLECTION-PROBE CAPTURE fragment
// (probeCapture(): sun_color.w = 1). That specialization of the lit FS:
//   * no probe sampling (sampleProbes returns false in captures): irradiance = the sky
//     ambient (amblit), radiance = skyColorAtOrigin(R,P);
//   * no SSR, planar mirror, AO, debug views, lit-debug switches or far-clip fade;
//   * local lights = the capture's own culled list (cluster CLUSTER_COUNT + list, written
//     by clean_probe_gpu.comp PH_LIGHTS, #478), projector shadows: shadow-map records take the
//     atlas (filter quality 0 = one compare tap, as captures do), ray-traced records are
//     unshadowed (captures never trace projector rays);
//   * haze / water fog from the capture eye (the probe origin).
// The one deliberate difference: the SUN / MOON visibility is ONE hard shadow ray against
// the TLAS (alpha-tested casters, translucent tint per RenderVKTranslucentShadows) at every
// hit facing the light. The raster face takes the cascades (only where they cover the
// MAIN camera's range) or, with the ray-traced sun, the soft multi-ray trace.
//
// Every BRDF term is the lit FS's own (clean_brdf.glsl); keep the branch structure in
// step with clean_draw.frag::shade when that changes.
//
// RT_SHADE_NO_HAZE (#87, the GI trace): no haze / water fog between the receiver and the hit -- the
// haze's in-scatter is sky light, which a GI ray inside a sealed room must not bring in (its
// bounces fed it back through the radiance cache: a lit-looking sealed room).
// MAIN-VIEW HOOKS (#859, the RT reflection pass in clean_deferred_light.comp; undefined in the
// probe capture, which keeps the capture rules above):
//   RT_SHADE_HAZE_K            -> a factor on the haze IN-SCATTER (sky light) between the receiver and
//                                 the hit, read after RT_SHADE_ENV (the reflections under GI: the hit's
//                                 occlusion ray -- a sealed room's air is not sky-lit); default 1
//   RT_SHADE_LIGHT_CLUSTER(P)  -> the light cluster whose list rtHitLocalLights walks (instead
//                                 of the capture's list CLUSTER_COUNT + list); RT_SHADE_ALL_LIGHTS
//                                 = walk every light of the frame (a hit outside the main view's
//                                 cluster grid: each light's own range test decides);
//   RT_SHADE_PROJ_RAY(L, P, N) -> the visibility of a RAY-TRACED projector record at the hit (one
//                                 hard shadow ray; the capture leaves those unshadowed);
//   RT_SHADE_ENV(P, N, R, rough, legacy, amblit, irr, rad, rad2) -> the environment at the hit:
//                                 irradiance, radiance along R at the roughness's lod and at
//                                 lod 0 (instead of amblit / skyColorAtOrigin(R,P)).
//   RT_SHADE_PROJ_K            -> (with RT_SHADE_PROJ_RAY) the HIT SHADOW-RAY BUDGET: at most this
//                                 many ray-traced projector records per hit get RT_SHADE_PROJ_RAY
//                                 (0 .. RT_SHADE_PROJ_MAXK, run-time uniform). Every contributing
//                                 record is shaded unshadowed in the walk; the K whose shadowable
//                                 share is the brightest are kept (insertion) and, after the walk,
//                                 traced once each and their share scaled by the visibility. The
//                                 rest stay unshadowed (the capture rule). Undefined: every RT
//                                 projector record takes its own ray inside the walk (the old
//                                 behaviour -- dozens of shadow rays per hit under a stage rig).
//   RT_SHADE_STAT(slot, n)     -> stats hook (the RT trace's counters): slot 0 lights in range at
//                                 a hit, 1 RT projector records wanting a shadow ray, 2 rays cast.
//   RT_SHADE_REGIR(P, N, pl, pw) -> (#869) the WORLD-SPACE LIGHT GRID (clean_regir.glsl): the lights
//                                 to evaluate at the hit instead of the cluster walk -- returns how
//                                 many (light index, weight) pairs it wrote into pl / pw (<=
//                                 REGIR_PICKS_MAX; the cell's exact list with weight 1, or the RIS
//                                 picks with their unbiased weights), or REGIR_NO_CELL = walk the
//                                 cluster as without the hook. Each picked light is evaluated in
//                                 full (its current record) and its share scaled by the weight;
//                                 the shadow-ray budget ranks the weighted shares.
//   RT_SHADE_LIGHT_NOTE(li)    -> stats hook: light li is evaluated at this hit (in range).
//   RT_SHADE_SUN_VIS(P, tint)  -> (#1039 M1, the visibility queue's RESOLVE stages) the hit's sun / moon
//                                 visibility x cloud shadow (returned) and tint from a stored result instead
//                                 of the inline ray (rtHitSunShadow); the producer queued the ray with
//                                 rtHitSunRay's set-up from the same record.
//   RT_SHADE_LOCAL(P)          -> (#1039 M2) the hit's local lights (rtHitLocalLights' value) from stored
//                                 results: the producer ran rtHitLocalLights on the same record with
//                                 RT_SHADE_BUDGET_OUT (below) and queued the budget's rays -- no light walk,
//                                 no shadow walk here.
//
// Requires before inclusion: clean_lights_common.glsl + the light buffers (set 0
// bindings 3-6: lights, cluster_count, cluster_index, linfo), clean_frame_info.glsl,
// clean_atmos_common.glsl, uTextures, clean_rt_common.glsl, clean_brdf.glsl,
// clean_light_eval.glsl, clean_projshadow.glsl (+ uProjShadowAtlas / uProjTransDepth /
// uProjTransColor at set 0 bindings 18 / 29 / 30), clean_shadow_rt.glsl,
// clean_rt_surface.glsl.
#ifndef CLEAN_RT_SHADE_GLSL
#define CLEAN_RT_SHADE_GLSL
#include "clean_rt_hit_lights.glsl"   // rtProjShadowMapQ0 / rtHitLocalLights (shared with the forward blend layers)

// Sun / moon visibility at a hit: one hard ray toward the light (the RT sun's caster
// rules: every class but water, alpha test, translucent casters dim / tint). The origin
// leaves along the light-facing GEOMETRIC normal of the hit triangle by the float error
// of the hit point (clean_rt_common.glsl offsetRayOrigin -- the textbook Wachter &
// Binder case: P is the barycentric interpolation of the transformed triangle, no
// depth reconstruction) plus the user's extra offset RenderVKRTShadowBias (sun_rt.z,
// default 0). foot = the hit's footprint (m): the casters' alpha-test texture LOD.
// SHADOW TERMINATOR (#664): lift = the hit triangle's Hanika lift (RtSurface.tlift, on
// Ng's side -- the side the ray arrived from; 0 = none). Escape-checked here (one short
// force-opaque ray, only for hits in the sun's band that got a lift), then the origin
// stays on that side, above the facet, as the main view's sunShadowRayCoreImpl.
float rtHitSunShadowCore(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint);
// #719: the volumetric cloud layer's sun transmittance on top (clean_cloud_shadow_map.glsl;
// 1 while the layer is off), as the main view's sunShadowT -- captured probes see cloud shadows.
float rtHitSunShadow(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint)
{
    return rtHitSunShadowCore(P, Ng, L, dist, foot, lift, tint) * cloudShadowMap(P);
}
// (#1039 M1) THE SUN RAY'S SET-UP, apart: what rtHitSunShadowCore traces, so the visibility queue's
// producer can queue the very same ray. False: no ray -- cvis is the visibility (1 shadows off, 0 the
// back-face self-shadow); true: trace rtSunRayVis(Po, L, tmax, ...) (the caller's foliage / blend state).
// The escape check (one short force-opaque ray, only for a lifted hit in the sun's band) runs here.
bool rtHitSunRay(vec3 P, vec3 Ng, vec3 L, vec3 lift, out vec3 Po, out float tmax, out float cvis)
{
    Po = P;
    tmax = 0.0;
    cvis = 1.0;
    if (ssr.shadow_params.w < 0.5) return false;   // sun / moon shadows off
    vec3 n = Ng;
    if (dot(lift, lift) > 0.0)
        lift = rtTerminatorEscape(P, offsetRayOrigin(P, Ng), Ng, lift);
    const bool lifted = dot(lift, lift) > 0.0;
    // BACK-FACE SELF-SHADOW (clean_shadow_rt.glsl rtBackfaceSelfShadow; Ng is the hit's side, the
    // one the arriving ray sees): turned away from the sun, its own surface shadows it -- no ray
    // whose origin would leave through a single-plane wall
    if (!lifted && rtBackfaceSelfShadow()) { if (!(dot(n, L) > 0.0)) { cvis = 0.0; return false; } }
    else if (!lifted && dot(n, L) < 0.0) n = -n;
    Po = offsetRayOrigin((lifted ? P + lift : P) + n * ssr.sun_rt.z, n);
    tmax = ssr.sun_rt.w > 1.0 ? ssr.sun_rt.w : 512.0;
    return true;
}
// The hit's sun ray state (rtSunRayVis' switches) as the visibility queue's request flags.
bool rtHitSunCoarse() { return (rtBlendOpts() & 4u) != 0u; }
float rtHitSunShadowCore(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint)
{
    tint = vec3(1.0);
    vec3 Po;
    float tmax, cvis;
    if (!rtHitSunRay(P, Ng, L, lift, Po, tmax, cvis)) return cvis;
    rt_cone_width0 = foot;
    rt_cone_spread = 0.0;
    // Translucent casters at their texture's mean (RenderVKProbeRTBlendCoarse): no UV /
    // LOD work per candidate; the capture's GGX filter erases texel detail anyway.
    rt_blend_coarse = rtHitSunCoarse();
    // Translucent split (RenderVKRTTranslucentSplit, clean_shadow_rt.glsl): the opaque
    // set first (binary, cut-outs alpha-tested); the translucent transmittance ray only
    // where it is lit -- shadowed hits never walk the blend casters at all.
    // Foliage transmission (#735): the main view's rules, so captured canopies match.
    // (clean_vis_walk.glsl rtSunRayVis, #1048: the one single-ray sun core -- one inlined walk.)
    rtFolBegin();
    const vec3 vis = rtSunRayVis(Po, L, tmax, false, rtSplitOn(), (rtBlendOpts() & 1u) != 0u, specTransOn());
    rtFolEnd();
    rt_blend_coarse = false;
    const float sv = max(max(vis.r, vis.g), vis.b);
    if (sv > 1e-4) tint = vis / sv;
    return sv;
}

// Water normal-map LOD for a world-space UV scale k (uv per metre) at the hit footprint.
float rtWaterLod(int idx, float k)
{
    if (idx < 0 || rt_surf_foot <= 0.0) return 0.0;
    const vec2 tsz = vec2(textureSize(uTextures[nonuniformEXT(uint(idx))], 0));
    const float levels = float(textureQueryLevels(uTextures[nonuniformEXT(uint(idx))]));
    return clamp(log2(max(rt_surf_foot * k * max(tsz.x, tsz.y), 1e-8)), 0.0, max(levels - 1.0, 0.0));
}
vec3 rtWaterNormalTex(int idx, vec2 uv, float lod)
{
    if (idx < 0) return vec3(0.0, 0.0, 1.0);
    return textureLod(uTextures[nonuniformEXT(uint(idx))], uv, lod).xyz * 2.0 - 1.0;
}

// THE WAVES (#1020, clean_sea_state.glsl): the slope of the sea under the water hit and the detail
// normals' strength, set by a caller that evaluated the sea there (the RT forward pass, CLEAN_DEFER_PASS
// 14); the flat surface and LL's layers as they were otherwise (this file never reads the sea itself:
// its other includers bind another set 0).
vec2  rt_water_slope = vec2(0.0);
float rt_water_detail = 1.0;

// The reflection direction of the water surface at P seen from eye (clean_water.frag's wave_ibl
// and R: the three wave layers, the frame's wave scale; #859 RT forward reflections). The
// footprint LOD is rt_surf_foot's (the caller's last rtSurfaceEval).
vec3 rtWaterReflectDir(vec3 P, vec3 eye, float time_s)
{
    const float time = time_s * 0.5;
    const vec3 viewVec = normalize(P - eye);
    vec2 wave1d = ssr.water[0].xy, wave2d = ssr.water[0].zw;
    vec3 v = P;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 bigWave = v.xy * vec2(0.04, 0.04) + wave1d * time * 0.055;
    vec4 littleWave;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + wave2d * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2)  + wave1d * time * 0.1;
    const int ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    const float blend = ssr.water[1].w;
    vec3 wave1 = mix(rtWaterNormalTex(ia, bigWave, rtWaterLod(ia, 0.04)),       rtWaterNormalTex(ib, bigWave, rtWaterLod(ib, 0.04)),       blend);
    vec3 wave2 = mix(rtWaterNormalTex(ia, littleWave.xy, rtWaterLod(ia, 0.9)), rtWaterNormalTex(ib, littleWave.xy, rtWaterLod(ib, 0.9)), blend);
    vec3 wave3 = mix(rtWaterNormalTex(ia, littleWave.zw, rtWaterLod(ia, 0.2)), rtWaterNormalTex(ib, littleWave.zw, rtWaterLod(ib, 0.2)), blend);
    // (#1020: as clean_water.frag -- the detail layers on the waves' slope)
    wave1 = vec3(wave1.xy * rt_water_detail + rt_water_slope * wave1.z, wave1.z);
    wave2 = vec3(wave2.xy * rt_water_detail + rt_water_slope * wave2.z, wave2.z);
    wave3 = vec3(wave3.xy * rt_water_detail + rt_water_slope * wave3.z, wave3.z);
    const vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;
    vec3 wave_ibl = wavef * ssr.water[1].xyz;
    wave_ibl.z *= 2.0;
    wave_ibl = normalize(wave_ibl);
    vec3 R = reflect(viewVec, wave_ibl);
    R.z = abs(R.z);
    return normalize(R);
}

// clean_water.frag, WATER_OPAQUE build (the capture water): waves, Fresnel, sky
// reflection, sun glint, fully fogged body; haze from the eye. time = draw time (s).
// In two halves around the glint's sun ray: rtShadeHit traces the sun ONCE for every surface model
// (one inlined sun walk -- the water's own call site was a second copy of the opaque / translucent /
// foliage walks in every kernel that shades hits).
vec3 rtShadeRawSky(vec3 direction,vec3 origin)
{
#ifdef RT_SHADE_NO_HAZE
    return cmOn() ? physkyRolloff(cmBoundarySky(origin,normalize(direction))) : skyColor(direction);
#else
    return skyColorAtOrigin(direction,origin);
#endif
}

struct RtWaterShade
{
    vec3 color;      // under water: the final colour (done)
    vec3 punctual;   // the sun glint before its visibility
    vec3 radiance;   // the reflection x df2.y
    vec3 fb;
    float mixK;      // min(1, df2.x)
    vec3 additive, atten;
    CameraTransport medium;
    bool done;       // no glint ray, color is final
    vec3 R;          // (#1039 M2) the reflection direction RT_SHADE_ENV is asked along (unset when done)
    float rough;     // (perf-4k60) the rest of the environment's arguments: rtShadeHit asks RT_SHADE_ENV itself
    vec3 amblit;     //   (one call site for water and the lit models), then radiance = its answer x rScale
    float rScale;    //   (df2.y)
};
// env false (#1039 M1: the visibility queue's producer asks only whether the glint wants its sun ray): no
// reflection (radiance stays 0) -- the probes / occlusion ray of RT_SHADE_ENV are skipped, the glint is the same.
RtWaterShade rtShadeWaterBegin(vec3 P, vec3 eye, float time_s, bool env)
{
    RtWaterShade w;
    w.color = vec3(0.0); w.punctual = vec3(0.0); w.radiance = vec3(0.0); w.fb = vec3(0.0); w.mixK = 0.0;
    w.additive = vec3(0.0); w.atten = vec3(1.0); w.done = false; w.R = vec3(0.0, 0.0, 1.0);
    w.rough = 0.0; w.amblit = vec3(0.0); w.rScale = 0.0;
    w.medium=cmIdentity();
#ifndef RT_SHADE_NO_HAZE
    if(cmOn())
    {
        const vec3 segment=P-eye;
        w.medium=cmFinite(eye,segment/max(length(segment),1e-7),0.0,length(segment),0.0);
    }
#endif
    const bool under = eye.z < waterHeight();
    const float time = time_s * 0.5;
    const vec3  up   = vec3(0.0, 0.0, 1.0);
    const vec3  view = P - eye;
    const float dist = length(view);
    const vec3  viewVec = view / max(dist, 1e-4);

    vec2 wave1d = ssr.water[0].xy, wave2d = ssr.water[0].zw;
    vec3 v = P;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 bigWave = v.xy * vec2(0.04, 0.04) + wave1d * time * 0.055;
    vec4 littleWave;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + wave2d * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2)  + wave1d * time * 0.1;
    const int ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    const float blend = ssr.water[1].w;
    const float l1a = rtWaterLod(ia, 0.04), l1b = rtWaterLod(ib, 0.04);
    const float l2a = rtWaterLod(ia, 0.9),  l2b = rtWaterLod(ib, 0.9);
    const float l3a = rtWaterLod(ia, 0.2),  l3b = rtWaterLod(ib, 0.2);
    const vec3 wave1 = mix(rtWaterNormalTex(ia, bigWave, l1a),       rtWaterNormalTex(ib, bigWave, l1b),       blend);
    const vec3 wave2 = mix(rtWaterNormalTex(ia, littleWave.xy, l2a), rtWaterNormalTex(ib, littleWave.xy, l2b), blend);
    const vec3 wave3 = mix(rtWaterNormalTex(ia, littleWave.zw, l3a), rtWaterNormalTex(ib, littleWave.zw, l3b), blend);
    const vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    if (under)
    {
        w.color = max(applyWaterFog(atmosSrgbToLinear(ssr.water[4].rgb), view, eye.z - waterHeight()), vec3(0.0));
        w.done = true;
        return w;
    }

    const float fscale = ssr.water[2].x, foffset = ssr.water[2].y;
    vec3 df3 = max(vec3(0.0), vec3(dot(viewVec, wave1),
                                   dot(viewVec, (wave2 + wave3) * 0.5),
                                   dot(viewVec, wave3)) * fscale + foffset);
    df3 *= df3;
    const vec2 df2 = max(vec2(0.0), vec2(df3.x + df3.y + df3.z, dot(viewVec, wavef) * fscale + foffset));

    vec3 sunlit, amblit, additive, atten;
    atmosLightTerms(view, normalize(wavef), sunlit, amblit, additive, atten);

    const float vdu = clamp(-dot(viewVec, up) * 2.0, 0.0, 1.0);
    vec3 wave_ibl = wavef * ssr.water[1].xyz;
    wave_ibl.z *= 2.0;
    wave_ibl = normalize(wave_ibl);

    const vec3 fb = applyWaterFog(vec3(1.0), viewVec * 2048.0, eye.z - waterHeight());
    const float rough = clamp(ssr.water[2].z, 0.0, 1.0);
    vec3 R = reflect(viewVec, wave_ibl);
    R.z = abs(R.z);
    w.R = R;
    w.rough = rough; w.amblit = amblit; w.rScale = df2.y;
    vec3 radiance = vec3(0.0);
    if (env)
    {
#ifdef RT_SHADE_ENV
        // (#859 main view: the probes around the water hit along R, as the main view's water FS)
        vec3 wirr, wrad2;
        RT_SHADE_ENV(P, up, R, rough, false, amblit, wirr, radiance, wrad2);
#else
        radiance = rtShadeRawSky(R,P);   // captures never sample the probes
#endif
    }

    const vec3 specularColor = atmosSrgbToLinear(ssr.water[3].rgb);
    const vec3 V = -viewVec;
    const vec3 glintN = normalize(wavef + up * max(dist, 32.0) / 32.0 * (1.0 - vdu));
    // (no screen derivatives here: the glint keeps the plain roughness -- the specular-AA
    // widening is a main-view anti-sparkle measure)
    float nl; vec3 diffPunc, specPunc;
    llPunctual(vec3(0.0), specularColor, rough, glintN, V, atmosLightDir(), nl, diffPunc, specPunc);
    w.punctual = clamp(nl * (diffPunc + specPunc), vec3(0.0), vec3(10.0)) * sunlit * atten.r;
    w.radiance = radiance * df2.y;
    w.fb = fb;
    w.mixK = min(1.0, df2.x);
    w.additive = additive;
    w.atten = atten;
    return w;
}
// The glint's sun visibility: one ray (the capture water takes the cascades, or nothing with the
// ray-traced sun) -- traced by rtShadeHit (up, no lift) where the glint is above 1e-4.
bool rtShadeWaterSunWanted(RtWaterShade w)
{
    return !w.done && max(max(w.punctual.r, w.punctual.g), w.punctual.b) > 1e-4;
}
vec3 rtShadeWaterEnd(RtWaterShade w, float sunVis)
{
    if (w.done) return w.color;
    vec3 color = mix(w.fb, w.radiance, w.mixK) + w.punctual * sunVis;
#ifndef RT_SHADE_NO_HAZE
    color = cmOn() ? cmApply(color,w.medium) : atmosApplyRay(color, w.additive, w.atten);
#else
    if(!cmOn()) color=atmosApplyRay(color,w.additive,w.atten);
#endif   // (#816: the light terms' haze, same ray view = P - eye)
    return max(color, vec3(0.0));
}

// Shade a hit seen from `eye` (the probe origin). list = the capture's light list,
// time_s = the draw time (texture animation / water waves). Returns linear HDR colour +
// the surface alpha (alpha-blend layers composite with it).
// The last rtShadeHit's light split (RenderVKRTReflectionsHitTermsDebug): x the ambient / irradiance
// term, y the environment (specular / shiny) term, z the local lights + emission, w the haze in-scatter
// (luminance each; 0 for water / fullbright).
vec4 g_rts_terms = vec4(0.0);
#ifndef RT_SHADE_HAZE_K
#define RT_SHADE_HAZE_K 1.0
#endif
float rtsLum(vec3 c) { return max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0); }
// (#1036) THE LIT SURFACE: the part of an RtSurface rtShadeHit reads (with the global rt_surf_foot --
// the hit's footprint: the sun ray's caster LOD, the water's normal-map LOD, the light grid's
// footprint threshold). rtSurfaceEval's other fields (ids, flags, material, texel, facing) are the
// material stage's alone. The split hit-shading kernels hand it from their material stage to their
// lighting stage as a packed record (clean_deferred_light.comp RT LIT RECORD); the fused paths copy
// it out of the RtSurface (rtLitSurface: field copies, the same values).
struct RtLitSurface {
    vec3  P, Ng, N, Nv;
    vec3  tlift;
    float tw;
    uint  model;
    vec4  base;
    float rough, metal, ao;
    vec3  emissive;
    vec3  spec_lin;
    float gloss, env;
    vec3  fb;
    float fb_env;
};
RtLitSurface rtLitSurface(RtSurface s)
{
    RtLitSurface l;
    l.P = s.P; l.Ng = s.Ng; l.N = s.N; l.Nv = s.Nv;
    l.tlift = s.tlift; l.tw = s.tw;
    l.model = s.model;
    l.base = s.base;
    l.rough = s.rough; l.metal = s.metal; l.ao = s.ao;
    l.emissive = s.emissive;
    l.spec_lin = s.spec_lin; l.gloss = s.gloss; l.env = s.env;
    l.fb = s.fb; l.fb_env = s.fb_env;
    return l;
}
vec4 rtShadeHit(RtLitSurface s, vec3 eye, uint list, float time_s)
{
    g_rts_terms = vec4(0.0);
    const vec3 P = s.P;
    const bool water = s.model == RT_SURF_WATER;
    RtWaterShade ws;
    if (water) ws = rtShadeWaterBegin(P, eye, time_s, false);   // (the environment below: one call site)

    const vec3 V = normalize(eye - P);
    // (perf-4k60 r2) FULLBRIGHT has no early return of its own: it meets the lit models at THE HAZE below (one
    // inlined copy of the medium / haze / water fog -- the fullbright copy fed the RT kernels' register spills).
    // Its haze terms are atmosLightTerms' (= atmosScatterHaze on the same ray; the ambient is not used).
    const bool fbm = !water && s.model == RT_SURF_FULLBRIGHT;
    const vec3 rel = P - eye;
    vec3 sunlit = vec3(0.0), amblit = vec3(0.0), additive = vec3(0.0), atten = vec3(1.0);
    if (!water) atmosLightTerms(rel, s.N, sunlit, amblit, additive, atten);
    vec3 color;
    if (fbm)
    {
        vec3 fb = s.fb;
        if (s.fb_env > 0.0)
        {
            // Fullbright + shiny: the sharp env (sky in captures) at the shininess level.
            const vec3 fR = reflect(-V, s.Nv);
            const vec3 env = rtShadeRawSky(fR,P);
            float fresnel = 1.0 + dot(-V, s.Nv);
            fresnel *= fresnel;
            fresnel = min(fresnel + s.fb_env, 1.0);
            fb = mix(fb, env * (s.fb_env * fresnel) * 0.5, s.fb_env);
        }
        color = fb;   // (clean_draw.frag main(): fullbright takes the haze / water fog afterwards -- THE HAZE)
    }
    else
    {
        const vec3 N = s.N;
        const bool classic = atmosClassic();
        const vec3 L = atmosLightDir();
        const float dist = length(rel);
        const float foot = rt_surf_foot;
        // (perf-4k60) the ray-cone state the sun ray overwrites (rt_cone_width0 / _spread): the WATER hit's environment
        // ran before its sun ray (its occlusion ray's alpha test saw the reflection ray's cone), the lit models' after it
        const float coneW0 = rt_cone_width0, coneSp = rt_cone_spread;
        // Sun / moon visibility, ONCE for every branch below -- the water's glint too (up, no lift):
        // each call site inlines the whole sun walk (opaque / translucent / foliage rays).
        vec3 sunTint = vec3(1.0);
        float sunVis = 1.0;
        if (water ? rtShadeWaterSunWanted(ws) : dot(N, L) > 0.0)
        {
#ifdef RT_SHADE_SUN_VIS
            sunVis = RT_SHADE_SUN_VIS(P, sunTint);   // (#1039 M1: the queued ray's result)
#else
            sunVis = rtHitSunShadow(P, water ? vec3(0.0, 0.0, 1.0) : s.Ng, L, dist, foot, water ? vec3(0.0) : s.tlift, sunTint);
#endif
#ifndef RT_SHADE_NO_HAZE
            // (the reflections' grazing fade: a terminator-band hit counts as lit -- a GI hit never: in a
            //  sealed room that fade let the sun in through every smooth wall it grazed)
            if (!water)
                sunVis = mix(1.0, sunVis, max(rtTerminatorFadeT(dot(s.Nv, L), s.tw), smoothstep(0.1, 0.2, dot(s.Ng, L))));
#endif
        }
        // (perf-4k60) the lit models' atmosphere terms and THE ENVIRONMENT (RT_SHADE_ENV: the probe walk + the sky
        // fallback, or the occlusion-aware GI variant) once for water and both models -- each with its own arguments,
        // (water: up, its wave reflection direction / roughness / ambient). One inlined copy instead of one per branch:
        // they fed the RT kernels' register spills.
        const bool pbr = s.model == RT_SURF_PBR;
        if (!water && classic) sunlit *= 1.35;   // (the terms: above, for fullbright's haze too)
        const vec3 R = reflect(-V, N);
#ifdef RT_SHADE_ENV
        vec3 envIrr = vec3(0.0), envRad = vec3(0.0), envRad2 = vec3(0.0);
        // (after the sun ray, as the lit models had it; the water's call sees the cone from before it, as it did)
        const float sunW0 = rt_cone_width0, sunSp = rt_cone_spread;
        if (water) { rt_cone_width0 = coneW0; rt_cone_spread = coneSp; }
        if (!(water && ws.done))
            RT_SHADE_ENV(P, water ? vec3(0.0, 0.0, 1.0) : N, water ? ws.R : R, water ? ws.rough : (pbr ? s.rough : 1.0 - s.gloss),
                         !water && !pbr, water ? ws.amblit : amblit, envIrr, envRad, envRad2);
        if (water) { ws.radiance = envRad * ws.rScale; rt_cone_width0 = sunW0; rt_cone_spread = sunSp; }
#else
        if (water && !ws.done) ws.radiance = rtShadeRawSky(ws.R, P) * ws.rScale;   // captures never sample the probes
#endif
        if (water) return vec4(rtShadeWaterEnd(ws, sunVis), 1.0);

        // The model's own terms below; the LOCAL LIGHTS and the haze after them, once for both models (one
        // inlined light walk: the grid / cluster loop, its BRDFs and the hit shadow-ray budget's rays).
        LegacySpec ls = legacySpecNone();
        if (pbr)
        {
#ifdef RT_SHADE_ENV
            const vec3 irradiance = envIrr, radiance = envRad;
#else
            const vec3 irradiance = amblit;
            const vec3 radiance = rtShadeRawSky(R,P);
#endif
            const vec3 diffuseColor  = s.base.rgb * (vec3(1.0) - vec3(0.04)) * (1.0 - s.metal);
            const vec3 specularColor = mix(vec3(0.04), s.base.rgb, s.metal);
            const float NdotV = clamp(abs(dot(N, V)), 0.001, 1.0);
            const vec3 iblDiff = irradiance * diffuseColor * s.ao;
            const vec3 iblSpec = radiance * EnvBRDFApprox(specularColor, s.rough, NdotV) * s.ao;
            float nl; vec3 diffPunc, specPunc;
            llPunctual(diffuseColor, specularColor, s.rough, N, V, L, nl, diffPunc, specPunc);
            vec3 stint = vec3(1.0);
            float scol = 1.0;
            if (dot(N, L) > 0.0) { scol = sunVis; stint = sunTint; }
            sunlit *= classic ? atmosLinearToSrgb(stint) : stint;
            if (classic)
            {
                const vec3 irr = atmosSrgbToLinear(irradiance * 0.9);
                const float da = pow(nl, 1.2);
                vec3 sun_contrib = vec3(min(da, scol));
                sun_contrib = atmosSrgbToLinear(atmosLinearToSrgb(sun_contrib) * sunlit * 0.7) * PI;
                const vec3 finalAmbient = irr * diffuseColor;
                const vec3 finalSun     = clamp(sun_contrib * ((diffPunc + specPunc) * scol), vec3(0), vec3(10));
                color = atmosSrgbToLinear(atmosLinearToSrgb(finalAmbient) + atmosLinearToSrgb(finalSun) * 1.1);
            }
            else
            {
                color = iblDiff + clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit * 3.0 * scol;
            }
            color += iblSpec;
            color += s.emissive;
            g_rts_terms.xy = vec2(rtsLum(classic ? atmosSrgbToLinear(irradiance * 0.9) * diffuseColor : iblDiff),
                                  rtsLum(iblSpec));
        }
        else
        {
            // ---- LEGACY ----
#ifdef RT_SHADE_ENV
            const vec3 irradiance = envIrr;
            const vec3 glossenv  = s.gloss > 0.0 ? clamp(envRad, vec3(0.0), vec3(10.0)) : vec3(0.0);
            const vec3 legacyenv = s.env > 0.0 ? envRad2 : vec3(0.0);
#else
            const vec3 irradiance = amblit;
            const vec3 glossenv  = s.gloss > 0.0 ? clamp(rtShadeRawSky(R,P), vec3(0.0), vec3(10.0)) : vec3(0.0);
            const vec3 legacyenv = s.env > 0.0 ? rtShadeRawSky(R,P) : vec3(0.0);
#endif
            float da = clamp(dot(N, L), 0.0, 1.0);
            vec3 stint = vec3(1.0);
            float scol = 1.0;
            if (dot(N, L) > 0.0) { scol = sunVis; stint = sunTint; }
            sunlit *= classic ? atmosLinearToSrgb(stint) : stint;
            color = irradiance;
            if (classic)
            {
                da = pow(da, 1.2);
                const vec3 sun_contrib = vec3(min(da, scol));
                color = atmosSrgbToLinear(color * 0.9 + atmosLinearToSrgb(sun_contrib) * sunlit * 0.7);
                sunlit = atmosSrgbToLinear(sunlit);
            }
            else
            {
                color += min(da, scol) * sunlit;
            }
            color *= s.base.rgb;
            if (s.gloss > 0.0)
            {
                {   // sun specular: clean_brdf.glsl legacySpecular (shared with the raster FS)
                    float slit;
                    const float ss = legacySpecular(N, V, L, s.gloss, slit);
                    color += (slit * scol * ss) * sunlit * s.spec_lin;
                }
                vec3 ge = glossenv * 0.5;
                float fresnel = clamp(1.0 + dot(-V, N), 0.3, 1.0);
                fresnel *= fresnel;
                fresnel *= s.gloss;
                ge *= s.spec_lin * fresnel;
                ge *= max(vec3(1.0) - color, vec3(0.0));   // HDR / emissive past 1.0 must not subtract the reflection
                color += ge * 0.5;
            }
            if (s.env > 0.0)
            {
                float fresnel = 1.0 + dot(-V, N);
                fresnel *= fresnel;
                fresnel = min(fresnel + s.env, 1.0);
                const vec3 reflected = legacyenv * (s.env * fresnel);
                color = mix(color, reflected * 0.5, s.env);
            }
            ls.spec = s.spec_lin; ls.gloss = s.gloss; ls.env = s.env;
            g_rts_terms.xy = vec2(rtsLum((classic ? atmosSrgbToLinear(irradiance * 0.9) : irradiance) * s.base.rgb),
                                  rtsLum(glossenv * 0.25 * s.spec_lin + (s.env > 0.0 ? legacyenv * 0.5 * s.env : vec3(0.0))));
        }
#ifdef RT_SHADE_LOCAL
        const vec3 local = RT_SHADE_LOCAL(P);   // (#1039 M2: the producer's walk + the queued rays' blocked parts)
#else
        const vec3 local = rtHitLocalLights(P, N, V, pbr, s.base.rgb, pbr ? s.metal : 0.0, pbr ? s.rough : 1.0, ls, list);
#endif
        g_rts_terms.z = rtsLum(pbr ? local + s.emissive : local);
        color = atmosClampHDR(color * (classic ? 1.1 : 1.0)) + local * (classic ? 0.9 : 1.0);
    }
#ifndef RT_SHADE_NO_HAZE
    // THE HAZE, one copy for both lit models and fullbright: color x mul + add (the medium's transport, or
    // the haze), then the water fog. Lit: cmWaterPrefix(cmApply(color, medium x K)) / atmosOrWaterFog(color,
    // additive x K); fullbright (K = 1): waterFogged ? applyWaterFog : atmosApply -- the same values.
    {
        const float l0 = rtsLum(color);
        const float hk = fbm ? 1.0 : RT_SHADE_HAZE_K;
        const bool wf = waterFogged(P);
        const bool cm = cmOn() && (!fbm || (!wf && atmosEnabled()));
        vec3 hmul = vec3(1.0), hadd = vec3(0.0);
        if (cm)
        {
            const CameraTransport segment = cmFinite(eye, rel / max(length(rel), 1e-7), 0.0, length(rel), 0.0);
            hmul = segment.T; hadd = segment.L * hk;
        }
        else if (!wf && atmosEnabled())
        {
            hmul = vec3(atten.r); hadd = atmosSrgbToLinear(additive * hk * 2.0) * atmosHdrScale();   // (atmosApplyHaze)
        }
        color = color * hmul + hadd;
        // the water fog: the medium's prefix under the eye's water (lit; cmWaterPrefix), else waterFogged's
        vec3 frel = rel;
        bool fog = wf;
        if (cm)
        {
            fog = !fbm && eye.z < waterHeight();
            if (P.z > waterHeight()) frel *= clamp((waterHeight() - eye.z) / max(frel.z, 1e-7), 0.0, 1.0);
        }
        if (fog) color = applyWaterFog(color, frel, eye.z - waterHeight());
        if (!fbm) g_rts_terms.w = max(rtsLum(color) - l0, 0.0);
    }
#endif
    return vec4(color, s.base.a);
}
vec4 rtShadeHit(RtSurface s, vec3 eye, uint list, float time_s)
{
    return rtShadeHit(rtLitSurface(s), eye, list, time_s);
}

#endif
