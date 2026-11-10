// vkclean -- THE SHADOW / VISIBILITY WALK LIBRARY (#1039 M0): the ONE any-hit walk every ray-traced
// visibility consumer calls -- the lit FS's sun / projector cores and the RT shadow mask
// (clean_shadow_rt.glsl), pass 15's generic core (clean_shadow_core.glsl), the RT hit shading
// (clean_rt_shade.glsl), RTAO, god rays, the volumetric fog, the GI reuse and the visibility queue's
// walk kernel (clean_deferred_light.comp VIS WALK). Before it there were two walk bodies that had
// drifted apart (traceShadowWalk here, traceShadowWalkG in the generic core: only the latter had the
// own-fixture rule and the opaque-first pre-ray) plus an inline fixture copy in the fog.
// Included by clean_rt_common.glsl (needs its candidate rules: rtCandidateAlphaCol, rtSelfCandidate,
// rtFol*, rtRaySane, rt_block_t). Kernel specialisations (SH_KIND_*, RT_WALK_OFIRST) are defined by
// the includer BEFORE clean_rt_common.glsl.
//
// One body serves every shadow-ray flavour; `kind` picks the candidate rules at run time (uniform per
// ray). It exists for COMPILE COST: glslc -O inlines every call, and the lit FS reaches these walks
// from many places (sun + projector cores, hard ray / disc / split tails, every lightSurface branch).
// Four separate walk functions, each inlining the whole candidate alpha test (rtCandidateAlphaCol: UV
// fetch, ray cone, texture sample -- ~7k SPIR-V instructions), made the ray-query lit FS ~870k words
// and a ~90 s / 1.7 GB driver compile per pipeline. A call site with a compile-time kind folds the
// other kinds' rules away; one with a run-time kind inlines one body for all of them.
//   RT_WALK_GREY / RT_WALK_RGB: the MIXED walk (traceShadowRay / traceShadowRayRGB).
//   RT_WALK_VIS: the opaque set, binary (traceShadowRayVis).
//   RT_WALK_TRANS / RT_WALK_TRANS_RGB: the translucent set (traceTransmittance).
//   RT_WALK_OPAQUE: every caster solid, except qualified world emitters (traceShadowRayOpaque).
//   RT_WALK_FOL / RT_WALK_FOL_RGB: the #735 foliage transmission ray (traceFoliageTransmit).
//
// Shadow / visibility ray (RT_WALK_GREY): 1.0 = unoccluded over [tmin, tmax], 0.0 = blocked.
// Double-sided (no facing cull). Alpha-mask / blend instances run the candidate
// alpha test; FORCE_OPAQUE instances (the vast majority) never produce candidates.
// Alpha-mask casters are binary (the raster's cutoff). Alpha-blend casters are
// TRANSLUCENT: each passes (1 - alpha) of the light, so a blended prim (light-beam
// cones, glow cards, animated-alpha effects) dims the light in proportion instead of
// switching it fully on/off as its alpha crosses a threshold (that flickered whole
// projectors on animated textures). Matches the shadow-map path's dithered blend.
// Coloured shadow ray (RT_WALK_RGB, #212): each alpha-blend caster passes
// translucentTransmit(colour, alpha) -- stained glass throws coloured light. Order
// doesn't matter (the product commutes); an opaque hit, or transmittance under 1%,
// ends the ray dark. (Grey = the same walk with (1 - alpha) in every channel.)
//
// TRANSLUCENT SPLIT (RenderVKRTTranslucentSplit; clean_shadow_rt.glsl): shadow
// visibility is two independent traversals instead of one mixed walk.
//   * RT_WALK_VIS: the OPAQUE set (callers pass a mask without RT_MASK_BLEND):
//     binary 0 / 1, alpha-mask cut-outs still alpha-tested (OMM where baked), blocker
//     distance recorded. No alpha-blend candidate ever reaches the shader, so a soft
//     shadow's disc rays stay 0 / 1 and the adaptive early-out settles.
//   * RT_WALK_TRANS(_RGB): the TRANSLUCENT set only (RT_MASK_BLEND), ONE ray per
//     receiver: the product of every blend caster's transmittance along it (order
//     doesn't matter), the same per-caster rules as the mixed walk. It never
//     touches rt_block_t: translucent casters do not size the denoiser's penumbra.
//     Honours rt_blend_parity / rt_blend_coarse like the mixed walks. A blend-class
//     instance whose material carries an alpha cutoff (rtCandidateAlphaCol: blend =
//     false) is a binary caster here, as in the mixed walk. Its foliage rule (#735):
//     an away-facing solid leaf passes in mode 1; mode 2 colours the light by the first
//     rt_fol_layers of them (the rest pass: the walk can't stop early -- light-facing
//     blend casters later in traversal order must still block). Their distance sizes
//     the denoiser's penumbra.
// Final visibility = opaque visibility x translucent transmittance.
#ifndef CLEAN_VIS_WALK_GLSL
#define CLEAN_VIS_WALK_GLSL

#extension GL_EXT_control_flow_attributes : require   // (rtVisTrace's [[dont_unroll]])

#define RT_WALK_GREY      0u
#define RT_WALK_RGB       1u
#define RT_WALK_VIS       2u
#define RT_WALK_TRANS     3u
#define RT_WALK_TRANS_RGB 4u
#define RT_WALK_OPAQUE    5u
#define RT_WALK_FOL       6u
#define RT_WALK_FOL_RGB   7u
// KIND SETS: what a call site knows at compile time about its kind -- the bit (1 << kind) of every kind it
// may pass. A caller with a run-time choice (the disc ray's split / mixed walk, traceTransmittance /
// traceFoliageTransmit's grey or coloured flavour) names its set, so the walk still folds every other
// kind's rules away -- a run-time kind alone would keep them all. RT_WALK_KS_ANY: the kind decides at run
// time (a constant kind still folds).
#define RT_WALK_KS_ANY    0xFFu
#define RT_WALK_KS_MIXED  ((1u << RT_WALK_GREY) | (1u << RT_WALK_RGB))
#define RT_WALK_KS_DISC   (RT_WALK_KS_MIXED | (1u << RT_WALK_VIS))   // a disc ray: the split's or the mixed walk
#define RT_WALK_KS_TRANS  ((1u << RT_WALK_TRANS) | (1u << RT_WALK_TRANS_RGB))
#define RT_WALK_KS_FOL    ((1u << RT_WALK_FOL) | (1u << RT_WALK_FOL_RGB))
// kind in `want` given that it is in `ks` (folded when ks decides it)
bool rtWalkIn(uint kind, uint ks, uint want)
{
    return (ks & want) == 0u ? false : ((ks & ~want) == 0u ? true : ((1u << kind) & want) != 0u);
}
bool rtWalkFol(uint kind, uint ks)
{
#ifdef SH_KIND_POSITIONAL
    return false;   // (local lights: no foliage transmission ray)
#else
    return rtWalkIn(kind, ks, RT_WALK_KS_FOL);
#endif
}
bool rtWalkTrans(uint kind, uint ks)  { return rtWalkIn(kind, ks, RT_WALK_KS_TRANS); }
bool rtWalkOpaque(uint kind, uint ks) { return rtWalkIn(kind, ks, 1u << RT_WALK_OPAQUE); }
bool rtWalkVisK(uint kind, uint ks)   { return rtWalkIn(kind, ks, 1u << RT_WALK_VIS); }
bool rtWalkRgb(uint kind) { return kind == RT_WALK_RGB || kind == RT_WALK_TRANS_RGB || kind == RT_WALK_FOL_RGB; }

// ---- (#1031) OPAQUE FIRST (RT_WALK_OFIRST: the includer maps it onto a dispatch-uniform input; pass 15
//      PDN_OPT_OFIRST, RenderVKShadowOpaqueFirst): a blocking walk first traces the opaque geometry alone
//      (hardware only, no candidate loop) and runs the candidate walk -- over the non-opaque geometry
//      only -- just when that ray comes back clear. The visibility is the same. What differs where an
//      opaque caster blocks: rt_block_t / the moving-caster note are that caster's (any-hit, as the
//      walk's own commit) -- translucent layers the walk might have met in front of it first
//      (traversal order) no longer shorten the penumbra estimate or flag a moving blocker. ----
#ifndef RT_WALK_OFIRST
#define RT_WALK_OFIRST false   // (#1031: read where used -- the includer maps it onto its push / UBO)
#endif
// ---- (#1031) KERNEL SPECIALISATION: an includer whose rays are all one light kind defines
//      SH_KIND_DIRECTIONAL (the sun / moon: no own-fixture code) or SH_KIND_POSITIONAL (local lights:
//      no foliage transmission walk) before clean_rt_common.glsl; the walk then compiles only those
//      paths -- the same results, fewer values live across it (clean_shadow_core.glsl compiles the
//      rest of its sample code the same way). ----
#if defined(SH_KIND_DIRECTIONAL) && defined(SH_KIND_POSITIONAL)
#error "one light kind per kernel"
#endif
// The own-fixture rule (below) compiles where the includer asks for it (RT_WALK_FIXTURE: the kernels that
// trace point lights -- pass 15's generic / positional core, the GI trace's budgeted point rays, the fog,
// the visibility queue's walk); everywhere else rt_fix_on could never be set and the rule folds away.
#if defined(SH_KIND_DIRECTIONAL) || !defined(RT_WALK_FIXTURE)
#define SH_FIX_ON false   // (the own-fixture rule is a positional light's)
#else
#define SH_FIX_ON rt_fix_on
#endif

// ---- OWN-FIXTURE RULE (#439 / #869: a POINT light's own lamp; clean_shadow_core.glsl SH_FIXTURE,
//      the RT hits' budgeted point-light rays, the fog's point rays) ----
// SL point lights sit inside their fixtures (a bulb in a shade, a lantern, a sconce): traced
// plainly, the fixture shadows its own light and the lamp goes dark. A caster instance whose own
// box (object space) holds the light's centre and whose world size is at most rt_fix_max is the
// light's FIXTURE: its translucent faces do not tint the ray, and when an opaque / alpha-tested
// face of it ends the ray, the ray is traced again in two parts (rtFixNext): up to where it ENTERS
// that box (rt_fix_t), as before, and from there to the light with every FIXTURE candidate passed
// over (rt_fix_skip) -- so only the lamp's own instances are exempt; a wall, a door or any other
// instance inside the box still shadows. (The first rule clipped the ray at the box entry: whatever
// else lay inside the box never shadowed, and a receiver inside it -- entry 0 -- was lit outright.
// With #1439's own prim at any size, a lamp prim whose box reached through a wall into the next room
// lit that room through the wall and its closed door, diffuse and specular, as if unshadowed.)
// Only a small enclosing box qualifies besides the own prim (a room-sized mesh around the light never
// does). Set per ray by the caller, which traces through rtVisTrace (the re-trace loop) and clears
// rt_fix_on after.
bool  rt_fix_on  = false;
vec3  rt_fix_P   = vec3(0.0);   // the light's centre (absolute world)
float rt_fix_max = 0.0;         // the largest fixture box edge (m; RenderVKReSTIRFixtureSize)
float rt_fix_t   = -1.0;        // out: the committed caster was the fixture -- its box entry t
bool  rt_fix_skip = false;      // (the walk's caller, rtFixNext's stage 2) fixture candidates are passed over:
                                // the walk runs NoOpaque, so the fixture's opaque faces arrive as candidates
// (#1039 occupancy: clean_rt_common.glsl WALK-STATE HOOKS -- the visibility queue's walks read these from the request)
#ifndef RT_FIX_P
#define RT_FIX_P rt_fix_P
#endif
#ifndef RT_FIX_MAX
#define RT_FIX_MAX rt_fix_max
#endif
// rtFixtureBox(id, q, committed, out a, out b): instance id is the light's OWN PRIM (any size), or its box
// (a, b) holds the light's centre and is small enough. (#1039 occupancy) In steps, each transform read from
// the query where it is used (the object-to-world's origin for the own test, the world-to-object for the
// centre, the object-to-world's scale only for a box that holds it) instead of all fetched up front as
// arguments -- 24 floats live at the candidate's peak.
// THE LIGHT'S OWN PRIM: a point light sits at its prim's centre, and every instance (face) of that prim has its
// object origin there (the volume's model matrix is the prim's pose). Such an instance never shadows its light,
// whatever its size: the prim IS the lamp -- a 3 m wood stove that is one prim with its fire's light, whose
// grate, firebox and chimney otherwise blocked every ray of its own light (the size limit only guards the
// OTHER enclosing objects: a room-sized mesh around a light still shadows it). RT_FIX_OWN_EPS: how far apart
// the two may be (m) -- the light's drawable position and the instance's object pose agree to float precision
// on a still prim; a prim moving between the two reads may miss it for a frame (the size rule still applies).
#define RT_FIX_OWN_EPS 0.02
bool rtFixtureOwn(mat4x3 o2w)
{
    const vec3 d = o2w[3] - RT_FIX_P;
    return dot(d, d) <= RT_FIX_OWN_EPS * RT_FIX_OWN_EPS;
}
#define rtFixtureBox(ID, Q, C, A, B) (rtFixtureBoxAB((ID), (A), (B)) \
    && (rtFixtureOwn(rayQueryGetIntersectionObjectToWorldEXT((Q), (C))) \
        || (rtFixtureHolds(rayQueryGetIntersectionWorldToObjectEXT((Q), (C)) * vec4(RT_FIX_P, 1.0), (A), (B)) \
            && rtFixtureSmall((A), (B), rayQueryGetIntersectionObjectToWorldEXT((Q), (C))))))
bool rtFixtureBoxAB(uint id, out vec3 a, out vec3 b)
{
    a = vec3(0.0); b = vec3(0.0);
    if (!rtIdIn(id)) return false;
    a = rt_instances[id].aabb_min.xyz;
    b = rt_instances[id].aabb_max.xyz;
    return true;
}
bool rtFixtureHolds(vec3 lo, vec3 a, vec3 b) { return all(greaterThanEqual(lo, a)) && all(lessThanEqual(lo, b)); }
bool rtFixtureSmall(vec3 a, vec3 b, mat4x3 o2w)
{
    const vec3 e = abs(b - a) * vec3(length(o2w[0]), length(o2w[1]), length(o2w[2]));
    return max(max(e.x, e.y), e.z) <= RT_FIX_MAX;
}
// A walk ended by a committed (opaque / confirmed) caster: blocked. OWN FIXTURE: the committed caster's
// box entry (the slab test in its object space: the ray parameter is the same through the affine map) --
// the caller re-traces up to it. Else the blocker's distance + identity (srad, the moving-caster bit).
// q: the walk's query, standing on the committed intersection (its transforms read only for the fixture).
vec3 rtWalkBlocked(vec3 origin, vec3 dir, bool trans_walk, rayQueryEXT q)
{
#if defined(RT_WALK_FIXTURE) || defined(RT_TRACK_BLOCKER)
    const uint fid = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)),
                                   uint(rayQueryGetIntersectionGeometryIndexEXT(q, true)));
#else
    const uint fid = 0u;   // (neither rule reads the committed caster's identity here: no id lookup)
#endif
    vec3 fa = vec3(0.0), fb = vec3(0.0);   // (#1039 occupancy: initialised -- no value carried across the walks)
    if (SH_FIX_ON && !rt_fix_skip && !trans_walk && rtFixtureBox(fid, q, true, fa, fb))
    {
        const mat4x3 w2o = rayQueryGetIntersectionWorldToObjectEXT(q, true);
        const vec3 oo = w2o * vec4(origin, 1.0);
        const vec3 od = w2o * vec4(dir, 0.0);
        const vec3 inv = vec3(1.0) / mix(od, vec3(1e-20), lessThan(abs(od), vec3(1e-20)));
        const vec3 t0 = (fa - oo) * inv, t1 = (fb - oo) * inv;
        const vec3 tn = min(t0, t1);
        rt_fix_t = max(max(max(tn.x, tn.y), tn.z), 0.0);
        return vec3(0.0);
    }
    if (!trans_walk)
    {
        rt_block_t = min(rt_block_t, rayQueryGetIntersectionTEXT(q, true));
        RT_NOTE_BLOCKER(fid);
    }
    return vec3(0.0);
}
// LAYER ON THE RECEIVER: an alpha-blend caster this close along the ray (metres; the ray leaves the
// receiver's surface) is a layer lying ON the receiver -- a blush, makeup or a tattoo shell a millimetre
// or two over the skin, a decal or grime layer over a wall -- and casts nothing on it: the layer is drawn
// over that surface already. Its shadow there dimmed (and tinted) the surface under every such layer, a
// near-solid core (alpha >= ALPHA_BLEND_SOLID under the parity rule) blacked it out, and a layer that
// takes the surface's own visibility or light (clean_draw_common.glsl CLEAN_VIS_REUSE) shadowed itself.
// 5 mm along the ray keeps a 2 mm shell out up to ~65 degrees off the light (beyond, N.L is small); a
// blend caster further off -- hair, a hat brim, glass -- shadows as before.
#define RT_LAYER_SKIP 0.005
// One alpha-tested candidate of the walk (it passed the self / facing rules: fol = rtFolAway's verdict):
// THE alpha test, then the kind's rule on the small accumulator (trans, n_fol). True: the foliage walk
// ends here (enough layers / no light left), a shadow walk confirms the candidate (a solid caster).
bool rtWalkCand(uint id, uint prim, vec2 bc, float t, vec3 dir, rayQueryEXT q, uint kind, uint ks, uint fol,
                inout vec3 trans, inout uint n_fol)
{
    const bool rgb = rtWalkRgb(kind);
    const bool trans_walk = rtWalkTrans(kind, ks);
    float cutoff = 0.0; bool blend = false; vec3 col = vec3(1.0);   // (rtCandidateAlphaCol sets them; initialised: nothing carried across walks)
    const float a = rtCandidateAlphaCol(id, prim, bc, t, dir, q, cutoff, blend, col);   // (the ONE alpha test)
    if (blend && t < RT_LAYER_SKIP) return false;   // a layer lying on the receiver (RT_LAYER_SKIP above)
#ifndef SH_KIND_POSITIONAL
    if (rtWalkFol(kind, ks))
    {
        if (a < cutoff) return false;   // a hole in the leaf
        trans *= rgb ? rtFolLayer(col) : vec3(rtFolStrength());
        rt_block_t = min(rt_block_t, t);
        RT_NOTE_BLOCKER(id);
        return ++n_fol >= rtFolLayers() || max(max(trans.r, trans.g), trans.b) < 0.01;
    }
#endif
    vec3 fa = vec3(0.0), fb = vec3(0.0);   // (rtFixtureBox's box; initialised: nothing carried across walks)
    if (rtWalkVisK(kind, ks))
        return a >= cutoff && fol == RT_FOL_OCCLUDE;   // #735
    if ((blend ? a >= RT_BLEND_SOLID : a >= cutoff) && fol == RT_FOL_LEAF)
    {
        // #735: a transmissive (away-facing solid) leaf. The mixed walks pass it; the translucent
        // walk colours the light by the first layers in mode 2.
        if (trans_walk && rtFolMode() == 2u && n_fol < rtFolLayers())
        {
            trans *= rgb ? rtFolLayer(col) : vec3(rtFolStrength());
            ++n_fol;
            rt_block_t = min(rt_block_t, t);
            return max(max(trans.r, trans.g), trans.b) < 0.01;
        }
        return false;
    }
    if (blend && SH_FIX_ON && rtFixtureBox(id, q, false, fa, fb))
        return false;   // (the light's own translucent shade: no tint)
    if (blend && rt_blend_parity && a >= ALPHA_BLEND_SOLID) return true;   // LL: solid caster
    if (blend && !(rt_blend_parity && a < ALPHA_BLEND_NONE))
    {
        trans *= rgb ? translucentTransmit(col, a) : vec3(1.0 - clamp(a, 0.0, 1.0));
        if (!trans_walk && a > ALPHA_TRANS_MIN) { rt_block_t = min(rt_block_t, t); RT_NOTE_BLOCKER(id); }
        return max(max(trans.r, trans.g), trans.b) < 0.01;
    }
    if (blend) return false;   // LL: casts nothing
    return a >= cutoff;
}
// THE WALK. Visibility (or the foliage ray's colour) over [tmin, tmax] along dir for walk `kind` over
// TLAS classes `mask`. Per-ray state the caller sets around it (globals; cleared after): rt_blend_parity
// / rt_blend_coarse (blend casters), rtFoliageSet .. rtFoliageEnd (sun / moon facing culls), rt_self_owner
// (no-self-shadow characters), rt_cone_* (the alpha test's LOD), rt_fix_* (own fixture: rtVisTrace).
// Writes rt_block_t (the nearest blocker distance, lowered) and RT_NOTE_BLOCKER. ks: the kinds this call
// site may pass (RT_WALK_KS_*; rtVisWalk: any).
// (#1039 occupancy) probeOut (a constant per call site): the walk does NOT run the self probe itself (see THE
// SELF PROBE OUTSIDE THE TRAVERSAL below) -- it ends at that candidate with rt_walk_probe set and its result
// void, and its caller probes and walks again from where its own results start (clean_shadow_core.glsl
// shadowSampleRays: the whole sample), so the probe's code and queries are not part of the walk's at all.
bool rt_walk_probe = false;
vec3 rtVisWalkFP(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind, uint ks, bool probeOut)
{
    const bool fol_walk = rtWalkFol(kind, ks);
    const bool trans_walk = rtWalkTrans(kind, ks);
    const bool opaque_walk = rtWalkOpaque(kind, ks);
    uint flags;
    if (fol_walk)
        // FOLIAGE TRANSMISSION (#735, mode 2): the LIGHT-facing faces culled (the shadow rays' side),
        // NoOpaque so an opacity micromap's "known opaque" micro-triangles still arrive as candidates
        // (their leaf colour) while its "known transparent" ones stay dropped (traceFoliageTransmit)
        flags = gl_RayFlagsNoOpaqueEXT | (rtFolFlip() ? gl_RayFlagsCullFrontFacingTrianglesEXT
                                                      : gl_RayFlagsCullBackFacingTrianglesEXT);
    else if (opaque_walk)
        // Every caster solid, except qualified world emitters and the receiver's own
        // no-self-shadow character. Ordinary FORCE_OPAQUE instances stay hardware hits;
        // a ray-wide Opaque override would hide the emitter's visibility predicate.
        flags = gl_RayFlagsTerminateOnFirstHitEXT | rtFolShadowFlags();
    else
        flags = gl_RayFlagsTerminateOnFirstHitEXT | (trans_walk ? gl_RayFlagsNoneEXT : rtFolShadowFlags());
    // (OWN FIXTURE, rtFixNext's stage 2) the part of the ray inside the fixture's box: every face a candidate,
    // so the candidate loop can pass the fixture's own instances over and stop on anything else (an opaque
    // face arriving as a candidate is solid: rtCandidateAlphaCol). No opaque-first pre-ray: it would commit
    // the fixture's opaque faces in hardware.
    const bool fixSkip = SH_FIX_ON && rt_fix_skip && !fol_walk;
    if (fixSkip) flags |= gl_RayFlagsNoOpaqueEXT;
    // (Xid 109, rtRaySane: an invalid ray is blocked as G-7; the foliage walk keeps no colour)
    RT_BC_SITE(RT_BC_SITE_VIS | ((kind & 15u) << 8) | (mask & 0xFFu));   // (#1128 hang beacon variants only)
    if (!rtRaySaneT(origin, tmin, dir, tmax)) return fol_walk ? vec3(1.0) : vec3(0.0);   // (#1039: the ray itself untouched)
    // OPAQUE FIRST (RT_WALK_OFIRST): a blocking walk over the full shadow classes (the disc rays: not the
    // translucent split's, the foliage transmission's or a force-opaque walk's -- those meet only one
    // kind of geometry anyway) first asks the hardware alone: the same ray and flags with the non-opaque
    // geometry culled -- no candidate, no alpha test. An opaque caster ends it, blocked, whatever lies
    // in front (every kind adds 0 for a committed caster: the walk below returns 0 then, tinted layers
    // and budget alike). Clear: nothing opaque lies on the segment, so the walk below meets only the
    // non-opaque layers -- it culls the opaque geometry and comes to the same result.
    if (RT_WALK_OFIRST && !fol_walk && !trans_walk && !fixSkip && (flags & gl_RayFlagsOpaqueEXT) == 0u)
    {
        rayQueryEXT rq0;
        rtDiagRay(dir);   // (RenderVKRTCandidateDiag)
        rayQueryInitializeEXT(rq0, uTLAS, flags | gl_RayFlagsCullNoOpaqueEXT, mask & 0xFFu, origin, tmin, dir, tmax);
#ifdef CLEAN_SHADOW_LOOP_GUARD
        uint opaqueSteps = 0u;
        while (rayQueryProceedEXT(rq0))
        {
            if (++opaqueSteps > RT_CANDIDATE_BUDGET_NOOPAQUE)
            { rayQueryTerminateEXT(rq0); ++rt_query_refused; return vec3(0.0); }
        }
#else
        while (rayQueryProceedEXT(rq0)) {}   // (no candidate reaches the shader: the loop never runs)
#endif
        if (rayQueryGetIntersectionTypeEXT(rq0, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            return rtWalkBlocked(origin, dir, false, rq0);
        flags |= gl_RayFlagsCullOpaqueEXT;
    }
    rayQueryEXT rq;
    vec3 trans = vec3(1.0);
    uint n_cand = 0u;
    uint n_fol = 0u;   // #735 transmissive layers coloured so far
    const uint budget = (fol_walk || fixSkip) ? RT_CANDIDATE_BUDGET_NOOPAQUE : RT_CANDIDATE_BUDGET;   // (NoOpaque walks)
    // (#1039 occupancy) THE SELF PROBE OUTSIDE THE TRAVERSAL: a receiver whose no-self-shadow owner is still
    // UNKNOWN (rtSelfCandidate) resolves it by its own probe ray the first time a throttled character's
    // candidate arrives. Run there, inside this loop, the probe's traversal was nested in the walk's -- two
    // queries and the walk's whole state live at once, the walk kernels' register peak. Instead the walk ENDS
    // at that candidate (no decision taken on it), the probe runs with no query live, and the walk runs again
    // from its start (pass 2: the owner known). Exact: every candidate before that one had owner 0, whose
    // rules never read the owner, so pass 2 takes the same decisions on them (the same ray, flags and
    // budget count), then decides the rest as the nested probe would have let the walk go on; rt_block_t
    // starts pass 2 from its entry value (rt_block_dyn only ever turns on, and pass 2 meets those casters
    // again). The ray was sanitised once (no second rtRaySane: the query budget counts as before). A walk
    // whose owner is known (0 or resolved) or a foliage walk (no self rules) takes one pass.
    const float bt0 = rt_block_t;
    [[dont_unroll]] for (uint pass = 0u; pass < (probeOut ? 1u : 2u); ++pass)   // (probeOut: one pass, no ray kept for a second)
    {
        bool probe = false;
        rtDiagRay(dir);   // (RenderVKRTCandidateDiag)
        rayQueryInitializeEXT(rq, uTLAS, flags, mask & 0xFFu, origin, tmin, dir, tmax);
#ifdef CLEAN_SHADOW_LOOP_GUARD
        uint querySteps = 0u;
#endif
        while (rayQueryProceedEXT(rq))
        {
#ifdef CLEAN_SHADOW_LOOP_GUARD
            // Count every yielded candidate, including unexpected procedural candidates.
            if (++querySteps > RT_CANDIDATE_BUDGET_NOOPAQUE + 1u)
            { rayQueryTerminateEXT(rq); ++rt_query_refused; return fol_walk ? trans : vec3(0.0); }
#endif
            if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
            const uint id = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                          uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
            if (++n_cand > budget)
            {
                // over budget (G-7): the shadow walks treat it as opaque -- the ray is blocked here; the
                // foliage ray keeps the colour gathered so far. A qualified emitter never
                // becomes a blocker on saturation: terminate with transmission so far;
                // occluders not visited within this bound can be missed in that case.
                if (!fol_walk && !rtEmissiveNoShadow(id)) rayQueryConfirmIntersectionEXT(rq);
                rayQueryTerminateEXT(rq);
                break;
            }
            // Before alpha/transmittance, self probes and opaque-mode confirmation.
            // Still counted against the fixed traversal budget (no unbounded beam walk).
            if (rtEmissiveNoShadow(id)) continue;
            if (fixSkip)
            {   // (rtFixNext stage 2) the light's own fixture: neither shadows nor tints; everything else walks on
                vec3 sa = vec3(0.0), sb = vec3(0.0);
                if (rtFixtureBox(id, rq, false, sa, sb)) continue;
            }
            uint fol = RT_FOL_OCCLUDE;
            if (fol_walk)
            {
                if (!rtIdIn(id) || (rt_instances[id].flags & 256u) != 0u) continue;   // double-sided: an occluder (/ #874 guard)
            }
            else
            {
                // No-self-shadow characters (rtSelfCandidate): the receiver's own stale geometry never
                // occludes it; another throttled character's opaque face is solid at once.
                const uint owner = RT_CAND_OWNER(rq);
                if (owner != 0u && rt_self_owner == RT_SELF_UNKNOWN) { probe = true; rayQueryTerminateEXT(rq); break; }
                const uint self_c = rtSelfCandidate(owner, id);
                if (self_c == 1u) continue;
                if (self_c == 2u || opaque_walk) { rayQueryConfirmIntersectionEXT(rq); continue; }
                // An away-facing face of a single-sided worn / rigged caster (rt_fol_rig; only alpha-blend
                // ones arrive -- the rest are culled in traversal): the shadow pass's alpha FS drops it at
                // ANY alpha, so it neither occludes nor tints -- no alpha fetch. (Static leaves keep #735's
                // solid-only rule in rtWalkCand.)
                fol = rtFolAway(id, rayQueryGetIntersectionFrontFaceEXT(rq, false));
                // (fixSkip: an OPAQUE-bucket face only arrives here because the walk is NoOpaque -- never a
                //  transmissive leaf, which a sun trace's facing rule, rt_fol_cull, would otherwise make it)
                if (fixSkip && fol == RT_FOL_LEAF && rtIdIn(id) && rtInstanceOpaque(rt_instances[id].flags))
                    fol = RT_FOL_OCCLUDE;
                if (fol == RT_FOL_PASS) continue;
            }
            // (the candidate's rule: its own function of the intersection + the small accumulator -- nothing
            //  of the caller is needed past the alpha test; the transforms are read inside, where used)
            if (rtWalkCand(id, RT_HIT_PRIM(rq, false, id), rayQueryGetIntersectionBarycentricsEXT(rq, false),
                           rayQueryGetIntersectionTEXT(rq, false), dir, rq, kind, ks, fol, trans, n_fol))
            {
                if (fol_walk) { rayQueryTerminateEXT(rq); break; }
                rayQueryConfirmIntersectionEXT(rq);
            }
        }
        if (!probe) break;
        if (probeOut) { rt_walk_probe = true; return vec3(0.0); }   // (the caller probes and walks again)
        // (pass 2: the walk from its start, the owner resolved)
        rt_self_owner = rtSelfProbe();
        rt_block_t = bt0;
        trans = vec3(1.0);
        n_cand = 0u;
        n_fol = 0u;
    }
    if (fol_walk) return trans;
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return trans;
    return rtWalkBlocked(origin, dir, trans_walk, rq);
}
vec3 rtVisWalkF(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind, uint ks)
{
    return rtVisWalkFP(origin, dir, tmin, tmax, mask, kind, ks, false);
}
vec3 rtVisWalk(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind)
{
    return rtVisWalkF(origin, dir, tmin, tmax, mask, kind, RT_WALK_KS_ANY);
}
// THE OWN-FIXTURE STEPS (rt_fix_*): ONE rule for every ray that takes the own-fixture re-trace (rtVisTraceK,
// rtVisQueued, clean_shadow_core.glsl shadowSampleRays; the CPU fixture scripts/check/checks/fixture_occluders.py
// replays it). A ray walks [t0, t1]; stage 0 is the whole ray [0, tFull]. After each walk (w its result, rt_fix_t
// the committed fixture's box entry or -1) this says whether the ray is DONE (w then its visibility) or sets
// the next walk's [t0, t1] and stage (the caller walks stage 2 with rt_fix_skip):
//   0 the whole ray: no fixture ended it -- done. Else stage 1 over [0, entry) (the part before the box), or
//     straight to stage 2 over the whole ray when the receiver lies inside the box (entry 0).
//   1 before the box: blocked -- done (0). Lit: stage 2 over [entry, tFull], its result times this one's tint.
//     Ended by ANOTHER fixture (a larger box entered earlier, a nested lamp): stage 2 over the whole ray.
//   2 [t0, tFull] with every fixture candidate passed over (NoOpaque, rt_fix_skip): a wall, a door, any other
//     instance inside the box still blocks -- done (w x stage 1's tint).
// At most three walks per ray, as the old clip (initial + two re-traces) -- and never "lit because the passes ran
// out": stage 2 cannot end on a fixture.
bool rtFixNext(inout uint stage, inout float t0, inout float t1, float tFull, inout vec3 acc, inout vec3 w)
{
    if (stage == 0u)
    {
        if (!(rt_fix_t >= 0.0)) return true;
        t0 = 0.0;
        t1 = min(rt_fix_t, tFull) - 0.002;
        if (t1 > 0.0) { stage = 1u; return false; }
        t1 = tFull;
        stage = 2u;
        return false;
    }
    if (stage == 1u)
    {
        if (rt_fix_t >= 0.0) t0 = 0.0;   // (another fixture: the whole ray, fixtures passed over)
        else if (!(max(max(w.r, w.g), w.b) > 0.0)) return true;   // blocked before the box
        else { acc = w; t0 = t1; }
        t1 = tFull;
        stage = 2u;
        return false;
    }
    w *= acc;
    return true;
}
// The walk with the OWN-FIXTURE steps (rtFixNext) through the same call site. Without rt_fix_on: rtVisWalk once.
vec3 rtVisTraceK(vec3 origin, vec3 dir, float tmax, uint mask, uint kind, uint ks)
{
    vec3 w = vec3(1.0), acc = vec3(1.0);
    float t0 = 0.0, t1 = tmax;
    uint stage = 0u;
    [[dont_unroll]] for (uint fr = 0u; fr < 3u; ++fr)
    {
        rt_fix_t = -1.0;
        rt_fix_skip = stage == 2u;
        w = rtVisWalkF(origin, dir, t0, t1, mask, kind, ks);   // THE walk site
        rt_fix_skip = false;
        if (rtFixNext(stage, t0, t1, tmax, acc, w)) break;
    }
    return w;
}
vec3 rtVisTrace(vec3 origin, vec3 dir, float tmax, uint mask, uint kind)
{
    return rtVisTraceK(origin, dir, tmax, mask, kind, RT_WALK_KS_ANY);
}

// ---- THE SINGLE SUN / MOON RAY (#1048): one ray along d with its tails -- the force-opaque walk (opaque_only),
//      else the split's opaque set (binary; RenderVKRTTranslucentSplit) or the mixed walk (parity: LL's
//      blend thresholds, RenderVKRTBlendShadowParity), then the translucent ray where the opaque one came
//      back lit, then the #735 foliage transmission ray where light is left (mode 2). The RT hit shading's
//      sun ray (clean_rt_shade.glsl rtHitSunShadowCore) and the RT god rays' march rays
//      (clean_godrays.comp) -- the god rays had a copy that gave the mixed walk no parity. Every ray
//      through ONE walk site. The caller holds the sun / moon trace state (rtFoliageSet .. rtFoliageEnd)
//      and rt_cone_* (the casters' alpha-test LOD). rgb: coloured translucency (#212). ----
// (#1039 M2) ... and the visibility queue's walk kernel, whose requests are not all sun rays: sun false = ONE
// ray of walk `kind` over TLAS classes `mask` (wkind / wmask; the sun's switches unused) with the own-fixture
// re-trace of rtVisTraceK (rt_fix_*: the caller sets them, at most two more passes) -- the hit occlusion ray
// (traceShadowRay) and the budgeted light rays (rtrProjRay) through the same walk site as the sun's.
vec3 rtVisQueued(vec3 o, vec3 d, float tmax, bool sun, uint wkind, uint wmask,
                 bool opaque_only, bool split, bool parity, bool rgb)
{
    uint kind = !sun ? wkind : (opaque_only ? RT_WALK_OPAQUE : (split ? RT_WALK_VIS : (rgb ? RT_WALK_RGB : RT_WALK_GREY)));
    uint mask = !sun ? wmask : ((opaque_only || split) ? (RT_MASK_SHADOW & ~RT_MASK_BLEND) : RT_MASK_SHADOW);
    bool par = sun && !opaque_only && !split && parity;
    vec3 vis = vec3(1.0), acc = vec3(1.0);
    float t0 = 0.0, tmc = tmax;
    uint stage = 0u;   // (not the sun: rtFixNext's)
    [[dont_unroll]] for (uint ph = 0u; ph < 3u; ++ph)
    {
        if (!sun) { rt_fix_t = -1.0; rt_fix_skip = stage == 2u; }
        else if (ph == 1u)
        {
            // the translucent split's ray, where the opaque set left the light
            if (opaque_only || !split || !(vis.x > 0.0)) continue;
            kind = rgb ? RT_WALK_TRANS_RGB : RT_WALK_TRANS;
            mask = RT_MASK_BLEND;
            par = parity;
        }
        else if (ph == 2u)
        {
            // foliage transmission (mode 2), where any light is left
            if (opaque_only || rtFolMode() != 2u || !(max(max(vis.r, vis.g), vis.b) > 0.0)) break;
            kind = rgb ? RT_WALK_FOL_RGB : RT_WALK_FOL;
            mask = RT_MASK_ALPHAMASK;
            par = false;
        }
        rt_blend_parity = par;
        const vec3 w = rtVisWalk(o, d, t0, tmc, mask, kind);   // THE walk site
        rt_blend_parity = false;
        rt_fix_skip = false;
        if (sun) { vis = ph == 0u ? w : vis * w; continue; }
        // one ray with its light's own-fixture steps (rtFixNext, rtVisTraceK's)
        vis = w;
        if (rtFixNext(stage, t0, tmc, tmax, acc, vis)) break;
    }
    return vis;
}
vec3 rtSunRayVis(vec3 o, vec3 d, float tmax, bool opaque_only, bool split, bool parity, bool rgb)
{
    return rtVisQueued(o, d, tmax, true, 0u, 0u, opaque_only, split, parity, rgb);
}

// ---- The walks by their old names (one inlined walk per call site: prefer one call with a run-time kind
//      where a caller picks between them) ----
vec3 traceShadowWalk(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, uint kind)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, kind);
}
float traceShadowRay(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_GREY).x;
}
vec3 traceShadowRayRGB(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_RGB);
}
float traceShadowRayVis(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_VIS).x;
}
// rgb = coloured (#212, translucentTransmit) else the grey (1 - alpha) of traceShadowRay.
vec3 traceTransmittance(vec3 origin, vec3 dir, float tmin, float tmax, bool rgb)
{
    return rtVisWalkF(origin, dir, tmin, tmax, RT_MASK_BLEND, rgb ? RT_WALK_TRANS_RGB : RT_WALK_TRANS, RT_WALK_KS_TRANS);
}
// FOLIAGE TRANSMISSION RAY (#735, mode 2): ONE ray per lit receiver over the alpha-mask
// class only, after the shadow rays found the light visible. Those rays already culled the
// away-facing leaf faces and stopped on any occluder -- opaque, worn, rigged, terrain
// AND light-facing leaves -- so nothing this walk could miss by stopping early is solid:
// it is free to end after rt_fol_layers transmissive leaves ("collects colour from N
// leaves and stops"), whatever order traversal hands them over in.
//   * Culls the LIGHT-facing faces (the shadow rays' side) in traversal: only away-facing
//     leaf faces become candidates. Double-sided instances (CULL_DISABLE) still arrive and
//     are skipped -- the shadow rays treat them as occluders from both sides.
//   * gl_RayFlagsNoOpaqueEXT: opacity micromaps commit "known opaque" micro-triangles in
//     hardware, with no candidate -- the ray would end there without the leaf's colour.
//     NoOpaque overrides the micromap's opaque result to non-opaque (a candidate), while
//     "known transparent" micro-triangles stay ignored (the micromap lookup happens first),
//     so the micromap still drops the holes for free. The mask holds only non-opaque
//     instances, so it adds no other candidate. (Modes 0 / 1 and the shadow rays keep the
//     micromaps' full benefit: facing culling runs before the micromap lookup.)
// rgb = coloured (RenderVKTranslucentShadows) else grey. Lowers rt_block_t at each
// coloured layer: the tinted spot is a shadow edge the mask denoiser must size and blur.
// (Over budget / an invalid ray: the colour gathered so far / none -- visibility is the shadow rays'.)
vec3 traceFoliageTransmit(vec3 origin, vec3 dir, float tmin, float tmax, bool rgb)
{
    return rtVisWalkF(origin, dir, tmin, tmax, RT_MASK_ALPHAMASK, rgb ? RT_WALK_FOL_RGB : RT_WALK_FOL, RT_WALK_KS_FOL);
}
// Opaque fast path: every instance treated as opaque (no alpha test, no candidates).
// A receiver that may sit on a no-self-shadow character (rt_self_owner != 0) drops the
// OPAQUE ray flag -- it would commit that character's own faces without a candidate -- and
// confirms every candidate but the receiver's own (rtSelfCandidate), so it stays the same
// all-opaque rule otherwise (one candidate before the first-hit termination, plus skips).
float traceShadowRayOpaque(vec3 origin, vec3 dir, float tmin, float tmax, uint mask)
{
    return rtVisWalk(origin, dir, tmin, tmax, mask, RT_WALK_OPAQUE).x;
}

#endif
