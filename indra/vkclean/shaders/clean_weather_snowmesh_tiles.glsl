// vkclean — SNOW MESH TILE CACHE layout (#1496; clean_weather_snowmesh.comp -DWX_MESH_TILES builds the
// tiles, clean_weather_snowmesh_tiles.comp plans / compacts / culls them, clean_weather_snowmesh.vert
// -DWX_MESH_TILES draws them; llcleanweathervk.cpp kTs* mirrors every constant here -- keep them in step).
//
// THE TILE CACHE. Each layer (0 the inner grid, 1 the outer ring) is cut into WORLD-ANCHORED tiles of
// 8 x 8 texels (a grid's origin is a whole number of tiles: it snaps by an eighth of its extent and the
// tile mode needs res % 64 == 0). A tile is built ONCE, camera-free, at full resolution: its vertices
// (clean_weather_snowmesh.comp VERTICES), the ring surface under its vertices (BAND, inner only) and
// one index list per stride 1 / 2 / 4 plus that stride's SEAM skirts (CELLS) in ONE contiguous range
// of the index pool. The LOD (the stride, the stride and ring geomorphs, the lift, the fades) is
// chosen at draw time from the live camera (clean_weather_snowmesh_tiles.comp CULL, the VS), so a
// moving / turning camera rebuilds nothing; only tiles that ENTER the window (a snap), CHANGE (the
// snow passes' marks, the reference scan) or come into reach are built.
//
// ADDRESSING. Grid tile g (0 .. T-1 per axis, the published window) is world tile W = o + g (o the
// window's origin in tiles); its record lives at slot (W mod T) -- WRAP-AROUND, so a snap moves no
// record and no vertex (clean_weather_snowmesh.comp vSlot does the same per texel). A record whose key
// is not W (or whose epoch is not the current one) holds nothing valid.

// ---- the header (ts[0 .. kTsHead)) ----
const uint kTsHead      = 64u;
const uint kHActive     = 0u;    // the pool half the live ranges are in (0 / 1)
const uint kHBump       = 1u;    // triangles allocated in the active half
const uint kHDead       = 2u;    // ... of them in freed ranges (the compaction reclaims them)
const uint kHFlags      = 3u;    // this recording: 1 an allocation failed, 2 compacted, 4 prefetch capped, 8 a list overflowed
const uint kHLive       = 4u;    // the compaction's live triangles
const uint kHSrc        = 5u;    // the compaction's source half (its copy reads it)
const uint kHCompact    = 7u;    // 1: this recording compacts
const uint kHVList      = 8u;    // [8..10] the VERTICES list's dispatch (x = its length)
const uint kHBList      = 11u;   // [11..13] the BAND list's (inner)
const uint kHCList      = 14u;   // [14..16] the CELLS list's
const uint kHCopy       = 17u;   // [17..19] the compaction copy's (x = tiles, or 0)
const uint kHPref       = 20u;   // prefetch builds taken this recording
const uint kHPrefCap    = 21u;   // ... their cap
const uint kHSoft       = 22u;   // scan-dirty tiles taken this recording (SELECT)
const uint kHSoftCap    = 23u;   // ... their cap
const uint kHEvict      = 24u;   // ranges evicted this recording (out of reach)
const uint kHDrawn      = 25u;   // tiles the main view draws
const uint kHAllocTris  = 26u;   // triangles the CELLS phase allocated this recording
const uint kHFailTiles  = 27u;   // tiles whose allocation failed
// (the host resets [3] and [8 .. 31] every recording; [0 .. 2], [4 .. 7] persist)

// ---- a tile record (16 words) ----
const uint kRecWords = 16u;
const uint kRKey     = 0u;   // world tile: x & 0xFFFF | y << 16 (int16 each)
const uint kRState   = 1u;   // kS* bits | the cells' rebuild count << 16
const uint kRBase    = 2u;   // its range's first triangle (absolute: half x pool + offset)
const uint kRTotal   = 3u;   // its range's triangles
const uint kRS1      = 4u;   // stride 1: main triangles | seam triangles << 16 (then stride 2, 4)
const uint kRHintLo  = 7u;   // its OWN texels' lowest base (m; the VERTICES phase: the plan's reach box)
const uint kRLo      = 8u;   // [8..10] the AABB of every vertex its cells index (x, y, base h)
const uint kRHi      = 11u;  // [11..13] ... (x, y, the highest drawn top + kTileZPad)
const uint kREpoch   = 14u;  // the epoch it was keyed in
const uint kRHintHi  = 15u;  // its own texels' highest top (m)
const uint kSV    = 1u;      // its vertices are valid
const uint kSC    = 2u;      // its cells (index lists) are valid
const uint kSLive = 4u;      // it owns a range of the pool
const uint kSHint = 8u;      // its height hint (kRHintLo / Hi) is valid (set with kSV)
const uint kSB1   = 16u, kSB2 = 32u, kSB4 = 64u;   // the strides its range holds

const float kTileZPad = 0.5;   // (m) over the AABB's top: the BAND refresh's ring surface may rise between cell builds

// The tile list entries: gx | gy << 15 | layer << 31 (grid tile coordinates).
uint tileEntry(uint layer, uvec2 g) { return g.x | (g.y << 15u) | (layer << 31u); }
uvec2 entryTile(uint e) { return uvec2(e & 0x7FFFu, (e >> 15u) & 0x7FFFu); }
uint entryLayer(uint e) { return e >> 31u; }

// The regions after the table (word offsets; TI / TR the tiles per side of the inner grid / the ring's
// ALLOCATED resolution -- the layout never depends on whether the ring runs this recording).
struct TsLayout { uint TI, TR, NI, NT, table, vlist, blist, clist, nbase, scanI, scanR, selI, selR, dueI, dueR, words; };
TsLayout tsLayout(uint resI, uint resR)
{
    TsLayout L;
    L.TI = resI / 8u; L.TR = resR / 8u;
    L.NI = L.TI * L.TI; L.NT = L.NI + L.TR * L.TR;
    L.table = kTsHead;
    L.vlist = L.table + kRecWords * L.NT;
    L.blist = L.vlist + L.NT;
    L.clist = L.blist + L.NI;
    L.nbase = L.clist + L.NT;
    const uint WI = (L.NI + 31u) / 32u, WR = (L.TR * L.TR + 31u) / 32u;
    L.scanI = L.nbase + L.NT;     // the reference scan's dirty tiles (grid tile index), inner
    L.scanR = L.scanI + WI;       // ... ring
    L.selI  = L.scanR + WR;       // SELECT's pick of them this recording (capped), inner
    L.selR  = L.selI + WI;        // ... ring
    L.dueI  = L.selR + WR;        // PLAN A's cell builds of this recording (PLAN B reads the 3 x 3), inner
    L.dueR  = L.dueI + WI;        // ... ring
    L.words = L.dueR + WR;        // (the masks [scanI .. words) are cleared every recording)
    return L;
}
// A layer's record of slot tile s (s = W mod T, row-major).
uint tsRecord(TsLayout L, uint layer, uint s) { return L.table + kRecWords * (layer == 0u ? s : L.NI + s); }
uint wrapTile(int w, uint T) { return uint(((w % int(T)) + int(T)) % int(T)); }

// The draws buffer (clean_weather_snowmesh_tiles.comp CULL; llcleanweathervk.cpp recordMeshDraw):
// [0 .. 7] the views' draw counts, then per view NT VkDrawIndexedIndirectCommand (5 words).
const uint kDrawHead = 16u;
const uint kMaxViews = 5u;   // the main view + 4 sun cascades
uint drawCmd(TsLayout L, uint view, uint i) { return kDrawHead + (view * L.NT + i) * 5u; }

// The VS's per-draw constants (clean_weather_snowmesh.vert -DWX_MESH_TILES; firstInstance = layer x 4
// + the stride's index): 8 records x 7 vec4 at the start of the params buffer; the cull's parameters
// from kParCull (vec4s).
const uint kVsRecVec4 = 7u;
const uint kParCull   = 64u;   // (vec4 index; 8 x 7 = 56 VS vec4s before it)

// The order-preserving uint of a float (atomicMin / atomicMax of floats as uints).
uint orderedBits(float f) { const uint b = floatBitsToUint(f); return (b & 0x80000000u) != 0u ? ~b : b | 0x80000000u; }
float orderedFloat(uint u) { return uintBitsToFloat((u & 0x80000000u) != 0u ? u & 0x7FFFFFFFu : ~u); }
