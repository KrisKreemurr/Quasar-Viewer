#version 450
// Quasar Viewer: the alpha test of a masked shadow caster drawn in Vulkan, as GL's shadow programs test it:
//   test 0, shadowAlphaMaskF.glsl (PASS_ALPHA_MASK): the texture's alpha against the cutoff less half an 8-bit step,
//           then times the vertex colour's alpha against 0.05;
//   test 1, the same under IS_FULLBRIGHT (PASS_FULLBRIGHT_ALPHA_MASK): no vertex alpha, still the 0.05;
//   test 2, treeShadowF.glsl (the legacy material masks): the cutoff test alone.
// The masked passes never stipple. The texture is its tile of the atlas, repeated as GL's wrap repeats it, sampled
// linearly half a texel inside the tile's edges so no neighbour bleeds in.

layout(set = 0, binding = 4) uniform sampler2D atlas;

layout(location = 0) in vec2 vary_texcoord0;
layout(location = 1) in float vertex_alpha;
layout(location = 2) flat in vec4 vary_tile;
layout(location = 3) flat in float vary_test;

const float ATLAS_SIZE = 4096.0;

void main()
{
    vec2 uv = fract(vary_texcoord0);
    vec2 at = (vary_tile.xy + 0.5 + uv * (vary_tile.z - 1.0)) / ATLAS_SIZE;
    float alpha = textureLod(atlas, at, 0.0).a;
    if (alpha < vary_tile.w - 0.001953125)
    {
        discard;
    }
    if (vary_test < 1.5)
    {
        if (vary_test < 0.5)
        {
            alpha *= vertex_alpha;
        }
        if (alpha < 0.05)
        {
            discard;
        }
    }
}
