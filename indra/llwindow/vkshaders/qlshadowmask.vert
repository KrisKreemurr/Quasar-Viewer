#version 450
// Quasar Viewer: alpha-masked (cut-out) shadow casters drawn in Vulkan (QuasarVulkanShadowsMasked,
// QLVulkanPresenter::shadowDrawMasked). What GL's shadowAlphaMaskV.glsl and treeShadowV.glsl do for an unrigged caster:
// the position by the draw's model-view-projection, the texture coordinate through the draw's texture matrix, and the
// vertex colour's alpha for the fragment's test. Each draw's texture is a tile of the presenter's alpha atlas, which
// GL fills from the caster's own texture (shadowMaskTexture); the draw's tile, cutoff and test are read by its instance.

layout(location = 0) in vec3 position;
layout(location = 2) in vec2 texcoord0;
layout(location = 3) in vec4 diffuse_color;

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    mat4 mvp[];
};

// s and t: the texture matrix's rows for x and y (u, v and the translation), cutoff in s.w and the test in t.w;
// tile: where the texture is in the atlas, in texels (x, y, size).
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

// A draw of several textures: its tiles, by the texture index its vertices carry (16 a draw; Mask.tile.w set).
layout(std430, set = 0, binding = 5) readonly buffer Tiles
{
    uint tiles[];
};

layout(location = 5) in int texture_index;

// QLVulkanPresenter's SH_TILE and SH_ATLAS_SIZE / SH_TILE.
const uint SH_TILE = 128u;
const uint SH_TILES_ACROSS = 32u;

layout(push_constant) uniform Push
{
    vec4 flags;
} pc;

layout(location = 0) out vec2 vary_texcoord0;
layout(location = 1) out float vertex_alpha;
layout(location = 2) flat out vec4 vary_tile;
layout(location = 3) flat out float vary_test;

void main()
{
    Mask m = mask[gl_InstanceIndex];
    vec4 p = mvp[gl_InstanceIndex] * vec4(position, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
    vec3 uv = vec3(texcoord0, 1.0);
    vary_texcoord0 = vec2(dot(m.s.xyz, uv), dot(m.t.xyz, uv));
    vertex_alpha = diffuse_color.a;
    vec3 tile = m.tile.xyz;
    if (m.tile.w > 0.5)
    {
        uint t = tiles[uint(gl_InstanceIndex) * 16u + uint(clamp(texture_index, 0, 15))];
        tile.xy = vec2(float((t % SH_TILES_ACROSS) * SH_TILE), float((t / SH_TILES_ACROSS) * SH_TILE));
    }
    vary_tile = vec4(tile, m.s.w);
    vary_test = m.t.w;
}
