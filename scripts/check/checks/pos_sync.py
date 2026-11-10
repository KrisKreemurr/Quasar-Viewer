"""Regression guard for avatar position sync over the helper (#1417).

Two halves:
  * compiles scripts/check/test_pos_sync.cpp against indra/newview/slpossyncmath.h with the host
    C++ compiler and runs it: the body never carries identity (a key in it rejects the line),
    the receiver's 2 m rejection and ease back to sim placement, the per-avatar table's cap and
    expiry, sequence order, and the share option / RLV gate;
  * reads the helper script and the viewer source for what the fixture cannot see: the helper
    forwards a position for llGetOwnerKey of an attachment that spoke (never a key from the
    line), says our own position only while the viewer shares it, the [POS_CHANNEL] placeholder
    is filled in (a missing one stops the helper compiling, which kills the bridge for the
    session), the viewer takes the sender from the helper's prefix and sends only through the
    share gate, and the option exists, persists, defaults on and is in Preferences.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "pos-sync"
STAGE = "source"
DESCRIPTION = "position sync: sim identity, 2 m bound, table cap/expiry, share option (#1417)"
GUARDS = ("Another avatar moved by a forged or modified sender; a shared position drawn more than "
          "2 m from the sim's; unbounded per-avatar state; position sent with sharing off or under "
          "RLV location restrictions; a helper that fails to compile.")

LSL = "indra/newview/app_settings/slbridge/slipstream_helper.lsl"


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def lsl_function(text, name):
    """The body of an LSL function `name(...)` up to its closing brace at column 0."""
    m = re.search(r"^\w*\s*%s\s*\([^)]*\)\s*\n\{\n(.*?)\n\}\n" % re.escape(name), text, re.S | re.M)
    return m.group(1) if m else None


def check_lsl(ctx, result):
    text = read(ctx, LSL)
    heard = lsl_function(text, "posHeard")
    if heard is None:
        result.fail("%s: no posHeard() (the POS_CHANNEL receive path)" % LSL)
    else:
        if "attachedOwner(id)" not in heard:
            result.fail("%s: posHeard() must take the sender from attachedOwner(id) (the sim's "
                        "llGetOwnerKey of an attachment)" % LSL)
        if re.search(r"\(key\)\s*llList2String\s*\(\s*f", heard) or re.search(r"\(key\)\s*llGetSubString\s*\(\s*msg", heard):
            result.fail("%s: posHeard() reads a key from the line's body; identity comes from the sim only" % LSL)
        if not re.search(r'llOwnerSay\("#slb:pos/1\|"\s*\+\s*\(string\)owner\s*\+', heard):
            result.fail("%s: posHeard() must forward as \"#slb:pos/1|\" + (string)owner + ..." % LSL)
        if "llListFindList(gSee" not in heard:
            result.fail("%s: posHeard() must forward only avatars the viewer listed (gSee)" % LSL)
    if not re.search(r"if\s*\(channel == POS_CHANNEL\)\s*\{\s*posHeard\(id, msg\);", text):
        result.fail("%s: the POS_CHANNEL listen must go through posHeard()" % LSL)
    # driven() gate comes before the POS_CHANNEL branch in listen()
    listen = text[text.find("listen(integer channel"):]
    if listen.find("if (!driven())") < 0 or listen.find("channel == POS_CHANNEL") < listen.find("if (!driven())"):
        result.fail("%s: position lines must be forwarded only while the viewer drives the helper" % LSL)

    send = lsl_function(text, "posSend")
    if send is None:
        result.fail("%s: no posSend() (our own position)" % LSL)
    else:
        first = send.strip().split("\n")[0].strip()
        if first != "if (!gPosShare)":
            result.fail("%s: posSend() must start with the share gate (if (!gPosShare) return;)" % LSL)
        if "llGetPos()" not in send or "POS_BOUND" not in send:
            result.fail("%s: posSend() must clamp against llGetPos() within POS_BOUND" % LSL)
    shouts = [m.start() for m in re.finditer(r"llShout\s*\(\s*POS_CHANNEL|llRegionSay\s*\(\s*POS_CHANNEL|llSay\s*\(\s*POS_CHANNEL", text)]
    if send is not None:
        lo = text.find(send)
        hi = lo + len(send)
        for at in shouts:
            if not lo <= at < hi:
                result.fail("%s:%d: POS_CHANNEL is spoken on outside posSend() (bypasses the share gate)"
                            % (LSL, text.count("\n", 0, at) + 1))
    if not re.search(r"POS_BOUND\s*=\s*2\.0\s*;", text):
        result.fail("%s: POS_BOUND must be 2.0 m" % LSL)
    if "gPosShare = FALSE;" not in (lsl_function(text, "start") or ""):
        result.fail("%s: start() must begin with sharing off (gPosShare = FALSE) until the viewer says so" % LSL)

    bridge = read(ctx, "indra/newview/slbridge.cpp")
    if "[POS_CHANNEL]" in text and '"[POS_CHANNEL]"' not in bridge:
        result.fail("slbridge.cpp: loadScriptSource() does not fill in [POS_CHANNEL]; the helper would not compile")


def check_viewer(ctx, result):
    src = read(ctx, "indra/newview/slpossync.cpp")
    m = re.search(r"void SLPosSync::onLine\(const std::string& rest\)\s*\{(.*?)\n\}", src, re.S)
    if not m:
        result.fail("slpossync.cpp: no SLPosSync::onLine")
    else:
        body = m.group(1)
        if "rest.substr(0, bar)" not in body or "parseFields(rest.substr(bar + 1)" not in body:
            result.fail("slpossync.cpp: onLine must take the sender from the helper's prefix and parse "
                        "only the fields after it")
        if "withinBound(" not in body:
            result.fail("slpossync.cpp: onLine must reject a line more than 2 m from the sim's position")
    m = re.search(r"void SLPosSync::tickSend\(\)\s*\{(.*?)\n\}", src, re.S)
    if not m or "shouldShare(isShareOptionOn(), rlvHidesPosition()" not in m.group(1):
        result.fail("slpossync.cpp: tickSend must gate on shouldShare(isShareOptionOn(), rlvHidesPosition(), ...)")
    elif m.group(1).find("if (!share") < 0 or m.group(1).find("if (!share") > m.group(1).find("sendLatest("):
        result.fail("slpossync.cpp: tickSend must return before sending when sharing is off")
    if len(re.findall(r'"pos/send/1\|', src)) != 1:
        result.fail("slpossync.cpp: pos/send/1 must be built in exactly one place (tickSend)")
    if 'gSavedSettings, "SLPosSyncShare"' not in src:
        result.fail("slpossync.cpp: isShareOptionOn must read SLPosSyncShare")
    for bhvr in ("canShowLocation", "RLV_BHVR_SHOWNAMES", "canShowNearbyAgents"):
        if bhvr not in src:
            result.fail("slpossync.cpp: rlvHidesPosition must check %s" % bhvr)

    settings = read(ctx, "indra/newview/app_settings/settings.xml")
    m = re.search(r"<key>SLPosSyncShare</key>\s*<map>(.*?)</map>", settings, re.S)
    if not m:
        result.fail("settings.xml: no SLPosSyncShare")
    else:
        if not re.search(r"<key>Persist</key>\s*<integer>1</integer>", m.group(1)):
            result.fail("settings.xml: SLPosSyncShare must persist")
        if not re.search(r"<key>Value</key>\s*<integer>1</integer>", m.group(1)):
            result.fail("settings.xml: SLPosSyncShare must default on")
    panel = read(ctx, "indra/newview/skins/default/xui/en/panel_preferences_privacy.xml")
    if 'control_name="SLPosSyncShare"' not in panel:
        result.fail("panel_preferences_privacy.xml: no check box for SLPosSyncShare")

    avatar = read(ctx, "indra/newview/llvoavatar.cpp")
    if "SLPosSync::rootOffset(" not in avatar:
        result.fail("llvoavatar.cpp: the shared position is not applied (SLPosSync::rootOffset)")


def run_fixture(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: position sync fixtures not run")
        return False
    src = ctx.path("scripts/check/test_pos_sync.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_pos_sync")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return False
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
    return True


def run(ctx, result):
    for rel in (LSL, "indra/newview/slpossync.cpp", "indra/newview/slpossyncmath.h",
                "scripts/check/test_pos_sync.cpp"):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing (position sync, #1417)" % rel)
            result.summary = "position sync missing"
            return
    ran = run_fixture(ctx, result)
    check_lsl(ctx, result)
    check_viewer(ctx, result)
    result.summary = ("fixtures + helper/viewer source rules" if ran else "helper/viewer source rules (no compiler)")
