// Pad state + rumble forwarding.
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace ps2x::host {
namespace {

std::atomic<uint16_t> g_buttons{PAD_BUTTON_MASK & 0u};
std::atomic<int8_t> g_lx{PAD_AXIS_CENTER};
std::atomic<int8_t> g_ly{PAD_AXIS_CENTER};
std::atomic<int8_t> g_rx{PAD_AXIS_CENTER};
std::atomic<int8_t> g_ry{PAD_AXIS_CENTER};

std::atomic<float> g_rumbleLow{0.0f};
std::atomic<float> g_rumbleHigh{0.0f};

int8_t ClampAxis(int value)
{
    if (value < PAD_AXIS_MIN)
        return PAD_AXIS_MIN;
    if (value > PAD_AXIS_MAX)
        return PAD_AXIS_MAX;
    return static_cast<int8_t>(value);
}

float ClampUnit(float value)
{
    if (!(value > 0.0f)) // also catches NaN
        return 0.0f;
    return value > 1.0f ? 1.0f : value;
}

} // namespace

void SetPadState(uint16_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry)
{
    g_buttons.store(static_cast<uint16_t>(buttons & PAD_BUTTON_MASK), std::memory_order_relaxed);
    g_lx.store(ClampAxis(lx), std::memory_order_relaxed);
    g_ly.store(ClampAxis(ly), std::memory_order_relaxed);
    g_rx.store(ClampAxis(rx), std::memory_order_relaxed);
    g_ry.store(ClampAxis(ry), std::memory_order_relaxed);
}

void GetPadState(uint16_t& buttons, int8_t& lx, int8_t& ly, int8_t& rx, int8_t& ry)
{
    buttons = g_buttons.load(std::memory_order_relaxed);
    lx = g_lx.load(std::memory_order_relaxed);
    ly = g_ly.load(std::memory_order_relaxed);
    rx = g_rx.load(std::memory_order_relaxed);
    ry = g_ry.load(std::memory_order_relaxed);
}

void SetRumble(float low, float high)
{
    low = ClampUnit(low);
    high = ClampUnit(high);

    const float previousLow = g_rumbleLow.exchange(low, std::memory_order_relaxed);
    const float previousHigh = g_rumbleHigh.exchange(high, std::memory_order_relaxed);

    // The engine may push actuator state every pad read; only cross the JNI
    // boundary when something audible actually changed.
    constexpr float kEpsilon = 0.02f;
    if (std::abs(low - previousLow) < kEpsilon && std::abs(high - previousHigh) < kEpsilon)
        return;

    detail::RumbleToJava(low, high);
}

} // namespace ps2x::host
