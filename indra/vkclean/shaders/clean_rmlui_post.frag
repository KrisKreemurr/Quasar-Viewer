#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "clean_rmlui.glsl"
layout(set=0,binding=0) uniform sampler2D images[];
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outColor;
void main() {
    vec4 c = texture(images[nonuniformEXT(pc.images.x)],uv-(pc.images.z==2u ? pc.params.xy : vec2(0)));
    if (pc.images.z == 1u) {
        float sigma = clamp(pc.params.z,0.5,16.0);
        vec4 sum = c;
        float total = 1;
        // Separable blur at reduced resolution; fixed work bound, no sigma loop.
        for (int i=1;i<=32;++i) {
            float w = exp(-float(i*i)/(2*sigma*sigma));
            vec2 delta = pc.params.xy*float(i);
            sum += (texture(images[nonuniformEXT(pc.images.x)],uv+delta)+
                    texture(images[nonuniformEXT(pc.images.x)],uv-delta))*w;
            total += 2*w;
        }
        c = sum/total;
    }
    if (pc.images.z == 2u) c = pc.tint*c.a;
    if (pc.images.z == 3u) c = vec4(1.0-c.a);
    if (pc.images.y != 0u) c *= texture(images[nonuniformEXT(pc.images.y)],(gl_FragCoord.xy-pc.viewport.xy)*pc.viewport.zw).a;
    c *= pc.params.w;
    vec3 straight = rmlDisplay(c.a>0 ? c.rgb/c.a : vec3(0));
    outColor = pc.images.w!=0u ? vec4(straight*c.a, c.a) : vec4(straight, c.a);
}
