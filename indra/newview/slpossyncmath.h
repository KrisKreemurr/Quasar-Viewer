/**
 * @file slpossyncmath.h
 * @brief Avatar position sync (#1417): the pure parts -- wire parsing, sequence order, the
 *        2 m bound, the bounded per-avatar table and the per-frame blend. No viewer types,
 *        so scripts/check/test_pos_sync.cpp compiles and runs it on its own.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_POS_SYNC_MATH_H
#define SL_POS_SYNC_MATH_H

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// Wire format (helper -> viewer, after the helper's own checks):
//   #slb:pos/1|<owner>|<seq>|<pos>|<vel>|<rot>|<flags>
// <owner> is the sim's llGetOwnerKey() of the attachment that spoke; it is the only identity
// there is. The fields after it never name anyone: parseFields() takes exactly five and
// rejects a line with more, so nothing in a body can say whose position it is.
//   <seq>   0..65535, the sender's counter (wraps)
//   <pos>   region-local render position, LSL vector syntax "<x, y, z>"
//   <vel>   region velocity, m/s
//   <rot>   the avatar's root rotation, LSL rotation syntax "<x, y, z, s>"
//   <flags> any of S (sitting), T (still), F (flying); may be empty
namespace SLPosSyncMath
{
constexpr double BOUND           = 2.0;     // m: a shared position never differs more from the sim's
constexpr double PREDICT_MAX     = 0.5;     // s: extrapolate a moving line at most this long
constexpr double EXPIRE_MOVING   = 3.0;     // s: three missed 1 s keep-alives
constexpr double EXPIRE_STILL    = 25.0;    // s: two missed 10 s keep-alives (and a half)
constexpr double STALE_STILL_SPEED = 0.5;   // m/s: the sim moves a "still" avatar this fast ...
constexpr double STALE_STILL_AGE = 0.75;    // s: ... this long after its last line: stale
constexpr double BLEND_IN_TAU    = 0.12;    // s: ease towards a shared position
constexpr double BLEND_OUT_TAU   = 0.3;     // s: ease back to the sim's placement
constexpr double ROT_MAX_COS_HALF = 0.70710678; // a shared rotation more than 90 degrees off is not shown
constexpr double MAX_SPEED       = 50.0;    // m/s: faster velocities are not trusted
constexpr double RESYNC_AFTER    = 10.0;    // s: a sender quiet this long may restart its counter
constexpr size_t MAX_ENTRIES     = 32;      // avatars followed at once

enum : uint8_t
{
    FLAG_SIT   = 1,
    FLAG_STILL = 2,
    FLAG_FLY   = 4,
};

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double ax, double ay, double az) : x(ax), y(ay), z(az) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Quat
{
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

struct Line
{
    uint16_t seq = 0;
    Vec3 pos;       // region-local
    Vec3 vel;
    Quat rot;
    uint8_t flags = 0;
};

inline double expiryFor(uint8_t flags)
{
    return (flags & FLAG_STILL) ? EXPIRE_STILL : EXPIRE_MOVING;
}

// a is newer than b (serial number arithmetic, the counter wraps at 65536)
inline bool seqNewer(uint16_t a, uint16_t b)
{
    return a != b && (uint16_t)(a - b) < 0x8000;
}

// The shared position may be shown: within BOUND of where the sim puts the avatar.
inline bool withinBound(const Vec3& shared, const Vec3& sim)
{
    const Vec3 d = shared - sim;
    return d.finite() && d.length() <= BOUND;
}

// Fixed-point text ("-12.345") that never follows the user's locale: the helper's LSL casts
// and parseTuple both expect a decimal point.
inline std::string formatFixed(double v, int decimals)
{
    if (!std::isfinite(v))
        v = 0.0;
    long long scale = 1;
    for (int i = 0; i < decimals; ++i)
        scale *= 10;
    long long q = std::llround(v * (double)scale);
    std::string out;
    if (q < 0)
    {
        out += '-';
        q = -q;
    }
    out += std::to_string(q / scale);
    if (decimals > 0)
    {
        const std::string frac = std::to_string(q % scale);
        out += '.';
        out.append((size_t)decimals - frac.size(), '0');
        out += frac;
    }
    return out;
}

// "<a,b,c>" / "<a,b,c,d>" of formatFixed numbers.
inline std::string formatTuple(const double* v, int n, int decimals)
{
    std::string out = "<";
    for (int i = 0; i < n; ++i)
    {
        if (i)
            out += ',';
        out += formatFixed(v[i], decimals);
    }
    return out + ">";
}

// "<a, b, c>" or "<a, b, c, d>" (LSL syntax, spaces optional) into n numbers.
inline bool parseTuple(const std::string& s, double* out, int n)
{
    if (s.size() < 5 || s.size() > 80 || s.front() != '<' || s.back() != '>')
        return false;
    const std::string body = s.substr(1, s.size() - 2);
    int got = 0;
    size_t start = 0;
    for (;;)
    {
        const size_t comma = body.find(',', start);
        if (got == n)
            return false;   // more numbers than asked for
        const std::string part = body.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // std::from_chars: never the user's locale (a decimal comma would split the tuple).
        const char* p = part.data();
        const char* end = p + part.size();
        while (p < end && *p == ' ')
            ++p;
        double v = 0.0;
        const std::from_chars_result r = std::from_chars(p, end, v);
        if (r.ec != std::errc() || r.ptr == p || !std::isfinite(v))
            return false;
        p = r.ptr;
        while (p < end && *p == ' ')
            ++p;
        if (p != end)
            return false;
        out[got++] = v;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return got == n;
}

// The five fields after the sim-given owner: "<seq>|<pos>|<vel>|<rot>|<flags>". Exactly five;
// anything else (a sixth field, an owner key smuggled into the body) is rejected whole.
inline bool parseFields(const std::string& fields, Line& out)
{
    std::array<std::string, 5> f;
    size_t start = 0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        const size_t bar = fields.find('|', start);
        if (i + 1 < f.size())
        {
            if (bar == std::string::npos)
                return false;
            f[i] = fields.substr(start, bar - start);
            start = bar + 1;
        }
        else
        {
            if (bar != std::string::npos)
                return false;
            f[i] = fields.substr(start);
        }
    }
    if (f[0].empty() || f[0].size() > 5 || f[0].find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long seq = std::strtoul(f[0].c_str(), nullptr, 10);
    if (seq > 65535)
        return false;
    double p[3], v[3], r[4];
    if (!parseTuple(f[1], p, 3) || !parseTuple(f[2], v, 3) || !parseTuple(f[3], r, 4))
        return false;
    if (f[4].size() > 3 || f[4].find_first_not_of("STF") != std::string::npos)
        return false;
    Line line;
    line.seq = (uint16_t)seq;
    line.pos = Vec3(p[0], p[1], p[2]);
    line.vel = Vec3(v[0], v[1], v[2]);
    if (line.vel.length() > MAX_SPEED)
        line.vel = line.vel * (MAX_SPEED / line.vel.length());
    const double n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (n < 0.5 || n > 1.5)
        return false;
    line.rot.x = r[0] / n;
    line.rot.y = r[1] / n;
    line.rot.z = r[2] / n;
    line.rot.w = r[3] / n;
    for (char c : f[4])
        line.flags |= (c == 'S') ? FLAG_SIT : (c == 'T') ? FLAG_STILL : FLAG_FLY;
    out = line;
    return true;
}

// One followed avatar, keyed by whatever the viewer uses for avatar ids. Fixed capacity: no
// growth with the crowd beyond MAX_ENTRIES.
template <class Key>
class Table
{
public:
    struct Entry
    {
        Key key{};
        bool used = false;
        uint16_t seq = 0;
        uint8_t flags = 0;
        double heard = 0.0;
    };

    // A line from key (as the sim identified it) with seq: true if it is to be used (a new
    // avatar with room, or newer than the last line). Full table: a newcomer is refused
    // until an entry expires; a stale-but-unexpired entry is never displaced by a stranger.
    bool accept(const Key& key, uint16_t seq, uint8_t flags, double now)
    {
        Entry* free_slot = nullptr;
        for (Entry& e : mEntries)
        {
            if (e.used && e.key == key)
            {
                if (!seqNewer(seq, e.seq) && now - e.heard < RESYNC_AFTER)
                    return false;   // repeated or out of order
                e.seq = seq;
                e.flags = flags;
                e.heard = now;
                return true;
            }
            if (!free_slot && (!e.used || now - e.heard > expiryFor(e.flags)))
                free_slot = &e;
        }
        if (!free_slot)
            return false;
        free_slot->key = key;
        free_slot->used = true;
        free_slot->seq = seq;
        free_slot->flags = flags;
        free_slot->heard = now;
        return true;
    }

    template <class F>
    void expire(double now, F on_drop)
    {
        for (Entry& e : mEntries)
        {
            if (e.used && now - e.heard > expiryFor(e.flags))
            {
                e.used = false;
                on_drop(e.key);
            }
        }
    }

    void erase(const Key& key)
    {
        for (Entry& e : mEntries)
        {
            if (e.used && e.key == key)
                e.used = false;
        }
    }

    bool contains(const Key& key) const
    {
        for (const Entry& e : mEntries)
        {
            if (e.used && e.key == key)
                return true;
        }
        return false;
    }

    size_t size() const
    {
        size_t n = 0;
        for (const Entry& e : mEntries)
            n += e.used ? 1 : 0;
        return n;
    }

    template <class F>
    void forEach(F f) const
    {
        for (const Entry& e : mEntries)
        {
            if (e.used)
                f(e.key);
        }
    }

private:
    std::array<Entry, MAX_ENTRIES> mEntries{};
};

// The per-avatar state a receiver keeps for drawing: the last accepted line (in a global
// frame the caller chooses) and what is shown now. step() runs every frame for an avatar
// that is busy(); the offset it returns is added to the sim-driven root position.
struct View
{
    // last accepted line
    bool hasTarget = false;
    Vec3 pos;           // global
    Vec3 vel;
    Quat rot;
    uint8_t flags = 0;
    double heard = 0.0;
    // shown
    Vec3 offset;
    double weight = 0.0;    // 0 = sim placement, 1 = the shared one
    Quat shownRot;
    bool rotShown = false;  // the shared rotation is part of what is shown
    // the sim-driven position seen last frame (receive-time bound check)
    Vec3 lastSim;
    bool haveSim = false;
    unsigned rejected = 0;
    // Measurements (#1417 test plan), per frame while a shared position is followed:
    //   gap      |shared - sim|: how far the sim-driven placement is from where the sender
    //            draws itself (what other viewers show without position sync)
    //   residual |shared - drawn|: what is left after the blend
    double gapSum = 0.0, gapMax = 0.0, residSum = 0.0, residMax = 0.0;
    unsigned frames = 0;

    void takeStats(double& gap_sum, double& gap_max, double& resid_sum, double& resid_max, unsigned& n)
    {
        gap_sum += gapSum;
        resid_sum += residSum;
        gap_max = std::fmax(gap_max, gapMax);
        resid_max = std::fmax(resid_max, residMax);
        n += frames;
        gapSum = gapMax = residSum = residMax = 0.0;
        frames = 0;
    }

    bool busy() const { return hasTarget || weight > 0.0; }

    void setTarget(const Line& line, const Vec3& global_pos, double now)
    {
        hasTarget = true;
        pos = global_pos;
        vel = line.vel;
        rot = line.rot;
        flags = line.flags;
        heard = now;
    }

    void clearTarget() { hasTarget = false; }

    // Seated (the seat places the avatar): back to the sim at once; the sit is itself a jump.
    void reset()
    {
        hasTarget = false;
        offset = Vec3();
        weight = 0.0;
        rotShown = false;
    }

    // The offset to draw this frame. sim: where the sim-driven placement puts the avatar
    // now (same global frame as pos); sim_rot: its rotation; sim_vel: its velocity.
    Vec3 step(const Vec3& sim, const Quat& sim_rot, const Vec3& sim_vel, double now, double dt)
    {
        lastSim = sim;
        haveSim = true;
        Vec3 target;
        bool valid = false;
        if (hasTarget && !(flags & FLAG_SIT))
        {
            const double age = now - heard;
            if (age > expiryFor(flags))
            {
                hasTarget = false;
            }
            else
            {
                const double t = (flags & FLAG_STILL) ? 0.0 : std::fmin(std::fmax(age, 0.0), PREDICT_MAX);
                const Vec3 predicted = pos + vel * t;
                const bool stale_still = (flags & FLAG_STILL) && age > STALE_STILL_AGE
                                         && sim_vel.length() > STALE_STILL_SPEED;
                if (!withinBound(predicted, sim))
                {
                    // Rejected: this line no longer counts; the next one is checked afresh.
                    hasTarget = false;
                    ++rejected;
                }
                else if (!stale_still)
                {
                    target = predicted - sim;
                    valid = true;
                }
            }
        }
        else if (hasTarget)
        {
            hasTarget = (now - heard) <= expiryFor(flags);
        }

        const double tau = valid ? BLEND_IN_TAU : BLEND_OUT_TAU;
        const double a = (dt > 0.0) ? 1.0 - std::exp(-dt / tau) : 0.0;
        offset = offset + (target - offset) * a;
        weight = weight + ((valid ? 1.0 : 0.0) - weight) * a;
        // Never more than BOUND from the sim's placement, whatever the blend is doing.
        const double len = offset.length();
        if (!std::isfinite(len))
        {
            offset = Vec3();
            weight = 0.0;
        }
        else if (len > BOUND)
        {
            offset = offset * (BOUND / len);
        }
        if (!valid && weight < 0.002)
        {
            offset = Vec3();
            weight = 0.0;
        }
        if (valid)
        {
            const double gap = target.length();
            const double resid = (target - offset).length();
            gapSum += gap;
            residSum += resid;
            gapMax = std::fmax(gapMax, gap);
            residMax = std::fmax(residMax, resid);
            ++frames;
        }

        // Rotation: eased towards the shared one; not shown when it is far from the sim's.
        if (valid)
        {
            const double d = rot.x * sim_rot.x + rot.y * sim_rot.y + rot.z * sim_rot.z + rot.w * sim_rot.w;
            if (std::fabs(d) < ROT_MAX_COS_HALF)
                valid = false;
        }
        if (valid)
        {
            if (!rotShown)
            {
                shownRot = sim_rot;
                rotShown = true;
            }
            Quat q = rot;
            const double d = q.x * shownRot.x + q.y * shownRot.y + q.z * shownRot.z + q.w * shownRot.w;
            if (d < 0.0)
            {
                q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
            }
            const double b = (dt > 0.0) ? 1.0 - std::exp(-dt / BLEND_IN_TAU) : 0.0;
            shownRot.x += (q.x - shownRot.x) * b;
            shownRot.y += (q.y - shownRot.y) * b;
            shownRot.z += (q.z - shownRot.z) * b;
            shownRot.w += (q.w - shownRot.w) * b;
            const double n = std::sqrt(shownRot.x * shownRot.x + shownRot.y * shownRot.y
                                       + shownRot.z * shownRot.z + shownRot.w * shownRot.w);
            if (n > 1e-6)
            {
                shownRot.x /= n; shownRot.y /= n; shownRot.z /= n; shownRot.w /= n;
            }
            else
            {
                rotShown = false;
            }
        }
        else
        {
            // Hand the rotation back: the avatar's own orientation code eases from here.
            rotShown = false;
        }
        return offset;
    }
};

// Whether our own position goes out: the user's option, no RLV restriction that hides
// where we are, and a working bridge.
inline bool shouldShare(bool option_on, bool rlv_hides_position, bool bridge_ready)
{
    return option_on && !rlv_hides_position && bridge_ready;
}
} // namespace SLPosSyncMath

#endif // SL_POS_SYNC_MATH_H
