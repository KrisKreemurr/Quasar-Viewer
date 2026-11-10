#version 460
#extension GL_EXT_nonuniform_qualifier : require
// shadowbench -- receiver G-buffer FS: world position (w = 1) + normal (facing the camera
// for double-sided faces); mask surfaces alpha-tested as the viewer's G-buffer does.
layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_wnrm;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in uint v_mat;
layout(location = 4) flat in uint v_flags;
layout(location = 0) out vec4 o_pos;
layout(location = 1) out vec4 o_nrm;
struct Material {
    uvec2 base_color_handle; uvec2 normal_handle; uvec2 mr_handle; uvec2 emissive_handle;
    vec4 base_color_factor; vec4 emissive_factor; vec4 spec_color_gloss;
    float metallic_factor; float roughness_factor; float alpha_cutoff; uint flags;
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
void main()
{
    const Material m = materials[v_mat];
    float a = m.base_color_factor.a;
    if ((m.flags & 1u) != 0u) a *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv).a;
    if ((v_flags & 4u) != 0u && a < m.alpha_cutoff) discard;
    vec3 n = normalize(v_wnrm);
    if (!gl_FrontFacing) n = -n;
    o_pos = vec4(v_wpos, 1.0);
    o_nrm = vec4(n, 0.0);
}
