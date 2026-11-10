#version 450
#extension GL_GOOGLE_include_directive : require
// Final display transfer ONLY. Input is already tone-mapped Rec.709 absolute
// linear nits. HDR: UI/HUD/overlays already composited at paper-white and no
// later UI writes. SDR: WORLD ONLY; tone-map here, then replay SDR UI/HUD.
// Never bind SDR-encoded scene/UI texels to uLinearNits.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uLinearNits;
layout(push_constant) uniform DisplayOutput {
    uint encoding; // 0: SDR capture, 1: Rec.2020/PQ HDR10, 2: scRGB
    float paperWhiteNits;
    float peakNits;
    float dither;
} display;
#include "clean_display_common.glsl"
vec3 pq(vec3 nits) {
    vec3 y=pow(clamp(nits/10000.0,0.0,1.0),vec3(2610.0/16384.0));
    return pow((3424.0/4096.0+(2413.0/128.0)*y)/(1.0+(2392.0/128.0)*y),vec3(2523.0/32.0));
}
float outputNoise() {
    vec2 p=gl_FragCoord.xy;
    float a=fract(52.9829189*fract(dot(p,vec2(.06711056,.00583715))));
    float b=fract(52.9829189*fract(dot(p+vec2(1.618,2.414),vec2(.06711056,.00583715))));
    return a+b-1.0;
}
void main() {
    vec3 nits=texture(uLinearNits,v_uv).rgb;
    if(any(isnan(nits))||any(isinf(nits))) nits=vec3(0.0);
    nits=clamp(nits,0.0,clamp(display.peakNits,80.0,10000.0));
    if(display.encoding==1u) {
        // Column-major Rec.709 -> Rec.2020, D65. Transfer is applied exactly once.
        mat3 to2020=mat3(.627404,.069097,.016391,
                         .329283,.919540,.088013,
                         .043313,.011362,.895595);
        vec3 encoded=pq(to2020*nits);
        if(display.dither>0.0) encoded+=outputNoise()/1023.0;
        outColor=vec4(clamp(encoded,0.0,1.0),1.0);
    } else if(display.encoding==2u) {
        outColor=vec4(nits/80.0,1.0);
    } else {
        // Dedicated world-only SDR capture target, never tone-map the UI layer.
        vec3 encoded=displayLinearToSrgb(displayNitsToSdrLinear(nits,display.paperWhiteNits,display.peakNits));
        if(display.dither>0.0) encoded+=outputNoise()/255.0;
        outColor=vec4(clamp(encoded,0.0,1.0),1.0);
    }
}
