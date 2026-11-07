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
layout(location = 7) in vec4 weight4;    // a rigged draw's joints and weights

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
    vec4 rig;  // palette's first vec4, whether it skins
};

// The skins' palettes (the shadows'): a skin's origin, then three vec4s a joint (objectSkinV.glsl's matrixPalette).
layout(std430, set = 0, binding = 1) readonly buffer Palettes
{
    vec4 palette[];
};

const int MAX_JOINTS_PER_MESH_OBJECT = 110;

mat3x4 joint(uint base, int i)
{
    uint at = base + 1u + uint(i) * 3u;
    return mat3x4(palette[at], palette[at + 1u], palette[at + 2u]);
}

// objectSkinV.glsl's getSkinBlend.
mat3x4 skinBlend(uint base)
{
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);
    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT - 1));
    index = max(index, vec4(0.0));
    w *= 1.0 / (w.x + w.y + w.z + w.w);
    mat3x4 b = joint(base, int(index.x)) * w.x;
    b += joint(base, int(index.y)) * w.y;
    b += joint(base, int(index.z)) * w.z;
    b += joint(base, int(index.w)) * w.w;
    return b;
}

// objectSkinV.glsl's skinNormal: through the blend's cofactor.
vec3 skinNormal(mat3x4 b, vec3 n)
{
    mat3 m = mat3(b);
    return mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1])) * n;
}

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
    // A rigged draw (objectSkinV.glsl): the position skinned about the skin's origin, split as skinTransformH splits it;
    // the normal through the blend's cofactor, the tangent through the blend, both then by the model-view.
    bool rigged = d.rig.y > 0.5;
    vec3 object_normal = normal;
    vec3 object_tangent = tangent.xyz;
    vec4 p;
    if (rigged)
    {
        uint base = uint(d.rig.x + 0.5);
        mat3x4 b = skinBlend(base);
        vec3 local = mat3(b) * position + vec3(b[0].w, b[1].w, b[2].w);
        p = d.mvp * vec4(local, 0.0) + d.mvp * vec4(palette[base].xyz, 1.0);
        object_normal = skinNormal(b, normal);
        object_tangent = mat3(b) * tangent.xyz;
    }
    else
    {
        p = d.mvp * vec4(position, 1.0);
    }
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
    vec3 n = normalize(mat3(d.normal_matrix) * object_normal);
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
        vary_tangent = normalize(mat3(d.normal_matrix) * object_tangent);
        vary_sign = tangent.w;
    }
    else
    {
        // PBR.
        base_color_texcoord = texture_transform(texcoord0, d.transforms[0], d.transforms[1], d.texture_matrix);
        normal_texcoord = texture_transform(texcoord0, d.transforms[2], d.transforms[3], d.texture_matrix);
        metallic_roughness_texcoord = texture_transform(texcoord0, d.transforms[4], d.transforms[5], d.texture_matrix);
        emissive_texcoord = texture_transform(texcoord0, d.transforms[6], d.transforms[7], d.texture_matrix);
        vec3 t = mat3(d.normal_matrix) * object_tangent;
        vec4 transformed_tangent = tangent_space_transform(vec4(t, tangent.w), n, d.transforms[2], d.texture_matrix);
        vary_tangent = normalize(transformed_tangent.xyz);
        vary_sign = transformed_tangent.w;
    }
}
