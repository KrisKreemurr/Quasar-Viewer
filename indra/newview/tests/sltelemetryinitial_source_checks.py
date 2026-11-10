#!/usr/bin/env python3
"""Source-bound ordering checks for the production telemetry frameTick path."""
from pathlib import Path
import argparse


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated production function: {signature}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    args = parser.parse_args()
    source = (args.source_root / "indra/newview/sltelemetry.cpp").read_text()
    tick = function_body(source, "void frameTick(")
    schedule = function_body(source, "bool schedule(unsigned action, bool wantSend, const PolledBaselines*")

    assert "initialReadyBeforeSample" not in tick
    align = tick.index("sCollectionClock.beforeSample(sWindow,now,flushWindow)")
    hour_cap = tick.index("if(sWindow.seconds>=3600)")
    frame_sample = tick.index("sWindow.frames[frameClass].add(milliseconds)")
    renderer_poll = tick.index("const auto renderer=rendererSample()")
    allocation_poll = tick.index("sWindow.allocFailures[i]=delta")
    cache_poll = tick.index("cacheTotals={cache->getStoreReadHits()")
    data_poll = tick.index("sWindow.dataMBLog2[i]=logCount")
    sampled_snapshot = tick.index("sampledBaselines={allocation,cacheTotals,data}")
    initial_seal = tick.index("sInitialSegment.tryHandoff")
    assert align < hour_cap < frame_sample < renderer_poll < allocation_poll < cache_poll < data_poll < sampled_snapshot < initial_seal
    assert "schedule(DISK_ACTION_INITIAL, true, &sampledBaselines)" in tick
    assert tick.index("if (!sNeedInitialize && sampledBaselinesReady") < initial_seal
    assert "secondsUntil(sNextSend) <= 0" in schedule
    assert "if (action == DISK_ACTION_INITIAL && !initialBaselines) return false;" in schedule
    assert "if (action == 3) sNeedInitialize = false;" in schedule
    assert "if (action == DISK_ACTION_INITIAL) resetBaselines(initialBaselines);" in schedule
    print("PASS: production frameTick aligns hour and caps old windows before sampling; initial seals after due renderer/counter polling")


if __name__ == "__main__":
    main()
