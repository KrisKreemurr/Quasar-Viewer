#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require
#extension GL_KHR_shader_subgroup_ballot : require   // the auto switch's fragment count
// vkclean -- ALPHA-BLEND COVER LIST PASS (#1147; clean_alpha_k.glsl has the contract,
// LLCleanAlphaKVK records it). Right before pass 2, over the four alpha lists pass 2 draws
// (pre-water / post-water x static / rigged) with the same VS modules (depth-mask twins:
// invariant position and uv), the same pad0 words and the same shading rate, no attachments:
// each fragment that pass 2 will draw records (its blend order, its alpha) in its bucket's slot
// of the pixels it covers: ONE atomicMax, the slot keeps its bucket's last-drawn fragment.
// The alpha is the lit FS's coverage before glare / Highlight Transparent can only raise it;
// every discard of the lit FS is repeated (more is safe: fewer entries); a pixel takes the
// entry only when the fragment is clearly in front of the opaque depth there, where pass 2's
// tests (the static tolerance test, the rigged hardware test) keep it.
#include "clean_frame_info.glsl"   // ssr: alpha_k, Highlight Transparent, the TAA mip bias
#include "clean_draw_view.glsl"    // g_draw_push.pad0
#include "clean_alpha_k.glsl"
layout(early_fragment_tests) in;   // (no depth attachment: only keeps the stores defined)

layout(location = 0) flat in uint v_material_id;
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;
layout(location = 18) flat in uint v_akdraw;

// 96 bytes -- matches LLCleanMaterialStore::Material (see clean_draw_common.glsl).
struct Material {
    uvec2 base_color_handle;
    uvec2 normal_handle;
    uvec2 mr_handle;
    uvec2 emissive_handle;
    vec4  base_color_factor;
    vec4  emissive_factor;
    vec4  spec_color_gloss;
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint  flags;
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
// Pass 1's final depth (+ the alpha-depth pre-pass's texels), as pass 2's FS reads it.
layout(set = 0, binding = 40) uniform sampler2DArray uBlendSceneDepth;
// ... and an avatar face's, while the avatar alpha depth is live (binding 69: pass 2 tests avatar faces
// against it -- clean_draw.frag blendDepthTolerance, the rigged group's own scope).
layout(set = 0, binding = 69) uniform sampler2DArray uAvatarSceneDepth;
#define GMDX_REZFX_FS
#include "gmdx_rezfx.glsl"   // <GMDX> the de-rez cut and fade, as the lit FS
#include "clean_blend_depth_rule.glsl"   // #1420 THE near-opaque rule (blendDepthKeeps): pass 2's skip

layout(buffer_reference, std430, buffer_reference_align = 8) buffer AkEntries64 { uint64_t e[]; };

void main()
{
    // Fragment size (coarse shading) and the depth slope, before any discard (derivatives).
    const vec2 fsz = clamp(round(vec2(abs(dFdx(gl_FragCoord.x)), abs(dFdy(gl_FragCoord.y)))), vec2(1.0), vec2(4.0));
    const vec2 dz = vec2(dFdx(gl_FragCoord.z), dFdy(gl_FragCoord.z)) / fsz;
    const uint pad0 = g_draw_push.pad0;
    if (!akBuilt(pad0) || ssr.ao_params2.z > 0.5) discard;
    // The lit FS's discards (clean_draw.frag decodeSurface), or more.
    if ((v_flags & (2048u | 65536u | 0x00400000u)) != 0u) discard;   // water, invisiprim, dying
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;    // single-sided back face
    rezfxDiscard(v_flags);
    // The pixels of the (coarse) fragment the raster covered (coverage index n - 1 - (px + py *
    // fw), as clean_draw.frag blendDepthTolerance) that lie clearly in front of the opaque
    // surface (reversed-Z: nearer = larger; 0 = sky) -- first, so hidden fragments skip the texel.
    const ivec2 fw = ivec2(fsz);
    const ivec2 org = ivec2(floor(gl_FragCoord.xy / fsz)) * fw;
    const int n = fw.x * fw.y;
    const uint cov = uint(gl_SampleMaskIn[0]);
    const uint W = akWidth(), H = akHeight(), K = akK();
    uint front = 0u;
    const bool avDepth = avatarDepthLive() && avatarDepthFace(v_flags);
    for (int py = 0; py < fw.y; ++py)
    {
        for (int px = 0; px < fw.x; ++px)
        {
            const uint bit = 1u << uint(n - 1 - (px + py * fw.x));
            const ivec2 pix = org + ivec2(px, py);
            if ((cov & bit) == 0u || uint(pix.x) >= W || uint(pix.y) >= H) continue;
            const float zF = gl_FragCoord.z + dot(dz, vec2(pix) + 0.5 - gl_FragCoord.xy);
            const float zO = avDepth ? texelFetch(uAvatarSceneDepth, ivec3(pix, 0), 0).r
                                     : texelFetch(uBlendSceneDepth, ivec3(pix, 0), 0).r;
            if (zO <= 0.0 || zF > zO * (1.0 + 1e-6)) front |= bit;
        }
    }
    if (front == 0u) discard;
    {   // The auto switch's cost counter (one atomic per subgroup): fragments in front.
        const uvec4 lanes = subgroupBallot(!gl_HelperInvocation);
        if (subgroupElect()) atomicAdd(akCounters().w[0], subgroupBallotBitCount(lanes));
    }
    const Material m = materials[v_material_id];
    vec4 base = m.base_color_factor * v_tint;
    if ((m.flags & 1u) != 0u)
        base *= texture(uTextures[nonuniformEXT(m.base_color_handle.x)], v_uv, taaLodBias(v_flags));
    // A cutoff discards below it in the lit FS (with the coverage scale, which only raises the
    // tested alpha: discarding at the unscaled value is the stricter test).
    if (m.alpha_cutoff >= 0.0 && base.a < m.alpha_cutoff) discard;
    // #1420 (RenderVKBlendOpaqueGBuffer): a texel the G-buffer took is not in pass 2's composite --
    // pass 2 skips it by the same rule and margin, so it is no layer here either.
    if ((pad0 & BLEND_GBUF_PUSH) != 0u && blendDepthKeeps(base.a, m.alpha_cutoff, v_flags, kBlendSkipAlpha)) discard;
    const float a = base.a * rezfxFadeOpacity(v_flags);
    // Rounded DOWN (and a hair below: the lit FS may associate the product differently).
    const uint a8 = uint(clamp(floor(a * 255.0 - 0.01), 0.0, 255.0));
    if (a8 < AK_MIN_A8) discard;   // too faint to matter as a cover (nor to displace one that does)

    const uvec2 own = akOrder(pad0, v_akdraw, uint(gl_PrimitiveID), gl_FrontFacing);
    const uint64_t key = (uint64_t(own.y) << 32) | uint64_t(own.x | a8);
    AkEntries64 L = AkEntries64(ssr.alpha_k.xy);
    AkWords tiles = akTiles();
    const uint tilesX = (W + 7u) >> 3u;
    const uint side = akSide(pad0);
    const uint bucket = akBucket(own, K);
    for (int py = 0; py < fw.y; ++py)
    {
        for (int px = 0; px < fw.x; ++px)
        {
            if ((front & (1u << uint(n - 1 - (px + py * fw.x)))) == 0u) continue;
            const ivec2 pix = org + ivec2(px, py);
            // A slot only grows (atomicMax), so a read already >= the key -- even a stale one --
            // means the atomic would change nothing.
            const uint slot = akSlot(pix, side, K) + bucket;
            if (L.e[slot] < key) atomicMax(L.e[slot], key);
            // The tile's dirty word: every inserting pixel of the tile hit the same word, so it
            // is stored only while it reads clear.
            const uint tw = (uint(pix.y) >> 3u) * tilesX + (uint(pix.x) >> 3u);
            if (tiles.w[tw] == 0u) tiles.w[tw] = 1u;
        }
    }
}
