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
uniform int qlWarpAvatarCount;    // the boxes given in the low byte; above it, WARP_FILL
#define WARP_FILL 0x100           // fill uncovered strips from the background
#define WARP_DEBUG 0x200          // tint what each rule did: red filled, green moved with an avatar, blue held as hair

// What forward() last did with a surface, for WARP_DEBUG: 0 nothing, 1 moved with an avatar, 2 held as hair.
int g_rule = 0;
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

// What of an avatar's box see-through hair can be in: its top part, the head and shoulders. Lower down, what is seen
// through the box past the body (the floor between the legs, a wall past an arm) is more often the world behind.
#define HAIR_FRACTION 0.3

// The kept frame's surface under ndc xy, carried to the generated camera's ndc xy; false where it falls behind it.
bool forward(vec2 src, out vec2 dst, out float depth)
{
    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    depth = textureLod(depthMap, uv, 0.0).r;
    vec4 p = qlWarpInvViewProj * vec4(src, ndc_depth(depth), 1.0);

    int count = min(qlWarpAvatarCount & 0xff, MAX_WARP_AVATARS);
    if (count > 0)
    {
        // The kept frame's eye: the point its projection sends to w = 0.
        vec4 e = qlWarpInvViewProj * vec4(0.0, 0.0, 1.0, 0.0);
        vec3 eye = e.xyz / e.w;
        // The surface, or for the sky a point far along its direction.
        vec3 pos = abs(p.w) > 1e-6 ? p.xyz / p.w : eye + normalize(p.xyz) * 1.0e4;

        // A surface in an avatar's box moves with that avatar.
        int hit = -1;
        for (int i = 0; i < count; ++i)
        {
            if (all(greaterThanEqual(pos, qlWarpAvatarMin[i].xyz)) && all(lessThanEqual(pos, qlWarpAvatarMax[i].xyz)))
            {
                hit = i;
                break;
            }
        }

        // See-through hair writes no depth, so its pixels hold the depth of what is behind it, and would slide over
        // the head as the camera orbits. A surface seen through the top of an avatar's box is taken to be there, in
        // the box's middle along the ray, and moves with it; of the boxes the ray passes, the nearest.
        g_rule = hit >= 0 ? 1 : 0;
        if (hit < 0)
        {
            vec3 ray = pos - eye;
            float nearest = 2.0;
            for (int i = 0; i < count; ++i)
            {
                // Only for the avatars close enough for their hair to slide visibly (the move's w).
                if (qlWarpAvatarMove[i].w < 0.5)
                {
                    continue;
                }
                vec3 lo = qlWarpAvatarMin[i].xyz;
                vec3 hi = qlWarpAvatarMax[i].xyz;
                lo.z = hi.z - (hi.z - lo.z) * HAIR_FRACTION;
                vec3 inv = 1.0 / ray;
                vec3 t0 = (lo - eye) * inv;
                vec3 t1 = (hi - eye) * inv;
                vec3 tmin = min(t0, t1);
                vec3 tmax = max(t0, t1);
                float enter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
                float leave = min(min(tmax.x, tmax.y), tmax.z);
                // The ray passes through, and the surface lies beyond the box.
                if (enter < leave && leave < 1.0 && enter < nearest)
                {
                    nearest = enter;
                    hit = i;
                    g_rule = 2;
                    pos = eye + ray * (0.5 * (enter + leave));
                }
            }
        }

        if (hit >= 0)
        {
            p = vec4(pos + qlWarpAvatarMove[hit].xyz, 1.0);
        }
        else if (abs(p.w) > 1e-6)
        {
            p = vec4(pos, 1.0);
        }
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

// Moves a guess at the source toward the one whose surface lands on `want`; the squared distance the last guess's
// surface still lands from it, in ndc, and the depth it found. A slanted floor seen at a grazing angle closes in
// slowly, so it is given six steps.
float settle(vec2 want, inout vec2 src, out float depth)
{
    float err = 1e9;
    depth = 0.0;
    for (int i = 0; i < 6; ++i)
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

// settle() with the depth held at `depth` and the camera alone moving: where a surface at that depth under the pixel
// came from, for a strip the kept frame did not see, taken to be the background around it.
vec2 settle_at(vec2 want, float depth)
{
    vec2 src = want;
    for (int i = 0; i < 4; ++i)
    {
        vec4 c = qlWarpViewProj * (qlWarpInvViewProj * vec4(src, ndc_depth(depth), 1.0));
        if (c.w <= 1e-6)
        {
            break;
        }
        vec2 delta = want - c.xy / c.w;
        src += delta;
        if (dot(delta, delta) < 1e-9)
        {
            break;
        }
    }
    return src;
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
    float settled = dot(pixel, pixel) * 2.25;   // within a pixel and a half

    vec2 src = want;
    float depth;
    float err = settle(want, src, depth);

    int rule = g_rule;
    bool filled = false;
    if (err > settled && (qlWarpAvatarCount & WARP_FILL) != 0)
    {
        // Uncovered: something nearer moved off a strip the kept frame never saw, and the guess swings between it and
        // what is behind. What belongs there is the background, so the farthest depth around the pixel is taken for
        // it, and the strip is found as a surface at that depth moved by the camera alone. The search is around where
        // the guess ended, in the kept frame, as wide as the guess still missed by, between two pixels and twelve: the
        // whole way the surface moved would reach across to the far side of the nearest edge in view, and floor beside
        // a doorway would take the room's depth.
        float reach = clamp(sqrt(err), 2.0 * length(pixel), 12.0 * length(pixel));
        float bg = depth;
        for (int k = 0; k < 8; ++k)
        {
            float a = float(k) * 0.785398;
            for (int r = 1; r <= 2; ++r)
            {
                vec2 at = clamp((src + vec2(cos(a), sin(a)) * reach * (0.5 * float(r))) * 0.5 + 0.5, vec2(0.0), vec2(1.0));
                float d = textureLod(depthMap, at, 0.0).r;
                if (farther(d, bg))
                {
                    bg = d;
                }
            }
        }
        src = settle_at(want, bg);
        filled = true;
    }

    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 win = qlWarpWorldRect.xy + uv * qlWarpWorldRect.zw;
    frag_color = textureLod(qlWarpWorldMap, win / qlWarpWindowSize, 0.0);

    if ((qlWarpAvatarCount & WARP_DEBUG) != 0)
    {
        vec3 tint = filled ? vec3(1.0, 0.0, 0.0) : rule == 1 ? vec3(0.0, 1.0, 0.0) : rule == 2 ? vec3(0.0, 0.3, 1.0) : vec3(0.0);
        if (filled || rule != 0)
        {
            frag_color.rgb = mix(frag_color.rgb, tint, 0.5);
        }
    }
}
