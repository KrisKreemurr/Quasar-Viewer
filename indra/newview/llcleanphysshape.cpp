/**
 * @file llcleanphysshape.cpp
 * @brief Show Physics Shape When Editing (#1176): the selection's physics shapes for the
 *        Vulkan renderer. See llcleanphysshape.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanphysshape.h"

#include "llcleaninstancestore.h"
#include "llcleanmeshasset.h"
#include "llcleanphysshapemesh.h"
#include "llcleanqvertex.h"
#include "llconvexdecomposition.h"
#include "llmemorygovernor.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llphysicsshapebuilderutil.h"
#include "llprimitive.h"
#include "llselectmgr.h"
#include "lltimer.h"
#include "lltoolmgr.h"
#include "llviewercontrol.h"
#include "llvolumemgr.h"
#include "llvovolume.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <sstream>

static_assert(LLPhysShapeColor::kColors == LLVKPhysShapes::kColors, "colour count = the renderer's buckets");
static_assert(LLPhysShapeMesh::kMaxSlotVerts == LLCleanQV::kMaxSlotVerts, "slot vertex limit = the store's");
static_assert(sizeof(LLVKPhysShapeRow) == 16, "row = clean_phys_shape.comp's uvec4");

namespace
{
const LLVector3 kPlaceholderHalf(0.25f, 0.25f, 0.25f);   // LL's placeholder / magenta box

// A prim volume's identity for the shape key: its parameters at a detail level.
std::string volumeKey(const LLVolumeParams& params, S32 detail)
{
    std::ostringstream s;
    s << std::setprecision(9) << detail << ':' << params << ':' << params.getSculptID() << ':'
      << (U32)params.getSculptType();
    return s.str();
}
} // namespace

LLCleanPhysShapeStore& LLCleanPhysShapeStore::instance()
{
    static LLCleanPhysShapeStore s;
    return s;
}

// static
S32 LLCleanPhysShapeStore::physicsDetail(const LLVector3& scale)
{
    S32 detail = 1;
    const F32 avg = (scale.mV[0] + scale.mV[1] + scale.mV[2]) / 3.f;
    if (avg > 5.f) { ++detail; if (avg > 25.f) ++detail; }
    return detail;
}

// static
LLCleanPhysShapeStore::HullBuildResult LLCleanPhysShapeStore::buildPrimHull(LLVolume* phys) try
{
    // The volume is shared. Never replace an existing (or incomplete) hull here.
    if (!phys) return HullBuildResult::Unavailable;
    if (phys->mHullPoints && phys->mHullIndices) return HullBuildResult::Ready;
    if (phys->mHullPoints || phys->mHullIndices) return HullBuildResult::Unavailable;
    if (!LLConvexDecomposition::isFunctional() || !LLConvexDecomposition::getInstance()) return HullBuildResult::Unavailable;
    std::vector<LLVector3> pos;
    std::vector<U16> index;
    S32 index_offset = 0;
    for (S32 i = 0; i < phys->getNumVolumeFaces(); ++i)
    {
        const LLVolumeFace& face = phys->getVolumeFace(i);
        if (face.mNumVertices < 0 || face.mNumIndices < 0 || face.mNumIndices % 3 != 0
            || face.mNumVertices > 65535 - index_offset
            || (face.mNumVertices && !face.mPositions) || (face.mNumIndices && !face.mIndices)
            || (size_t)face.mNumIndices > (size_t)std::numeric_limits<S32>::max() - index.size())
            return HullBuildResult::Unavailable;
        for (S32 j = 0; j < face.mNumVertices; ++j) pos.emplace_back(face.mPositions[j].getF32ptr());
        for (S32 j = 0; j < face.mNumIndices; ++j)
        {
            if (face.mIndices[j] >= face.mNumVertices) return HullBuildResult::Unavailable;
            index.push_back((U16)(face.mIndices[j] + index_offset));
        }
        index_offset += face.mNumVertices;
    }
    if (pos.empty() || index.empty()) return HullBuildResult::Unavailable;
    LLCDMeshData mesh;
    mesh.mIndexBase = &index[0];
    mesh.mVertexBase = pos[0].mV;
    mesh.mNumVertices = (int)pos.size();
    mesh.mVertexStrideBytes = 12;
    mesh.mIndexStrideBytes = 6;
    mesh.mIndexType = LLCDMeshData::INT_16;
    mesh.mNumTriangles = (int)index.size() / 3;
    LLCDMeshData res{};
    if (LLConvexDecomposition::getInstance()->generateSingleHullMeshFromMesh(&mesh, &res) != LLCD_OK
        || res.mNumVertices <= 0 || res.mNumTriangles <= 0)
        return HullBuildResult::Unavailable;
    if (!res.mVertexBase || !res.mIndexBase || res.mNumVertices > 65536
        || res.mNumTriangles > std::numeric_limits<S32>::max() / 3
        || (res.mIndexType != LLCDMeshData::INT_16 && res.mIndexType != LLCDMeshData::INT_32))
        return HullBuildResult::Unavailable;
    const size_t index_width = res.mIndexType == LLCDMeshData::INT_16 ? sizeof(U16) : sizeof(U32);
    const size_t size_limit = std::numeric_limits<size_t>::max();
    // Check strides before converting them to unsigned; avoid overflow even on 32-bit hosts.
    auto valid_span = [size_limit](int count, int stride, size_t item_bytes) {
        return stride >= 0 && (size_t)stride >= item_bytes
            && (size_t)(count - 1) <= (size_limit - item_bytes) / (size_t)stride;
    };
    if (!valid_span(res.mNumVertices, res.mVertexStrideBytes, 3 * sizeof(F32))
        || !valid_span(res.mNumTriangles, res.mIndexStrideBytes, 3 * index_width)
        || (size_t)res.mNumVertices > size_limit / sizeof(LLVector4a))
        return HullBuildResult::Unavailable;
    const size_t index_count = (size_t)res.mNumTriangles * 3;
    if (index_count > (size_limit - 15) / sizeof(U16)) return HullBuildResult::Unavailable;
    const size_t index_bytes = (index_count * sizeof(U16) + 15) & ~size_t(15);
    auto free_aligned = [](void* ptr) { ll_aligned_free_16(ptr); };
    std::unique_ptr<LLVector4a, decltype(free_aligned)> points(
        (LLVector4a*)ll_aligned_malloc_16(sizeof(LLVector4a) * (size_t)res.mNumVertices), free_aligned);
    std::unique_ptr<U16, decltype(free_aligned)> indices(
        (U16*)ll_aligned_malloc_16(index_bytes), free_aligned);
    // Keep the author's combined null check and warning; RAII frees either
    // successful allocation, and NoMemory selects the store's bounded retry.
    if (!points || !indices)
    {
        LL_WARNS_ONCE("PhysicsShape") << "Out of memory for a prim physics hull of " << res.mNumVertices
                                      << " points; it is tried again later" << LL_ENDL;
        throw std::bad_alloc();
    }
    for (S32 i = 0; i < res.mNumVertices; ++i)
    {
        F32 xyz[3];
        std::memcpy(xyz, (const U8*)res.mVertexBase + (size_t)i * res.mVertexStrideBytes, sizeof(xyz));
        points.get()[i].load3(xyz);
    }
    for (S32 i = 0; i < res.mNumTriangles; ++i)
    {
        const U8* t = (const U8*)res.mIndexBase + (size_t)i * res.mIndexStrideBytes;
        for (S32 k = 0; k < 3; ++k)
        {
            U32 value;
            if (res.mIndexType == LLCDMeshData::INT_16)
            {
                U16 index16;
                std::memcpy(&index16, t + (size_t)k * index_width, sizeof(index16));
                value = index16;
            }
            else std::memcpy(&value, t + (size_t)k * index_width, sizeof(value));
            if (value >= (U32)res.mNumVertices) return HullBuildResult::Unavailable;
            indices.get()[(size_t)i * 3 + k] = (U16)value;
        }
    }
    // Publish only after both allocations and every source index have succeeded.
    phys->mHullPoints = points.release();
    phys->mHullIndices = indices.release();
    phys->mNumHullPoints = res.mNumVertices;
    phys->mNumHullIndices = (S32)index_count;
    return HullBuildResult::Ready;
}
catch (const std::bad_alloc&)
{
    // The legacy overlay caller has no exception boundary; it can safely draw its box.
    // The persistent store translates this result into its existing bounded retry policy.
    LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_MAIN_FRAME, 0, "physics hull build");
    return HullBuildResult::NoMemory;
}

void LLCleanPhysShapeStore::fillFrame(LLVKWorldFrame& wf)
{
    const F64 now = LLTimer::getTotalSeconds();
    if (!mSelConn.connected() && LLSelectMgr::instanceExists())
        mSelConn = LLSelectMgr::getInstance()->mUpdateSignal.connect([this]() { onSelectionUpdate(); });

    const bool active = activeNow();
    bool rebuild_now = active != mActive || (active && mDirty);
    if (active && !rebuild_now)
    {
        // A change the update signal did not report (some deselect paths only recentre the
        // selection), or something still loading / waiting.
        LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
        rebuild_now = sel.get() != mSelPtr || sel->getNumNodes() != mSelNodes || sel->getFirstObject() != mSelFirst
                   || (mPending && now >= mNextPoll);
    }
    if (rebuild_now)
    {
        mActive = active;
        mDirty = false;
        rebuild(now);
    }
    if (mIdleShapes && now >= mNextReap)
    {
        reap(now, 0);
        mNextReap = now + 1.0;
    }
    logStats(now);

    wf.phys.set = mSet;   // (the colours are fixed: the renderer's own table)
    wf.phys.gen = mGen;
}

bool LLCleanPhysShapeStore::activeNow() const
{
    static LLCachedControl<bool> s_show(gSavedSettings, "ShowPhysicsShapeInEdit", false);
    if (!s_show || !LLSelectMgr::instanceExists() || !LLToolMgr::instanceExists() || !LLToolMgr::getInstance()->inEdit())
        return false;
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    // HUD objects: no world depth to test them against.
    return sel.notNull() && !sel->isEmpty() && sel->getSelectType() != SELECT_TYPE_HUD;
}

void LLCleanPhysShapeStore::rebuild(F64 now)
{
    mPending = false;
    std::vector<LLVKPhysShapeRow> buckets[LLVKPhysShapes::kBuckets];
    U32 total = 0, dropped = 0;
    std::unordered_set<LLUUID> pose_rows;   // this rebuild's (the rest are released below)
    if (mActive)
    {
        LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
        mSelPtr = sel.get();
        mSelNodes = sel->getNumNodes();
        mSelFirst = sel->getFirstObject();
        LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
        const F64 deadline = (F64)LLTimer::getTotalSeconds() + kBuildBudgetSec;
        U32 builds = 0;
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
        {
            LLSelectNode* node = *it;
            LLViewerObject* o = node ? node->getObject() : nullptr;
            if (!o || o->isDead() || o->mDrawable.isNull() || o->getPCode() != LL_PCODE_VOLUME) continue;
            // HUDs: no world depth. glTF scene objects: node-local rows. Worn rigged mesh:
            // physics never applies to a worn attachment, and the shape would float at the
            // attach point.
            if (o->getRootEdit()->isHUDAttachment() || o->mGLTFAsset || (o->isAttachment() && o->isRiggedMesh()))
                continue;
            // The pose: the object's first registered face (every face carries the object's).
            U32 inst = 0;
            bool found = false;
            for (S32 te = 0; te < (S32)o->getNumTEs() && !found; ++te)
                found = store.findInstanceId(o->getID(), te, inst);
            if (!found) continue;
            // An animesh: its rows turn rigged (no world pose) while its control avatar plays,
            // so its shape always takes the object's own pose from a pose row.
            if (o->isAnimatedObject())
            {
                if (!store.findInstanceId(o->getID(), LLCleanInstanceStore::kPoseRowFace, inst))
                    inst = store.upsertPoseRow(o);
                if (inst == 0xFFFFFFFFu) continue;
                pose_rows.insert(o->getID());
            }
            Desc d;
            if (!describe((LLVOVolume*)o, d)) continue;
            mPending = mPending || d.pending;
            Shape* s = acquire(d, o->getID(), now, deadline, builds);
            bool magenta = false;
            if (s && s->state == ST_FAILED)
            {
                Desc box;
                placeholder(box, false);
                s = acquire(box, o->getID(), now, deadline, builds);
                magenta = true;
            }
            if (!s)
            {
                mPending = true;   // waiting for a retry (or built next frame: mDirty)
                continue;
            }
            if (s->state != ST_READY) continue;   // (empty, or the magenta box failed too)
            const F32 cost = o->getObjectCost();   // (asks the region when stale)
            mPending = mPending || o->isCostStale();
            const U32 color = (magenta || s->no_hull) ? LLPhysShapeColor::kNoHull : LLPhysShapeColor::costStep(cost);
            const U32 bucket = color + ((d.fill_only && !magenta) ? LLVKPhysShapes::kColors : 0u);
            for (U32 slot : s->slots)
            {
                if (total >= kRowCap)
                {
                    ++dropped;
                    continue;
                }
                LLVKPhysShapeRow row;
                row.instance_id = inst;
                row.slot_id = slot;
                buckets[bucket].push_back(row);
                ++total;
            }
        }
    }
    else
    {
        mSelPtr = nullptr;
        mSelNodes = -1;
        mSelFirst = nullptr;
    }
    // Pose rows no row names any more go (an object that died took its own).
    for (const LLUUID& id : mPoseRows)
        if (!pose_rows.count(id)) LLCleanInstanceStore::instance().releasePoseRow(id);
    mPoseRows.swap(pose_rows);

    auto set = std::make_shared<LLVKPhysShapes::Set>();
    std::vector<LLVKPhysShapeRow>& rows = set->rows;
    rows.reserve(total);
    for (U32 b = 0; b < LLVKPhysShapes::kBuckets; ++b)
    {
        set->first[b] = (U32)rows.size();
        set->n[b] = (U32)buckets[b].size();
        rows.insert(rows.end(), buckets[b].begin(), buckets[b].end());
    }
    // Unchanged (a poll that found nothing new): keep the set and its generation, so the
    // renderer uploads nothing, and poll less often.
    const bool same = mSet && mSet->rows.size() == rows.size()
        && (rows.empty() || !std::memcmp(mSet->rows.data(), rows.data(), rows.size() * sizeof(LLVKPhysShapeRow)))
        && !std::memcmp(set->first, mSet->first, sizeof(set->first)) && !std::memcmp(set->n, mSet->n, sizeof(set->n));
    if (same)
    {
        mPollSec = std::min(mPollSec * 2.0, kPendingPollMaxSec);
    }
    else
    {
        mSet = std::move(set);
        ++mGen;
        mPollSec = kPendingPollSec;
    }
    mNextPoll = now + mPollSec;
    if (dropped)
    {
        mRowsDropped += dropped;
        LL_WARNS_ONCE("VKPhysShape") << dropped << " physics-shape draws over the " << kRowCap
                                     << " cap not shown (a selection that large)" << LL_ENDL;
    }

    // References as of this rebuild; a shape nobody names any more starts its grace period.
    mIdleShapes = 0;
    for (auto& kv : mShapes)
    {
        Shape& s = kv.second;
        if (s.counting == 0 && s.refs > 0) s.idle_since = now;
        s.refs = s.counting;
        s.counting = 0;
        if (s.refs == 0) ++mIdleShapes;
    }
}

// static
void LLCleanPhysShapeStore::placeholder(Desc& d, bool pending)
{
    d.src = SRC_PLACEHOLDER;
    d.key = "placeholder";
    d.fill_only = false;
    d.pending = pending;
}

// LL renderPhysicsShape (llspatialpartition.cpp; Develop > Physics Shapes in pipeline.cpp):
// which shape an object shows. False: none (no physics shape, flexible, or a sculpt -- LL
// had no display for those).
bool LLCleanPhysShapeStore::describe(LLVOVolume* vol, Desc& d) const
{
    const U8 physics_type = vol->getPhysicsShapeType();
    if (physics_type == LLViewerObject::PHYSICS_SHAPE_NONE || vol->isFlexible() || !vol->getVolume())
        return false;
    const LLVolumeParams& params = vol->getVolume()->getParams();
    LLPhysicsVolumeParams physics_params(params, physics_type == LLViewerObject::PHYSICS_SHAPE_CONVEX_HULL);
    LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification spec;
    LLPhysicsShapeBuilderUtil::determinePhysicsShape(physics_params, vol->getScale(), spec);
    using Spec = LLPhysicsShapeBuilderUtil::PhysicsShapeSpecification;
    const LLUUID mesh_id = params.getSculptID();

    // A mesh's single base hull, made from its decomposition when there is one; LL's
    // placeholder box while that loads.
    auto baseHull = [&](LLModel::Decomposition* decomp, bool pending)
    {
        if (decomp && decomp->mBaseHullMesh.empty()) gMeshRepo.buildPhysicsMesh(*decomp);
        if (decomp && !decomp->mBaseHullMesh.empty())
        {
            d.src = SRC_MESH_BASE_HULL;
            d.mesh_id = mesh_id;
            d.key = "mesh-base-hull:" + mesh_id.asString();
            d.pending = pending;
        }
        else
        {
            placeholder(d, true);
        }
    };

    switch (spec.getType())
    {
    case Spec::USER_MESH:
    {
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(mesh_id);   // (requests it)
        if (!decomp)
        {
            placeholder(d, true);
        }
        else if (!decomp->mHull.empty())
        {
            d.src = SRC_MESH_HULLS;
            d.mesh_id = mesh_id;
            d.key = "mesh-hulls:" + mesh_id.asString();
        }
        else if (!decomp->mPhysicsShapeMesh.empty())
        {
            d.src = SRC_MESH_PHYSICS;
            d.mesh_id = mesh_id;
            d.key = "mesh-physics:" + mesh_id.asString();
        }
        else
        {
            gMeshRepo.fetchPhysicsShape(mesh_id);
            baseHull(decomp, true);   // until the physics shape arrives
        }
        return true;
    }
    case Spec::USER_CONVEX:
    case Spec::PRIM_CONVEX:
        if (vol->isMesh())
        {
            baseHull(gMeshRepo.getDecomposition(mesh_id), false);
            return true;
        }
        d.src = SRC_PRIM_HULL;
        d.params = params;
        d.detail = physicsDetail(vol->getScale());
        d.key = "prim-hull:" + volumeKey(params, d.detail);
        return true;
    case Spec::BOX:
    {
        // The spec's box in unit space: its size over the object's (LL: the box scaled by
        // the inverse of the volume's scale).
        const LLVector3 vscale = vol->getScale() * 2.f;
        const LLVector3 sc = spec.getScale();
        d.src = SRC_BOX;
        d.fill_only = true;
        d.center = spec.getCenter();
        d.half.set(sc.mV[0] / vscale.mV[0], sc.mV[1] / vscale.mV[1], sc.mV[2] / vscale.mV[2]);
        if (!d.center.isFinite() || !d.half.isFinite()) return false;
        d.key = llformat("box:%.4f,%.4f,%.4f:%.4f,%.4f,%.4f", d.center.mV[0], d.center.mV[1], d.center.mV[2],
                         d.half.mV[0], d.half.mV[1], d.half.mV[2]);
        return true;
    }
    case Spec::SPHERE:
    case Spec::CYLINDER:
        d.src = spec.getType() == Spec::SPHERE ? SRC_SPHERE : SRC_CYLINDER;
        d.fill_only = true;
        d.key = d.src == SRC_SPHERE ? "sphere" : "cylinder";
        return true;
    case Spec::PRIM_MESH:
        d.src = SRC_PRIM_MESH;
        d.params = params;
        d.detail = physicsDetail(vol->getScale());
        d.key = "prim-mesh:" + volumeKey(params, d.detail);
        return true;
    default:
        return false;
    }
}

bool LLCleanPhysShapeStore::buildMesh(const Desc& d, LLPhysShapeMesh& mesh, FailReason& hull_fail) const
{
    hull_fail = FAIL_COUNT;
    std::vector<LLVector3> pts;
    auto addVolume = [&](const LLVolume* vol)
    {
        for (S32 f = 0; vol && f < vol->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& face = vol->getVolumeFace(f);
            pts.resize(face.mNumVertices);
            for (S32 i = 0; i < face.mNumVertices; ++i) pts[i].set(face.mPositions[i].getF32ptr());
            mesh.addIndexed(pts.data(), (U32)pts.size(), face.mIndices, (U32)face.mNumIndices);
        }
    };
    LLVolumeMgr* vm = LLPrimitive::getVolumeManager();
    switch (d.src)
    {
    case SRC_PLACEHOLDER:
        mesh.addBox(LLVector3(0.f, 0.f, 0.f), kPlaceholderHalf);
        break;
    case SRC_BOX:
        mesh.addBox(d.center, d.half);
        break;
    case SRC_SPHERE:
    case SRC_CYLINDER:
    {
        LLVolumeParams vp;
        if (d.src == SRC_SPHERE) vp.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
        else                     vp.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
        vp.setBeginAndEndS(0.f, 1.f);
        vp.setBeginAndEndT(0.f, 1.f);
        vp.setRatio(1, 1);
        vp.setShear(0, 0);
        LLVolume* shape = vm ? vm->refVolume(vp, 3) : nullptr;
        addVolume(shape);
        if (shape) vm->unrefVolume(shape);
        break;
    }
    case SRC_MESH_HULLS:
    case SRC_MESH_PHYSICS:
    case SRC_MESH_BASE_HULL:
    {
        LLModel::Decomposition* decomp = gMeshRepo.getDecomposition(d.mesh_id);
        if (!decomp) return false;
        if (d.src == SRC_MESH_HULLS)
        {
            if (decomp->mMesh.empty()) gMeshRepo.buildPhysicsMesh(*decomp);
            for (const LLModel::PhysicsMesh& m : decomp->mMesh) mesh.addTriangles(m.mPositions);
        }
        else
        {
            mesh.addTriangles(d.src == SRC_MESH_PHYSICS ? decomp->mPhysicsShapeMesh.mPositions
                                                        : decomp->mBaseHullMesh.mPositions);
        }
        break;
    }
    case SRC_PRIM_HULL:
    {
        LLVolume* phys = vm ? vm->refVolume(d.params, d.detail) : nullptr;
        auto unref = [vm](LLVolume* volume) { if (volume) vm->unrefVolume(volume); };
        // Vector or C-allocation failure must also release the temporary volume reference.
        std::unique_ptr<LLVolume, decltype(unref)> volume_ref(phys, unref);
        if (phys && !phys->mHullPoints && buildPrimHull(phys) == HullBuildResult::NoMemory)
            throw std::bad_alloc();
        if (phys && phys->mHullPoints && phys->mHullIndices)
        {
            pts.resize(phys->mNumHullPoints);
            for (S32 i = 0; i < phys->mNumHullPoints; ++i) pts[i].set(phys->mHullPoints[i].getF32ptr());
            mesh.addIndexed(pts.data(), (U32)pts.size(), phys->mHullIndices, (U32)phys->mNumHullIndices);
        }
        else
        {
            // LL: the magenta box
            hull_fail = LLConvexDecomposition::isFunctional() && LLConvexDecomposition::getInstance()
                            ? FAIL_NO_HULL : FAIL_NO_LIBRARY;
            mesh.addBox(LLVector3(0.f, 0.f, 0.f), kPlaceholderHalf);
        }
        break;
    }
    case SRC_PRIM_MESH:
    {
        LLVolume* phys = vm ? vm->refVolume(d.params, d.detail) : nullptr;
        addVolume(phys);
        if (phys) vm->unrefVolume(phys);
        break;
    }
    }
    return !mesh.empty();
}

LLCleanPhysShapeStore::Shape* LLCleanPhysShapeStore::acquire(const Desc& d, const LLUUID& object_id, F64 now,
                                                              F64 deadline, U32& builds)
{
    auto it = mShapes.find(d.key);
    if (it == mShapes.end())
    {
        it = mShapes.emplace(d.key, Shape()).first;
        it->second.key.generate("slipstream-physshape:" + d.key);
        it->second.idle_since = now;
    }
    Shape& s = it->second;
    ++s.counting;   // wanted: kept (and its retry count) while the object stays selected
    if (s.state == ST_READY || s.state == ST_EMPTY || s.state == ST_FAILED) return &s;
    if (s.state == ST_RETRY && now < s.retry_at) return nullptr;
    // At least one build per rebuild, more within the frame's budget; the rest next frame.
    if (builds > 0 && (F64)LLTimer::getTotalSeconds() > deadline)
    {
        mDirty = true;
        return nullptr;
    }
    ++builds;

    std::vector<LLPhysShapeMesh::Chunk> chunks;
    FailReason hull_fail = FAIL_COUNT;
    bool out_of_memory = false;
    try
    {
        LLPhysShapeMesh mesh;
        if (buildMesh(d, mesh, hull_fail)) chunks = mesh.build(LLCleanQV::kMaxSlotVerts);
    }
    catch (const std::bad_alloc&)
    {
        LLMemoryGovernor::noteAllocFailure(LLMemoryGovernor::ALLOC_MAIN_FRAME, 0, "physics shape build");
        chunks.clear();
        out_of_memory = true;   // (retried like a refused capture)
    }
    if (chunks.empty() && !out_of_memory)
    {
        s.state = ST_EMPTY;   // nothing LL would draw (an empty decomposition)
        return &s;
    }
    FailReason why = FAIL_MEMORY;
    if (!out_of_memory && capture(s, chunks, now, why))
    {
        s.state = ST_READY;
        s.no_hull = hull_fail != FAIL_COUNT;
        s.attempts = 0;
        ++mBuilt;
        if (s.no_hull) noteFailure(d, object_id, hull_fail, now, std::string());
        return &s;
    }
    ++mRefused;
    // A shape over the whole budget never fits: no retries for it.
    if (why == FAIL_TOO_BIG || ++s.attempts > LLMemoryGovernor::retryLimit())
    {
        // Shown as the magenta box; the entry goes kGraceSec after its last row, so the shape
        // is tried again later.
        s.state = ST_FAILED;
        noteFailure(d, object_id, why, now,
                    llformat("%u attempts, %llu KB of %llu KB held; ", s.attempts, (unsigned long long)(mBytes / 1024),
                             (unsigned long long)(kBudgetBytes / 1024)) + LLMemoryGovernor::allocFailString());
        return &s;
    }
    s.state = ST_RETRY;
    s.retry_at = now + LLMemoryGovernor::retryDelaySec(s.attempts);
    return nullptr;
}

const char* LLCleanPhysShapeStore::failName(FailReason why)
{
    switch (why)
    {
    case FAIL_NO_LIBRARY: return "no convex decomposition library";
    case FAIL_NO_HULL:    return "no prim hull";
    case FAIL_TOO_BIG:    return "too big";
    case FAIL_BUDGET:     return "budget full";
    case FAIL_STORE:      return "store refused";
    case FAIL_MEMORY:     return "out of memory";
    default:              return "unknown";
    }
}

void LLCleanPhysShapeStore::noteFailure(const Desc& d, const LLUUID& object_id, FailReason why, F64 now,
                                        const std::string& detail)
{
    ++mFails[why];
    if (why == FAIL_NO_LIBRARY)
    {
        // Not the asset's doing, and the same for every prim hull: said once.
        LL_WARNS_ONCE("VKPhysShape") << "no convex decomposition library: prim hulls show as the magenta box" << LL_ENDL;
        return;
    }
    if (now >= mFailLogUntil)
    {
        if (mFailUnlogged > 0)
        {
            LL_WARNS("VKPhysShape") << mFailUnlogged << " more magenta-box physics shapes were not logged" << LL_ENDL;
        }
        mFailLogUntil = now + kFailLogWindowSec;
        mFailLogged = mFailUnlogged = 0;
    }
    if (mFailLogged >= kFailLogsPerWindow)
    {
        ++mFailUnlogged;
        return;
    }
    ++mFailLogged;
    LL_WARNS("VKPhysShape") << "magenta box (" << failName(why) << ") for physics shape '" << d.key << "' of object "
                            << object_id << (detail.empty() ? "" : ": ") << detail << LL_ENDL;
}

bool LLCleanPhysShapeStore::capture(Shape& s, const std::vector<LLPhysShapeMesh::Chunk>& chunks, F64 now,
                                    FailReason& why)
{
    U64 bytes = 0;
    for (const LLPhysShapeMesh::Chunk& c : chunks)
        bytes += (U64)(c.pos.size() + LLCleanQV::kHeaderVerts) * LLCleanQV::kStride + (U64)c.idx.size() * sizeof(U16);
    if (bytes > kBudgetBytes)
    {
        why = FAIL_TOO_BIG;   // (nothing to free for it)
        return false;
    }
    if (mBytes + bytes > kBudgetBytes) reap(now, bytes);
    if (mBytes + bytes > kBudgetBytes)
    {
        why = FAIL_BUDGET;
        return false;
    }
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    std::vector<LLCleanMeshAsset::MirrorVertex> verts;
    s.slots.clear();
    for (size_t c = 0; c < chunks.size(); ++c)
    {
        const LLPhysShapeMesh::Chunk& ch = chunks[c];
        verts.resize(ch.pos.size());
        for (size_t i = 0; i < ch.pos.size(); ++i)
        {
            LLCleanMeshAsset::MirrorVertex& v = verts[i];
            std::memcpy(v.pos, ch.pos[i].mV, sizeof(v.pos));
            v.uv[0] = v.uv[1] = 0.f;
            std::memcpy(v.normal, ch.normal[i].mV, sizeof(v.normal));
            v.tangent[0] = 1.f; v.tangent[1] = 0.f; v.tangent[2] = 0.f; v.tangent[3] = 1.f;
        }
        const U32 slot = ms.captureRawSlot(s.key, 0, (S32)c, verts.data(), nullptr, ch.idx.data(),
                                           (U32)ch.pos.size(), (U32)ch.idx.size(), false);
        if (slot == 0xFFFFFFFFu)
        {
            if (!s.slots.empty()) ms.freeAsset(s.key);
            s.slots.clear();
            why = FAIL_STORE;
            return false;
        }
        s.slots.push_back(slot);
    }
    s.bytes = bytes;
    mBytes += bytes;
    mPeakBytes = std::max(mPeakBytes, mBytes);
    publishLive();
    return true;
}

void LLCleanPhysShapeStore::freeShape(Shape& s)
{
    if (s.slots.empty()) return;
    LLCleanMeshAsset::instance().freeAsset(s.key);
    mBytes -= std::min(mBytes, s.bytes);
    s.bytes = 0;
    s.slots.clear();
}

void LLCleanPhysShapeStore::reap(F64 now, U64 need)
{
    // Idle: no selected object wanted it in the last rebuild, nor in one in progress (which
    // includes the shape being captured).
    auto idle = [](const Shape& s) { return s.refs == 0 && s.counting == 0; };
    bool freed = false;
    for (auto it = mShapes.begin(); it != mShapes.end();)
    {
        Shape& s = it->second;
        if (idle(s) && now - s.idle_since >= kGraceSec)
        {
            freed = freed || !s.slots.empty();
            freeShape(s);
            it = mShapes.erase(it);
            ++mEvicted;
        }
        else
        {
            ++it;
        }
    }
    while (need && mBytes + need > kBudgetBytes)
    {
        auto oldest = mShapes.end();
        for (auto it = mShapes.begin(); it != mShapes.end(); ++it)
            if (idle(it->second) && !it->second.slots.empty()
                && (oldest == mShapes.end() || it->second.idle_since < oldest->second.idle_since))
                oldest = it;
        if (oldest == mShapes.end()) break;
        freeShape(oldest->second);
        mShapes.erase(oldest);
        ++mEvicted;
        freed = true;
    }
    mIdleShapes = 0;
    for (const auto& kv : mShapes) if (kv.second.refs == 0) ++mIdleShapes;
    if (freed) publishLive();
}

void LLCleanPhysShapeStore::publishLive()
{
    std::vector<U32> live;
    for (const auto& kv : mShapes) live.insert(live.end(), kv.second.slots.begin(), kv.second.slots.end());
    std::lock_guard<std::mutex> lk(mLiveMutex);
    mLiveSlots.swap(live);
}

void LLCleanPhysShapeStore::markLive(std::vector<U8>& live) const
{
    std::lock_guard<std::mutex> lk(mLiveMutex);
    for (U32 sl : mLiveSlots)
        if (sl < live.size()) live[sl] = 1;
}

void LLCleanPhysShapeStore::reset()
{
    for (const LLUUID& id : mPoseRows) LLCleanInstanceStore::instance().releasePoseRow(id);
    mPoseRows.clear();
    mShapes.clear();
    mSet.reset();
    ++mGen;
    mBytes = 0;
    mIdleShapes = 0;
    mDirty = true;
    std::lock_guard<std::mutex> lk(mLiveMutex);
    mLiveSlots.clear();
}

void LLCleanPhysShapeStore::logStats(F64 now)
{
    if (now < mNextStats || mShapes.empty()) return;
    mNextStats = now + 60.0;
    std::string fails;   // by reason, the nonzero ones
    for (U32 r = 0; r < FAIL_COUNT; ++r)
    {
        if (mFails[r] == 0) continue;
        fails += llformat("%s%s %llu", fails.empty() ? "" : ", ", failName((FailReason)r), (unsigned long long)mFails[r]);
    }
    if (fails.empty()) fails = "none";
    LL_INFOS("VKPhysShape") << "physics shapes: " << mShapes.size() << " held (" << mBytes / 1024 << " KB of "
                            << kBudgetBytes / 1024 << " KB, peak " << mPeakBytes / 1024 << " KB), "
                            << (mSet ? mSet->rows.size() : 0) << " rows; built " << mBuilt << ", freed " << mEvicted
                            << ", refused " << mRefused << ", rows over the cap "
                            << mRowsDropped << "; magenta boxes " << fails << LL_ENDL;
}
