#version 450
// Quasar frame generation, simple mode: the frame halfway between the previous and the
// current HUD-less frame, warped along the renderer's motion vectors (cur - prev, in UV).
// A pixel at uv in the in-between frame shows what the current frame has at uv + t*mv and
// the previous frame at uv - (1-t)*mv. Both are blended where they agree; where they do
// not (something appeared or disappeared there), the current frame's sample wins.
layout(set = 0, binding = 0) uniform sampler2D uCur;     // HUD-less current frame (display size)
layout(set = 0, binding = 1) uniform sampler2D uPrev;    // HUD-less previous frame
layout(set = 0, binding = 2) uniform sampler2D uMotion;  // render size, xy = cur - prev (UV)

layout(push_constant) uniform Push
{
    vec2  invTarget;     // 1 / display size
    float t;             // 0.5: halfway
    float motionValid;   // 0: no motion this frame (warp off)
    float motionScale;   // 1 (QuasarFrameGenMotionScale)
    float flipY;         // 1: motion y is flipped against the image rows
    float blendPrev;     // 1: blend with the previous frame where they agree
    float pad;
} pc;

layout(location = 0) out vec4 oColor;

vec2 motionAt(vec2 uv)
{
    vec2 m = texture(uMotion, uv).xy;
    if (pc.flipY > 0.5) m.y = -m.y;
    return m * pc.motionScale;
}

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.invTarget;
    vec2 mv = vec2(0.0);
    if (pc.motionValid > 0.5)
    {
        // The largest motion among the centre and four diagonal neighbours: a moving
        // foreground edge pulls its own motion over the background it uncovers, which
        // hides most of the holes a centre-only lookup leaves behind it.
        vec2 texel = 1.0 / vec2(textureSize(uMotion, 0));
        mv = motionAt(uv);
        float best = dot(mv, mv);
        vec2 o[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
        for (int i = 0; i < 4; ++i)
        {
            vec2 m = motionAt(uv + o[i] * texel);
            float d = dot(m, m);
            if (d > best) { best = d; mv = m; }
        }
    }
    vec4 cur = texture(uCur, uv + pc.t * mv);
    vec4 prv = texture(uPrev, uv - (1.0 - pc.t) * mv);
    vec3 diff = abs(cur.rgb - prv.rgb);
    float agree = 1.0 - smoothstep(0.04, 0.12, max(diff.r, max(diff.g, diff.b)));
    float w = pc.blendPrev > 0.5 ? (1.0 - pc.t) * agree : 0.0;
    oColor = vec4(mix(cur.rgb, prv.rgb, w), 1.0);
}
