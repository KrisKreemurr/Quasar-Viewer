#version 450
// Quasar Viewer: simple prims drawn into a reflection probe face's G-buffer by Vulkan (QuasarVulkanProbeDraws,
// QLVulkanPresenter::gbufferDraw). What GL's deferred/diffuseV.glsl does for an unrigged PASS_SIMPLE draw under
// LINEAR_DIFFUSE: the position by the draw's model-view-projection, the normal by its normal matrix (the inverse
// transpose of the model-view's upper 3x3, as LLRender works it), the texture coordinate as it is (a probe face applies
// no texture matrix: LLRenderPass::pushBatch skips it under gCubeSnapshot), and the tint linearised as
// textureUtilV.glsl's linearizeVertexTint does. Each draw's matrices are found by its instance.

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord0;
layout(location = 3) in vec4 diffuse_color;

struct Draw
{
    mat4 mvp;
    mat4 normal_matrix;  // the 3x3 in its first three columns
};

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    Draw draws[];
};

layout(push_constant) uniform Push
{
    vec4 flags;
} pc;

layout(location = 0) out vec3 vary_normal;
layout(location = 1) out vec2 vary_texcoord0;
layout(location = 2) out vec4 vertex_color;

vec4 linearizeVertexTint(vec4 tint)
{
    vec3 cs = max(tint.rgb, vec3(0.0));
    vec3 low_range = cs / vec3(12.92);
    vec3 high_range = pow((cs + vec3(0.055)) / vec3(1.055), vec3(2.4));
    return vec4(mix(high_range, low_range, lessThanEqual(cs, vec3(0.04045))), tint.a);
}

void main()
{
    Draw d = draws[gl_InstanceIndex];
    vec4 p = d.mvp * vec4(position, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
    vary_normal = normalize(mat3(d.normal_matrix) * normal);
    vary_texcoord0 = texcoord0;
    vertex_color = linearizeVertexTint(diffuse_color);
}
