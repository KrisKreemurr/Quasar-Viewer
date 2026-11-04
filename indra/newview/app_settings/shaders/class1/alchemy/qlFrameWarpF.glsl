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
// surface lands on it. qlWarpReproject carries the real frame's clip space to the generated camera's; a guess at the
// source is moved by however far its surface lands from the pixel wanted, a few times. Under the infinite reverse-Z
// projection the sky's depth is 0, a point at infinity, which the matrix carries as a direction: the sky turns with the
// camera and does not move with it, as it should.

out vec4 frag_color;

uniform sampler2D qlWarpWorldMap;
uniform sampler2D qlWarpFrameMap;
uniform sampler2D depthMap;

uniform mat4 qlWarpReproject;
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

// The real frame's surface under ndc xy, carried to the generated camera's ndc xy; false where it falls behind it.
bool forward(vec2 src, out vec2 dst)
{
    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    float d = textureLod(depthMap, uv, 0.0).r;
    vec4 c = qlWarpReproject * vec4(src, ndc_depth(d), 1.0);
    if (c.w <= 1e-6)
    {
        dst = src;
        return false;
    }
    dst = c.xy / c.w;
    return true;
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
    vec2 src = want;
    for (int i = 0; i < 4; ++i)
    {
        vec2 lands;
        if (!forward(src, lands))
        {
            break;
        }
        vec2 delta = want - lands;
        src += delta;
        if (dot(delta, delta) < 1e-8)
        {
            break;
        }
    }

    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 win = qlWarpWorldRect.xy + uv * qlWarpWorldRect.zw;
    frag_color = textureLod(qlWarpWorldMap, win / qlWarpWindowSize, 0.0);
}
