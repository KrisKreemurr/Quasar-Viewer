#version 450
// Quasar DLSS Frame Generation inputs: the renderer's motion vectors and depth, at the
// display size, in the formats Streamline is told of. Motion: Streamline wants the
// offset from the current pixel to where it was (prev - cur, in UV); the renderer writes
// cur - prev. Depth: the display depth as it is (reversed Z).
layout(set = 0, binding = 0) uniform sampler2D uMotion;  // render size, xy = cur - prev (UV)
layout(set = 0, binding = 1) uniform sampler2D uDepth;   // display size depth view

layout(push_constant) uniform Push
{
    vec2  invTarget;
    float motionValid;
    float flipY;
} pc;

layout(location = 0) out vec2  oMotion;
layout(location = 1) out float oDepth;

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.invTarget;
    vec2 m = pc.motionValid > 0.5 ? texture(uMotion, uv).xy : vec2(0.0);
    if (pc.flipY > 0.5) m.y = -m.y;
    oMotion = -m;
    oDepth = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
}
