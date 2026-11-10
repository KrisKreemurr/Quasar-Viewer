#version 450
#extension GL_GOOGLE_include_directive : require
// LLRenderVK immediate-mode fragment shader.
//
// A single shader serves both textured and untextured batches. Untextured
// batches bind a 1x1 white texture, so `texture()*vColor == vColor`. This keeps
// the pipeline count down and mirrors gGL's "vertex color modulates the bound
// texture, and the default bind is white" behavior exactly.

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 outColor;

#include "ui_display.glsl"

void main() {
    outColor = displayUiColour(texture(uTex, vUV) * vColor, 1.0);
}
