#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean -- THE WAVE SURFACE's vertex shader (#1020): the main view's water as one procedural,
// camera-centred grid, displaced by the sea state (clean_sea_state.glsl seaSurface). No vertex
// or index buffer: one non-indexed draw of rings x N x N x 6 vertices (LLCleanRendererVK
// recordWaterWaves; the counts are ssr.sea[5]). The LL water planes stay the flat surface of the
// reflection-probe / mirror captures and of the flat-water mode.
//
// RINGS (geometry clipmap): ring r has spacing s0 x 2^r and N x N quads, its centre snapped to
// 2 x its spacing (vertices never swim). Each ring but the finest drops the quads its finer
// neighbour covers (the hole lands on its own quad edges: N is a multiple of 4). The outermost
// ring's border is pushed out to sea[5].w (the horizon). LOD SEAMS: a ring's outer band morphs
// its odd vertices onto the coarser ring's edges -- the AVERAGE of the two neighbours' displaced
// positions, not the surface at the midpoint -- reaching exactly the coarse edge at the border,
// whatever the camera's offset inside the snap: crack free. Each wave's geometry fades by the
// camera distance alone (4 of its wavelengths: the grid's resolution there), never by the ring,
// so a shared position is the same on both sides of a seam. (The FS keeps every wave's slope
// to the pixel footprint: far water is flat in geometry, wavy in shading.)
// ORDER: the water pipe blends without depth writes (pass 2's depth is the read-only scene), so
// a crest in front of a trough must be drawn after it: rings outermost first, and inside a ring
// the rows far to near along the main camera's forward (ssr.sea[6]) -- primitive order is the
// blend order (painter's order over a heightfield).
// Outputs: clean_draw_common.glsl's FS inputs. v_uv carries the UNDISPLACED point (agent xy):
// the water FS evaluates the sea there (its normal belongs to that point, not to the displaced one).
#include "clean_draw_view.glsl"
#include "clean_frame_info.glsl"
#include "clean_sea_state.glsl"

layout(location = 0) flat out uint v_material_id;
layout(location = 1)      out vec2 v_uv;
layout(location = 2)      out vec3 v_world_normal;
layout(location = 3) flat out vec4 v_tint;
layout(location = 4) flat out uint v_flags;
layout(location = 5)      out vec3 v_world_pos;
layout(location = 7)      out vec4 v_world_tangent;
layout(location = 8)      out vec2 v_uv_raw;
layout(location = 9) flat out uint v_terrain_rec;

const ivec2 kCorner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));

// The displaced surface point of the flat point xy (agent m).
vec3 seaPoint(vec2 xy, vec2 cam, out vec3 n)
{
    const SeaSurf s = seaSurface(xy, length(xy - cam), 0.0);
    n = s.n;
    return vec3(xy + s.disp.xy, ssr.water[3].w + s.dh + s.disp.z);
}

void main()
{
    v_material_id = 0u;
    v_tint = vec4(1.0);
    v_flags = 2048u | 2u;          // water (bit 11), alpha-blend layer (bit 1: the RT forward hook)
    v_world_tangent = vec4(0.0);
    v_uv_raw = vec2(0.0);
    v_terrain_rec = 0u;

    const int N = int(ssr.sea[5].x + 0.5);
    const int L = int(ssr.sea[5].y + 0.5);
    const float s0 = ssr.sea[5].z;
    const int perRing = N * N * 6;
    const int ring = L - 1 - gl_VertexIndex / perRing;   // outermost first
    const int q = (gl_VertexIndex / 6) % (N * N);
    const int row = q / N, col = q % N;
    const vec2 fwd = ssr.sea[6].xy;
    ivec2 cell;
    if (abs(fwd.x) >= abs(fwd.y)) cell = ivec2(fwd.x > 0.0 ? N - 1 - row : row, col);
    else                          cell = ivec2(col, fwd.y > 0.0 ? N - 1 - row : row);

    const vec2 cam = ssr.cam_origin.xy;   // the main eye (one grid for every view)
    const float s = s0 * exp2(float(ring));
    const vec2 cL = floor(cam / (2.0 * s)) * (2.0 * s);
    const float half_n = float(N / 2);

    // The finer ring's footprint: no quads here.
    if (ring > 0)
    {
        const vec2 cF = floor(cam / s) * s;
        const float hF = float(N / 4) * s;
        const vec2 q0 = cL + (vec2(cell) - half_n) * s;
        if (all(greaterThanEqual(q0, cF - hF)) && all(lessThanEqual(q0 + s, cF + hF)))
        {
            v_uv = vec2(0.0); v_world_normal = vec3(0.0, 0.0, 1.0); v_world_pos = vec3(0.0);
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);   // outside the depth range: nothing rasterizes
            return;
        }
    }

    const ivec2 g = cell + kCorner[gl_VertexIndex % 6];
    vec2 xy0 = cL + (vec2(g) - half_n) * s;
    const bool border = g.x == 0 || g.y == 0 || g.x == N || g.y == N;
    if (ring == L - 1 && border)
    {
        // the horizon: the outermost ring's edge pushed out (flat there: every wave has faded)
        const vec2 d = xy0 - cam;
        xy0 = cam + d * (ssr.sea[5].w / max(max(abs(d.x), abs(d.y)), 1.0));
    }

    vec3 n;
    vec3 P = seaPoint(xy0, cam, n);
    if (ring < L - 1)
    {
        // Geomorph band: the outer N/8 cells, 1 at every border position the snap allows.
        const float band = max(float(N / 8), 1.0);
        const vec2 dc = abs(xy0 - cam) / s;
        const float a = clamp((max(dc.x, dc.y) - (half_n - 2.0 - band)) / band, 0.0, 1.0);
        const bool ox = (g.x & 1) != 0, oy = (g.y & 1) != 0;
        if (a > 0.0 && (ox || oy))
        {
            // the coarse ring's edge (or, inside a coarse quad, its diagonal (1, 0) - (0, 1))
            const vec2 ea = ox && oy ? vec2(s, -s) : (ox ? vec2(s, 0.0) : vec2(0.0, s));
            vec3 na, nb;
            const vec3 Pa = seaPoint(xy0 + ea, cam, na);
            const vec3 Pb = seaPoint(xy0 - ea, cam, nb);
            P = mix(P, 0.5 * (Pa + Pb), a);
            n = normalize(mix(n, normalize(na + nb), a));
        }
    }

    v_uv = xy0;
    v_world_normal = n;
    v_world_pos = P;
    // CAMERA-RELATIVE PROJECTION (#555): as clean_draw.vert.
    gl_Position = pc.u_view_proj * vec4(P - pc.u_origin.xyz, 1.0);
}
