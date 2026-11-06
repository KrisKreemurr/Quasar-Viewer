#version 450
// Quasar Viewer: the shadow map hand-over's self-test (QLVulkanPresenter::depthTest). One triangle over the whole
// target, no vertex buffer, its depth a plane: 0.1 + 0.4 u + 0.4 v, u and v the target's 0..1 across and down
// (Vulkan's framebuffer rows run top down). Interpolated over a w = 1 triangle, every pixel holds the plane's value
// at its centre, which the test checks GL reads back. Drawn with no fragment stage: depth only, as a shadow pass.

void main()
{
    vec2 p   = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0) (2,0) (0,2)
    vec2 ndc = p * 2.0 - 1.0;                                       // covers -1..1 and past
    vec2 uv  = ndc * 0.5 + 0.5;
    gl_Position = vec4(ndc, 0.1 + 0.4 * uv.x + 0.4 * uv.y, 1.0);
}
