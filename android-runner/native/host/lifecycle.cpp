// Pause gate, exit gate, 60 Hz tick clock and frame pacing.
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace ps2x::host {
namespace {

using SteadyClock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr int64_t kTickHz = HOST_REFRESH_HZ;
constexpr int64_t kTickPeriodNs = 1000000000LL / kTickHz;

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<int> g_sdkInt{0};
bool g_paused = false;
bool g_shutdown = false;
bool g_exitRequested = false;

// Tick clock: g_anchorTick ticks had elapsed at g_anchorTime.  Pausing freezes
// the counter (anchorTick advanced, anchorTime reset on resume) so a long
// background pause never turns into a burst of catch-up frames and the counter
// stays monotonic.
uint64_t g_anchorTick = 0;
SteadyClock::time_point g_anchorTime = SteadyClock::now();

bool UnblockedLocked()
{
    return g_shutdown || g_exitRequested;
}

// Must be called with g_mutex held.
uint64_t TickLocked()
{
    if (g_paused)
        return g_anchorTick;
    const int64_t elapsed = std::chrono::duration_cast<Nanoseconds>(SteadyClock::now() - g_anchorTime).count();
    if (elapsed <= 0)
        return g_anchorTick;
    return g_anchorTick + static_cast<uint64_t>((elapsed * kTickHz) / 1000000000LL);
}

} // namespace

// Android API level, reported by the Java host in nativeStart().  Logged at
// session start and used by patches that need a version gate.
void SetSdkInt(int sdk)
{
    std::atomic_store(&g_sdkInt, sdk);
}

int GetSdkInt()
{
    return std::atomic_load(&g_sdkInt);
}

void SetPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (UnblockedLocked()) {
        // Shutdown/exit wins: stay parked so the game thread can leave.
        if (!g_paused) {
            g_anchorTick = TickLocked();
            g_paused = true;
        }
        g_cv.notify_all();
        return;
    }
    if (paused == g_paused)
        return;

    if (paused) {
        g_anchorTick = TickLocked();
        g_paused = true;
        Log("host/lifecycle", "paused at tick %llu", static_cast<unsigned long long>(g_anchorTick));
    } else {
        g_anchorTime = SteadyClock::now();
        g_paused = false;
        Log("host/lifecycle", "resumed at tick %llu", static_cast<unsigned long long>(g_anchorTick));
    }
    g_cv.notify_all();
}

bool IsPaused()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_paused;
}

uint64_t VsyncTick()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return TickLocked();
}

void WaitFrame()
{
    const auto period = Nanoseconds(kTickPeriodNs);
    std::unique_lock<std::mutex> lock(g_mutex);

    for (;;) {
        if (UnblockedLocked())
            return;
        if (g_paused) {
            g_cv.wait(lock, [&] { return !g_paused || UnblockedLocked(); });
            continue;
        }

        const auto now = SteadyClock::now();
        const auto elapsed = std::chrono::duration_cast<Nanoseconds>(now - g_anchorTime).count();
        const int64_t ticks = elapsed > 0 ? elapsed / kTickPeriodNs : 0;
        const auto next = g_anchorTime + period * (ticks + 1);

        g_cv.wait_until(lock, next, [&] { return g_paused || UnblockedLocked(); });
        if (UnblockedLocked())
            return;
        if (g_paused)
            continue;
        if (SteadyClock::now() >= next)
            return;
    }
}

void GameFrame()
{
    if (ShouldExit())
        return;
    WaitFrame();
}

void RequestExit()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_exitRequested) {
            g_exitRequested = true;
            Log("host/lifecycle", "exit requested");
        }
    }
    g_cv.notify_all();
}

bool ShouldExit()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_shutdown || g_exitRequested;
}

namespace detail {

void RequestShutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_shutdown = true;
        if (!g_paused) {
            g_anchorTick = TickLocked();
            g_paused = true;
        }
    }
    g_cv.notify_all();
}

bool ShutdownRequested()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_shutdown;
}

void ResetSession()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_shutdown = false;
        g_exitRequested = false;
        g_paused = false;
        g_anchorTick = 0;
        g_anchorTime = SteadyClock::now();
    }
    g_cv.notify_all();
}

} // namespace detail

} // namespace ps2x::host
