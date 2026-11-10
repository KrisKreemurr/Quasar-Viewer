"""Regression guard: pipeline-cache driver calls go through the driver gate; unattributed crashes never strike.

Intel Windows (igvk64, Arc A750 driver 101.8991, #1490 / #1491) crashed at login with an access violation
on a 'VK pipeline batch' thread that held NO compile crumb, while another thread was 0.0 s into a (gated)
create. Every create was already serialized (#1334); the unserialized driver call was the progress save's
vkGetPipelineCacheData on the ~80 MB shared cache, run by a compile worker beside that create. The fix:
  * vkGetPipelineCacheData / vkMergePipelineCaches / vkCreatePipelineCache / vkDestroyPipelineCache are never
    called raw under indra/newview: the wrappers in vk_pipeline.cpp (vkPipelineCache*Gated) hold
    VkPipelineDriverGate (low priority) plus a driver-op crumb around the one driver call;
  * vkpipecache::save logs "save start" / "save end", so a crash inside a save is visible next time;
  * a crash whose thread holds no pipeline crumb writes the in-flight names to the marker's 'unattributed'
    line; the next start logs them and never counts a quarantine strike against them.
Old code (raw calls, no wrappers, no 'unattributed' line) fails every part. A raw call that genuinely must stay
(a dev tool, a wrapper body) opts out with the comment  // cache-gate-ok  (tools/ is not scanned).
"""
import os
import re

NAME = "pipeline-cache-gate"
STAGE = "source"
DESCRIPTION = "pipeline-cache driver calls hold the driver gate; unattributed crashes are candidates, not strikes"
GUARDS = ("Intel Windows igvk64 access violation at login from a pipeline-cache read (vkGetPipelineCacheData) "
          "beside a gated create, which left no compile crumb and so no quarantine (#1490 / #1491, #1334)")

RAW = re.compile(r"\bvk(?:GetPipelineCacheData|MergePipelineCaches|CreatePipelineCache|DestroyPipelineCache)\s*\(")
EXTS = (".cpp", ".h", ".hpp", ".inl")
WRAPPERS = ("vkPipelineCacheGetDataGated", "vkPipelineCacheMergeGated", "vkPipelineCacheCreateGated",
            "vkPipelineCacheDestroyGated")


def strip(line):
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    return line.split("//", 1)[0]


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def body_of(src, name):
    """The brace-balanced body of the definition of `name` (the one followed by a '{'), or None."""
    for m in re.finditer(r"\b" + name + r"\s*\([^;{)]*\)\s*\{", src):
        depth, i = 0, m.end() - 1
        while i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            if depth == 0:
                return src[m.start():i + 1]
            i += 1
    return None


def run(ctx, result):
    scanned = 0
    for base, dirs, files in os.walk(ctx.path("indra", "newview")):
        dirs[:] = [d for d in dirs if d not in ("tools", "third_party") and not d.startswith("build-")]
        for f in files:
            if not f.endswith(EXTS):
                continue
            scanned += 1
            path = os.path.join(base, f)
            in_block = False
            for n, raw in enumerate(read(path).splitlines(), 1):
                text = raw.strip()
                if in_block:
                    in_block = "*/" not in text
                    continue
                if text.startswith("/*"):
                    in_block = "*/" not in text
                    continue
                if text.startswith("*") or "cache-gate-ok" in raw:
                    continue
                if RAW.search(strip(raw)):
                    result.fail("%s:%d calls a pipeline-cache driver function directly; use vkPipelineCache*Gated "
                                "(vk_pipeline.h): %s" % (ctx.relative(path), n, text[:100]))

    vkdir = ctx.path("indra", "newview", "vkgfx")
    pipeline = read(os.path.join(vkdir, "vk_pipeline.cpp"))
    scope = re.search(r"struct CacheOpScope\s*\{.*?\n\};", pipeline, re.S)
    if not scope or "VkPipelineDriverGate" not in scope.group(0) or "VkPipelineCompileCrumb" not in scope.group(0):
        result.fail("vk_pipeline.cpp: CacheOpScope (gate + driver-op crumb) is missing")
    for name in WRAPPERS:
        body = body_of(pipeline, name)
        if body is None:
            result.fail("vk_pipeline.cpp: %s is not defined" % name)
        elif "CacheOpScope" not in body:
            result.fail("vk_pipeline.cpp: %s does not hold the driver gate (CacheOpScope)" % name)
    if "unattributed " not in pipeline:
        result.fail("vk_pipeline.cpp: the crash marker has no 'unattributed' line for a crash without a crumb")

    cache = read(os.path.join(vkdir, "vk_pipecache.cpp"))
    save = body_of(cache, "vkpipecache::save")
    if save is None or "save start" not in save or "save end" not in save:
        result.fail("vk_pipecache.cpp: vkpipecache::save does not log 'save start' / 'save end'")

    app = read(ctx.path("indra", "newview", "llappviewer.cpp"))
    if "crash_candidates" not in app:
        result.fail("llappviewer.cpp: the next start does not read the marker's unattributed candidates")
    for m in re.finditer(r"updatePipelineQuarantine\s*\(([^;]*)\)\s*;", app):
        if "candidate" in m.group(1):
            result.fail("llappviewer.cpp: unattributed candidates reach updatePipelineQuarantine (a strike)")

    result.summary = "%d files scanned; %d wrappers" % (scanned, len(WRAPPERS))
