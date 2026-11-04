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

// Drawn over the whole window, into the default framebuffer, by QLFrameWarp.
//
// Each real frame is kept twice: its world as the final blit left it, before any interface was drawn over it, and the
// frame as it was shown. Where the two differ the interface drew, and the interface stays where it is on screen, so
// those pixels are the newest frame's as shown. Everywhere else the world is warped.
//
// From one frame (QuasarFrameWarpMode 0) the generated frame is made of the last real frame alone, toward where the
// next one's camera will be. From two (mode 1) it is made between two real frames already drawn, the later held back
// to be shown after it: each is warped to the generated camera on its own, and what one could not see the other
// usually did.
//
// The warp is a backward reprojection: for each pixel of the generated camera, find the pixel of a real frame whose
// surface lands on it. A surface is carried from that frame's clip space to the agent's space, moved with the avatar
// whose box it lies in (if any), and on into the generated camera's clip space; a guess at the source is moved by
// however far its surface lands from the pixel wanted, a few times. Under the infinite reverse-Z projection the sky's
// depth is 0, a point at infinity, which the matrices carry as a direction: the sky turns with the camera and does not
// move with it, as it should.
//
// Where no guess settles, the pixel is one no real frame saw: something in front moved off it, and what was behind is
// wanted. The farthest depth around is taken for it and the strip found as a surface at that depth moved by the
// camera alone, so it fills from the background rather than stretching the edge of what moved past it.

#define MAX_WARP_AVATARS 32

#define WARP_FILL 0x100           // fill uncovered strips from the background
#define WARP_DEBUG 0x200          // tint what each rule did
#define WARP_BOTH 0x400           // two real frames, the earlier through the plain names and the later through Next

out vec4 frag_color;

uniform sampler2D qlWarpWorldMap;       // the earlier frame's world (the only one, from one frame)
uniform sampler2D depthMap;             // and its depth
uniform mat4 qlWarpInvViewProj;         // and its clip space to the agent's
uniform sampler2D qlWarpNextWorldMap;   // the later frame's world, from two
uniform sampler2D qlWarpNextDepthMap;
uniform mat4 qlWarpNextInvViewProj;
uniform sampler2D qlWarpFrameMap;       // the newest frame as shown, the interface's source

uniform mat4 qlWarpViewProj;            // the agent's space to the generated camera's clip space
uniform float qlWarpAlpha;              // how far the generated frame is from the earlier frame toward the later

// The nearest avatars, as the earlier frame drew them, and how far each moved by the later (or the next, from one).
uniform vec4 qlWarpAvatarMin[MAX_WARP_AVATARS];
uniform vec4 qlWarpAvatarMax[MAX_WARP_AVATARS];
uniform vec4 qlWarpAvatarMove[MAX_WARP_AVATARS];    // w: 1 where the hair rule is for this avatar
uniform int qlWarpAvatarCount;          // the boxes in the low byte, the switches above

uniform vec4 qlWarpWorldRect;           // left, bottom, width, height, window pixels
uniform vec2 qlWarpWindowSize;

// What forward() last did with a surface, for WARP_DEBUG: 0 nothing, 1 moved with an avatar, 2 held as hair.
int g_rule = 0;

// What of an avatar's box see-through hair can be in: its top part, the head and shoulders. Lower down, what is seen
// through the box past the body (the floor between the legs, a wall past an arm) is more often the world behind.
#define HAIR_FRACTION 0.3

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

float source_depth(int side, vec2 src)
{
    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    return side == 0 ? textureLod(depthMap, uv, 0.0).r : textureLod(qlWarpNextDepthMap, uv, 0.0).r;
}

vec4 source_world(int side, vec2 src)
{
    vec2 uv = clamp(src * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 win = (qlWarpWorldRect.xy + uv * qlWarpWorldRect.zw) / qlWarpWindowSize;
    return side == 0 ? textureLod(qlWarpWorldMap, win, 0.0) : textureLod(qlWarpNextWorldMap, win, 0.0);
}

mat4 source_inv_view_proj(int side)
{
    return side == 0 ? qlWarpInvViewProj : qlWarpNextInvViewProj;
}

// A real frame's surface under ndc xy, carried to the generated camera's ndc xy, and its distance there (clip w);
// false where it falls behind the generated camera.
bool forward(int side, vec2 src, out vec2 dst, out float depth, out float dist)
{
    depth = source_depth(side, src);
    mat4 inv_view_proj = source_inv_view_proj(side);
    vec4 p = inv_view_proj * vec4(src, ndc_depth(depth), 1.0);
    g_rule = 0;

    int count = min(qlWarpAvatarCount & 0xff, MAX_WARP_AVATARS);
    if (count > 0)
    {
        // The frame's eye: the point its projection sends to w = 0.
        vec4 e = inv_view_proj * vec4(0.0, 0.0, 1.0, 0.0);
        vec3 eye = e.xyz / e.w;
        // The surface, or for the sky a point far along its direction.
        vec3 pos = abs(p.w) > 1e-6 ? p.xyz / p.w : eye + normalize(p.xyz) * 1.0e4;

        // From the earlier frame an avatar moves on by the fraction; from the later, back by the rest; and the later
        // frame drew it where it had moved to.
        float toward = side == 0 ? qlWarpAlpha : qlWarpAlpha - 1.0;
        float drawn = side == 0 ? 0.0 : 1.0;

        // A surface in an avatar's box moves with that avatar.
        int hit = -1;
        for (int i = 0; i < count; ++i)
        {
            vec3 offset = qlWarpAvatarMove[i].xyz * drawn;
            if (all(greaterThanEqual(pos, qlWarpAvatarMin[i].xyz + offset)) && all(lessThanEqual(pos, qlWarpAvatarMax[i].xyz + offset)))
            {
                hit = i;
                g_rule = 1;
                break;
            }
        }

        // See-through hair writes no depth, so its pixels hold the depth of what is behind it, and would slide over
        // the head as the camera orbits. A surface seen through the top of a near avatar's box is taken to be there,
        // in the box's middle along the ray, and moves with it; of the boxes the ray passes, the nearest.
        if (hit < 0)
        {
            vec3 ray = pos - eye;
            vec3 inv = 1.0 / ray;
            float nearest = 2.0;
            for (int i = 0; i < count; ++i)
            {
                if (qlWarpAvatarMove[i].w < 0.5)
                {
                    continue;
                }
                vec3 offset = qlWarpAvatarMove[i].xyz * drawn;
                vec3 lo = qlWarpAvatarMin[i].xyz + offset;
                vec3 hi = qlWarpAvatarMax[i].xyz + offset;
                lo.z = hi.z - (hi.z - lo.z) * HAIR_FRACTION;
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
            p = vec4(pos + qlWarpAvatarMove[hit].xyz * toward, 1.0);
        }
        else if (abs(p.w) > 1e-6)
        {
            p = vec4(pos, 1.0);
        }
    }

    vec4 c = qlWarpViewProj * p;
    dist = c.w;
    if (c.w <= 1e-6)
    {
        dst = src;
        return false;
    }
    dst = c.xy / c.w;
    return true;
}

// Moves a guess at a frame's source toward the one whose surface lands on `want`; the squared distance the last
// guess's surface still lands from it, in ndc, with the depth and distance it found. A slanted floor seen at a
// grazing angle closes in slowly, so it is given six steps.
float settle(int side, vec2 want, inout vec2 src, out float depth, out float dist)
{
    float err = 1e9;
    depth = 0.0;
    dist = 1e9;
    for (int i = 0; i < 6; ++i)
    {
        vec2 lands;
        if (!forward(side, src, lands, depth, dist))
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
// came from, for a strip no frame saw, taken to be the background around it.
vec2 settle_at(int side, vec2 want, float depth)
{
    mat4 inv_view_proj = source_inv_view_proj(side);
    vec2 src = want;
    for (int i = 0; i < 4; ++i)
    {
        vec4 c = qlWarpViewProj * (inv_view_proj * vec4(src, ndc_depth(depth), 1.0));
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

// The background behind a strip a frame did not see, around where its guess ended.
vec2 fill(int side, vec2 want, vec2 src, float depth, float err, vec2 pixel)
{
    // How far the last guess still missed is how wide the strip is: the whole way the surface moved would reach across
    // to the far side of the nearest edge in view, and floor beside a doorway would take the room's depth.
    float reach = clamp(sqrt(err), 2.0 * length(pixel), 12.0 * length(pixel));
    float bg = depth;
    for (int k = 0; k < 8; ++k)
    {
        float a = float(k) * 0.785398;
        for (int r = 1; r <= 2; ++r)
        {
            float d = source_depth(side, src + vec2(cos(a), sin(a)) * reach * (0.5 * float(r)));
            if (farther(d, bg))
            {
                bg = d;
            }
        }
    }
    return settle_at(side, want, bg);
}

void main()
{
    bool both = (qlWarpAvatarCount & WARP_BOTH) != 0;
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 shown = texelFetch(qlWarpFrameMap, px, 0);
    vec4 world = both ? texelFetch(qlWarpNextWorldMap, px, 0) : texelFetch(qlWarpWorldMap, px, 0);

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

    // The earlier frame (the only one, from one frame).
    vec2 src_a = want;
    float depth_a, dist_a;
    float err_a = settle(0, want, src_a, depth_a, dist_a);
    int rule_a = g_rule;
    bool ok_a = err_a <= settled;

    // The later frame, from two.
    vec2 src_b = want;
    float depth_b = 0.0, dist_b = 1e9, err_b = 1e9;
    int rule_b = 0;
    bool ok_b = false;
    if (both)
    {
        err_b = settle(1, want, src_b, depth_b, dist_b);
        rule_b = g_rule;
        ok_b = err_b <= settled;
    }

    int rule = 0;
    bool filled = false;
    bool blended = false;
    if (ok_a && ok_b)
    {
        // Both saw a surface here. The same surface, each a little off: the nearer frame in time counts for more.
        // Different surfaces: the nearer hides the farther, which the other frame saw past an edge.
        // The same surface, each a little off, and alike in colour: the nearer frame in time counts for more. Two
        // colours far apart there mean one of the frames found the wrong thing (a see-through surface holds the depth
        // of what is behind it): blended, both would show, a double image, so the newer is taken alone.
        float closest = min(dist_a, dist_b);
        vec4 col_a = source_world(0, src_a);
        vec4 col_b = source_world(1, src_b);
        vec3 apart = abs(col_a.rgb - col_b.rgb);
        bool alike = max(max(apart.r, apart.g), apart.b) < 0.12;
        if (abs(dist_a - dist_b) <= closest * 0.03 + 0.02)
        {
            frag_color = alike ? mix(col_a, col_b, qlWarpAlpha) : col_b;
            blended = alike;
            rule = max(rule_a, rule_b);
        }
        else if (dist_a < dist_b)
        {
            frag_color = source_world(0, src_a);
            rule = rule_a;
        }
        else
        {
            frag_color = source_world(1, src_b);
            rule = rule_b;
        }
    }
    else if (ok_b)
    {
        frag_color = source_world(1, src_b);
        rule = rule_b;
    }
    else if (ok_a)
    {
        frag_color = source_world(0, src_a);
        rule = rule_a;
    }
    else if ((qlWarpAvatarCount & WARP_FILL) != 0)
    {
        // No frame saw this: the background around it, from the newest frame.
        int side = both ? 1 : 0;
        vec2 src = side == 0 ? src_a : src_b;
        frag_color = source_world(side, fill(side, want, src, side == 0 ? depth_a : depth_b, side == 0 ? err_a : err_b, pixel));
        filled = true;
    }
    else
    {
        frag_color = both ? source_world(1, src_b) : source_world(0, src_a);
    }

    if ((qlWarpAvatarCount & WARP_DEBUG) != 0)
    {
        // Red filled, green moved with an avatar, blue held as hair; from two frames, yellow where only one saw it.
        vec3 tint = vec3(0.0);
        bool tinted = true;
        if (filled)
        {
            tint = vec3(1.0, 0.0, 0.0);
        }
        else if (rule == 1)
        {
            tint = vec3(0.0, 1.0, 0.0);
        }
        else if (rule == 2)
        {
            tint = vec3(0.0, 0.3, 1.0);
        }
        else if (both && ok_a != ok_b)
        {
            tint = vec3(1.0, 0.9, 0.0);
        }
        else
        {
            tinted = false;
        }
        if (tinted)
        {
            frag_color.rgb = mix(frag_color.rgb, tint, 0.5);
        }
    }
}
