#version 450
// vkclean -- AVATAR LAYER HOLDOUT, TRANSPARENT LAYERS (#1328; vkgfx/llcleansnapmattevk.cpp). An
// avatar-only recording's alpha-blended layers (hair, lace, glass attachments) write no depth, so
// clean_snap_holdout.comp cannot test them after the fact: the world occluder it wrote for this
// recording (already biased farther; 0 = none) goes INTO the depth buffer before they draw, and
// their own depth test cuts each fragment the world is in front of -- per fragment, so a soft hair
// fringe in front of a wall stays soft and one behind it goes. A fullscreen depth-only draw (VS:
// clean_glow_combine.vert) with GREATER and depth write (reversed-Z): the occluder replaces the
// stored depth only where it is nearer than the avatar's opaque surface or the far plane. Nothing
// reads this recording's opaque depth as "avatar" after it (the holdout compute, the lighting and
// the camera medium ran).

layout(std430, set = 0, binding = 2) readonly buffer Occluder { float occ[]; };

layout(push_constant) uniform Push {
    uvec4 params;   // x, y: size
} pc;

void main()
{
    const uvec2 p = min(uvec2(gl_FragCoord.xy), pc.params.xy - 1u);
    gl_FragDepth = occ[p.y * pc.params.x + p.x];
}
