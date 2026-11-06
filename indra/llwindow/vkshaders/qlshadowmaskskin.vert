#version 450
// Quasar Viewer: rigged alpha-masked shadow casters drawn in Vulkan (QLVulkanPresenter::shadowDrawMasked with a
// palette). qlshadowskin.vert's skinning, step for step, with qlshadowmask.vert's texture coordinate and colour.

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 weight4;
layout(location = 2) in vec2 texcoord0;
layout(location = 3) in vec4 diffuse_color;

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    mat4 mvp[];
};

layout(std430, set = 0, binding = 1) readonly buffer DrawPalettes
{
    uint paletteBase[];
};

layout(std430, set = 0, binding = 2) readonly buffer Palettes
{
    vec4 palette[];
};

struct Mask
{
    vec4 s;
    vec4 t;
    vec4 tile;
};

layout(std430, set = 0, binding = 3) readonly buffer Masks
{
    Mask mask[];
};

layout(push_constant) uniform Push
{
    vec4 flags;
} pc;

layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out float vertex_alpha;
layout(location = 2) flat out vec4 vary_tile;
layout(location = 3) flat out float vary_test;

const int MAX_JOINTS_PER_MESH_OBJECT = 110;

mat3x4 joint(uint base, int i)
{
    uint at = base + 1u + uint(i) * 3u;
    return mat3x4(palette[at], palette[at + 1u], palette[at + 2u]);
}

void main()
{
    uint base = paletteBase[gl_InstanceIndex];

    vec4 w = fract(weight4);
    vec4 index = floor(weight4);
    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT - 1));
    index = max(index, vec4(0.0));
    w *= 1.0 / (w.x + w.y + w.z + w.w);

    mat3x4 b = joint(base, int(index.x)) * w.x;
    b += joint(base, int(index.y)) * w.y;
    b += joint(base, int(index.z)) * w.z;
    b += joint(base, int(index.w)) * w.w;

    vec3 local = mat3(b) * position + vec3(b[0].w, b[1].w, b[2].w);
    vec3 origin = palette[base].xyz;
    mat4 mat = mvp[gl_InstanceIndex];
    vec4 p = mat * vec4(local, 0.0) + mat * vec4(origin, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;

    Mask m = mask[gl_InstanceIndex];
    vec3 uv = vec3(texcoord0, 1.0);
    vary_texcoord0 = vec2(dot(m.s.xyz, uv), dot(m.t.xyz, uv));
    vertex_alpha = diffuse_color.a;
    vary_tile = vec4(m.tile.xyz, m.s.w);
    vary_test = m.t.w;
}
