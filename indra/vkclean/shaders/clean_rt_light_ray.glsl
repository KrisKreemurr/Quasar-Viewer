// vkclean -- A LOCAL LIGHT'S SHADOW RAY AT A RAY-TRACED RECEIVER: the light list a hit walks (the main-view
// cluster it projects into, or every light) and the hard shadow ray toward a light's centre / apex
// (rtrProjRaySetup / rtrProjRay). Shared by the RT reflection / GI hits (clean_deferred_light.comp, through
// clean_rt_hit_lights.glsl's RT_SHADE_LIGHT_CLUSTER / RT_SHADE_PROJ_RAY hooks) and the forward alpha-blend
// layers' own light (clean_draw.frag CLEAN_BLEND_HIT), which are shaded the way a hit is.
// Requires: clean_lights_common.glsl + the light buffers, clean_rt_common.glsl, clean_vis_walk.glsl (rtVisTraceK),
// mainClusterProject. DEFER_GI_TRACE (the GI trace kernels) adds the light's own fixture rule.
#ifndef CLEAN_RT_LIGHT_RAY_GLSL
#define CLEAN_RT_LIGHT_RAY_GLSL
// Local lights at a hit: the main-view cluster it projects into while it lies inside the view (the
// cluster lists are culled to their cells), else every light of the frame (each light's range
// test decides: off-screen hits -- behind the camera, beside the view -- no longer take an edge
// cell's lights).
#define RT_SHADE_ALL_LIGHTS 0xFFFFFFFFu
uint rtrLightCluster(vec3 P)
{
    const vec4 v = linfo.view * vec4(P, 1.0);   // -Z forward
    const float z = -v.z;
    if (z <= 1e-3) return RT_SHADE_ALL_LIGHTS;
    const float sx = (v.x / CL_VIEW_Z(linfo, z)) / max(linfo.grid.z, 1e-4);
    const float sy = (v.y / CL_VIEW_Z(linfo, z)) / max(linfo.grid.w, 1e-4);
    if (abs(sx) > 1.0 || abs(sy) > 1.0) return RT_SHADE_ALL_LIGHTS;
    return mainClusterProject(P);
}
// A ray-traced projector record at a hit: one hard shadow ray toward its origin, from the hit
// offset along its light-facing normal, over the OPAQUE casters only (alpha-mask cut-outs still
// alpha-tested): the alpha-blend class -- light-beam cones, glow cards, haze sheets, which in a
// club sit right along every stage light's own path -- never becomes a candidate here (the main
// view keeps their translucent tint; a reflection hit's projector shadow does not pay a texture
// fetch per beam prim).
// (#1039 M2) its set-up apart (the visibility queue's producer queues the very same ray): the origin o, the
// direction l, the length tmax, and whether the light's own fixture rule applies (fix; rt_fix_P = its centre,
// rt_fix_max = gi.prm5.z).
#define RTR_PROJ_MASK (RT_MASK_SHADOW & ~RT_MASK_BLEND)
void rtrProjRaySetup(Light L, vec3 P, vec3 N, out vec3 o, out vec3 l, out float tmax, out bool fix)
{
    // (#87 GI, RT_SHADE_POINT_BUDGET) a POINT light in the budget: toward its centre, stopping at its
    // fixture sphere (proj_axis.x = its prim's bounding radius, pass 15's SH_SPHERE rule: the bulb
    // never shadows its own light)
    const bool point = L.proj_origin.w < 0.5;
    const vec3 lv = (point ? L.pos_radius.xyz : L.proj_origin.xyz) - P;
    const float d = length(lv);
    l = lv / max(d, 1e-4);
    const vec3 n = dot(N, l) < 0.0 ? -N : N;
    o = offsetRayOrigin(P + n * 0.01, n);
    const float stop = point ? clamp(L.proj_axis.x, 0.0, RS_POINT_NEAR_MAX) : 0.05;   // (pass 15's cap)
    tmax = max(d - stop, 0.0);
#if DEFER_GI_TRACE
    // (#1041) ... and its OWN FIXTURE (pass 15's SH_FIXTURE, clean_vis_walk.glsl): a lamp's shade /
    // lantern / sconce around the point -- a small box holding its centre -- neither shadows nor tints
    // its light; the ray is traced again up to where it enters that box. (Projectors: their near
    // plane is that clip already.)
    fix = point && gi.prm5.z > 0.0;
#else
    fix = false;
#endif
}
vec3 rtrProjRay(Light L, vec3 P, vec3 N)
{
    vec3 o, l;
    float tmax;
    bool fix;
    rtrProjRaySetup(L, P, N, o, l, tmax, fix);
#if DEFER_GI_TRACE
    rt_fix_on = fix;
    rt_fix_P = L.pos_radius.xyz;
    rt_fix_max = gi.prm5.z;
#endif
    rtFacingCullSet(ssr.foliage_rt);   // (clean_rt_common.glsl: the sun's facing cull, shadow-map parity)
    const vec3 v = rtVisTraceK(o, l, tmax, RTR_PROJ_MASK, RT_WALK_RGB, 1u << RT_WALK_RGB);
    rtFoliageEnd();
    rt_fix_on = false;
    return v;
}
// THE OCCLUSION RAY of a reflection (the RT reflection hits under GI, clean_deferred_light.comp rtrOccRay / rtrHitEnv;
// the forward blend layers' environment, clean_surface_light.glsl blendHitEnv): from P along R, R bent back onto the
// arrival side of the receiver's plane (ng: its geometric normal turned toward the side the view / reflection ray
// arrived from) where it grazes / crosses it, the origin lifted off that plane on that side; bent: whether R was bent.
// Traced to RTR_OCC_FAR against RT_MASK_WORLD_BLOCK: a first-hit query, and only a true miss is the open sky.
#define RTR_OCC_FAR 4096.0
// (One body, a statement macro: rtrOccRay expands it in place, exactly the statements it always ran -- the RT kernels'
//  SPIR-V is unchanged, which a function call, even inlined, was not.)
#define RT_OCC_RAY_SETUP(P, R, ng, o, Ro, bent) \
    Ro = R; \
    const float rn = dot(Ro, ng); \
    bent = rn < 0.02; \
    if (bent) Ro = normalize(Ro + ng * (0.02 - rn)); \
    o = offsetRayOrigin(P + ng * 0.01, ng);
void rtOccRaySetup(vec3 P, vec3 R, vec3 ng, out vec3 o, out vec3 Ro, out bool bent)
{
    RT_OCC_RAY_SETUP(P, R, ng, o, Ro, bent)
}
#endif
