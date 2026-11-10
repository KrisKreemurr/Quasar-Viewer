#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — SNOW SURFACE MESH draw (#783 M3; clean_weather_snowmesh.comp builds it,
// vkgfx/llcleanweathervk.cpp recordMeshDraw). The world draw layout (draw sets 0 / 1 + the
// 16 B view push: clean_draw_view.glsl), the mesh's own vertex / index buffers bound as such:
// one indexed indirect draw per pass (the inner grid's and the outer ring's triangles alike).
// -DWX_MESH_DEPTH: the shadow cascades' position-only variant (no fragment stage: both layers
// cast where they overlap in the hand-over band -- the same surface, no double darkening).
#include "clean_draw_view.glsl"

// The depth pre-pass draw (clean_weather_snowmesh.frag -DWX_MESH_PRE) and the later G-buffer
// draw (LEQUAL) must land on the same depth across pipelines (the world VS's rule: clean_draw.vert).
invariant gl_Position;

#ifdef WX_MESH_TILES
// #1496 THE TILE CACHE (clean_weather_snowmesh.comp -DWX_MESH_TILES: THE VERTEX RECORD): the record is
// camera-free; the LOD is done HERE from the live camera -- the stride morph, the inner -> ring
// geomorph (its height and normal onto the ring surface the build stored), the depth fades, the lift
// -- with the old build's formulas, so a tile built in any frame draws what a build now would.
layout(location = 0) in vec3 a_pos;    // x, y (the smoothed outline's), the top s0 (no lift, no morph)
layout(location = 1) in uint a_nrm;    // octahedral 12-bit x | y << 12 | the flags << 24 (clean_weather_snowmesh.comp kVf*)
layout(location = 2) in vec3 a_morph;  // the base h, the stride morph's target, the ring surface under it
layout(location = 3) in uint a_rnrm;   // the ring surface's normal (octahedral 24 bits)
// The draw's constants (instance rate: firstInstance = layer x 4 + the stride's index; the params
// buffer's VS records -- llcleanweathervk.cpp recordMeshTiles):
layout(location = 4) in vec4 c0;       // the inner grid: x0, y0, 1 / extent, top
layout(location = 5) in vec4 c1;       // its shear xy, its reach rOut (the band), 1 = the ring runs
layout(location = 6) in vec4 c2;       // this layer's grid: x0, y0, 1 / extent, top
layout(location = 7) in vec4 c3;       // its shear xy, its reach (the fade), the fade (0 none, 1 the inner band: no ring, 2 the ring's own)
layout(location = 8) in vec4 c4;       // this layer's stride-2 / stride-4 distances, the lift (m) and its growth (m / m)
layout(location = 9) in vec4 c5;       // the stride's debug code (0 / 1 / 2), 1 = the ring layer, the depth debug, the depth threshold (m)
layout(location = 10) in vec4 c6;      // xyz the MAIN camera (the cull's: every view morphs alike)
#else
layout(location = 0) in vec3 a_pos;   // world (agent) position of the snow surface
layout(location = 1) in uint a_nrm;    // octahedral 12-bit x | y << 12 | the band byte << 24
#endif

#if !defined(WX_MESH_DEPTH)
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_wpos;
layout(location = 2) out vec2 v_band;  // x the hand-over t (0 inner .. 1 ring), y 1 = the ring's
layout(location = 3) out float v_dbg;  // #783 the depth debug's code (bits 0-1 the stride: 1 stride 2, 2 stride 4; 4 an apron vertex)
#ifdef WX_MESH_TILES
layout(location = 4) out float v_cut;  // #1496 the FS drops a fragment under 0: what a fade thinned under the threshold
#endif
#endif
#if !defined(WX_MESH_DEPTH) || defined(WX_MESH_TILES)

vec3 octDecode(uint w)
{
    const vec2 e = max(vec2(float(int(w << 20) >> 20), float(int(w << 8) >> 20)) / 2047.0, vec2(-1.0));
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
#endif

#ifdef WX_MESH_DEPTH
// #783 HAND-OVER IN THE SUN CASCADES: the G-buffer interleaves the two layers by a screen-space
// dither, which a shadow map cannot do -- both cast in full across the band. The inner layer there
// is still its own fine surface near the band's start (it morphs onto the ring's only by its
// end), so the coarse ring stood above it in places (ridges, kerbs, berms, a bridge deck's edge)
// and shadowed it: a thin dark line along the fine grid's hand-over. Here the RING sinks under
// the inner layer while t < 1 (the inner layer is the band's caster; at t = 1 the two surfaces
// are one and the ring casts alone beyond): kRingSink x (1 - t) metres, linear in t, so the
// ring's triangles stay continuous.
const float kRingSink = 1.0;
#endif

#ifdef WX_MESH_TILES
// A grid's edge fade at P (clean_weather_snowmesh.comp edgeFade: 1 within 2 % of its border, 0 inside 6 %).
float edgeFadeAt(vec4 F0, vec4 F1, vec3 P)
{
    const vec2 uv = (P.xy - (F0.w - P.z) * F1.xy - F0.xy) * F0.z;
    const vec2 e = min(uv, 1.0 - uv);
    return 1.0 - smoothstep(0.02, 0.06, min(e.x, e.y));
}
void main()
{
    const uint fl = a_nrm >> 24u;
    const float h = a_morph.x;
    const vec3 Q = vec3(a_pos.xy, h);           // (the build's column point: the morphs', band's and lift's distance)
    const vec3 cam = c6.xyz;
    const float dist = distance(Q, cam);
    const float rxy = length(Q.xy - cam.xy);
    // THE STRIDE MORPH (the old build's: onto the coarser lattice over 75 .. 92 % of the switch distance;
    // the cull draws a tile coarse only past the switch distance of its AABB, which holds Q -- complete)
    float z = a_pos.z;
    const uint cls = fl & 3u;
    if (cls == 1u) z = mix(z, a_morph.y, smoothstep(0.75 * c4.x, 0.92 * c4.x, dist));
    else if (cls == 2u) z = mix(z, a_morph.y, smoothstep(0.75 * c4.y, 0.92 * c4.y, dist));
    // THE HAND-OVER: the inner layer's band t (the camera's radius, the inner grid's edge); the inner
    // vertices geomorph onto the ring surface under them (h where the ring has none: the sheet tapers)
    const bool ring = (fl & 32u) != 0u;
    const float rOut = c1.z;
    const float band = max(smoothstep(0.75 * rOut, rOut, rxy), edgeFadeAt(c0, c1, Q));
    const bool morphRing = !ring && c1.w > 0.5;
    if (morphRing) z = mix(z, a_morph.z, band);
    // THE FADES (the old build faded the depth itself: the ring over 0.8 .. 1 x its reach and its edge;
    // the inner layer without a ring over its band)
    float f = 0.0;
    if (c3.w > 1.5) f = max(smoothstep(0.8 * c3.z, c3.z, rxy), edgeFadeAt(c2, c3, Q));
    else if (c3.w > 0.5) f = band;
    z = h + (z - h) * (1.0 - f);
    vec3 p = vec3(a_pos.xy, z + c4.z + c4.w * dist);   // + the lift (0.4 mm / m: the old build's)
#ifdef WX_MESH_DEPTH
    if (ring) p.z -= kRingSink * (1.0 - band);   // (the cascades' ring sink: above)
#endif
    gl_Position = pc.u_view_proj * vec4(p - pc.u_origin.xyz, 1.0);
#ifndef WX_MESH_DEPTH
    vec3 n = octDecode(a_nrm);
    if (morphRing && band > 0.0)
    {
        // (the old build mixed the gradients: g = -n.xy / n.z of each)
        const vec3 nr = octDecode(a_rnrm);
        const vec2 g = mix(-n.xy / max(n.z, 1e-4), -nr.xy / max(nr.z, 1e-4), band);
        n = normalize(vec3(-g, 1.0));
    }
    v_nrm = n;
    v_wpos = p;
    v_band = vec2(band, ring ? 1.0 : 0.0);
    v_dbg = c5.x + ((fl & 8u) != 0u ? 4.0 : 0.0) + ((fl & 16u) != 0u ? 8.0 : 0.0);
    // (a skirt's foot never cuts: the wall stands while its top does)
    v_cut = (fl & 4u) != 0u ? 1e3 : (z - h) - c5.w * f;
#endif
}
#else
void main()
{
    // (camera-relative, #555: the view's origin is subtracted before the projection)
#ifdef WX_MESH_DEPTH
    vec3 p = a_pos;
    {
        const uint b = a_nrm >> 24u;
        if ((b & 128u) != 0u) p.z -= kRingSink * (1.0 - float(b & 127u) / 127.0);
    }
    gl_Position = pc.u_view_proj * vec4(p - pc.u_origin.xyz, 1.0);
#else
    gl_Position = pc.u_view_proj * vec4(a_pos - pc.u_origin.xyz, 1.0);
#endif
#ifndef WX_MESH_DEPTH
    v_nrm = octDecode(a_nrm);
    v_wpos = a_pos;
    const uint b = a_nrm >> 24u;
    v_band = vec2(float(b & 127u) / 127.0, float(b >> 7u));
    v_dbg = float(a_nrm & 15u);
#endif
}
#endif   // WX_MESH_TILES
