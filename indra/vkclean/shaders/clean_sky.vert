#version 450
// vkclean — sky full-screen pass (vertex).
//
// Emits a single oversized triangle covering the whole viewport; no vertex
// buffer. Passes NDC to the fragment shader, which un-projects it to a world
// view direction. The sky is drawn before the world with depth writes off, so
// it acts as a pure background fill.
#include "clean_frame_info.glsl"

layout(location = 0) out vec2 v_ndc;
layout(location = 2) noperspective out vec4 v_ortho_water;   // parallel ray.xyz, origin height offset.w; 0 for perspective

void main()
{
    // Fullscreen-triangle trick: 3 verts covering NDC [-1,3].
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 pos = uv * 2.0 - 1.0;               // (-1,-1),(3,-1),(-1,3)
    gl_Position = vec4(pos, 0.0, 1.0);       // depth 0; depth test off anyway
    v_ndc = pos;
    v_ortho_water = vec4(0.0);
    // The live orthographic camera deliberately pushes a perspective inverse for the sky's
    // appearance. Water must use the actual projection, shared with the geometry fade.
    // Its VP has a constant homogeneous w; invert at three vertices, never per pixel.
    const mat4 VP = ssr.taa_cur_vp;
    if (VP[0].w == 0.0 && VP[1].w == 0.0 && VP[2].w == 0.0 && VP[3].w != 0.0)
    {
        const mat4 invVP = inverse(VP);
        vec4 p = invVP * vec4(pos - ssr.taa_jitter.zw, 0.5, 1.0);
        vec3 ray = normalize(-invVP[2].xyz);   // reversed Z: depth decreases forward
        vec3 rel = p.xyz / p.w;
        v_ortho_water = vec4(ray, rel.z - ray.z * dot(rel, ray));
    }
}
