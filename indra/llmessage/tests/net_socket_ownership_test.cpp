/** Source-backed socket ownership regression. Link with the actual net.cpp and
 * LLHost dependencies. Uses IPv4 loopback only; no SOCKS or GPU is exercised. */
#include "linden_common.h"
#include "net.h"
#include "llhost.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <arpa/inet.h>
#endif

namespace
{
constexpr int iterations = 200;

// Evaluate operations in every build configuration; a failure terminates the
// process so another worker cannot remain blocked at a rendezvous.
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    std::abort(); \
} } while (false)

void worker(int index, std::barrier<>& rendezvous)
{
    int recv_socket = -1, send_socket = -1;
    int recv_port = NET_USE_OS_ASSIGNED_PORT, send_port = NET_USE_OS_ASSIGNED_PORT;
    CHECK(start_net(recv_socket, recv_port) == 0);
    CHECK(start_net(send_socket, send_port) == 0);
    const U32 loopback = ip_string_to_u32("127.0.0.1");
    const U32 formatted = ip_string_to_u32(index ? "10.20.30.40" : "192.168.1.2");
    const char* expected = index ? "10.20.30.40" : "192.168.1.2";
    std::array<char, NET_BUFFER_SIZE> data{};

    for (int round = 0; round < iterations; ++round)
    {
        const char payload = static_cast<char>('a' + index);
        rendezvous.arrive_and_wait();
        CHECK(send_packet(send_socket, &payload, 1, loopback, recv_port));
        NetReceiveInfo packet;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        S32 size = 0;
        do
        {
            size = receive_packet(recv_socket, data.data(), packet);
            if (!size) std::this_thread::yield();
        } while (!size && std::chrono::steady_clock::now() < deadline);
        CHECK(size == 1 && data[0] == payload);
        CHECK(packet.senderAddress == loopback && packet.senderPort == static_cast<U32>(send_port));
#if LL_LINUX
        CHECK(packet.receivingAddress == loopback);
#endif
        // Hold returned metadata and the legacy-format pointer across the other
        // thread's receive and formatter call.
        const char* text = u32_to_ip_string(formatted);
        rendezvous.arrive_and_wait();
        CHECK(std::strcmp(text, expected) == 0);
        CHECK(packet.senderPort == static_cast<U32>(send_port));
        rendezvous.arrive_and_wait();

        CHECK(send_packet(send_socket, &payload, 1, loopback, recv_port));
        size = 0;
        const auto legacy_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do
        {
            size = receive_packet(recv_socket, data.data());
            if (!size) std::this_thread::yield();
        } while (!size && std::chrono::steady_clock::now() < legacy_deadline);
        CHECK(size == 1 && data[0] == payload);
        rendezvous.arrive_and_wait();
        CHECK(get_sender_ip() == loopback && get_sender_port() == static_cast<U32>(send_port));
#if LL_LINUX
        CHECK(get_receiving_interface_ip() == loopback);
#endif
        rendezvous.arrive_and_wait();
    }

    // Preserve the 0 return for both empty reads and zero-length datagrams;
    // explicit metadata distinguishes the latter by its sender.
    NetReceiveInfo info{1, 2, 3};
    CHECK(receive_packet(recv_socket, data.data(), info) == 0);
#if LL_WINDOWS
    CHECK(WSAGetLastError() == WSAEWOULDBLOCK);
#else
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
#endif
    CHECK(info.senderAddress == 0 && info.senderPort == 0 && info.receivingAddress == 0);
    CHECK(receive_packet(recv_socket, data.data()) == 0);
    CHECK(get_sender_ip() == loopback && get_sender_port() == static_cast<U32>(send_port));
#if LL_LINUX
    CHECK(get_receiving_interface_ip() == 0);
#endif
    CHECK(send_packet(send_socket, "", 0, loopback, recv_port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        CHECK(receive_packet(recv_socket, data.data(), info) == 0);
        if (!info.senderPort) std::this_thread::yield();
    } while (!info.senderPort && std::chrono::steady_clock::now() < deadline);
    CHECK(info.senderAddress == loopback && info.senderPort == static_cast<U32>(send_port));
    end_net(send_socket);
    end_net(recv_socket);
}
}

int main()
{
    // An external runner should also enforce a process timeout. This watchdog
    // bounds a blocked socket call or rendezvous even without such a runner.
    std::atomic<bool> finished{false};
    std::thread watchdog([&finished]
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                std::fputs("socket ownership test timed out\n", stderr);
                std::abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::barrier rendezvous(2);
    std::thread first(worker, 0, std::ref(rendezvous));
    std::thread second(worker, 1, std::ref(rendezvous));
    first.join();
    second.join();
    finished.store(true, std::memory_order_release);
    watchdog.join();
}
