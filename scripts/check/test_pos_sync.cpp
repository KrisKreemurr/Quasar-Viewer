// Fixtures for avatar position sync (#1417; indra/newview/slpossyncmath.h), run by
// scripts/check/checks/pos_sync.py (compiled with the host C++ compiler, no viewer build).
//   PARSE    exactly five fields after the sim-given owner; a body that carries a key (or any
//            sixth field) is rejected whole, so identity can only come from the sim.
//   BOUND    a shared position more than 2 m from the sim's placement is rejected and the
//            avatar eases back to sim placement; within 2 m it is followed, never snapped.
//   TABLE    the per-avatar table is capped (a newcomer waits) and entries expire.
//   SEQ      repeated / older lines are dropped, the counter wraps.
//   SHARE    the option (and RLV) gate what is sent.
#include "slpossyncmath.h"

#include <cstdio>
#include <string>

using namespace SLPosSyncMath;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static const char* GOOD = "17|<128.50000, 64.25000, 22.00000>|<1.00000, 0.00000, 0.00000>|<0.00000, 0.00000, 0.00000, 1.00000>|F";

static void testParse()
{
    Line l;
    CHECK(parseFields(GOOD, l), "a well-formed line parses");
    CHECK(l.seq == 17 && l.pos.x == 128.5 && l.vel.x == 1.0 && (l.flags & FLAG_FLY), "fields read back");
    CHECK(parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "no spaces, empty flags");
    // An owner key in the body, before or after the fields: rejected, never read as identity.
    CHECK(!parseFields("a2e76fcd-9360-4f6d-a924-000000000003|17|<1,2,3>|<0,0,0>|<0,0,0,1>|F", l),
          "a key in front of the fields is not an identity");
    CHECK(!parseFields(std::string(GOOD) + "|a2e76fcd-9360-4f6d-a924-000000000003", l),
          "a sixth field (a key) is rejected");
    CHECK(!parseFields("17|<1,2,3>|<0,0,0>|<0,0,0,1>", l), "four fields rejected");
    CHECK(!parseFields("70000|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "seq out of range");
    CHECK(!parseFields("-1|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "negative seq");
    CHECK(!parseFields("1|<1,2>|<0,0,0>|<0,0,0,1>|", l), "short vector");
    CHECK(!parseFields("1|<1,2,3,4>|<0,0,0>|<0,0,0,1>|", l), "long vector");
    CHECK(!parseFields("1|<nan,2,3>|<0,0,0>|<0,0,0,1>|", l), "NaN position");
    CHECK(!parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,0>|", l), "zero rotation");
    CHECK(!parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,1>|Q", l), "unknown flag");
    CHECK(parseFields("1|<1,2,3>|<900,0,0>|<0,0,0,1>|", l) && l.vel.length() <= MAX_SPEED + 1e-9,
          "velocity clamped to MAX_SPEED");
    // The sender's numbers never follow the user's locale (a decimal comma breaks the tuple).
    CHECK(formatFixed(128.5, 3) == "128.500" && formatFixed(-1.5, 2) == "-1.50" && formatFixed(-0.0004, 3) == "0.000"
          && formatFixed(0.05, 3) == "0.050", "fixed-point formatting");
    const double p[3] = { 12.345, -0.5, 4000.25 };
    const double r[4] = { 0.0, 0.0, 0.70710678, 0.70710678 };
    const std::string sent = "9|" + formatTuple(p, 3, 3) + "|" + formatTuple(p, 3, 2) + "|" + formatTuple(r, 4, 4) + "|T";
    CHECK(parseFields(sent, l) && std::fabs(l.pos.x - 12.345) < 1e-9 && std::fabs(l.pos.z - 4000.25) < 1e-9,
          "what the sender formats parses back (%s)", sent.c_str());
    CHECK(!parseFields("1|<1,5,2,0,3,0>|<0,0,0>|<0,0,0,1>|", l), "decimal-comma numbers are rejected");
}

static void testBound()
{
    CHECK(withinBound(Vec3(10, 10, 10), Vec3(11.9, 10, 10)), "1.9 m is within the bound");
    CHECK(!withinBound(Vec3(10, 10, 10), Vec3(12.1, 10, 10)), "2.1 m is outside the bound");

    // Followed within 2 m: eases towards the shared position, no snap on the first frame.
    View v;
    Line l;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|T", l);
    v.setTarget(l, Vec3(101.5, 100, 20), 0.0);
    const Quat q;
    Vec3 off = v.step(Vec3(100, 100, 20), q, Vec3(), 0.0167, 0.0167);
    CHECK(off.x > 0.0 && off.x < 0.3, "first frame moves only part of the way (%.3f)", off.x);
    for (int i = 0; i < 120; ++i)
        off = v.step(Vec3(100, 100, 20), q, Vec3(), 0.0167 * (i + 2), 0.0167);
    CHECK(std::fabs(off.x - 1.5) < 0.01 && v.weight > 0.99, "settles on the shared position (%.3f)", off.x);

    // The sim moves the avatar so the shared position is 2.5 m away: rejected, eases back.
    off = v.step(Vec3(99, 100, 20), q, Vec3(), 2.1, 0.0167);
    CHECK(!v.hasTarget && v.rejected == 1, "over 2 m: the line is rejected");
    CHECK(off.x > 0.5, "no snap back either (%.3f)", off.x);
    for (int i = 0; i < 200; ++i)
        off = v.step(Vec3(99, 100, 20), q, Vec3(), 2.2 + 0.0167 * i, 0.0167);
    CHECK(off.length() == 0.0 && !v.busy(), "back on sim placement, idle");

    // A line that is far away from the start is never shown, not even partly.
    View w;
    w.setTarget(l, Vec3(110, 100, 20), 0.0);
    off = w.step(Vec3(100, 100, 20), q, Vec3(), 0.01, 0.0167);
    CHECK(off.length() == 0.0 && w.rejected == 1, "a 10 m line moves nothing");

    // Even mid-blend the offset never exceeds the bound.
    View x;
    x.offset = Vec3(5, 0, 0);
    x.weight = 1.0;
    off = x.step(Vec3(), q, Vec3(), 0.0, 0.0167);
    CHECK(off.length() <= BOUND + 1e-9, "offset clamped to the bound");

    // Seated: sim placement.
    View s;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|S", l);
    s.setTarget(l, Vec3(101, 100, 20), 0.0);
    off = s.step(Vec3(100, 100, 20), q, Vec3(), 0.01, 0.0167);
    CHECK(off.length() == 0.0, "a seated avatar keeps the sim's placement");

    // A "still" line goes stale when the sim moves the avatar on and no new line comes.
    View t;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|T", l);
    t.setTarget(l, Vec3(100.5, 100, 20), 0.0);
    for (int i = 0; i < 60; ++i)
        t.step(Vec3(100, 100, 20), q, Vec3(), 0.0167 * i, 0.0167);
    const double before = t.offset.x;
    for (int i = 0; i < 120; ++i)
        off = t.step(Vec3(100, 100, 20), q, Vec3(2, 0, 0), 1.0 + 0.0167 * i, 0.0167);
    CHECK(before > 0.4 && off.length() < 0.01, "stale still line released (%.3f -> %.3f)", before, off.x);
}

static void testTable()
{
    Table<int> table;
    for (int i = 0; i < (int)MAX_ENTRIES; ++i)
        CHECK(table.accept(i, 1, 0, 0.0), "room for avatar %d", i);
    CHECK(table.size() == MAX_ENTRIES, "table full");
    CHECK(!table.accept(1000, 1, 0, 1.0), "a newcomer waits while the table is full");
    CHECK(table.accept(5, 2, 0, 1.0), "a known avatar still updates");
    // Moving entries expire after EXPIRE_MOVING; the refreshed one (5) stays.
    int dropped = 0;
    table.expire(EXPIRE_MOVING + 0.5, [&](int) { ++dropped; });
    CHECK(dropped == (int)MAX_ENTRIES - 1 && table.size() == 1 && table.contains(5), "expired (%d)", dropped);
    CHECK(table.accept(1000, 1, 0, EXPIRE_MOVING + 0.6), "room again after expiry");
    // A still entry lives longer.
    Table<int> still;
    still.accept(1, 1, FLAG_STILL, 0.0);
    still.expire(EXPIRE_MOVING + 1.0, [](int) {});
    CHECK(still.contains(1), "a still entry outlives the moving expiry");
    still.expire(EXPIRE_STILL + 1.0, [](int) {});
    CHECK(!still.contains(1), "and expires after EXPIRE_STILL");
    // An expired (not yet swept) entry makes room for a newcomer.
    Table<int> full;
    for (int i = 0; i < (int)MAX_ENTRIES; ++i)
        full.accept(i, 1, 0, 0.0);
    CHECK(full.accept(999, 1, 0, EXPIRE_MOVING + 0.1), "an expired slot is reused");
}

static void testSeq()
{
    CHECK(seqNewer(2, 1) && !seqNewer(1, 2) && !seqNewer(5, 5), "plain order");
    CHECK(seqNewer(0, 65535) && seqNewer(3, 65530), "wraps");
    Table<int> t;
    CHECK(t.accept(1, 10, 0, 0.0), "first");
    CHECK(!t.accept(1, 10, 0, 0.1), "repeat dropped");
    CHECK(!t.accept(1, 9, 0, 0.2), "older dropped");
    CHECK(t.accept(1, 11, 0, 0.3), "newer used");
    CHECK(t.accept(1, 3, 0, 0.3 + RESYNC_AFTER + 0.1), "a restarted counter is taken after a quiet spell");
}

static void testShare()
{
    CHECK(shouldShare(true, false, true), "on, no RLV, bridge ready: send");
    CHECK(!shouldShare(false, false, true), "option off: nothing is sent");
    CHECK(!shouldShare(true, true, true), "an RLV restriction that hides position: nothing is sent");
    CHECK(!shouldShare(true, false, false), "no bridge: nothing is sent");
}

int main()
{
    testParse();
    testBound();
    testTable();
    testSeq();
    testShare();
    if (g_fail)
    {
        std::printf("%d position sync fixture(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
