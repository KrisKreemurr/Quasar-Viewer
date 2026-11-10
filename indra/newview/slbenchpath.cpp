/**
 * @file slbenchpath.cpp
 * @brief Render benchmark camera path (#1304). See slbenchpath.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slbenchpath.h"

#include "llcoordframe.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
    // Quaternion math in double (x, y, z, w; Hamilton product), only for interpolation.
    struct Qd
    {
        double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
    };

    Qd toQd(const LLQuaternion& q) { return Qd{ q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW] }; }

    LLQuaternion fromQd(const Qd& q)
    {
        const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!(n > 1e-12)) return LLQuaternion();
        return LLQuaternion((F32)(q.x / n), (F32)(q.y / n), (F32)(q.z / n), (F32)(q.w / n));
    }

    Qd mul(const Qd& a, const Qd& b)
    {
        return Qd{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                   a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                   a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                   a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }

    Qd conj(const Qd& q) { return Qd{ -q.x, -q.y, -q.z, q.w }; }

    double dot(const Qd& a, const Qd& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

    // log of a unit quaternion: (axis * angle, 0)
    Qd qlog(const Qd& q)
    {
        const double w = std::clamp(q.w, -1.0, 1.0);
        const double theta = std::acos(w);
        const double s = std::sin(theta);
        if (std::fabs(s) < 1e-9) return Qd{ 0.0, 0.0, 0.0, 0.0 };
        const double k = theta / s;
        return Qd{ q.x * k, q.y * k, q.z * k, 0.0 };
    }

    // exp of a pure quaternion (v, 0)
    Qd qexp(const Qd& v)
    {
        const double theta = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (theta < 1e-12) return Qd{ 0.0, 0.0, 0.0, 1.0 };
        const double k = std::sin(theta) / theta;
        return Qd{ v.x * k, v.y * k, v.z * k, std::cos(theta) };
    }

    // Plain slerp (no hemisphere flip: squad needs the arcs it was built for).
    Qd slerp(const Qd& a, const Qd& b, double u)
    {
        const double c = std::clamp(dot(a, b), -1.0, 1.0);
        double wa = 1.0 - u, wb = u;
        if (std::fabs(c) < 0.9995)
        {
            const double th = std::acos(c);
            const double s = std::sin(th);
            wa = std::sin((1.0 - u) * th) / s;
            wb = std::sin(u * th) / s;
        }
        Qd r{ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
        const double n = std::sqrt(dot(r, r));
        if (n > 1e-12) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
        return r;
    }

    // Squad inner control point at key i (Shoemake).
    Qd squadInner(const Qd& prev, const Qd& cur, const Qd& next)
    {
        const Qd inv = conj(cur);
        const Qd a = qlog(mul(inv, next));
        const Qd b = qlog(mul(inv, prev));
        return mul(cur, qexp(Qd{ -(a.x + b.x) * 0.25, -(a.y + b.y) * 0.25, -(a.z + b.z) * 0.25, 0.0 }));
    }

    bool readVec3(const boost::json::value* v, LLVector3& out)
    {
        if (!v || !v->is_array() || v->as_array().size() != 3) return false;
        for (size_t i = 0; i < 3; ++i)
        {
            const boost::json::value& e = v->as_array()[i];
            if (!e.is_number()) return false;
            out.mV[i] = (F32)e.to_number<double>();
        }
        return out.isFinite();
    }

    const std::string kDefaultSegment = "default";
    const std::string kNoSegment;
}

LLQuaternion SLBenchPath::lookRotation(const LLVector3& pos, const LLVector3& focus)
{
    LLCoordFrame f;
    f.lookAt(pos, focus);
    return f.getQuaternion();
}

bool SLBenchPath::append(Key k)
{
    if (!mKeys.empty() && k.t < mKeys.back().t) return false;
    if (!(k.fov > 0.01f && k.fov < 3.13f)) k.fov = kDefaultFov;
    if (k.segment.empty()) k.segment = mKeys.empty() ? kDefaultSegment : mKeys.back().segment;
    k.rot.normalize();
    mKeys.push_back(std::move(k));
    fixHemispheres();
    return true;
}

void SLBenchPath::fixHemispheres()
{
    for (size_t i = 1; i < mKeys.size(); ++i)
    {
        LLQuaternion& q = mKeys[i].rot;
        if (dot(toQd(mKeys[i - 1].rot), toQd(q)) < 0.0)
            q = LLQuaternion(-q.mQ[VX], -q.mQ[VY], -q.mQ[VZ], -q.mQ[VW]);
    }
}

size_t SLBenchPath::keyAt(F64 t) const
{
    if (mKeys.empty()) return 0;
    // upper_bound: first key with key.t > t
    auto it = std::upper_bound(mKeys.begin(), mKeys.end(), t, [](F64 v, const Key& k) { return v < k.t; });
    return it == mKeys.begin() ? 0 : (size_t)(it - mKeys.begin()) - 1;
}

const std::string& SLBenchPath::segmentAt(F64 t) const
{
    return mKeys.empty() ? kNoSegment : mKeys[keyAt(t)].segment;
}

std::vector<std::string> SLBenchPath::segmentNames() const
{
    std::vector<std::string> out;
    for (const Key& k : mKeys)
        if (std::find(out.begin(), out.end(), k.segment) == out.end()) out.push_back(k.segment);
    return out;
}

SLBenchPath::Pose SLBenchPath::sample(F64 t) const
{
    Pose p;
    const size_t n = mKeys.size();
    if (n == 0) return p;
    if (n == 1 || t <= mKeys.front().t)
    {
        p.pos = mKeys.front().pos; p.rot = mKeys.front().rot; p.fov = mKeys.front().fov;
        return p;
    }
    if (t >= mKeys.back().t)
    {
        p.pos = mKeys.back().pos; p.rot = mKeys.back().rot; p.fov = mKeys.back().fov;
        return p;
    }
    const size_t i = std::min(keyAt(t), n - 2);
    const Key& k0 = mKeys[i];
    const Key& k1 = mKeys[i + 1];
    const F64 dt = k1.t - k0.t;
    if (!(dt > 1e-9))
    {
        p.pos = k1.pos; p.rot = k1.rot; p.fov = k1.fov;
        return p;
    }
    const F64 u = std::clamp((t - k0.t) / dt, 0.0, 1.0);

    // Catmull-Rom tangents (per second) over non-uniform times; one-sided at the ends.
    auto tangent = [&](size_t j) -> LLVector3
    {
        const size_t a = j > 0 ? j - 1 : j;
        const size_t b = j + 1 < n ? j + 1 : j;
        const F64 span = mKeys[b].t - mKeys[a].t;
        if (!(span > 1e-9)) return LLVector3::zero;
        return (mKeys[b].pos - mKeys[a].pos) * (F32)(1.0 / span);
    };
    const LLVector3 m0 = tangent(i) * (F32)dt;
    const LLVector3 m1 = tangent(i + 1) * (F32)dt;
    const F64 u2 = u * u, u3 = u2 * u;
    const F32 h00 = (F32)(2.0 * u3 - 3.0 * u2 + 1.0);
    const F32 h10 = (F32)(u3 - 2.0 * u2 + u);
    const F32 h01 = (F32)(-2.0 * u3 + 3.0 * u2);
    const F32 h11 = (F32)(u3 - u2);
    p.pos = k0.pos * h00 + m0 * h10 + k1.pos * h01 + m1 * h11;

    // Squad over the hemisphere-fixed keys.
    const Qd q0 = toQd(k0.rot), q1 = toQd(k1.rot);
    const Qd s0 = (i > 0) ? squadInner(toQd(mKeys[i - 1].rot), q0, q1) : q0;
    const Qd s1 = (i + 2 < n) ? squadInner(q0, q1, toQd(mKeys[i + 2].rot)) : q1;
    p.rot = fromQd(slerp(slerp(q0, q1, u), slerp(s0, s1, u), 2.0 * u * (1.0 - u)));

    p.fov = (F32)(k0.fov + (k1.fov - k0.fov) * u);
    return p;
}

bool SLBenchPath::parse(const std::string& json, std::string& err)
{
    mKeys.clear();
    boost::system::error_code ec;
    const boost::json::value root = boost::json::parse(json, ec);
    if (ec || !root.is_object())
    {
        err = "not a JSON object";
        return false;
    }
    const boost::json::object& o = root.as_object();
    if (const boost::json::value* f = o.if_contains("format"); f && (!f->is_string() || f->as_string() != "slipstream-bench-path"))
    {
        err = "not a slipstream-bench-path file";
        return false;
    }
    if (const boost::json::value* v = o.if_contains("version"); v && (!v->is_number() || v->to_number<int>() > 1))
    {
        err = "unsupported path version";
        return false;
    }
    const boost::json::value* keys = o.if_contains("keys");
    if (!keys || !keys->is_array() || keys->as_array().empty())
    {
        err = "no keys";
        return false;
    }
    for (const boost::json::value& kv : keys->as_array())
    {
        if (!kv.is_object())
        {
            err = "key is not an object";
            return false;
        }
        const boost::json::object& ko = kv.as_object();
        Key k;
        const boost::json::value* t = ko.if_contains("t");
        if (!t || !t->is_number() || !readVec3(ko.if_contains("pos"), k.pos))
        {
            err = "key without t / pos";
            return false;
        }
        k.t = t->to_number<double>();
        if (const boost::json::value* r = ko.if_contains("rot"); r && r->is_array() && r->as_array().size() == 4)
        {
            const boost::json::array& a = r->as_array();
            for (size_t i = 0; i < 4; ++i)
                k.rot.mQ[i] = a[i].is_number() ? (F32)a[i].to_number<double>() : 0.f;
        }
        else
        {
            LLVector3 focus;
            if (!readVec3(ko.if_contains("focus"), focus))
            {
                err = "key without rot or focus";
                return false;
            }
            k.rot = lookRotation(k.pos, focus);
        }
        if (const boost::json::value* f = ko.if_contains("fov"); f && f->is_number()) k.fov = (F32)f->to_number<double>();
        if (const boost::json::value* s = ko.if_contains("segment"); s && s->is_string()) k.segment = std::string(s->as_string());
        if (const boost::json::value* s = ko.if_contains("still"); s && s->is_number())
            k.still = (U32)std::clamp(s->to_number<double>(), 0.0, 4096.0);
        if (!std::isfinite(k.t) || !append(std::move(k)))
        {
            err = "keys out of time order";
            mKeys.clear();
            return false;
        }
    }
    return true;
}

std::string SLBenchPath::serialize() const
{
    boost::json::array keys;
    std::string prev_segment;
    for (const Key& k : mKeys)
    {
        boost::json::object ko;
        ko["t"] = k.t;
        ko["pos"] = { k.pos.mV[VX], k.pos.mV[VY], k.pos.mV[VZ] };
        ko["rot"] = { k.rot.mQ[VX], k.rot.mQ[VY], k.rot.mQ[VZ], k.rot.mQ[VW] };
        ko["fov"] = k.fov;
        if (k.segment != prev_segment) ko["segment"] = k.segment;
        if (k.still) ko["still"] = k.still;
        prev_segment = k.segment;
        keys.push_back(std::move(ko));
    }
    boost::json::object root;
    root["format"] = "slipstream-bench-path";
    root["version"] = 1;
    root["keys"] = std::move(keys);
    return boost::json::serialize(root);
}

bool SLBenchPath::load(const std::string& file, std::string& err)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        err = "cannot open " + file;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parse(ss.str(), err))
    {
        err = file + ": " + err;
        return false;
    }
    return true;
}

bool SLBenchPath::save(const std::string& file, std::string& err) const
{
    const std::string tmp = file + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err = "cannot write " + tmp;
            return false;
        }
        out << serialize() << '\n';
        if (!out)
        {
            err = "write failed: " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), file.c_str()) != 0)
    {
        err = "cannot rename " + tmp;
        return false;
    }
    return true;
}
