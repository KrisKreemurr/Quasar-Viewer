#!/usr/bin/env python3
"""#1435 source checks: LLViewerFetchedTexture uses LLTextureRefetchPolicy.

A failed re-fetch of a texture that delivered data earlier is retried after a delay instead of
being marked missing (drawn white for the rest of the session). The policy itself is tested by
texture_refetch_policy_checks.cpp; this checks the wiring.

    python3 indra/newview/tests/texture_refetch_checks.py [--source-root .]
"""
import argparse
import pathlib
import re
import sys


def body(src, signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    raise AssertionError("unbalanced " + signature)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", default=".")
    root = pathlib.Path(ap.parse_args().source_root)
    cpp = (root / "indra/newview/llviewertexture.cpp").read_text()
    hdr = (root / "indra/newview/llviewertexture.h").read_text()
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    check('#include "lltexturerefetchpolicy.h"' in cpp, "llviewertexture.cpp includes the policy")
    for member in ("mFetchedOnce", "mRefetchFailures", "mRefetchRetryAt"):
        check(re.search(r"\b" + member + r"\b", hdr), "member " + member + " declared")

    pfr = body(cpp, "bool LLViewerFetchedTexture::processFetchResults(")
    # Data arrived: remembered, failures reset.
    good = pfr[pfr.index("vftuf - data good"):]
    check(re.search(r"mFetchedOnce\s*=\s*true", good), "data arrival sets mFetchedOnce")
    check(re.search(r"mRefetchFailures\s*=\s*0", good), "data arrival resets mRefetchFailures")
    # The no-data failure branch asks the policy before marking missing.
    fail = pfr[pfr.index("We finished but received no data"):]
    ask = fail.find("retryInsteadOfMissing(")
    missing = fail.find("setIsMissingAsset()")
    check(ask >= 0 and missing >= 0 and ask < missing, "failure branch asks the policy before setIsMissingAsset")
    check("retryDelaySeconds(" in fail, "failure branch schedules the retry with retryDelaySeconds")
    check(re.search(r"FTT_DEFAULT", fail[:missing if missing >= 0 else None]), "only FTT_DEFAULT textures are kept")

    uf = body(cpp, "bool LLViewerFetchedTexture::updateFetch(")
    gate = uf.find("mayRequest(")
    create = uf.find("createRequest(")
    check(gate >= 0 and create >= 0 and gate < create, "updateFetch gates new requests with mayRequest before createRequest")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("texture_refetch_checks: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
