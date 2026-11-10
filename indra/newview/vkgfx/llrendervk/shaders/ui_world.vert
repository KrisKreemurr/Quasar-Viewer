#version 450
#extension GL_EXT_multiview : require
// LLRenderVK, VR IN-WORLD TEXT (#1133): the pre-HUD text layer (name tags, chat bubbles,
// hover text, beacon labels) as world-space billboards, drawn ONCE for both eyes (multiview,
// one layer per eye). The vertices are world (agent) positions built on the CPU
// (LLRenderVK::recordWorldTextViews): the labels are laid out by LL's code in a head-facing
// basis, so both eyes see the same quad at its true depth (no double image). Paired with
// ui_world.frag. VR stereo only: never built or bound on the desktop.

layout(push_constant) uniform Push {
    mat4 viewProj[2];   // world -> clip per eye (unjittered, VK clip, the world's depth convention)
} pc;

layout(location = 0) in vec3 aPos;     // world (agent) position
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;   // RGBA8 unorm
layout(location = 3) in uint aFlags;   // bits 0-3 texture slot, bit 8 additive, bit 9 occluded
layout(location = 4) in vec4 aOccl;    // per eye: (hide depth, 1 / (hide - show)): xy eye 0, zw eye 1

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vClip;              // clip xyw (perspective-correct: exact under clipping)
layout(location = 3) flat out vec2 vOccl;
layout(location = 4) flat out uint vFlags;

void main() {
    const uint v = min(gl_ViewIndex, 1u);
    gl_Position = pc.viewProj[v] * vec4(aPos, 1.0);
    vClip = gl_Position.xyw;
    vColor = aColor;
    vUV = aUV;
    vOccl = v == 0u ? aOccl.xy : aOccl.zw;
    vFlags = aFlags;
}
