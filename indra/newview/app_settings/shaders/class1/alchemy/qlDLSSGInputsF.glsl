/**
 * @file qlDLSSGInputsF.glsl
 * @brief Quasar Viewer: the depth and motion vectors NVIDIA DLSS Frame Generation reads, from the frame just drawn.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Quasar Viewer Source Code (based on Alchemy Viewer)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// Drawn over the whole window by QLDLSSGInputs, once the final blit has drawn the world and before the interface.
// GL's rows run bottom up; the presenter turns both outputs over as it copies them for Vulkan, so this writes them as
// GL lays them out, and only the motion's vertical sign is Vulkan's (top down) here.
//
// The motion is the camera's alone: each pixel's surface, carried from this frame's clip space to the agent's by its
// depth and on into the last frame's, lands where the last frame drew it. What moved by itself (avatars dancing) DLSS
// Frame Generation finds with its own optical flow. Under the infinite reverse-Z projection the sky's depth is 0, a
// point at infinity, which the matrices carry as a direction: the sky turns with the camera and does not move with it.

out vec4 frag_data[2];          // 0: depth (R32F), as the scene stored it; 1: motion (RG16F), in window UV, top down

uniform sampler2D depthMap;     // the scene's depth, over the world view
uniform mat4 qlWarpInvViewProj; // this frame's clip space to the agent's
uniform mat4 qlWarpViewProj;    // the agent's space to the last frame's clip space
uniform vec4 qlWarpWorldRect;   // the world view in window pixels: left, bottom, width, height
uniform vec2 qlWarpWindowSize;  // the window in pixels
uniform float qlWarpAlpha;      // what the motion is multiplied by: 1, or 0 when there is no last frame to move from

float ndc_depth(float d)
{
#ifdef REVERSE_Z
    return d;
#else
    return d * 2.0 - 1.0;
#endif
}

void main()
{
#ifdef REVERSE_Z
    const float far_depth = 0.0;
#else
    const float far_depth = 1.0;
#endif

    vec2 local = (gl_FragCoord.xy - qlWarpWorldRect.xy) / qlWarpWorldRect.zw;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0))))
    {
        frag_data[0] = vec4(far_depth, 0.0, 0.0, 0.0);
        frag_data[1] = vec4(0.0);
        return;
    }

    float depth = textureLod(depthMap, local, 0.0).r;

    // Homogeneous all the way: a point at infinity stays a direction until the last frame's divide.
    vec4 agent = qlWarpInvViewProj * vec4(local * 2.0 - 1.0, ndc_depth(depth), 1.0);
    vec4 prev = qlWarpViewProj * agent;

    vec2 motion = vec2(0.0);
    if (prev.w > 1e-6)
    {
        vec2 prev_local = prev.xy / prev.w * 0.5 + 0.5;
        motion = (prev_local - local) * qlWarpWorldRect.zw / qlWarpWindowSize;
        motion.y = -motion.y;  // Vulkan's rows run top down
        motion *= qlWarpAlpha;
    }

    frag_data[0] = vec4(depth, 0.0, 0.0, 0.0);
    frag_data[1] = vec4(motion, 0.0, 0.0);
}
