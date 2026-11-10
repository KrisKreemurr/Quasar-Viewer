// #703 persistent UI records. C++ mirrors these scalar/std430 layouts.
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require // addresses as uvec2: no shaderInt64 needed
struct RmlVertex { float x; float y; uint color; float u; float v; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer RmlVertices { RmlVertex vertices[]; };
struct RmlDraw {
    mat4 transform;
    vec4 translation; // xy pixels, zw reserved
    vec4 clip;        // left/top/right/bottom framebuffer pixels
    uvec4 textures;   // sampled index, mask index, gradient index+1, flags
    uvec2 vertices;   // device address (C++: uint64_t, little endian)
    uvec2 reserved;
};
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer RmlDraws { RmlDraw draws[]; };
struct RmlGradient {
    vec4 p; // origin xy, direction/radius inverse zw
    uvec4 flags; // type 1 linear,2 radial,3 conic; repeating; stop count
    vec4 colors[16];
    vec4 stops[4];
};
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer RmlGradients { RmlGradient gradients[]; };
layout(push_constant, std430) uniform RmlPush {
    RmlDraws records;
    RmlGradients gradients;
    vec4 viewport; // xy extent, zw inverse extent
    vec4 params;   // post direction xy / sigma / opacity
    vec4 tint;
    uvec4 images;  // source, mask, mode, reserved
    vec4 display;  // x: paper white nits of a linear HDR target, 0 = SDR / an intermediate
} pc;
// #703: what the LLView UI writes into a linear-nits HDR target (ui_display.glsl): sRGB UI
// colour -> linear x paper white. Identity on SDR targets and intermediates.
vec3 rmlDisplay(vec3 c) {
    if (pc.display.x <= 0.0) return c;
    c = clamp(c, 0.0, 1.0);
    vec3 lin = mix(pow((c + .055) / 1.055, vec3(2.4)), c / 12.92, lessThanEqual(c, vec3(.04045)));
    return lin * clamp(pc.display.x, 80.0, 1000.0);
}
