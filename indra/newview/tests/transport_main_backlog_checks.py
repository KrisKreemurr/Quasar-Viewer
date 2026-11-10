#!/usr/bin/env python3
"""Compile actual main backlog consumers with deterministic transport endpoints."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = args.source.resolve()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)

def between(text, start, end):
    assert text.count(start) == 1, start
    return text.split(start, 1)[1].split(end, 1)[0]

app = (source / "indra/newview/llappviewer.cpp").read_text()
startup = (source / "indra/newview/llstartup.cpp").read_text()
app_block = "LLTransportBacklog backlog;" + between(app,
    "            LLTransportBacklog backlog;", "            // Handle per-frame message system processing.")
startup_block = "LLTransportBacklog backlog;" + between(startup,
    "        LLTransportBacklog backlog;", "        lmc.processAcks();")
throttle_block = "LLTransportBacklog throttle_backlog;" + between(app,
    "    LLTransportBacklog throttle_backlog;", "    // Check that the circuit")
fixture = r'''#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
struct LLTransportBacklog { uint32_t pendingPackets=0; float loadRate=0; };
struct Endpoint {
    bool active=false, known=true, ingress=false; uint32_t packets=0;
    int drains=0, drainResult=0; float load=0;
    bool transportOwnerActive() const { return active; }
    bool getTransportBacklog(LLTransportBacklog& out) {
        if (!known) return false;
        out.pendingPackets=packets;out.loadRate=load;return true;
    }
    bool getTransportIngressPendingOnMain(bool& out) {
        if (!known) return false;
        out=ingress; return true;
    }
    int drainUdpSocket() { REQUIRE(!active);++drains;return drainResult; }
} *gMessageSystem;
struct Throttle { float load=0.25f;int updates=0;
    void setBufferLoadRate(float value) { load=value; }
    void updateDynamicThrottle() { ++updates; }
} gViewerThrottle;
constexpr float CHECK_MESSAGES_CEILING_MAX_TIME=0.005f;
constexpr float CHECK_MESSAGES_DEFAULT_MAX_TIME=0.002f;
static float CheckMessagesMaxTime=0.003f;
template<class T> T llmin(T a,T b) { return std::min(a,b); }
static void mainPump(bool needs_drain) { auto* network_message=gMessageSystem; APP_BLOCK }
static void startupPump(bool needs_drain) { auto* network_message=gMessageSystem; STARTUP_BLOCK }
static void throttlePump() { THROTTLE_BLOCK }
int main() {
    Endpoint endpoint;gMessageSystem=&endpoint;
    // APP readiness is independent of the ring count used for throttling.
    endpoint.active=true;endpoint.packets=0;endpoint.ingress=true;
    mainPump(false);REQUIRE(endpoint.drains==0);REQUIRE(CheckMessagesMaxTime>0.003f);
    CheckMessagesMaxTime=0.004f;endpoint.known=false;
    mainPump(false);REQUIRE(CheckMessagesMaxTime==0.004f);REQUIRE(endpoint.drains==0);
    mainPump(true);REQUIRE(CheckMessagesMaxTime==0.004f);REQUIRE(endpoint.drains==0);
    endpoint.known=true;endpoint.packets=0;endpoint.ingress=false;
    mainPump(false);REQUIRE(CheckMessagesMaxTime==CHECK_MESSAGES_DEFAULT_MAX_TIME);
    endpoint.active=false;endpoint.packets=4;endpoint.drainResult=5;
    mainPump(false);REQUIRE(endpoint.drains==1);REQUIRE(CheckMessagesMaxTime>CHECK_MESSAGES_DEFAULT_MAX_TIME);
    CheckMessagesMaxTime=0.004f;endpoint.packets=0;endpoint.drainResult=0;
    mainPump(true);REQUIRE(endpoint.drains==2);REQUIRE(CheckMessagesMaxTime==0.004f);
    mainPump(false);REQUIRE(CheckMessagesMaxTime==CHECK_MESSAGES_DEFAULT_MAX_TIME);
    endpoint.active=true;endpoint.known=false;
    startupPump(true);REQUIRE(endpoint.drains==2);
    endpoint.known=true;endpoint.packets=8;startupPump(false);REQUIRE(endpoint.drains==2);
    endpoint.active=false;startupPump(false);REQUIRE(endpoint.drains==3);
    endpoint.active=true;endpoint.known=false;throttlePump();
    REQUIRE(gViewerThrottle.load==0.25f);REQUIRE(gViewerThrottle.updates==0);
    endpoint.known=true;endpoint.load=0.75f;throttlePump();
    REQUIRE(gViewerThrottle.load==0.75f);REQUIRE(gViewerThrottle.updates==1);
    endpoint.active=false;endpoint.known=false;throttlePump();
    REQUIRE(gViewerThrottle.load==0.75f);REQUIRE(gViewerThrottle.updates==2);
    std::puts("actual main backlog consumers: PASS");
}
'''
fixture = fixture.replace("APP_BLOCK", app_block).replace("STARTUP_BLOCK", startup_block).replace("THROTTLE_BLOCK", throttle_block)
cpp = out / "actual_consumers.cpp"
cpp.write_text(fixture)
env = os.environ.copy()
for key in ("TMPDIR", "TMP", "TEMP", "XDG_CACHE_HOME"):
    env[key] = str(out)
env["CCACHE_DISABLE"] = "1"
commands = [[os.environ.get("CXX", "/usr/bin/c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(out / "checks")], [str(out / "checks")]]
(out / "commands.json").write_text(json.dumps(commands, indent=2))
for command in commands:
    subprocess.run(command, cwd=out, env=env, check=True)
