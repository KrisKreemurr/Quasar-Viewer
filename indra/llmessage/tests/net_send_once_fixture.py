#!/usr/bin/env python3
"""Generate a CPU fake-syscall test from the actual send_packet_once source.

Usage: python3 net_send_once_fixture.py OUTPUT.cpp
Compile/run OUTPUT.cpp twice with -DLL_WINDOWS=0 and -DLL_WINDOWS=1. The latter
exercises Windows error branching with a fake WSAGetLastError; it is not a
Windows ABI or live socket test. No compiler is invoked by this generator.
"""
from pathlib import Path
import hashlib
import sys

here = Path(__file__).resolve()
root = here.parents[1]
cpp = (root / "net.cpp").read_text()
header = (root / "net.h").read_text()

def block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for i in range(opening, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise ValueError(marker)

status = block(header, "enum class NetSendStatus") + ";"
result = block(header, "struct NetSendResult") + ";"
method = block(cpp, "NetSendResult send_packet_once(")
prologue = r'''
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using U32 = uint32_t;
static int injected_error = 0, calls = 0, fake_result = 0;
#if LL_WINDOWS
constexpr int WSAEWOULDBLOCK = 10035, WSAECONNRESET = 10054, WSAECONNREFUSED = 10061;
static int WSAGetLastError() { return injected_error; }
#endif
static int fake_sendto(int fd, const void* data, size_t size, int flags,
                       const sockaddr* dest, socklen_t destlen)
{
    ++calls;
    if (fd != 42 || !data || size != 3 || flags || destlen != sizeof(sockaddr_in)) std::abort();
    const auto* addr = reinterpret_cast<const sockaddr_in*>(dest);
    if (addr->sin_family != AF_INET || addr->sin_addr.s_addr != htonl(0x7f000001u)
        || ntohs(addr->sin_port) != 12345) std::abort();
    errno = injected_error;
    return fake_result;
}
#define sendto fake_sendto
'''
tests = r'''
#undef sendto
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"CHECK failed: %s\n", #x); std::abort(); } } while (false)
static void check(int syscall_result, int error, NetSendStatus expected)
{
    calls = 0; fake_result = syscall_result; injected_error = error;
    const NetSendResult actual = send_packet_once(42, "abc", 3, htonl(0x7f000001u), 12345);
    CHECK(calls == 1);
    CHECK(actual.status == expected);
    CHECK(actual.osError == (syscall_result >= 0 ? 0 : error));
}
int main()
{
    check(3, 0, NetSendStatus::Sent);
    check(0, 0, NetSendStatus::Sent);
#if LL_WINDOWS
    check(-1, WSAEWOULDBLOCK, NetSendStatus::WouldBlock);
    check(-1, WSAECONNRESET, NetSendStatus::Refused);
    check(-1, WSAECONNREFUSED, NetSendStatus::Refused);
#else
    check(-1, EAGAIN, NetSendStatus::WouldBlock);
    check(-1, EWOULDBLOCK, NetSendStatus::WouldBlock);
    check(-1, ECONNREFUSED, NetSendStatus::Refused);
    check(-1, ECONNRESET, NetSendStatus::Refused);
#endif
    check(-1, 1234, NetSendStatus::Failed);
}
'''
if len(sys.argv) != 2:
    raise SystemExit(__doc__)
output = Path(sys.argv[1])
output.write_text(prologue + "\n" + status + "\n" + result + "\n" + method + "\n" + tests)
print("net.cpp sha256", hashlib.sha256(cpp.encode()).hexdigest())
print("net.h sha256", hashlib.sha256(header.encode()).hexdigest())
print("fixture sha256", hashlib.sha256(output.read_bytes()).hexdigest())
