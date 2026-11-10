// Bounded optional KDE display calibration. Never logs KScreen JSON or EDID.
#include "linden_common.h"
#include "vk_display_calibration.h"
#if LL_LINUX
#include "vk_display_calibration_kde.h"
#include "vk_display_calibration_child_policy.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <poll.h>
#include <atomic>
#include <condition_variable>
#include <linux/sched.h>
#include <memory>
#include <mutex>
#include <sys/syscall.h>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace vkdisplay
{
namespace
{
// At most one child can be outstanding, including an uninterruptible killed
// child whose pidfd reaper is still waiting. No numeric PID is ever signalled.
[[maybe_unused]] std::atomic<bool> sHelperActive{false};
struct ReapState
{
    std::mutex mutex;
    std::condition_variable ready;
    bool handedOff = false;
    bool terminate = false;
    int pidfd = -1;
};
struct ReapHandoff
{
    std::shared_ptr<ReapState> state;
    int pidfd = -1;
    bool terminate = false;
    ~ReapHandoff()
    {
        { std::lock_guard<std::mutex> lock(state->mutex); state->pidfd = pidfd;
          state->terminate = terminate; state->handedOff = true; }
        state->ready.notify_one();
    }
};
struct ScopedFd
{
    int fd = -1;
    ~ScopedFd() { if (fd >= 0) close(fd); }
};

bool readKscreen(std::string& data)
{
#if !defined(SYS_clone3) || !defined(SYS_pidfd_open) || !defined(SYS_pidfd_send_signal) || !defined(SYS_waitid) || !defined(SYS_close_range)
    return false; // Optional discovery requires kernel headers with safe pidfd lifecycle support.
#else
    // Fail before starting a helper if the sandbox/kernel cannot inspect and
    // signal pidfds. Self is not a child, so supported waitid returns ECHILD.
    ScopedFd selfFd{static_cast<int>(syscall(SYS_pidfd_open, getpid(), 0))};
    if (selfFd.fd < 0 || syscall(SYS_pidfd_send_signal, selfFd.fd, 0, nullptr, 0) < 0) return false;
    siginfo_t probe{};
    if (syscall(SYS_waitid, 3, selfFd.fd, &probe, WEXITED | WNOHANG | WNOWAIT, nullptr) != -1 || errno != ECHILD)
        return false;
    auto reap = std::make_shared<ReapState>();
    bool expected = false;
    if (!sHelperActive.compare_exchange_strong(expected, true)) return false;
    try
    {
        std::thread([reap]() {
            int fd;
            bool terminate;
            {
                std::unique_lock<std::mutex> lock(reap->mutex);
                reap->ready.wait(lock, [&] { return reap->handedOff; });
                fd = reap->pidfd;
                terminate = reap->terminate;
            }
            if (fd >= 0)
            {
                // Never drop the one-child gate on an unrecognized wait failure.
                // Retry off-thread until it is reaped or ECHILD proves ownership lost.
                for (;;)
                {
                    siginfo_t result{};
                    const long rc = syscall(SYS_waitid, 3, fd, &result, WEXITED | WNOHANG, nullptr);
                    const auto decision = classifyPidfdWait(rc, rc < 0 ? errno : 0, result.si_pid != 0);
                    if (releasePidfdGate(decision)) break;
                    if (decision == ChildWaitDecision::Running && terminate)
                        (void)syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                close(fd);
            }
            sHelperActive.store(false);
        }).detach();
    }
    catch (...) { sHelperActive.store(false); return false; }
    ReapHandoff handoff{reap};
    int raw[2];
    if (pipe2(raw, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    ScopedFd input{raw[0]}, output{raw[1]};
    if (input.fd < 3 || output.fd < 3) return false;
    const int flags = fcntl(output.fd, F_GETFL);
    if (flags < 0 || fcntl(output.fd, F_SETFL, flags & ~O_NONBLOCK) < 0) return false;
    int pidfd = -1;
    clone_args args{};
    args.flags = CLONE_PIDFD;
    args.pidfd = reinterpret_cast<uint64_t>(&pidfd);
    args.exit_signal = SIGCHLD;
    const long child = syscall(SYS_clone3, &args, sizeof(args));
    if (child == 0)
    {
        // clone3 is fork-like here. Only async-signal-safe operations before exec.
        if (dup2(output.fd, STDOUT_FILENO) < 0) _exit(126);
        const int nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
        if (syscall(SYS_close_range, 3u, ~0u, 0u) < 0) _exit(126);
        char executable[] = "/usr/bin/kscreen-doctor", arg[] = "-j";
        char* argv[] = {executable, arg, nullptr};
        execve(executable, argv, environ);
        _exit(127);
    }
    close(output.fd); output.fd = -1;
    if (child < 0 || pidfd < 0) return false;
    handoff.pidfd = pidfd;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    bool eof = false, exited = false, valid = true, owned = true, success = false;
    data.clear();
    auto inspectChild = [&]() {
        siginfo_t info{};
        const long rc = syscall(SYS_waitid, 3, pidfd, &info, WEXITED | WNOHANG | WNOWAIT, nullptr);
        if (rc < 0)
        {
            if (errno == EINTR) return;
            // Includes ECHILD: a foreign reaper may already own the status.
            owned = false; valid = false;
            return;
        }
        if (info.si_pid != 0) { exited = true; success = info.si_code == CLD_EXITED && info.si_status == 0; }
    };
    while (valid && !(eof && exited))
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { valid = false; break; }
        if (!exited) inspectChild();
        if (!valid) break;
        if (eof) { poll(nullptr, 0, static_cast<int>(std::min<int64_t>(remaining, 5))); continue; }
        pollfd fd{input.fd, POLLIN | POLLHUP, 0};
        const int ready = poll(&fd, 1, static_cast<int>(std::min<int64_t>(remaining, 25)));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (fd.revents & (POLLERR | POLLNVAL))) { valid = false; break; }
        if (ready == 0) continue;
        if (fd.revents & (POLLIN | POLLHUP))
        {
            char buffer[4096];
            const ssize_t count = read(input.fd, buffer, sizeof(buffer));
            if (count > 0)
            {
                if (data.size() + static_cast<size_t>(count) > 256u * 1024u) { valid = false; break; }
                try { data.append(buffer, static_cast<size_t>(count)); }
                catch (...) { valid = false; break; }
            }
            else if (count == 0) eof = true;
            else if (errno != EAGAIN && errno != EINTR) valid = false;
        }
    }
    if (!exited && owned)
    {
        // Re-check ownership; pidfd signaling cannot target a reused PID even if
        // another library reaps the child between this check and the signal.
        inspectChild();
        if (!exited && owned) (void)syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
    }
    handoff.terminate = !valid || !exited;
    return valid && exited && success;
#endif
}

bool connectedEdid(const std::string& connector, std::vector<uint8_t>& bytes)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it("/sys/class/drm", ec), end;
    if (ec) return false;
    fs::path selected;
    unsigned visited = 0, matches = 0;
    for (; it != end && !ec; it.increment(ec))
    {
        if (++visited > 512) return false;
        const std::string entry = it->path().filename().string();
        if (entry.rfind("card", 0) != 0) continue;
        size_t p = 4; while (p < entry.size() && entry[p] >= '0' && entry[p] <= '9') ++p;
        if (p == 4 || p >= entry.size() || entry[p] != '-' || entry.substr(p + 1) != connector) continue;
        std::ifstream status(it->path() / "status");
        std::array<char, 32> text{}; status.getline(text.data(), text.size());
        if (std::strcmp(text.data(), "connected") != 0) continue;
        selected = it->path() / "edid"; ++matches;
    }
    if (ec || matches != 1) return false;
    std::ifstream input(selected, std::ios::binary);
    if (!input) return false;
    std::array<char, 1025> data{};
    input.read(data.data(), data.size());
    const auto count = input.gcount();
    if (count < 128 || count > 1024) return false;
    bytes.assign(data.begin(), data.begin() + count);
    return true;
}
}

void queryKscreenLuminance(const LogicalBounds& bounds, ReportedLuminance& result)
{
    std::string json;
    if (!readKscreen(json)) return;
    KscreenMatch output;
    if (!matchKscreenOutput(json, bounds, output)) return;
    result.referenceWhiteNits = output.whiteNits;
    result.referenceSource = "KScreen current-output SDR brightness";
    std::vector<uint8_t> edid;
    float maximum = 0.f;
    if (connectedEdid(output.connector, edid) && ctaAdvertisedMaximum(edid, maximum))
    {
        result.peakNits = maximum;
        result.peakSource = "connected DRM EDID CTA advertised maximum";
    }
}
}
#endif
