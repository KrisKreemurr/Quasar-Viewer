#version 450
// Quasar Viewer: the G-buffer texels of a simple prim, as GL's deferred/diffuseIndexedF.glsl writes them: the diffuse
// (decoded by its sRGB view, as GL's LINEAR_DIFFUSE sampler decodes it) times the linear tint into the sRGB albedo
// attachment (encoded on store, as GL_FRAMEBUFFER_SRGB does), the tint's alpha as the legacy specular and glossiness,
// and the normal octahedron-encoded with the tint's alpha as environment intensity and the atmospherics flag
// (globalF.glsl's encodeNormal, GBUFFER_FLAG_HAS_ATMOS). The emissive attachment, where the G-buffer has one, takes zero.

layout(set = 1, binding = 0) uniform sampler2D diffuseMap;

layout(location = 0) in vec3 vary_normal;
layout(location = 1) in vec2 vary_texcoord0;
layout(location = 2) in vec4 vertex_color;

layout(location = 0) out vec4 frag_data0;
layout(location = 1) out vec4 frag_data1;
layout(location = 2) out vec4 frag_data2;
layout(location = 3) out vec4 frag_data3;

const float GBUFFER_FLAG_HAS_ATMOS = 0.34;

vec2 OctWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

void main()
{
    vec3 col = vertex_color.rgb * texture(diffuseMap, vary_texcoord0).rgb;
    frag_data0 = vec4(col, 0.0);
    frag_data1 = vec4(vec3(vertex_color.a), vertex_color.a);
    vec3 nvn = normalize(vary_normal);
    frag_data2 = vec4(octEncode(nvn), vertex_color.a, GBUFFER_FLAG_HAS_ATMOS);
    frag_data3 = vec4(0.0);
}
