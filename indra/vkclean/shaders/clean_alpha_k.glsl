// vkclean -- ALPHA-BLEND COVER LISTS (#1147; LLCleanAlphaKVK, llcleanalphakvk.cpp).
//
// The main view's alpha-blend layers are composited in DRAW order (clean_alpha_sort.comp sorts
// draws back to front by their centre; inside a draw, triangle order), and every layer used to
// be fully lit -- local lights, probes, shadows, haze -- however much of it the layers composited
// over it hide. Stacked hair cards and the grinder's shells paid that per layer.
//
// Before pass 2, a cheap pass over the same alpha lists (clean_alpha_kvis.frag: the base alpha
// only) records, per pixel and water side, K of the fragments LAST in the blend order with their
// alpha: K slots, a fragment going to slot akBucket (a hash of draw and triangle, mod K) with ONE
// atomicMax, so each slot keeps the latest fragment of its bucket. In pass 2 the alpha-blend FS
// (clean_draw.frag -DCLEAN_BLEND_TOL) multiplies (1 - alpha) over the slots holding a fragment
// composited AFTER it: below AK_EPS the fragment can change the pixel by less than AK_EPS of its
// colour, so it is dropped before any lighting.
// (The first version kept the exact K last fragments with an insertion chain: K contended
// 64-bit atomics per fragment, as every new layer entered at the top -- at the grinder the list
// pass cost 77 % of the lit pass it saved. The buckets cull about as much: CPU model
// aktest3.py in the #1147 notes, 86 % vs 89 % of grinder-like fragments at K 4, shells in one
// draw included.) The key is the blend
// ORDER, not depth: what is dropped is exactly what the current compositing hides, so the image
// is the current one (to AK_EPS) whatever the sort gets wrong -- intersecting meshes keep their
// present order (#759 / #761 are not changed by this).
//
// Conservative by construction: a list entry is a fragment pass 2 draws there (same lists,
// discards, alpha and shading rate; only pixels clearly in front of the opaque depth) with an
// alpha rounded down; entries of the same draw and triangle never cover each other; every slot
// holds a distinct real fragment, so the product over any of them bounds the true one; faint
// fragments (alpha < AK_MIN_A8) are not recorded (they would only displace stronger ones). The pre-water and post-water sides have separate lists: a
// pre-water layer is never dropped for a post-water one (the water's refraction copy shows
// pre-water layers at other pixels).
//
// Memory (LLCleanAlphaKVK): entries[((y * W + x) * 2 + side) * K + i], 8 B each -- the order in
// hi | lo[31:8], the alpha (floor, 8 bits) in lo[7:0]; (0, 0) = empty; slots in bucket order
// (not sorted). After the entries, one dirty word per 8 x 8 tile (the clear
// pass zeroes only the tiles the previous frame's lists touched), then (16-byte aligned) the
// AUTO SWITCH's counters (LLCleanAlphaKVK, RenderVKAlphaKCull 2): [0] list-pass fragments in
// front of the opaque surface (its cost), [1] pass-2 fragments dropped (the lighting it saved).
//
// NEAR-OPAQUE COVERS (#1147, the grinder: ONE draw of parallel planes, untextured alpha, tint
// 252/255 -- each plane composites at 98.8 % over the one before, so the LAST-drawn plane at a
// pixel is what shows, from either end). A cheap pass before the lists (clean_alpha_cover.frag)
// draws the same four alpha lists in pass 2's exact order -- per list the back-face pass, then the
// front-face pass, static then rigged, pre-water then post-water -- into an R32_UINT target per
// water side with the hardware depth test (in front of the opaque surface) and NO blending: the
// ROP keeps the LAST cover fragment's order key (rasterization order), no atomics. A cover is a
// fragment pass 2 draws there with alpha >= AK_COVER_A. Copied to a buffer (alpha_cover), pass 2
// drops a fragment wherever a later cover lies over every pixel it would write: it is left at
// most (1 - AK_COVER_A) visible -- 2 % at worst, 1.2 % for the grinder's 252/255 -- the one
// bounded change to the image (owner-approved threshold). Independent of the K lists.
#ifndef CLEAN_ALPHA_K_GLSL
#define CLEAN_ALPHA_K_GLSL
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// The draw push's pad0 word in the main view's alpha-blend draws (pass 2 and the list pass);
// bits 0-7 stay the visibility-reuse flags (clean_draw_common.glsl VISR_*).
const uint AK_ON         = 0x100u;   // this frame's lists are built: test against them
const uint AK_LIST_SHIFT = 9u;       // 2 bits: 0 pre-water static, 1 pre-water rigged, 2 / 3 post-water
const uint AK_TWO_PASS   = 0x800u;   // the list draws back faces first, then front faces (drawListBlend)
const uint AK_KMAX       = 4u;       // the most entries per pixel and side (LLCleanAlphaKVK::kMaxK)
const uint AK_MIN_A8     = 64u;      // the faintest recorded alpha (8-bit): a quarter
// Hidden below this transmittance: the 0.99 of the alpha-blend depth pre-pass (clean_prepass_blend.frag).
const float AK_EPS = 0.01;
const uint AK_COVER       = 0x1000u;   // pad0: this frame's near-opaque covers are built
const float AK_COVER_A    = 0.98;      // a cover's least alpha (its layers below show <= 2 %)

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer AkEntries { uvec2 e[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer AkWords { uint w[]; };

uint akWidth()  { return ssr.alpha_k.z & 0xFFFFu; }
uint akHeight() { return ssr.alpha_k.z >> 16u; }
uint akK()      { return min(ssr.alpha_k.w & 0xFFu, AK_KMAX); }
bool akBuilt(uint pad0) { return (pad0 & AK_ON) != 0u && (ssr.alpha_k.x | ssr.alpha_k.y) != 0u; }

// A fragment's place in the blend order (lo with the alpha byte clear, hi): list, face pass,
// draw, triangle. Draws past 32767 / triangles past 2^24 saturate: ties never cover each other.
uvec2 akOrder(uint pad0, uint drawId, uint prim, bool front)
{
    const uint list = (pad0 >> AK_LIST_SHIFT) & 3u;
    const uint face = ((pad0 & AK_TWO_PASS) != 0u && front) ? 1u : 0u;
    const uint d = min(drawId, 0x7FFFu);
    const uint p = min(prim, 0xFFFFFFu);
    return uvec2((p & 0x3FFu) << 22u, (list << 30u) | (face << 29u) | (d << 14u) | (p >> 10u));
}
uint akSide(uint pad0) { return ((pad0 >> AK_LIST_SHIFT) & 3u) >> 1u; }
// A device address + a byte offset (lo, hi).
uvec2 akAddr(uvec2 a, uint off)
{
    uint carry;
    a.x = uaddCarry(a.x, off, carry);
    return uvec2(a.x, a.y + carry);
}
// The tile words (after the entries) and the counters (after them, 16-byte aligned).
uint akTileBytes() { return ((akWidth() + 7u) >> 3u) * ((akHeight() + 7u) >> 3u) * 4u; }
AkWords akTiles() { return AkWords(akAddr(ssr.alpha_k.xy, akWidth() * akHeight() * 2u * akK() * 8u)); }
AkWords akCounters()
{
    return AkWords(akAddr(ssr.alpha_k.xy, akWidth() * akHeight() * 2u * akK() * 8u + ((akTileBytes() + 15u) & ~15u)));
}

// The slot a fragment of order `own` records into: a HASH of (draw, triangle), mod K. Not a plain
// sum: a mesh's layers sit a fixed triangle count apart (box shells in one draw: 12; hair cards:
// 2), so (draw + triangle) mod K put every layer of a pixel in ONE slot -- only the latest
// survived and nothing was ever covered (the owner's grinder A/B of 6410a2a746). Hashed, layers
// spread over the slots whatever the mesh's layout. (lowbias32, C. Wellons.)
uint akBucket(uvec2 own, uint K)
{
    const uint d = (own.y >> 14u) & 0x7FFFu;
    const uint p = ((own.y & 0x3FFFu) << 10u) | (own.x >> 22u);
    uint h = (d << 17u) ^ p;
    h ^= h >> 16u; h *= 0x7FEB352Du;
    h ^= h >> 15u; h *= 0x846CA68Bu;
    h ^= h >> 16u;
    return h % K;
}
// First entry of pixel pix's list on one side.
uint akSlot(ivec2 pix, uint side, uint K) { return ((uint(pix.y) * akWidth() + uint(pix.x)) * 2u + side) * K; }
// Entry e was composited after the fragment of order own.
bool akLater(uvec2 e, uvec2 own) { return e.y > own.y || (e.y == own.y && (e.x >> 8u) > (own.x >> 8u)); }

// NEAR-OPAQUE COVERS: the 32-bit order key (0 = none): list, face pass, draw (13 bits),
// triangle (16 bits). 13 draw bits: a busy region's alpha list passes 4096 draws (the grinder sat
// at rank ~3660 in the owner's test; the sort's own cap is 4096 and its overflow is appended), and
// a saturated draw takes no part in the culling. A saturated draw saturates the triangle too, so
// saturated keys tie (and
// never cover each other) instead of comparing triangles of different draws.
uint akCoverKey(uint pad0, uint drawId, uint prim, bool front)
{
    const uint list = (pad0 >> AK_LIST_SHIFT) & 3u;
    const uint face = ((pad0 & AK_TWO_PASS) != 0u && front) ? 1u : 0u;
    const bool sat = drawId >= 0x1FFFu;
    const uint d = sat ? 0x1FFFu : drawId;
    const uint p = sat ? 0xFFFFu : min(prim, 0xFFFFu);
    return (list << 30u) | (face << 29u) | (d << 16u) | p;
}
// The cover self-check's counters after the keys (LLCleanAlphaKVK coverCounterOffset, 32 B): [0]
// pass-2 fragments the covers dropped, [1] fragments tested against the covers and lit, [2..5]
// the tested fragments by class at their first kept pixel (AKC_*), [6] the largest own key of the
// fragments at the screen-centre pixel, [7] the stored key there (the bisect readout).
AkWords akCoverCounters()
{
    const uint W = ssr.alpha_cover.z & 0xFFFFu, H = ssr.alpha_cover.z >> 16u;
    return AkWords(akAddr(ssr.alpha_cover.xy, (W * H * 2u * 4u + 15u) & ~15u));
}
// RenderVKAlphaCoverDebug (alpha_cover.w, a GPU bisect): bit 0 / 1 / 2 leave the face-pass /
// draw / triangle field out of the comparison, bit 3 counts would-be drops but drops nothing,
// bit 4 paints each tested fragment by its class (and drops nothing).
const uint AKD_NO_FACE = 1u, AKD_NO_DRAW = 2u, AKD_NO_TRI = 4u, AKD_COUNT_ONLY = 8u, AKD_COLOUR = 16u;
// Classes of the stored key against the fragment's own. EARLIER is normal: the fragment is drawn
// after the last cover at that pixel -- it is no cover itself (alpha < AK_COVER_A: glass, hair
// edges, later blended objects), or the pixel is one the cover pass skipped by design (pass 2 also
// keeps pixels within the static tolerance BEHIND the opaque depth, and the coarse-fragment test
// reads every pixel of the block, including those its triangle does not cover). It never drops.
const uint AKC_EQUAL = 0u, AKC_LATER = 1u, AKC_NONE = 2u, AKC_EARLIER = 3u;
uint akCoverDebug() { return ssr.alpha_cover.w; }
uint akCoverMask()
{
    const uint d = akCoverDebug();
    return ~(((d & AKD_NO_FACE) != 0u ? (1u << 29u) : 0u) | ((d & AKD_NO_DRAW) != 0u ? (0x1FFFu << 16u) : 0u)
             | ((d & AKD_NO_TRI) != 0u ? 0xFFFFu : 0u));
}
bool akCoverBuilt(uint pad0) { return (pad0 & AK_COVER) != 0u && (ssr.alpha_cover.x | ssr.alpha_cover.y) != 0u; }
// covers[(side * H + y) * W + x]: the last cover's key at the pixel on one water side.
uint akCoverStored(ivec2 pix, uint side)
{
    const uint W = ssr.alpha_cover.z & 0xFFFFu, H = ssr.alpha_cover.z >> 16u;
    if (uint(pix.x) >= W || uint(pix.y) >= H) return 0u;
    AkWords C = AkWords(ssr.alpha_cover.xy);
    return C.w[(side * H + uint(pix.y)) * W + uint(pix.x)];
}
// The stored key's class against the fragment's own (the debug mask's fields left out).
uint akCoverClass(uint stored, uint ownKey)
{
    if (stored == 0u) return AKC_NONE;
    const uint m = akCoverMask();
    return (stored & m) == (ownKey & m) ? AKC_EQUAL : (stored & m) > (ownKey & m) ? AKC_LATER : AKC_EARLIER;
}
bool akCoverLater(ivec2 pix, uint side, uint ownKey) { return akCoverClass(akCoverStored(pix, side), ownKey) == AKC_LATER; }

// True when the fragments listed at pixel pix and composited after `own` leave it less than
// AK_EPS of transmittance.
bool akCovered(ivec2 pix, uint side, uvec2 own)
{
    const uint K = akK();
    if (uint(pix.x) >= akWidth() || uint(pix.y) >= akHeight()) return false;
    AkEntries L = AkEntries(ssr.alpha_k.xy);
    const uint base = akSlot(pix, side, K);
    float T = 1.0;
    for (uint i = 0u; i < AK_KMAX; ++i)
    {
        if (i >= K) break;
        const uvec2 e = L.e[base + i];
        if ((e.x | e.y) != 0u && akLater(e, own)) T *= 1.0 - float(e.x & 0xFFu) * (1.0 / 255.0);
    }
    return T < AK_EPS;
}

#endif
