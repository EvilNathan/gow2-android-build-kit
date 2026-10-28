// ANativeWindow ownership for the Vulkan GS backend.
#include "ps2x_host.h"

#include <mutex>

namespace ps2x::host {
namespace {

std::mutex g_windowMutex;
ANativeWindow* g_window = nullptr;
int g_surfaceWidth = 0;
int g_surfaceHeight = 0;

} // namespace

void AttachWindow(ANativeWindow* window)
{
    // Take our own reference before publishing so a concurrent release from the
    // UI thread cannot destroy the window under us.
    if (window != nullptr)
        ANativeWindow_acquire(window);

    ANativeWindow* previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowMutex);
        previous = g_window;
        g_window = window;
    }
    if (previous != nullptr)
        ANativeWindow_release(previous);

    if (window != nullptr) {
        const int width = ANativeWindow_getWidth(window);
        const int height = ANativeWindow_getHeight(window);
        SetSurfaceSize(width, height);
        Log("host/window", "surface attached %dx%d", width, height);
    } else {
        SetSurfaceSize(0, 0);
        Log("host/window", "surface detached");
    }
}

ANativeWindow* NativeWindow()
{
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return g_window;
}

void SetSurfaceSize(int width, int height)
{
    if (width < 0)
        width = 0;
    if (height < 0)
        height = 0;
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_surfaceWidth = width;
    g_surfaceHeight = height;
}

void GetSurfaceSize(int& width, int& height)
{
    std::lock_guard<std::mutex> lock(g_windowMutex);
    width = g_surfaceWidth;
    height = g_surfaceHeight;
}

} // namespace ps2x::host
