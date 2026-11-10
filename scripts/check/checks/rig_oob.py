"""Shader / source guards for the rigged pre-skin's allocation-full paths (2026-11-07).

Each guard keeps a full or lagging buffer from reading or writing outside its bounds:
  RC-ROW-GUARD      clean_cull.comp: every per-instance rig_cache state write is inside the
                    row's bounds test (the rows lag the instance table after a refused grow).
  JOINT-ROW-GUARD   clean_draw_rigged.vert: a pelvis-local range is placed only by a joint-table
                    row that exists (deferred joint-table grow); otherwise the draw is clipped.
  RIG-DROP-EMPTY    clean_cull.comp: a capacity-dropped work item has no vertices and destination
                    0, so no work-list consumer can reach past the skinned-vertex capacity.
  CACHE-BUMP-CLAMP  clean_cull.comp: the skin-cache bump pointer stops adding once past the arena.
  GROW-BACKOFF      llcleanrenderervk.cpp: a refused rigged grow backs off through
                    LLCleanRigSizing::growBackoffFrames (capped while the ladder is calm; the
                    rig-sizing fixture checks the cap), never a literal doubling.
Removing a guard (or rewriting the guarded code without it) fails this check.
"""
import re

NAME = "rig-oob"
STAGE = "source"
DESCRIPTION = "rigged pre-skin bounds guards (rig_cache rows, joint rows, dropped items, cache bump, backoff)"
GUARDS = ("Rigged parts placed by memory past a lagging joint table / state buffer, dropped work items "
          "pointing past the skinned-vertex capacity, an unbounded skin-cache bump, 80 s grow backoffs "
          "on a calm GPU.")

CULL = "indra/vkclean/shaders/clean_cull.comp"
VERT = "indra/vkclean/shaders/clean_draw_rigged.vert"
CPP = "indra/newview/vkgfx/llcleanrenderervk.cpp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    cull = _strip_comments(open(ctx.path(CULL), encoding="utf-8").read())
    vert = _strip_comments(open(ctx.path(VERT), encoding="utf-8").read())
    cpp = open(ctx.path(CPP), encoding="utf-8").read()
    bad = []

    # RC-ROW-GUARD: the state / source words cullOne writes for every rigged main-cull draw.
    if not re.search(r"const bool rc_row = cb \+ RC_WORDS <= uint\(rig_cache\.length\(\)\);\s*"
                     r"if \(rc_row\)\s*\{\s*rig_cache\[cb \+ 6u\] = 0u;\s*rig_cache\[cb \+ RC_SRC\] = src_base;",
                     cull):
        bad.append("RC-ROW-GUARD: clean_cull.comp's per-draw rig_cache state / RC_SRC writes are not inside "
                   "the row bounds test")
    # ... and the phase-2 drop's key invalidation.
    if not re.search(r"if \(rmode != 1u && cb \+ RC_WORDS <= uint\(rig_cache\.length\(\)\)\)\s*"
                     r"\{\s*rig_cache\[cb \+ 2u\] = 0xFFFFFFFFu;", cull):
        bad.append("RC-ROW-GUARD: clean_cull.comp's phase-2 drop invalidates the cache key without the row "
                   "bounds test")

    # JOINT-ROW-GUARD: every avatar_joints[slot] / prev_avatar_joints[slot] read in the rigged VS
    # follows a length test of slot against that table.
    for table in ("avatar_joints", "prev_avatar_joints"):
        for m in re.finditer(r"(?<![A-Za-z_])%s\[slot\]" % table, vert):
            before = vert[:m.start()]
            guard = re.search(r"slot (>=|<) uint\(%s\.length\(\)\)" % table, before)
            if not guard:
                line = vert.count("\n", 0, m.start()) + 1
                bad.append("JOINT-ROW-GUARD: clean_draw_rigged.vert reads %s[slot] (comment-stripped line ~%d) "
                           "without a %s.length() test" % (table, line, table))
    if not re.search(r"if \(slot >= uint\(avatar_joints\.length\(\)\)\)\s*\{\s*gl_Position = vec4\(0\.0, 0\.0, -2\.0, 1\.0\);"
                     r"\s*return;", vert):
        bad.append("JOINT-ROW-GUARD: a pelvis-local draw without its joint row is not clipped")

    # RIG-DROP-EMPTY
    if "w.vert_count = ok ? s.vert_count : 0u;" not in cull:
        bad.append("RIG-DROP-EMPTY: a dropped work item keeps its vertex count")
    if "w.dst_vert_base = ok ? dst : 0u;" not in cull:
        bad.append("RIG-DROP-EMPTY: a dropped per-frame work item keeps its past-capacity destination")

    # CACHE-BUMP-CLAMP: every add to the bump pointer is behind the past-capacity test.
    adds = re.findall(r"[^\n]*atomicAdd\(rig_alloc\.cache_used[^\n]*", cull)
    if not adds:
        bad.append("CACHE-BUMP-CLAMP: the skin-cache bump (atomicAdd(rig_alloc.cache_used, ...)) is gone; "
                   "update this check with its replacement")
    for a in adds:
        ctxt = cull[max(0, cull.find(a) - 200):cull.find(a) + len(a)]
        if not re.search(r"rig_alloc\.cache_used < pc\.u_cache_cap\s*\?\s*atomicAdd\(rig_alloc\.cache_used", ctxt):
            bad.append("CACHE-BUMP-CLAMP: an add to rig_alloc.cache_used is not behind the past-capacity test")

    # GROW-BACKOFF
    m = re.search(r"bool LLCleanRendererVK::ensureRiggedBuffers\(.*?\n\}\n", cpp, flags=re.S)
    if not m:
        bad.append("GROW-BACKOFF: ensureRiggedBuffers not found in llcleanrenderervk.cpp")
    else:
        body = m.group(0)
        if "LLCleanRigSizing::growBackoffFrames(" not in body:
            bad.append("GROW-BACKOFF: ensureRiggedBuffers does not back off through growBackoffFrames")
        if re.search(r"mRigGrowRetryFrame\s*=\s*\(uint32_t\)gFrameCount\s*\+\s*\(\s*\d+u?\s*<<", body):
            bad.append("GROW-BACKOFF: a literal doubling backoff is back in ensureRiggedBuffers (uncapped while calm)")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "5 guards present"
