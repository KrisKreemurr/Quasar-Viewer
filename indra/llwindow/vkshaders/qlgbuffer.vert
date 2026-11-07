#version 450
// Quasar Viewer: prims drawn into a G-buffer by Vulkan (QuasarVulkanProbeDraws, QuasarVulkanMainDraws,
// QuasarVulkanPBRDraws; QLVulkanPresenter::gbufferDraw), as GL's vertex stages for an unrigged draw:
//   - simple (kind 0), deferred/diffuseV.glsl under LINEAR_DIFFUSE: the position by the draw's model-view-projection,
//     the normal by its normal matrix (the inverse transpose of the model-view's upper 3x3, as LLRender works it), the
//     texture coordinate by the texture matrix (texture animation; identity where LLRenderPass::pushBatch loads none),
//     the tint linearised as textureUtilV.glsl's linearizeVertexTint does;
//   - PBR (kind 1), deferred/pbropaqueV.glsl: the same, and each map's coordinate through its KHR transform and the
//     texture matrix (textureUtilV.glsl's texture_transform), the tangent by the normal matrix and turned with the
//     normal map's transform (tangent_space_transform).
//   - legacy material (kind 2), deferred/materialV.glsl under LINEAR_DIFFUSE: the three texture coordinates by the
//     texture matrix, the tangent by the normal matrix, normalised, its w the sign.
// Each draw's data is found by its instance.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord0;
layout(location = 3) in vec4 diffuse_color;
layout(location = 4) in vec4 tangent;
layout(location = 5) in vec2 texcoord1;  // a legacy material's normal map's
layout(location = 6) in vec2 texcoord2;  // and its specular map's

struct Draw
{
    mat4 mvp;
    mat4 normal_matrix;  // the 3x3 in its first three columns
    mat4 texture_matrix;
    vec4 transforms[8];  // KHR packed, two a map: base colour, normal, ORM, emissive
    vec4 pbr;            // metallic, roughness, kind, the slots whose views decode sRGB (bits)
    vec4 emissive;
    vec4 specular;
    vec4 material;
};

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    Draw draws[];
};

layout(push_constant) uniform Push
{
    vec4 flags;  // depth remap, GBUFFER_NORM_HDR, emissive attachment
} pc;

layout(location = 0) out vec3 vary_normal;
layout(location = 1) out vec2 base_color_texcoord;
layout(location = 2) out vec4 vertex_color;
layout(location = 3) out vec2 normal_texcoord;
layout(location = 4) out vec2 metallic_roughness_texcoord;
layout(location = 5) out vec2 emissive_texcoord;
layout(location = 6) out vec3 vary_tangent;
layout(location = 7) flat out float vary_sign;
layout(location = 8) flat out uint draw_index;

vec4 linearizeVertexTint(vec4 tint)
{
    vec3 cs = max(tint.rgb, vec3(0.0));
    vec3 low_range = cs / vec3(12.92);
    vec3 high_range = pow((cs + vec3(0.055)) / vec3(1.055), vec3(2.4));
    return vec4(mix(high_range, low_range, lessThanEqual(cs, vec3(0.04045))), tint.a);
}

vec2 khr_texture_transform(vec2 texcoord, vec2 scale, float rotation, vec2 offset)
{
    mat3 scale_mat = mat3(scale.x,0,0, 0,scale.y,0, 0,0,1);
    mat3 offset_mat = mat3(1,0,0, 0,1,0, offset.x, offset.y, 1);
    mat3 rotation_mat = mat3(
        cos(rotation),-sin(rotation), 0,
        sin(rotation), cos(rotation), 0,
                    0,             0, 1
    );
    mat3 transform = offset_mat * rotation_mat * scale_mat;
    return (transform * vec3(texcoord, 1)).xy;
}

vec2 texture_transform(vec2 vertex_texcoord, vec4 khr0, vec4 khr1, mat4 sl_animation_transform)
{
    vec2 texcoord = vertex_texcoord;
    texcoord = (sl_animation_transform * vec4(texcoord, 0, 1)).xy;
    texcoord.y = 1.0 - texcoord.y;
    texcoord = khr_texture_transform(texcoord, khr0.xy, khr0.z, khr1.xy);
    texcoord.y = 1.0 - texcoord.y;
    return texcoord;
}

vec4 tangent_space_transform(vec4 vertex_tangent, vec3 vertex_normal, vec4 khr0, mat4 sl_animation_transform)
{
    vec2 weights = vec2(1, 0);
    float khr_rotation = -khr0.z;
    mat2 khr_rotation_mat = mat2(
        cos(khr_rotation),-sin(khr_rotation),
        sin(khr_rotation), cos(khr_rotation)
    );
    weights = khr_rotation_mat * weights;
    vec2 khr_scale_sign = sign(khr0.xy);
    weights *= khr_scale_sign.xy;
    mat2 inv_sl_rot_scale;
    inv_sl_rot_scale[0][0] = sl_animation_transform[0][0];
    inv_sl_rot_scale[0][1] = sl_animation_transform[0][1];
    inv_sl_rot_scale[1][0] = sl_animation_transform[1][0];
    inv_sl_rot_scale[1][1] = sl_animation_transform[1][1];
    weights = inv_sl_rot_scale * weights;
    weights.x += 1.0 - abs(sign(sign(weights.x) + (0.5 * sign(weights.y))));
    weights = normalize(weights);
    weights.y = -weights.y;
    vec3 vertex_binormal = vertex_tangent.w * cross(vertex_normal, vertex_tangent.xyz);
    float sign_flip = khr_scale_sign.x * khr_scale_sign.y;
    return vec4((weights.x * vertex_tangent.xyz) + (weights.y * vertex_binormal.xyz), vertex_tangent.w * sign_flip);
}

void main()
{
    Draw d = draws[gl_InstanceIndex];
    draw_index = uint(gl_InstanceIndex);
    vec4 p = d.mvp * vec4(position, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
    vec3 n = normalize(mat3(d.normal_matrix) * normal);
    vary_normal = n;
    vertex_color = linearizeVertexTint(diffuse_color);
    if (d.pbr.z < 0.5)
    {
        // Simple.
        base_color_texcoord = (d.texture_matrix * vec4(texcoord0, 0, 1)).xy;
        normal_texcoord = metallic_roughness_texcoord = emissive_texcoord = base_color_texcoord;
        vary_tangent = vec3(1.0, 0.0, 0.0);
        vary_sign = 1.0;
    }
    else if (d.pbr.z > 1.5)
    {
        // Legacy material.
        base_color_texcoord = (d.texture_matrix * vec4(texcoord0, 0, 1)).xy;
        normal_texcoord = (d.texture_matrix * vec4(texcoord1, 0, 1)).xy;
        metallic_roughness_texcoord = (d.texture_matrix * vec4(texcoord2, 0, 1)).xy;
        emissive_texcoord = base_color_texcoord;
        vary_tangent = normalize(mat3(d.normal_matrix) * tangent.xyz);
        vary_sign = tangent.w;
    }
    else
    {
        // PBR.
        base_color_texcoord = texture_transform(texcoord0, d.transforms[0], d.transforms[1], d.texture_matrix);
        normal_texcoord = texture_transform(texcoord0, d.transforms[2], d.transforms[3], d.texture_matrix);
        metallic_roughness_texcoord = texture_transform(texcoord0, d.transforms[4], d.transforms[5], d.texture_matrix);
        emissive_texcoord = texture_transform(texcoord0, d.transforms[6], d.transforms[7], d.texture_matrix);
        vec3 t = mat3(d.normal_matrix) * tangent.xyz;
        vec4 transformed_tangent = tangent_space_transform(vec4(t, tangent.w), n, d.transforms[2], d.texture_matrix);
        vary_tangent = normalize(transformed_tangent.xyz);
        vary_sign = transformed_tangent.w;
    }
}
