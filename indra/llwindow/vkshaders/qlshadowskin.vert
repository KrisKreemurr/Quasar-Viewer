#version 450
// Quasar Viewer: rigged shadow casters drawn in Vulkan (QuasarVulkanShadows, QLVulkanPresenter::shadowDrawRigged).
// What GL's shadowV.glsl does under HAS_SKIN with objectSkinV.glsl, step for step: the weights decoded (the integer
// part a joint, the fraction its weight, normalised), the four joints' lines blended, the point skinned avatar-local and
// taken through the matrix with the rebase origin as a separate w = 1 term (objectSkinV.glsl's precision contract).
//
// The palettes are the CPU's (LLVOAvatar::MatrixPaletteCache::mGLMp): written once a frame for each avatar's skin into
// a ring of vec4s, the origin then each joint's three columns; each draw's palette is found by its instance.

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 weight4;

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

layout(push_constant) uniform Push
{
    vec4 flags;
} pc;

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
    mat4 m = mvp[gl_InstanceIndex];
    vec4 p = m * vec4(local, 0.0) + m * vec4(origin, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
}
