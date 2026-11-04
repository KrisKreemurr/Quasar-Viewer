/**
 * @file qlFrameWarpF.glsl
 * @brief Quasar Viewer: a generated frame, the last real frame reprojected by its depth to a camera between it and the next.
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

// Drawn over the whole window, into the default framebuffer, by QLFrameWarp::present.
//
// Two copies of the last real frame: qlWarpWorldMap, the world as the final blit left it, before any interface was
// drawn over it, and qlWarpFrameMap, the frame as it was shown. Where the two differ the interface drew, and the
// interface stays where it is on screen, so those pixels are the shown frame's. Everywhere else the world is warped.
//
// The warp is a backward reprojection: for each pixel of the generated camera, find the pixel of the real frame whose
// surface lands on it. A surface is carried from the kept frame's clip space to the agent's space, moved with the
// avatar whose box it lies in (if any), and on into the generated camera's clip space; a guess at the source is moved
// by however far its surface lands from the pixel wanted, a few times. Under the infinite reverse-Z projection the
// sky's depth is 0, a point at infinity, which the matrices carry as a direction: the sky turns with the camera and
// does not move with it, as it should.
//
// Where the guess does not settle, the pixel is one the kept frame did not see: something in front moved off it, and
// what was behind is wanted. A few more starts either side are tried, and of those that settle the farthest surface
// is taken, so the strip fills from the background rather than stretching the edge of what moved past it.

#define MAX_WARP_AVATARS 32

out vec4 frag_color;

uniform sampler2D qlWarpWorldMap;
uniform sampler2D qlWarpFrameMap;
uniform sampler2D depthMap;

uniform mat4 qlWarpInvViewProj;
uniform mat4 qlWarpViewProj;
uniform vec4 qlWarpAvatarMin[MAX_WARP_AVATARS];
uniform vec4 qlWarpAvatarMax[MAX_WARP_AVATARS];
uniform vec4 qlWarpAvatarMove[MAX_WARP_AVATARS];
uniform int qlWarpAvatarCount;
uniform vec4 qlWarpWorldRect;   // left, bottom, width, height, window pixels
uniform vec2 qlWarpWindowSize;

float ndc_depth(float d)
{
#ifdef REVERSE_Z
    return d;
#else
    return d * 2.0 - 1.0;
#endif
}

// Whether depth a is farther from the camera than depth b.
bool farther(float a, float b)
{
#ifdef REVERSE_Z
    return a < b;
#else
    return a > b;
#endif
}

// The kept frame's surface under ndc xy, carried to the generated camera's ndc xy; false where it falls behind it.
bool forward(vec2 src, out vec2 dst, out float depth)
{
    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    depth = textureLod(depthMap, uv, 0.0).r;
    vec4 p = qlWarpInvViewProj * vec4(src, ndc_depth(depth), 1.0);

    // A surface at a finite distance moves with the avatar it belongs to.
    if (abs(p.w) > 1e-6)
    {
        vec3 pos = p.xyz / p.w;
        int count = min(qlWarpAvatarCount, MAX_WARP_AVATARS);
        for (int i = 0; i < count; ++i)
        {
            if (all(greaterThanEqual(pos, qlWarpAvatarMin[i].xyz)) && all(lessThanEqual(pos, qlWarpAvatarMax[i].xyz)))
            {
                pos += qlWarpAvatarMove[i].xyz;
                break;
            }
        }
        p = vec4(pos, 1.0);
    }

    vec4 c = qlWarpViewProj * p;
    if (c.w <= 1e-6)
    {
        dst = src;
        return false;
    }
    dst = c.xy / c.w;
    return true;
}

// Moves a guess at the source toward the one whose surface lands on `want`; the squared distance it still lands from
// it, in ndc, and the depth it found.
float settle(vec2 want, inout vec2 src, out float depth)
{
    float err = 1e9;
    depth = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec2 lands;
        if (!forward(src, lands, depth))
        {
            return 1e9;
        }
        vec2 delta = want - lands;
        err = dot(delta, delta);
        if (err < 1e-9)
        {
            break;
        }
        src += delta;
    }
    return err;
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 shown = texelFetch(qlWarpFrameMap, px, 0);
    vec4 world = texelFetch(qlWarpWorldMap, px, 0);

    // The interface, which does not move with the camera.
    vec4 diff = abs(shown - world);
    if (max(max(diff.r, diff.g), max(diff.b, diff.a)) > (0.5 / 255.0))
    {
        frag_color = shown;
        return;
    }

    vec2 rel = (gl_FragCoord.xy - qlWarpWorldRect.xy) / qlWarpWorldRect.zw;
    if (any(lessThan(rel, vec2(0.0))) || any(greaterThan(rel, vec2(1.0))))
    {
        frag_color = shown;
        return;
    }

    vec2 want = rel * 2.0 - 1.0;
    vec2 pixel = 2.0 / qlWarpWorldRect.zw;      // one pixel, in ndc
    float settled = dot(pixel, pixel) * 0.5;    // within about seven tenths of a pixel

    vec2 src = want;
    float depth;
    float err = settle(want, src, depth);
    vec2 miss = want - src;                     // how far the source was found from the pixel itself

    if (err > settled)
    {
        // Uncovered: try starts either side along the way the guess was pulled, and keep the farthest that settles.
        vec2 best = src;
        float best_depth = depth;
        bool found = false;
        vec2 dir = length(miss) > 1e-6 ? normalize(miss) : vec2(1.0, 0.0);
        float reach = max(length(miss), 4.0 * length(pixel));
        for (int k = 0; k < 4; ++k)
        {
            float t = (k < 2 ? 1.0 : 2.0) * reach * ((k & 1) == 0 ? 1.0 : -1.0);
            vec2 alt = want + dir * t;
            float alt_depth;
            float alt_err = settle(want, alt, alt_depth);
            if (alt_err <= settled && (!found || farther(alt_depth, best_depth)))
            {
                best = alt;
                best_depth = alt_depth;
                found = true;
            }
        }
        src = best;
    }

    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 win = qlWarpWorldRect.xy + uv * qlWarpWorldRect.zw;
    frag_color = textureLod(qlWarpWorldMap, win / qlWarpWindowSize, 0.0);
}
