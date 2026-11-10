#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_shader_draw_parameters : require
// shadowbench -- the receiver G-buffer (main camera): world position + normal per pixel. The
// same quantized vertex stream and slim instance records as the viewer's world VS.
layout(location = 0) in ivec4 a_qpos;
layout(location = 1) in ivec2 a_quv;
layout(location = 2) in vec2  a_onrm;
layout(location = 3) in vec2  a_otan;
layout(std430, set = 0, binding = 39) readonly buffer QvWords { uint qv_words[]; };
#include "clean_qvertex.glsl"
#include "clean_vs_inst.glsl"
layout(std430, set = 0, binding = 54) readonly buffer VsInstances { VsInst vs_inst[]; };
#include "clean_draw_view.glsl"
layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_wnrm;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out uint v_mat;
layout(location = 4) flat out uint v_flags;
void main()
{
    const QvHeader qh = QV_LOAD_HEADER(qv_words, uint(max(gl_BaseVertexARB, int(QV_HEADER_VERTS))));
    const vec3 position = qh.pos_c + vec3(a_qpos.xyz) * qh.pos_s;
    const vec2 uv = qh.uv_c + vec2(a_quv) * qh.uv_s;
    const vec3 normal = qvOctDecode(a_onrm);
    const VsInst inst = vs_inst[gl_InstanceIndex];
    const mat4 M = vsiModel(inst);
    const vec3 wp = (M * vec4(position, 1.0)).xyz;
    const mat3 M3 = mat3(M);
    const mat3 Ncof = mat3(cross(M3[1], M3[2]), cross(M3[2], M3[0]), cross(M3[0], M3[1]));
    v_wpos = wp;
    v_wnrm = Ncof * normal;
    v_uv = uv;
    v_mat = inst.material_id;
    v_flags = inst.flags;
    gl_Position = pc.u_view_proj * vec4(wp - pc.u_origin.xyz, 1.0);
}
