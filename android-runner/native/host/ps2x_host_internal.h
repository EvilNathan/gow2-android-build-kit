// Internal glue between the host translation units.  Not part of the engine
// contract; engine patches include ps2x_host.h only.
#ifndef PS2X_HOST_INTERNAL_H
#define PS2X_HOST_INTERNAL_H

#include <cstddef>

namespace ps2x::host::detail {

// --- lifecycle.cpp ---------------------------------------------------------
// Unblocks GameFrame()/WaitFrame() for shutdown (nativeStop).  Once set,
// WaitFrame() returns immediately and SetPaused(false) no longer clears it.
void RequestShutdown();
[[nodiscard]] bool ShutdownRequested();
// Clears the pause/exit/shutdown gates for a new game session (nativeStart).
void ResetSession();

// --- storage.cpp -----------------------------------------------------------
// Records the app files directory (public FilesDir() returns it).
void SetFilesDir(const char* directory);

// --- disc.cpp --------------------------------------------------------------
// Result of the last ExtractDiscFromIso() call, for nativeExtractDisc()'s JSON.
struct DiscExtractInfo
{
    bool ok = false;
    bool sha1Matches = false;
    char error[256] = {0};
    char elfName[128] = {0};
    char elfSha1[64] = {0};
};

[[nodiscard]] const DiscExtractInfo& LastDiscExtract();

// --- log.cpp ---------------------------------------------------------------
// Called by Log(); keeps the last 256 formatted lines for crash dumps.
void LogRingPush(const char* line);
// Copies the ring buffer (newest last) into dst; returns bytes written.
size_t LogRingDump(char* dst, size_t cap);
// "<files>/logs" once StartLog() ran, otherwise an empty string.
[[nodiscard]] const char* LogDirectory();

// --- crash.cpp -------------------------------------------------------------
// Installs the SIGSEGV/SIGABRT/SIGBUS/SIGILL handlers (sigaltstack + unwind).
void CrashInstall();
// Writes a crash-style report (no signal raised); used by std::terminate and by
// the nativeCrashTest() export.
void CrashWriteReport(int sig, const char* reason);

// --- jni_bridge.cpp --------------------------------------------------------
// Forwards rumble to the Java vibrator (NativeBridge.dispatchRumble).  Safe to
// call from any thread; silently does nothing when Java is not attached.
void RumbleToJava(float low, float high);

} // namespace ps2x::host::detail

#endif // PS2X_HOST_INTERNAL_H
