#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "clean_rmlui.glsl"
layout(set=0,binding=0) uniform sampler2D images[];
layout(location=0) in vec4 color;
layout(location=1) in vec2 uv;
layout(location=2) in vec2 localPosition;
layout(location=3) flat in uint drawIndex;
layout(location=0) out vec4 outColor;
vec4 gradient(uint index) {
    RmlGradient g = pc.gradients.gradients[index];
    vec2 q = uv - g.p.xy;
    float t = 0;
    if (g.flags.x == 1u) t = dot(q,g.p.zw) / max(dot(g.p.zw,g.p.zw),1e-12);
    if (g.flags.x == 2u) t = length(q*g.p.zw);
    if (g.flags.x == 3u) t = 0.5 + atan(-dot(q,g.p.zw),dot(q,vec2(-g.p.w,g.p.z))) / 6.28318530718;
    uint count = clamp(g.flags.z,1u,16u);
    float firstStop = g.stops[0][0];
    float lastStop = g.stops[(count-1u)/4u][(count-1u)%4u];
    if (g.flags.y != 0u && lastStop > firstStop) t = mod(t-firstStop,lastStop-firstStop)+firstStop;
    vec4 result = g.colors[0];
    for (uint i=1u; i<count; ++i) {
        float a = g.stops[(i-1u)/4u][(i-1u)%4u];
        float b = g.stops[i/4u][i%4u];
        result = mix(result,g.colors[i],b>a ? clamp((t-a)/(b-a),0,1) : step(b,t));
    }
    return result;
}
void main() {
    RmlDraw d = pc.records.draws[drawIndex];
    if (any(lessThan(gl_FragCoord.xy,d.clip.xy)) || any(greaterThanEqual(gl_FragCoord.xy,d.clip.zw))) discard;
    vec4 t = texture(images[nonuniformEXT(d.textures.x)],uv);
    vec4 c = color * t;
    if ((d.textures.w & 0x100u) != 0u) c = vec4((d.textures.w & 0x200u) != 0u ? 0.0 : 1.0);
    // #703 "llimage-solid" (type 4): LLUIImage::drawSolid, the vertex colour through the image's alpha.
    if (d.textures.z != 0u) c = pc.gradients.gradients[d.textures.z-1u].flags.x == 4u ? color * t.a : c * gradient(d.textures.z-1u);
    if (d.textures.y != 0u) c *= texture(images[nonuniformEXT(d.textures.y)],gl_FragCoord.xy*pc.viewport.zw).a;
    // The shared pipeline builder's over operator expects straight source RGB;
    // all retained textures and intermediates store premultiplied RGBA.
    outColor = vec4(rmlDisplay(c.a > 0 ? c.rgb/c.a : vec3(0)),c.a);
}
