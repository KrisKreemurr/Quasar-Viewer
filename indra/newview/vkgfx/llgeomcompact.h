/**
 * @file llgeomcompact.h
 * @brief #1426 IN-PLACE COMPACTION of the clean renderer's packed geometry arenas (the
 *        vertex / index / weight buffers): the offset bookkeeping and the move planner.
 *
 * An arena is one GPU buffer addressed in elements (a QVertex incl. the 2-vertex slot
 * headers, a U16 index, a 10 B weight record). Every live range has one owner (a slot
 * row); freed ranges sit in a FreeList (best fit, neighbours coalesce). The planner moves
 * live ranges TOWARD THE START OF THE SAME BUFFER, a bounded number of elements per call
 * (one call per frame), so the arena end falls and the garbage disappears without a second
 * copy of the buffer:
 *
 *   1. TAIL FIT. From the top down, each live range moves into the best-fitting free range
 *      below it (source and destination are disjoint: the hole was free, the range live).
 *   2. SLIDE. A run of live ranges right after a hole (up to the next hole) moves down by
 *      the hole's length L, merging the hole into the next one. The copy goes in CHUNKS of
 *      L elements from the bottom up: chunk k reads [run + kL, +L) and writes where chunk
 *      k-1 read, so no single copy overlaps itself; the caller records chunk round k of
 *      every slide as one batch and a COPY -> COPY barrier between rounds. A run of at most
 *      `maxChunks` x L only (the cost stays proportional to the garbage it removes; no
 *      scratch memory at all).
 *
 * Every move strictly lowers a live range's offset, so the planner terminates for any live
 * set. Within one plan no range is written twice and no destination overlaps a source:
 * sources are not given back to the free list until the caller recorded the copies
 * (release(): the HOLDS), moved ranges are stamped and not moved again in the same plan,
 * and a slide's run must be fully known, contiguous and unstamped. The caller rewrites the
 * slot rows (Plan::rows) in the same frame as the copies, so every reader of that frame
 * sees the new offsets and the moved bytes; earlier frames' readers are ordered before the
 * copies by the barrier the caller records first (one queue).
 *
 * Pure CPU code (no Vulkan): unit-tested by newview/tests/llgeomcompact_test.cpp.
 */
#ifndef LL_LLGEOMCOMPACT_H
#define LL_LLGEOMCOMPACT_H

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

namespace LLGeomCompact
{

// FREE LIST (element units): freed ranges, reused by later placements (best fit, neighbours
// coalesce). (#584 / #595 GeomFreeList, moved here for #1426's planner.)
struct FreeList
{
    std::map<uint32_t, uint32_t> byOff;            // offset -> length
    std::multimap<uint32_t, uint32_t> bySize;      // length -> offset
    uint64_t total = 0;
    uint32_t rejected = 0;   // overlapping releases refused (would double-allocate)

    void clear() { byOff.clear(); bySize.clear(); total = 0; }
    void eraseSize(uint32_t len, uint32_t off)
    {
        auto r = bySize.equal_range(len);
        for (auto it = r.first; it != r.second; ++it) if (it->second == off) { bySize.erase(it); return; }
    }
    bool release(uint32_t off, uint32_t len)
    {
        if (!len) return true;
        auto next = byOff.lower_bound(off);
        // Never accept a range that overlaps free space: handing it out twice
        // corrupts whoever got it first.
        if ((next != byOff.end() && next->first < off + len)
            || (next != byOff.begin() && std::prev(next)->first + std::prev(next)->second > off))
        {
            ++rejected;
            return false;
        }
        total += len;
        if (next != byOff.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == off)
            {
                eraseSize(prev->second, prev->first);
                off = prev->first; len += prev->second;
                byOff.erase(prev);
            }
        }
        if (next != byOff.end() && off + len == next->first)
        {
            eraseSize(next->second, next->first);
            len += next->second;
            byOff.erase(next);
        }
        byOff[off] = len;
        bySize.emplace(len, off);
        return true;
    }
    // #595: give the free tail back to the arena (`end` drops to the last live range).
    void trimTail(uint32_t& end)
    {
        while (!byOff.empty())
        {
            auto it = std::prev(byOff.end());
            if ((uint64_t)it->first + it->second != end) break;
            end = it->first;
            total -= it->second;
            eraseSize(it->second, it->first);
            byOff.erase(it);
        }
    }
    bool take(uint32_t len, uint32_t& off)
    {
        auto it = bySize.lower_bound(len);
        if (it == bySize.end()) return false;
        const uint32_t blen = it->first, boff = it->second;
        bySize.erase(it);
        byOff.erase(boff);
        total -= len;
        off = boff;
        if (blen > len) { byOff[boff + len] = blen - len; bySize.emplace(blen - len, boff + len); }
        return true;
    }
    // #1426: take exactly [off, off + len), which must lie inside one free range.
    bool takeAt(uint32_t off, uint32_t len)
    {
        if (!len) return true;
        auto it = byOff.upper_bound(off);
        if (it == byOff.begin()) return false;
        --it;
        const uint32_t boff = it->first, blen = it->second;
        if ((uint64_t)off + len > (uint64_t)boff + blen) return false;
        eraseSize(blen, boff);
        byOff.erase(it);
        total -= len;
        if (off > boff) { byOff[boff] = off - boff; bySize.emplace(off - boff, boff); }
        const uint32_t tail = (boff + blen) - (off + len);
        if (tail) { byOff[off + len] = tail; bySize.emplace(tail, off + len); }
        return true;
    }
};

// LIVE INDEX: offset -> owner of each packed range of one arena (the reverse of the slot
// rows, which the planner needs to find "the range at this offset"). Entries can go stale
// (the caller's row moved on): the planner checks every entry it uses against the row
// (the `valid` callback) and drops stale ones. Missing entries only make it skip a region.
struct Live
{
    uint32_t slot;
    uint32_t len;
    uint32_t stamp;   // the plan that last moved it (not moved twice in one plan)
};
struct LiveIndex
{
    std::map<uint32_t, Live> byOff;
    void clear() { byOff.clear(); }
    void add(uint32_t off, uint32_t len, uint32_t slot)
    {
        if (len) byOff[off] = Live{slot, len, 0u};
    }
    // Ascending-offset rebuild (a full repack lays the slots out in order): O(1) per entry.
    void addInOrder(uint32_t off, uint32_t len, uint32_t slot)
    {
        if (len) byOff.emplace_hint(byOff.end(), off, Live{slot, len, 0u});
    }
    void remove(uint32_t off, uint32_t slot)
    {
        auto it = byOff.find(off);
        if (it != byOff.end() && it->second.slot == slot) byOff.erase(it);
    }
};

struct Move { uint32_t slot, from, to, len; };   // a slot's range changes place (elements)
struct Span { uint32_t from, to, len; };         // a GPU copy [from, from + len) -> [to, to + len)
struct Hold { uint32_t off, len; };              // freed by the plan; free list after the copies

struct Plan
{
    std::vector<Span> direct;   // round 0: disjoint sources and destinations
    std::vector<Span> slides;   // to = from - L; chunk k = {from + kL, to + kL, min(L, len - kL)}
    std::vector<Move> rows;     // every slot whose offset this plan changed
    std::vector<Hold> holds;
    uint64_t moved = 0;         // elements copied
    uint32_t tailMoves = 0, rounds = 0;   // rounds: the most chunks of any slide (>= 1 when any copy)
    void clear()
    {
        direct.clear(); slides.clear(); rows.clear(); holds.clear();
        moved = 0; tailMoves = rounds = 0;
    }
    bool empty() const { return rows.empty() && holds.empty(); }
    uint64_t heldElems() const
    {
        uint64_t n = 0;
        for (const Hold& h : holds) n += h.len;
        return n;
    }
    // Chunk `k` of slide `s` (false past its end).
    static bool chunk(const Span& s, uint32_t k, Span& c)
    {
        const uint32_t shift = s.from - s.to;
        const uint64_t at = (uint64_t)k * shift;
        if (!shift || at >= s.len) return false;
        c.from = s.from + (uint32_t)at;
        c.to = s.to + (uint32_t)at;
        c.len = (uint32_t)std::min<uint64_t>(shift, s.len - at);
        return true;
    }
};

struct Params
{
    uint64_t budget = 0;        // elements copied per plan
    uint32_t maxOps = 1024;     // range moves + index steps per plan (CPU bound)
    uint32_t maxScan = 256;     // passed-over ranges (tail fit) / free ranges (slide) per plan
    uint32_t maxRun = 4096;     // live ranges in one slide run
    uint32_t maxChunks = 8;     // a slide moves at most this x the hole it closes (= copy rounds)
};

// Per-arena planner state across plans.
struct State
{
    uint32_t stamp = 0;
    uint32_t cursor = 0;        // slide scan position (offset)
    uint32_t wraps = 0;         // slide scans that reached the arena end
};

// Plans one step. `end` is the arena end (elements; trim the free tail first). `valid(slot,
// off, len)` says whether that slot's row still owns [off, off + len) in this arena. Updates
// `fl` (destinations taken) and `li` (entries re-keyed) at once; the caller rewrites the rows
// (out.rows), records the copies, then calls release() with out.holds.
template<class Valid>
void plan(FreeList& fl, LiveIndex& li, uint32_t end, State& st, const Params& p, Valid valid, Plan& out)
{
    if (++st.stamp == 0u)
    {   // (stamp wrapped: clear the old stamps so none reads as "this plan")
        for (auto& kv : li.byOff) kv.second.stamp = 0u;
        st.stamp = 1u;
    }
    uint64_t budget = p.budget;
    uint32_t ops = 0;
    // Re-key one entry (ascending order inside a run keeps keys unique: see the slide).
    auto rekey = [&](std::map<uint32_t, Live>::iterator it, uint32_t to)
    {
        Live l = it->second;
        const uint32_t from = it->first;
        l.stamp = st.stamp;
        li.byOff.erase(it);
        li.byOff[to] = l;
        out.rows.push_back(Move{l.slot, from, to, l.len});
    };

    // ---- 1. TAIL FIT: from the top down, each range into the best-fitting hole below it ----
    // A range too large for every hole below it is passed over (at most maxScan of them):
    // the ranges under it still move down, so the free space gathers below it until it fits
    // a hole or slides (2) into the one right under it.
    {
        auto it = li.byOff.end();
        uint32_t skips = 0;
        while (ops < p.maxOps && budget > 0 && skips < p.maxScan && it != li.byOff.begin())
        {
            --it;
            ++ops;   // (a stale entry dropped counts too: bounded work)
            // Moved by this plan already. (Checked first: the caller rewrites the rows after
            // the plan, so a stamped entry does not match its row yet.)
            if (it->second.stamp == st.stamp) { ++skips; continue; }
            if (!valid(it->second.slot, it->first, it->second.len))
            {
                it = li.byOff.erase(it);   // (the next --it reaches the entry below it)
                continue;
            }
            const uint32_t off = it->first, len = it->second.len;
            // Smallest free range that fits and lies below it.
            uint32_t hoff = 0;
            bool found = false;
            if (len <= budget)
            {
                auto c = fl.bySize.lower_bound(len);
                for (int k = 0; k < 16 && c != fl.bySize.end(); ++k, ++c)
                    if ((uint64_t)c->second + len <= off) { hoff = c->second; found = true; break; }
            }
            if (!found || !fl.takeAt(hoff, len)) { ++skips; continue; }
            const bool lowest = it == li.byOff.begin();
            const auto below = lowest ? li.byOff.end() : std::prev(it);
            out.direct.push_back(Span{off, hoff, len});
            out.holds.push_back(Hold{off, len});
            rekey(it, hoff);
            budget -= len;
            out.moved += len;
            ++out.tailMoves;
            out.rounds = std::max(out.rounds, 1u);
            if (lowest) break;
            it = std::next(below);   // (the next --it reaches `below`)
        }
    }

    // ---- 2. SLIDE short runs down into the hole before them ----
    uint32_t scanned = 0;
    uint32_t cur = st.cursor;
    std::vector<std::map<uint32_t, Live>::iterator> run;
    while (ops < p.maxOps && budget > 0 && scanned < p.maxScan)
    {
        auto h = fl.byOff.lower_bound(cur);
        if (h == fl.byOff.end())
        {
            cur = 0;
            ++st.wraps;
            break;   // (the next plan starts over at the bottom)
        }
        ++scanned;
        const uint32_t hoff = h->first, hlen = h->second, hend = hoff + hlen;
        auto nx = std::next(h);
        const uint32_t gap_end = nx != fl.byOff.end() ? nx->first : end;
        cur = hend;
        if (gap_end <= hend) continue;
        const uint32_t t = gap_end - hend;
        if ((uint64_t)t > (uint64_t)hlen * p.maxChunks || t > budget) continue;
        // The run: known, contiguous, current, not moved by this plan.
        run.clear();
        bool ok = true;
        uint32_t pos = hend;
        auto it = li.byOff.find(pos);
        while (pos < gap_end)
        {
            if (it == li.byOff.end() || it->first != pos || it->second.stamp == st.stamp
                || run.size() >= p.maxRun || (uint64_t)pos + it->second.len > gap_end
                || !valid(it->second.slot, it->first, it->second.len))
            { ok = false; break; }
            run.push_back(it);
            pos += it->second.len;
            ++it;
        }
        ops += (uint32_t)run.size() + 1u;
        if (!ok || pos != gap_end || run.empty()) continue;
        if (!fl.takeAt(hoff, hlen)) continue;
        // Ascending: a range's new key (old - hlen) is below its old key and above the
        // already moved ranges' new keys -- never a key still in use.
        for (auto& r : run) rekey(r, r->first - hlen);
        out.slides.push_back(Span{hend, hoff, t});
        out.holds.push_back(Hold{hoff + t, hlen});
        out.rounds = std::max(out.rounds, (t + hlen - 1u) / hlen);
        budget -= t;
        out.moved += t;
        // Skip the next hole: the region freed here (held) joins it next plan; sliding its
        // run now would only push the two holes along side by side.
        cur = nx != fl.byOff.end() ? nx->first + nx->second : end;
    }
    st.cursor = cur;
}

// The copies of a plan are recorded: its freed ranges join the free list; the free tail
// goes back to the arena.
inline void release(FreeList& fl, const std::vector<Hold>& holds, uint32_t& end)
{
    for (const Hold& h : holds) fl.release(h.off, h.len);
    fl.trimTail(end);
}

} // namespace LLGeomCompact

#endif
