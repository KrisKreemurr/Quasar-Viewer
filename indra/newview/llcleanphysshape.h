/**
 * @file llcleanphysshape.h
 * @brief Show Physics Shape When Editing (#1176): the selection's physics shapes for the
 *        Vulkan renderer -- shape geometry in the clean mesh store, one draw row per selected
 *        object.
 *
 * Build > Options > Show Physics Shape When Editing (ShowPhysicsShapeInEdit, also the eye on
 * the build floater's Features tab) shows the physics shape of every selected object while
 * the edit tool is up. Nothing here runs per object per frame:
 *
 *  - GEOMETRY. Each distinct shape -- a mesh's convex hulls, physics mesh or base hull, a
 *    prim's convex hull or physics-LOD volume at its detail level, LL's analytic box, sphere
 *    and cylinder -- is built ONCE in unit space by LL's renderPhysicsShape rules (the same
 *    choice Develop > Physics Shapes makes in pipeline.cpp) through llcleanphysshapemesh.h,
 *    and captured as non-reclaimable raw slots of the clean mesh store
 *    (LLCleanMeshAsset::captureRawSlot), keyed by its content. A shape no selected object
 *    wants any more is freed kGraceSec later, and all shapes together stay within
 *    kBudgetBytes (least recently used first; wanted shapes are never evicted). A capture
 *    the store refuses is retried with the memory governor's backoff, then the row shows LL's
 *    magenta box. Builds run here on the main thread, at most kBuildBudgetSec a frame.
 *  - ROWS. One per selected object and shape slot: the object's clean instance id (the GPU
 *    places the shape with the object's own pose, so edit drags, spin and worn attachments
 *    on their joint follow without CPU work) and the slot. Rebuilt only when the selection
 *    changes (LLSelectMgr's update signal, or its size / first object between frames), the
 *    toggle or the edit tool does -- and re-checked every kPendingPollSec (backing off to
 *    kPendingPollMaxSec while nothing changes) as long as a shape is still loading or waiting
 *    for a retry, or an object's cost is unknown. A frame's builds past kBuildBudgetSec go
 *    to the next frame. Sorted into colour buckets (LLVKPhysShapes); the renderer
 *    re-uploads them when their generation changes and completes their draws on the GPU
 *    (clean_phys_shape.comp).
 *  - RIGGED MESH. A rezzed rigged mesh (not animesh) has ordinary world rows: it is only
 *    skinned worn or as a playing animesh (registerCleanFace). An animesh's rows are rigged
 *    while its control avatar plays -- no world pose -- so its shape is placed by a pose row
 *    (LLCleanInstanceStore::upsertPoseRow): the object's own static pose, the one the server
 *    collides with, kept current by the object's moves like its other rows. Pose rows live
 *    while the object is in the rows (mPoseRows) and go with the next rebuild after that.
 *  - NOT SHOWN: HUD attachments (and a HUD selection), worn rigged meshes (physics never
 *    applies to a worn attachment; the shape would float at the attach point), flexible
 *    prims, PHYSICS_SHAPE_NONE, sculpts (LL had no display for them), glTF scene objects,
 *    objects without a clean instance row.
 *
 * Threading: main thread, except markLive (the store-fill worker; mLiveMutex).
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"
#include "llcleanphysshapemesh.h"
#include "llvolume.h"
#include "v3math.h"
#include "vkgfx/llvulkancontext.h"   // LLVKPhysShapeRow / LLVKPhysShapes

#include <boost/signals2/connection.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLViewerObject;
class LLVOVolume;

class LLCleanPhysShapeStore
{
public:
    static LLCleanPhysShapeStore& instance();

    // Fixed limits: constants, deliberately not settings.
    static constexpr U32 kRowCap         = LLVKPhysShapes::kRowCap;
    static constexpr U64 kBudgetBytes    = 16ull << 20;     // shape geometry, all shapes together
    static constexpr F64 kGraceSec       = 10.0;            // an unreferenced shape is freed after this
    static constexpr F64 kBuildBudgetSec = 0.002;           // shape builds per frame (main thread)
    static constexpr F64 kPendingPollSec = 0.25;            // re-check of loading shapes / unknown costs,
    static constexpr F64 kPendingPollMaxSec = 2.0;          // backing off while nothing changes
    static constexpr U32 kFailLogsPerWindow = 5;            // magenta-box failure lines per window
    static constexpr F64 kFailLogWindowSec  = 60.0;

    // Main thread, from the world-frame build: this frame's rows into wf.phys.
    void fillFrame(LLVKWorldFrame& wf);
    // Full geometry repack (the store-fill worker): our slots are no LOD group's, keep them.
    void markLive(std::vector<U8>& live) const;
    // The mesh store is shutting down: our slot ids mean nothing now (nothing is freed).
    void reset();

    // LL get_physics_detail (llspatialpartition.cpp): the physics-LOD volume detail of an
    // object this size.
    static S32 physicsDetail(const LLVector3& scale);
    // LL renderPhysicsShape's inline prim hull (gMeshRepo.buildHull is an empty stub): the
    // convex hull of the physics-LOD volume through the convex decomposition library, cached
    // on the shared LLVolume (mHullPoints / mHullIndices, freed with the volume). Without a
    // functional library nothing is cached and the caller shows LL's magenta box.
    enum class HullBuildResult { Ready, Unavailable, NoMemory };
    static HullBuildResult buildPrimHull(LLVolume* phys);

private:
    LLCleanPhysShapeStore() = default;

    // Where a shape's triangles come from (LL renderPhysicsShape's cases).
    enum Source : U8
    {
        SRC_PLACEHOLDER,    // LL's box while a mesh's decomposition loads (and the magenta box)
        SRC_BOX,            // analytic box: center / half
        SRC_SPHERE,
        SRC_CYLINDER,
        SRC_MESH_HULLS,     // a mesh's convex hull decomposition
        SRC_MESH_PHYSICS,   // a mesh's physics mesh
        SRC_MESH_BASE_HULL, // a mesh's single base hull
        SRC_PRIM_HULL,      // a prim's convex hull at its physics detail
        SRC_PRIM_MESH,      // a prim's physics-LOD volume
    };
    // What one object shows.
    struct Desc
    {
        std::string key;            // the shape's identity (its content)
        Source  src = SRC_PLACEHOLDER;
        LLVector3 center, half;     // SRC_BOX
        LLUUID  mesh_id;            // SRC_MESH_*
        LLVolumeParams params;      // SRC_PRIM_*
        S32     detail = 0;         // SRC_PRIM_*
        bool    fill_only = false;  // LL draws it filled, without edges (box / sphere / cylinder)
        bool    pending = false;    // a placeholder for something still loading
    };
    enum State : U8 { ST_NEW, ST_READY, ST_EMPTY, ST_RETRY, ST_FAILED };
    // Why a shape shows as the magenta box, counted per failed build and logged rate-limited
    // with the shape's key (a mesh key names the asset), to tell a bad asset from a bug.
    enum FailReason : U8
    {
        FAIL_NO_LIBRARY,    // no convex decomposition library: every prim hull (logged once)
        FAIL_NO_HULL,       // the library made no hull of this prim
        FAIL_TOO_BIG,       // the shape alone is over the budget (not retried)
        FAIL_BUDGET,        // the shapes in use left no room for it
        FAIL_STORE,         // the mesh store refused a slot
        FAIL_MEMORY,        // out of memory building it
        FAIL_COUNT          // (none)
    };
    struct Shape
    {
        LLUUID key;                 // mesh-store key (captureRawSlot's mesh id)
        std::vector<U32> slots;     // one per chunk (ST_READY)
        U64   bytes = 0;
        U32   refs = 0;             // selected objects wanting it, as of the last rebuild
        U32   counting = 0;         // the same in the rebuild in progress
        F64   idle_since = 0.0;     // refs fell to 0 at
        U32   attempts = 0;         // refused captures in a row
        F64   retry_at = 0.0;       // ST_RETRY: next capture attempt
        State state = ST_NEW;
        bool  no_hull = false;      // the prim hull could not be made: LL's magenta box
    };

    void   onSelectionUpdate() { mDirty = true; }
    bool   activeNow() const;
    void   rebuild(F64 now);
    bool   describe(LLVOVolume* vol, Desc& d) const;
    static void placeholder(Desc& d, bool pending);
    // hull_fail: FAIL_NO_LIBRARY / FAIL_NO_HULL when a prim hull fell back to the magenta box
    bool   buildMesh(const Desc& d, LLPhysShapeMesh& mesh, FailReason& hull_fail) const;
    // The shape for d (counted as wanted): ready, empty, failed, or nullptr (not built yet:
    // deferred to the next frame by the build budget, or waiting for a retry).
    Shape* acquire(const Desc& d, const LLUUID& object_id, F64 now, F64 deadline, U32& builds);
    // false: refused, why says by what (FAIL_TOO_BIG / FAIL_BUDGET / FAIL_STORE)
    bool   capture(Shape& s, const std::vector<LLPhysShapeMesh::Chunk>& chunks, F64 now, FailReason& why);
    void   noteFailure(const Desc& d, const LLUUID& object_id, FailReason why, F64 now, const std::string& detail);
    static const char* failName(FailReason why);
    void   freeShape(Shape& s);
    // Free unreferenced shapes past their grace period, then the longest idle ones until
    // `need` more bytes fit the budget.
    void   reap(F64 now, U64 need);
    void   publishLive();
    void   logStats(F64 now);

    std::unordered_map<std::string, Shape> mShapes;
    std::shared_ptr<const LLVKPhysShapes::Set> mSet;   // the rows (shared with the frames)
    std::unordered_set<LLUUID> mPoseRows;              // animesh with a pose row (upsertPoseRow)
    U64  mGen = 0;
    U64  mBytes = 0, mPeakBytes = 0;
    U32  mIdleShapes = 0;
    U64  mBuilt = 0, mEvicted = 0, mRefused = 0, mRowsDropped = 0;
    U64  mFails[FAIL_COUNT] = {};   // magenta boxes by reason
    F64  mFailLogUntil = 0.0;       // the failure log's current window
    U32  mFailLogged = 0, mFailUnlogged = 0;

    bool mActive = false;
    bool mDirty = true;
    bool mPending = false;
    F64  mNextPoll = 0.0;
    F64  mPollSec = kPendingPollSec;
    F64  mNextReap = 0.0;
    F64  mNextStats = 0.0;
    // The selection as of the last rebuild (between-frame changes the signal did not report).
    const void*     mSelPtr = nullptr;
    S32             mSelNodes = -1;
    LLViewerObject* mSelFirst = nullptr;
    boost::signals2::scoped_connection mSelConn;

    mutable std::mutex mLiveMutex;
    std::vector<U32>   mLiveSlots;   // every slot we hold (markLive)
};
