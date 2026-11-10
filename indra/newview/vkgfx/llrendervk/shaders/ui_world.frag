#version 450
#extension GL_EXT_multiview : require
#extension GL_EXT_nonuniform_qualifier : require
// LLRenderVK, VR IN-WORLD TEXT (#1133), fragment half of ui_world.vert.
//
// One draw for every label: the glyph atlas pages and the bubble images the layer used this
// frame sit in one small sampler array (the vertex picks its slot). Occluded labels test each
// pixel against this eye's world depth layer at the label's anchor depth, as the desktop's
// ui_occl.frag: vis = clamp((hide - d) / (hide - show), 0, 1) -- 1 where the scene lies behind
// the anchor (minus its bias), 0 where it lies in front of it by more than the fade band.
// The depth is the render-size layered world depth (render scale / DLSS: the label's screen
// position is resolution independent). Output is premultiplied (blend ONE, ONE_MINUS_SRC_ALPHA):
// an additive batch writes alpha 0, so one pipeline serves both blend modes.

const uint kTexSlots = 16u;

layout(set = 0, binding = 0) uniform sampler2D uTex[kTexSlots];
layout(set = 0, binding = 1) uniform sampler2DArray uSceneDepth;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vClip;
layout(location = 3) flat in vec2 vOccl;
layout(location = 4) flat in uint vFlags;

layout(location = 0) out vec4 outColor;

void main() {
    float vis = 1.0;
    if ((vFlags & 0x200u) != 0u) {
        const ivec3 size = textureSize(uSceneDepth, 0);
        const vec2 uv = (vClip.xy / vClip.z) * 0.5 + 0.5;
        const ivec2 p = clamp(ivec2(uv * vec2(size.xy)), ivec2(0), size.xy - 1);
        const float d = texelFetch(uSceneDepth, ivec3(p, int(gl_ViewIndex)), 0).r;
        vis = clamp((vOccl.x - d) * vOccl.y, 0.0, 1.0);
        if (vis <= 0.0) discard;
    }
    const vec4 c = texture(uTex[nonuniformEXT(vFlags & 0xFu)], vUV) * vColor;
    const float a = c.a * vis;
    outColor = vec4(c.rgb * a, (vFlags & 0x100u) != 0u ? 0.0 : a);
}
