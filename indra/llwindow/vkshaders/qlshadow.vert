#version 450
// Quasar Viewer: shadow casters drawn in Vulkan (QuasarVulkanShadows, QLVulkanPresenter::shadowBegin). What GL's
// shadowV.glsl does for an unrigged caster: the position by the draw's model-view-projection, nothing else. The
// matrices are worked out on the CPU exactly as GL's matrix stack works them (LLRender's setMul order), one a draw, and
// the draw's index into them is its instance: every draw is one instance whose first instance is its index, so a whole
// shadow map is one indirect draw.
//
// GL's clip space may hold depth from -1 to 1 (glClipControl NEGATIVE_ONE_TO_ONE, forward depth): flags.x is then 1,
// and the depth is moved into Vulkan's 0 to 1 the way GL's viewport transform moves it.

layout(location = 0) in vec3 position;

layout(std430, set = 0, binding = 0) readonly buffer Draws
{
    mat4 mvp[];
};

layout(push_constant) uniform Push
{
    vec4 flags;
} pc;

void main()
{
    vec4 p = mvp[gl_InstanceIndex] * vec4(position, 1.0);
    if (pc.flags.x > 0.5)
    {
        p.z = 0.5 * (p.z + p.w);
    }
    gl_Position = p;
}
