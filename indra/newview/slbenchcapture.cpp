/**
 * @file slbenchcapture.cpp
 * @brief Render benchmark scene capture (#1304). See slbenchcapture.h, slbenchscene.h.
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

#include "slbenchcapture.h"

#include "hbxxh.h"
#include "llagent.h"
#include "llenvironment.h"
#include "llimagepng.h"
#include "llsdserialize.h"
#include "lltexturedata.h"
#include "lltimer.h"
#include "llversioninfo.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llworldclock.h"

#include <boost/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

using namespace SLBenchScene;

namespace
{
    constexpr U32 kSentinel = 0xFFFFFFFFu;

    template <typename T>
    void appendPod(std::vector<U8>& out, const T& v)
    {
        const U8* p = reinterpret_cast<const U8*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }

    void appendBytes(std::vector<U8>& out, const void* p, size_t n)
    {
        const U8* b = static_cast<const U8*>(p);
        out.insert(out.end(), b, b + n);
    }

    MoverRec moverFrom(U32 row, const LLCleanInstanceStore::Instance& in, U32 kind)
    {
        MoverRec m;
        std::memset(&m, 0, sizeof(m));
        m.row = row;
        m.kind = kind;
        std::memcpy(m.model, in.model_matrix, sizeof(m.model));
        std::memcpy(m.spin_axis, in.spin_axis, sizeof(m.spin_axis));
        std::memcpy(m.spin_pivot, in.spin_pivot, sizeof(m.spin_pivot));
        return m;
    }

    bool samePose(const MoverRec& a, const MoverRec& b)
    {
        return !std::memcmp(a.model, b.model, sizeof(a.model)) && !std::memcmp(a.spin_axis, b.spin_axis, sizeof(a.spin_axis))
            && !std::memcmp(a.spin_pivot, b.spin_pivot, 3 * sizeof(F32));
    }

    // One file being written: content hashed as it goes.
    struct Sink
    {
        FILE* f = nullptr;
        HBXXH64 hash;
        U64 bytes = 0;
        bool ok = true;

        explicit Sink(const std::string& path) : f(std::fopen(path.c_str(), "wb")) { ok = f != nullptr; }
        ~Sink() { if (f) std::fclose(f); }
        void put(const void* p, size_t n)
        {
            if (!ok || !n) return;
            ok = std::fwrite(p, 1, n, f) == n;
            hash.update(p, n);
            bytes += n;
        }
        bool close()
        {
            if (f) { ok = (std::fclose(f) == 0) && ok; f = nullptr; }
            return ok;
        }
    };

    bool writeBin(const std::string& dir, const char* name, U32 kind, U64 count, const void* payload, size_t bytes,
                  boost::json::object& files)
    {
        Sink s(dir + "/" + name);
        FileHeader h;
        std::memcpy(h.magic, kMagic, sizeof(h.magic));
        h.version = kFormatVersion;
        h.kind = kind;
        h.count = count;
        s.put(&h, sizeof(h));
        s.put(payload, bytes);
        const U64 n = s.bytes;
        const std::string digest = hex64(s.hash.digest());
        if (!s.close()) return false;
        boost::json::object fo;
        fo["bytes"] = n;
        fo["xxh64"] = digest;
        files[name] = std::move(fo);
        return true;
    }

    bool writeText(const std::string& path, const std::string& text)
    {
        const std::string tmp = path + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = (std::fclose(f) == 0) && ok;
        return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
    }
}

// A texture being gathered. Its CPU copy, when it has one, is held under a lease (#707);
// otherwise LL's loaded-callback path (the one Save As uses, #655) decodes it from the local
// cache at the level the GPU shows and hands the raw pixels to onTexLoaded.
struct SLBenchCapture::TexWait
{
    LLUUID id;
    LLPointer<LLViewerFetchedTexture> tex;
    LLVKCpuLease lease;
    S32 want = 0;                 // discard level asked for
    LLPointer<LLImageRaw> got;    // from the callback
    S32 got_discard = -1;
    bool done = false;
};

namespace
{
    // The capture a loaded callback reports to; generation-checked (a callback can outlive it).
    SLBenchCapture* sActiveCapture = nullptr;
    U32 sCaptureGen = 0;
    struct TexCb { U32 gen; size_t index; };
}

SLBenchCapture::SLBenchCapture() = default;

SLBenchCapture::~SLBenchCapture()
{
    // (SLBench::shutdown aborts a capture while the texture list still exists: no callbacks
    // are pending by the time this static goes)
    if (sActiveCapture == this) sActiveCapture = nullptr;
    if (mWriter.joinable()) mWriter.join();
}

// static
void SLBenchCapture::onTexLoaded(bool success, LLViewerFetchedTexture* /*src_vi*/, LLImageRaw* src, LLImageRaw* /*aux*/,
                                 S32 discard, bool final, void* userdata)
{
    TexCb* cb = static_cast<TexCb*>(userdata);
    SLBenchCapture* self = sActiveCapture;
    if (self && cb->gen == sCaptureGen && self->mPhase == TEXTURES && cb->index < self->mTexWaits.size())
    {
        TexWait& w = *self->mTexWaits[cb->index];
        if (success && src && src->getData() && src->getWidth() && src->getHeight() && !w.done
            && (w.got.isNull() || discard < w.got_discard))
        {
            try
            {
                w.got = new LLImageRaw(src->getData(), src->getWidth(), src->getHeight(), src->getComponents());
                w.got_discard = discard;
            }
            catch (const std::bad_alloc&)
            {
                w.got = nullptr;   // left out (counted missing); never fatal
            }
        }
        if (final || !success || (w.got.notNull() && w.got_discard <= w.want)) w.done = true;
    }
    if (final || !success) delete cb;
}

const char* SLBenchCapture::phaseName(Phase p)
{
    switch (p)
    {
        case IDLE: return "idle";
        case SNAPSHOT: return "snapshot";
        case RECORD: return "record";
        case TEXTURES: return "textures";
        case WRITING: return "writing";
        case DONE: return "done";
        case FAILED: return "failed";
    }
    return "?";
}

bool SLBenchCapture::start(const std::string& dir, F32 seconds, U64 budget_bytes, std::string& err)
{
    if (busy())
    {
        err = "a capture is already running";
        return false;
    }
    if (mWriter.joinable()) mWriter.join();
    if (!makeOutputDir(dir, err)) return false;
    {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(dir) / "manifest.json", ec))
        {
            err = dir + " already holds a capture";
            return false;
        }
    }
    mDir = dir;
    mSeconds = llclamp(seconds, 0.f, 600.f);
    mBudget = budget_bytes;
    mError.clear();
    mMessage.clear();
    mTruncated = false;
    mLastSample = -1;
    mData = std::make_unique<Data>();
    mCamera.clear();
    mRowGen.clear();
    mLast.clear();
    mGone.clear();
    mMoved.clear();
    mPendingSlots.clear();
    mSlotsDone.clear();
    mTexIds.clear();
    dropTextureWaits();
    sActiveCapture = this;
    mSkipRigged = mSkipHud = mSkipWorn = mSlotsMissing = mTexMissing = mLightsMax = 0;
    mWriteResult = 0;
    mPhase = SNAPSHOT;
    LL_INFOS("Bench") << "capture: starting, " << mSeconds << " s into " << mDir << " (budget "
                      << (mBudget >> 20) << " MB)" << LL_ENDL;
    return true;
}

void SLBenchCapture::dropTextureWaits()
{
    ++sCaptureGen;   // late callbacks report to nobody
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackList);   // (each pending one is called final + failed)
    mTexWaits.clear();   // leases released
}

void SLBenchCapture::fail(const std::string& why)
{
    mError = why;
    mPhase = FAILED;
    mData.reset();
    dropTextureWaits();
    LL_WARNS("Bench") << "capture failed: " << why << LL_ENDL;
}

void SLBenchCapture::abort(const std::string& why)
{
    if (busy() && mPhase != WRITING) fail("aborted: " + why);
}

void SLBenchCapture::shutdown()
{
    abort("viewer exiting");
    if (mWriter.joinable()) mWriter.join();
}

U64 SLBenchCapture::heldBytes() const
{
    if (!mData) return 0;
    const Data& d = *mData;
    U64 n = d.instances.size() * sizeof(InstanceRec) + d.groups.size() * sizeof(GroupRec) + d.slots.size()
          + d.materials.size() * sizeof(MaterialRec) + d.terrain.size() * sizeof(TerrainRec)
          + d.probes.size() * sizeof(LLVKProbe) + d.regions.size() * sizeof(LLVKProbeRegion)
          + d.lights.size() + d.movers.size();
    for (const Texture& t : d.textures)
        if (t.raw.notNull()) n += (U64)t.raw->getDataSize();
    return n;
}

void SLBenchCapture::noteTexture(const LLUUID& id)
{
    if (id.notNull()) mTexIds.insert(id);
}

void SLBenchCapture::noteTexture(const U8 bytes[16])
{
    LLUUID id;
    std::memcpy(id.mData, bytes, 16);
    noteTexture(id);
}

bool SLBenchCapture::captureSlot(U32 slot)
{
    LLCleanMeshAsset& mesh = LLCleanMeshAsset::instance();
    LLCleanMeshAsset::SlotInfo si{};
    if (!mesh.debugGetSlotInfo(slot, si) || si.index_count == 0) return true;   // nothing resident: nothing to keep
    U32 nv = 0, ni = 0, nw = 0;
    const LLCleanMeshAsset::QVertex* range = mesh.cpuSlotPacked(slot, nv);
    const U16* idx = mesh.cpuSlotIndices(slot, ni);
    if (!range || !idx || nv == 0 || ni == 0)
    {
        mesh.requestMirror(slot, "bench capture");
        return false;
    }
    const LLCleanMeshAsset::MirrorWeight* wts = mesh.cpuSlotWeights(slot, nw);
    const bool has_w = wts && nw == nv;
    SlotRec r{ slot, nv, ni, has_w ? 1u : 0u };
    std::vector<U8>& out = mData->slots;
    appendPod(out, r);
    appendBytes(out, range, (size_t)(nv + LLCleanMeshAsset::QV_HEADER) * sizeof(LLCleanMeshAsset::QVertex));
    appendBytes(out, idx, (size_t)ni * sizeof(U16));
    if (has_w) appendBytes(out, wts, (size_t)nv * sizeof(LLCleanMeshAsset::MirrorWeight));
    ++mData->slot_count;
    return true;
}

bool SLBenchCapture::snapshot(const LLVKWorldFrame& wf)
{
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    const U8* bytes = store.cpuInstanceBytes();
    const U32 n = store.cpuInstanceCount();
    if (!bytes || !n)
    {
        fail("the instance store is empty (not in world yet?)");
        return false;
    }
    const U32* gens = store.rowGenerations();
    F32 d[3];
    store.renderOffset(d);
    const F32 to_agent[3] = { -d[0], -d[1], -d[2] };
    mStartTime = wf.time;
    Data& data = *mData;

    std::set<U32> groups, mats, trecs;
    for (U32 id = 0; id < n; ++id)
    {
        LLCleanInstanceStore::Instance in;
        std::memcpy(&in, bytes + (size_t)id * sizeof(in), sizeof(in));
        if (in.lod_group_id == kSentinel) continue;
        if (in.flags & kFlagRigged) { ++mSkipRigged; continue; }
        if (in.flags & kFlagHud) { ++mSkipHud; continue; }
        if (in.flags & kFlagWorn) { ++mSkipWorn; continue; }
        LLCleanInstanceStore::translateRow(in, to_agent);   // store frame -> agent frame (#568)
        in.flags &= ~LLCleanInstanceStore::REZFX_FLAGS;     // effect-manager slots mean nothing on replay
        if (in.spin_axis[0] != 0.f || in.spin_axis[1] != 0.f || in.spin_axis[2] != 0.f)
            in.spin_axis[3] -= (F32)mStartTime;             // spin t0 relative to the capture start
        InstanceRec rec;
        rec.row = id;
        rec.active = 0;
        rec.inst = in;
        data.instances.push_back(rec);
        mRowGen[id] = gens ? gens[id] : 0u;
        mLast[id] = moverFrom(id, in, MOVER_POSE);
        groups.insert(in.lod_group_id);
        mats.insert(in.material_id);
        if (in.flags & kFlagTerrain) trecs.insert(in.skin_id);
    }
    if (data.instances.empty())
    {
        fail("no static scene rows to capture");
        return false;
    }

    // LOD groups and their geometry.
    const LLCleanMeshAsset::LodGroup* lg = reinterpret_cast<const LLCleanMeshAsset::LodGroup*>(
        LLCleanMeshAsset::instance().cpuLodGroupBytes());
    const U32 ng = LLCleanMeshAsset::instance().cpuLodGroupCount();
    std::set<U32> slots;
    for (U32 gid : groups)
    {
        if (!lg || gid >= ng) continue;
        GroupRec g;
        g.gid = gid;
        for (S32 l = 0; l < LLCleanMeshAsset::NUM_LOD; ++l)
        {
            g.slot[l] = lg[gid].slot[l];
            if (g.slot[l] != LLCleanMeshAsset::LOD_SENTINEL) slots.insert(g.slot[l]);
        }
        data.groups.push_back(g);
    }
    for (U32 s : slots)
    {
        if (captureSlot(s)) mSlotsDone.insert(s);
        else mPendingSlots.insert(s);
        if (heldBytes() > mBudget)
        {
            fail("the scene geometry alone exceeds the capture budget (BenchCaptureBudgetMB)");
            return false;
        }
    }

    // Terrain records (their materials join the material set).
    if (!trecs.empty())
    {
        const U8* tb = LLCleanMaterialStore::instance().cpuTerrainBytes();
        const U32 nt = LLCleanMaterialStore::instance().cpuTerrainCount();
        for (U32 t : trecs)
        {
            if (!tb || t >= nt) continue;
            TerrainRec tr{};
            tr.idx = t;
            std::memcpy(&tr.rec, tb + (size_t)t * sizeof(LLCleanMaterialStore::TerrainRecord), sizeof(tr.rec));
            for (U32 m : tr.rec.mat) mats.insert(m);
            data.terrain.push_back(tr);
        }
    }

    // Materials (texture UUIDs noted for the texture phase).
    for (U32 m : mats)
    {
        LLCleanMaterialStore::MaterialDesc desc;
        if (!LLCleanMaterialStore::instance().descFor(m, desc)) continue;
        data.materials.push_back(toRec(m, desc));
        noteTexture(desc.base_color_id);
        noteTexture(desc.normal_id);
        noteTexture(desc.mr_id);
        noteTexture(desc.emissive_id);
    }

    // Environment, probes and their textures. The settings objects too: some renderer modules
    // (lens flare, stars, physical sky, fog) read LLEnvironment directly.
    if (LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky())
    {
        std::ostringstream os;
        LLSDSerialize::toXML(sky->getSettings(), os);
        data.sky_xml = os.str();
    }
    if (LLSettingsWater::ptr_t water = LLEnvironment::instance().getCurrentWater())
    {
        std::ostringstream os;
        LLSDSerialize::toXML(water->getSettings(), os);
        data.water_xml = os.str();
    }
    envFromFrame(wf, data.env);
    data.probes = wf.probes;
    data.regions = wf.probe_regions;
    for (const auto& t : wf.water_normal) noteTexture(t);
    for (const auto& t : wf.cloud_noise) noteTexture(t);
    for (const auto& t : wf.sky_tex) noteTexture(t);

    LL_INFOS("Bench") << "capture: snapshot " << data.instances.size() << " rows (" << mSkipRigged << " rigged, "
                      << mSkipHud << " HUD, " << mSkipWorn << " worn skipped), " << data.groups.size() << " LOD groups, "
                      << data.slot_count << " slots (" << mPendingSlots.size() << " mirrors requested), "
                      << data.materials.size() << " materials, " << data.terrain.size() << " terrain records, "
                      << mTexIds.size() << " textures so far" << LL_ENDL;
    return true;
}

void SLBenchCapture::record(const LLVKWorldFrame& wf)
{
    const F64 t = wf.time - mStartTime;
    const S64 sample = (S64)std::floor(t * kTrackHz);
    if (sample <= mLastSample) return;
    mLastSample = sample;
    Data& data = *mData;

    // Released mirrors asked back at the snapshot.
    for (auto it = mPendingSlots.begin(); it != mPendingSlots.end();)
    {
        if (captureSlot(*it)) { mSlotsDone.insert(*it); it = mPendingSlots.erase(it); }
        else ++it;
    }

    // Lights, verbatim.
    {
        SampleHdr h{ (U32)sample, (U32)wf.lights.size() };
        appendPod(data.lights, h);
        if (!wf.lights.empty()) appendBytes(data.lights, wf.lights.data(), wf.lights.size() * sizeof(LLVKLight));
        ++data.light_samples;
        mLightsMax = llmax(mLightsMax, (U32)wf.lights.size());
        for (const LLVKLight& l : wf.lights) noteTexture(l.tex_uuid);
    }

    // Movers: captured rows whose pose changed since their last recorded one, or that went away.
    {
        LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
        const U8* bytes = store.cpuInstanceBytes();
        const U32 n = store.cpuInstanceCount();
        const U32* gens = store.rowGenerations();
        F32 d[3];
        store.renderOffset(d);
        const F32 to_agent[3] = { -d[0], -d[1], -d[2] };
        std::vector<MoverRec> changed;
        for (const auto& kv : mRowGen)
        {
            const U32 id = kv.first;
            if (mGone.count(id)) continue;
            const bool same_row = bytes && id < n && (!gens || gens[id] == kv.second);
            LLCleanInstanceStore::Instance in;
            if (same_row) std::memcpy(&in, bytes + (size_t)id * sizeof(in), sizeof(in));
            if (!same_row || in.lod_group_id == kSentinel)
            {
                MoverRec gone = mLast[id];
                gone.kind = MOVER_GONE;
                changed.push_back(gone);
                mGone.insert(id);
                continue;
            }
            LLCleanInstanceStore::translateRow(in, to_agent);
            if (in.spin_axis[0] != 0.f || in.spin_axis[1] != 0.f || in.spin_axis[2] != 0.f)
                in.spin_axis[3] -= (F32)mStartTime;
            const MoverRec m = moverFrom(id, in, MOVER_POSE);
            MoverRec& last = mLast[id];
            if (!samePose(m, last))
            {
                changed.push_back(m);
                last = m;
                mMoved.insert(id);
            }
        }
        SampleHdr h{ (U32)sample, (U32)changed.size() };
        appendPod(data.movers, h);
        if (!changed.empty()) appendBytes(data.movers, changed.data(), changed.size() * sizeof(MoverRec));
        ++data.mover_samples;
    }

    // Camera, as a path key.
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        SLBenchPath::Key k;
        k.t = (F64)sample / kTrackHz;
        k.pos = cam->getOrigin();
        k.rot = cam->getQuaternion();
        k.fov = cam->getView();
        k.segment = "capture";
        mCamera.append(k);
    }

    if (heldBytes() > mBudget)
    {
        mTruncated = true;
        LL_WARNS("Bench") << "capture: tracks reached the budget at " << t << " s; stopping the recording there" << LL_ENDL;
        beginTextures();
        return;
    }
    if (t >= mSeconds) beginTextures();
}

void SLBenchCapture::beginTextures()
{
    mPhase = TEXTURES;
    mTexStart = LLTimer::getTotalSeconds();
    for (const LLUUID& id : mTexIds)
    {
        auto w = std::make_unique<TexWait>();
        w->id = id;
        w->tex = LLViewerTextureManager::findFetchedTexture(id, TEX_LIST_STANDARD);
        LLTextureData* gl = w->tex.notNull() ? w->tex->getTextureData() : nullptr;
        if (!gl)
        {
            w->done = true;   // not loaded in this session: missing
        }
        else if (gl->getVKCpuData())
        {
            w->lease.hold(gl, LLTextureData::VKLEASE_CALLBACK);   // the copy stays until written
            w->done = true;
        }
        else
        {
            // No CPU copy (dropped, or served straight from the GPU-ready disk cache): ask for
            // a decode at the level the texture shows now (the budget bounds what is kept).
            w->want = llclamp(w->tex->getDiscardLevel(), 0, (S32)MAX_DISCARD_LEVEL);
            w->tex->forceToSaveRawImage(w->want);
            w->tex->setLoadedCallback(&SLBenchCapture::onTexLoaded, w->want, true, false,
                                      new TexCb{ sCaptureGen, mTexWaits.size() }, &mCallbackList);
        }
        mTexWaits.push_back(std::move(w));
    }
    LL_INFOS("Bench") << "capture: recorded " << mData->light_samples << " samples; gathering " << mTexIds.size()
                      << " textures" << LL_ENDL;
}

void SLBenchCapture::finishTextures()
{
    Data& data = *mData;
    for (const auto& w : mTexWaits)
    {
        LLTextureData* gl = w->tex.notNull() ? w->tex->getTextureData() : nullptr;
        const LLImageRaw* raw = w->got.notNull() ? w->got.get() : (gl ? gl->getVKCpuData() : nullptr);
        if (!raw && w->tex.notNull() && w->tex->hasSavedRawImage()) raw = w->tex->getSavedRawImage();
        if (!raw || !raw->getData() || raw->getWidth() == 0 || raw->getHeight() == 0)
        {
            ++mTexMissing;
            continue;
        }
        if (heldBytes() + (U64)raw->getDataSize() > mBudget)
        {
            ++mTexMissing;
            mTruncated = true;
            continue;
        }
        Texture t;
        t.id = w->id;
        t.discard = w->got.notNull() ? w->got_discard : w->tex->getDiscardLevel();
        t.raw = new LLImageRaw(raw->getData(), raw->getWidth(), raw->getHeight(), raw->getComponents());
        if (t.raw.isNull() || !t.raw->getData())
        {
            ++mTexMissing;   // allocation failed: left out, never fatal
            continue;
        }
        data.textures.push_back(std::move(t));
    }
    dropTextureWaits();
    for (InstanceRec& r : data.instances) r.active = mMoved.count(r.row) ? 1u : 0u;

    // Manifest head (the writer adds "files").
    boost::json::object m;
    m["format"] = "slipstream-bench-capture";
    m["version"] = kFormatVersion;
    m["created"] = stamp();
    m["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
    if (LLViewerRegion* region = gAgent.getRegion()) m["region"] = region->getName();
    m["seconds"] = (double)(mLastSample + 1) / kTrackHz;
    m["track_hz"] = kTrackHz;
    m["truncated"] = mTruncated;
    boost::json::object c;
    c["instances"] = data.instances.size();
    c["skipped_rigged"] = mSkipRigged;
    c["skipped_hud"] = mSkipHud;
    c["skipped_worn"] = mSkipWorn;
    c["groups"] = data.groups.size();
    c["slots"] = data.slot_count;
    c["slots_missing"] = (U32)mPendingSlots.size();
    c["materials"] = data.materials.size();
    c["terrain_records"] = data.terrain.size();
    c["probes"] = data.probes.size();
    c["probe_regions"] = data.regions.size();
    c["light_samples"] = data.light_samples;
    c["lights_max"] = mLightsMax;
    c["mover_samples"] = data.mover_samples;
    c["textures"] = data.textures.size();
    c["textures_missing"] = mTexMissing;
    m["counts"] = std::move(c);
    m["missing"] = { "avatars (rigged rows, skins, joint and pose tracks)", "HUD and worn rigid attachments",
                     "particles", "objects that appear after the capture starts",
                     "environment changes during the capture (the first frame's sky and water are kept)",
                     "light-takeover face links" };
    boost::json::array tex;
    for (const Texture& t : data.textures)
    {
        boost::json::object to;
        to["id"] = t.id.asString();
        to["w"] = t.raw->getWidth();
        to["h"] = t.raw->getHeight();
        to["c"] = (S32)t.raw->getComponents();
        to["discard"] = t.discard;
        tex.push_back(std::move(to));
    }
    m["textures"] = std::move(tex);
    data.camera_json = mCamera.serialize();

    LL_INFOS("Bench") << "capture: writing " << (heldBytes() >> 20) << " MB (" << data.textures.size()
                      << " textures, " << mTexMissing << " missing)" << LL_ENDL;
    mPhase = WRITING;
    mWriteResult = 0;
    mWriter = std::thread(&SLBenchCapture::writeAll, mData.release(), mDir, boost::json::serialize(m), &mWriteResult,
                          &mWriteMessage);
}

void SLBenchCapture::worldFrame(const LLVKWorldFrame& wf)
{
    if (!wf.enabled || (mPhase != SNAPSHOT && mPhase != RECORD)) return;
    try
    {
        if (mPhase == SNAPSHOT)
        {
            if (snapshot(wf)) mPhase = RECORD;
            return;
        }
        record(wf);
    }
    catch (const std::bad_alloc&)
    {
        fail("out of memory");   // allocation failure: the capture is dropped, the viewer goes on
    }
}

void SLBenchCapture::tick()
{
    if (mPhase == TEXTURES)
    {
        bool all = true;
        for (const auto& w : mTexWaits)
        {
            if (w->done) continue;
            LLTextureData* gl = w->tex.notNull() ? w->tex->getTextureData() : nullptr;
            if (!gl || (gl->getVKCpuData() && gl->getVKCpuData()->getData()))
            {
                if (gl) w->lease.hold(gl, LLTextureData::VKLEASE_CALLBACK);   // a copy arrived another way
                w->done = true;
            }
            else
            {
                all = false;
            }
        }
        if (all || LLTimer::getTotalSeconds() - mTexStart > kTextureWaitSec)
        {
            try { finishTextures(); }
            catch (const std::bad_alloc&) { fail("out of memory while gathering textures"); }
        }
        return;
    }
    if (mPhase == WRITING && mWriteResult.load() != 0)
    {
        if (mWriter.joinable()) mWriter.join();
        if (mWriteResult.load() > 0)
        {
            mPhase = DONE;
            mMessage = mWriteMessage;
            LL_INFOS("Bench") << "capture: done, " << mDir << " (" << mWriteMessage << ")" << LL_ENDL;
        }
        else
        {
            fail(mWriteMessage);
        }
    }
}

void SLBenchCapture::writeAll(Data* data_ptr, std::string dir, std::string manifest_head, std::atomic<int>* result,
                              std::string* message)
{
    std::unique_ptr<Data> data(data_ptr);
    boost::json::object files;
    U64 total = 0;
    auto fail = [&](const std::string& why) { *message = why; result->store(-1); };

    const Data& d = *data;
    std::vector<U8> env;
    appendPod(env, d.env);
    if (!d.probes.empty()) appendBytes(env, d.probes.data(), d.probes.size() * sizeof(LLVKProbe));
    if (!d.regions.empty()) appendBytes(env, d.regions.data(), d.regions.size() * sizeof(LLVKProbeRegion));

    if (!writeBin(dir, "instances.bin", KIND_INSTANCES, d.instances.size(), d.instances.data(),
                  d.instances.size() * sizeof(InstanceRec), files)
        || !writeBin(dir, "groups.bin", KIND_GROUPS, d.groups.size(), d.groups.data(), d.groups.size() * sizeof(GroupRec), files)
        || !writeBin(dir, "slots.bin", KIND_SLOTS, d.slot_count, d.slots.data(), d.slots.size(), files)
        || !writeBin(dir, "materials.bin", KIND_MATERIALS, d.materials.size(), d.materials.data(),
                     d.materials.size() * sizeof(MaterialRec), files)
        || !writeBin(dir, "terrain.bin", KIND_TERRAIN, d.terrain.size(), d.terrain.data(), d.terrain.size() * sizeof(TerrainRec), files)
        || !writeBin(dir, "env.bin", KIND_ENV, 1, env.data(), env.size(), files)
        || !writeBin(dir, "lights.bin", KIND_LIGHTS, d.light_samples, d.lights.data(), d.lights.size(), files)
        || !writeBin(dir, "movers.bin", KIND_MOVERS, d.mover_samples, d.movers.data(), d.movers.size(), files))
    {
        fail("cannot write the capture files in " + dir + " (disk full?)");
        return;
    }
    const std::pair<const char*, const std::string*> texts[] = { { "camera.json", &d.camera_json },
                                                                 { "sky.xml", &d.sky_xml },
                                                                 { "water.xml", &d.water_xml } };
    for (const auto& text : texts)
    {
        if (text.second->empty()) continue;
        if (!writeText(dir + "/" + text.first, *text.second))
        {
            fail(std::string("cannot write ") + text.first);
            return;
        }
        boost::json::object fo;
        fo["bytes"] = text.second->size();
        fo["xxh64"] = fileDigest(dir + "/" + text.first);
        files[text.first] = std::move(fo);
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir) / "textures", ec);
    U32 tex_written = 0;
    for (const Texture& t : d.textures)
    {
        const std::string rel = "textures/" + t.id.asString() + ".png";
        const std::string path = dir + "/" + rel;
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->encode(t.raw.get(), 0.f) || !png->save(path)) continue;   // left out (manifest has no entry)
        boost::json::object fo;
        fo["bytes"] = (U64)png->getDataSize();
        fo["xxh64"] = fileDigest(path);
        files[rel] = std::move(fo);
        ++tex_written;
    }

    boost::system::error_code jec;
    boost::json::value head = boost::json::parse(manifest_head, jec);
    if (jec || !head.is_object())
    {
        fail("internal: bad manifest head");
        return;
    }
    for (const auto& kv : files)
        if (kv.value().is_object()) total += kv.value().as_object().at("bytes").to_number<U64>();
    head.as_object()["files"] = std::move(files);
    head.as_object()["bytes"] = total;
    // The manifest goes last: a directory without one is an incomplete capture.
    if (!writeText(dir + "/manifest.json", boost::json::serialize(head)))
    {
        fail("cannot write manifest.json");
        return;
    }
    *message = llformat("%.1f MB, %u textures", (double)total / (1024.0 * 1024.0), tex_written);
    result->store(1);
}

LLSD SLBenchCapture::status() const
{
    LLSD r;
    r["phase"] = phaseName(mPhase);
    r["dir"] = mDir;
    if (!mError.empty()) r["error"] = mError;
    if (!mMessage.empty()) r["result"] = mMessage;
    if (mPhase == TEXTURES)
    {
        S32 pending = 0;
        for (const auto& w : mTexWaits) pending += w->done ? 0 : 1;
        r["textures_pending"] = pending;
    }
    if (mData) r["held_mb"] = (S32)(heldBytes() >> 20);
    return r;
}
