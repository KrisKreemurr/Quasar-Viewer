#version 460
#extension GL_GOOGLE_include_directive : require
#include "clean_rmlui.glsl"
layout(location=0) out vec4 color;
layout(location=1) out vec2 uv;
layout(location=2) out vec2 localPosition;
layout(location=3) flat out uint drawIndex;
void main() {
    drawIndex = gl_BaseInstance;
    RmlDraw d = pc.records.draws[drawIndex];
    RmlVertex v = RmlVertices(d.vertices).vertices[gl_VertexIndex];
    localPosition = vec2(v.x,v.y);
    vec4 p = d.transform * vec4(localPosition + d.translation.xy, 0, 1);
    gl_Position = vec4(p.xy * pc.viewport.zw * 2.0 - p.ww, 0, p.w);
    uv = vec2(v.u,v.v);
    color = unpackUnorm4x8(v.color);
}
