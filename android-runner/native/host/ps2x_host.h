// PS2Recomp Android host API — God of War II (PS2) static-recomp port.
//
// This header is the contract between the engine patches (PS2Recomp) and the
// Android host implemented under android-runner/native/host/.  It must stay
// dependency-free: C++20, NDK headers only, no engine headers.
//
// Threading model
//   * AttachWindow/SetSurfaceSize come from the Android UI thread; safe to call
//     at any time.  The GS thread reads the window through NativeWindow().
//   * GameFrame/WaitFrame/VsyncTick are called by the game thread.
//   * SetPadState may come from the UI thread (touch) and from Java (gamepad).
//   * StartAudio/StopAudio come from the game thread; the fill callback runs on
//     an AAudio realtime thread.
//
// All entry points are noexcept-by-construction: none of them throw.
#ifndef PS2X_HOST_H
#define PS2X_HOST_H

#include <android/native_window.h>

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__clang__) || defined(__GNUC__)
#define PS2X_HOST_PRINTF(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))
#else
#define PS2X_HOST_PRINTF(fmtIndex, argIndex)
#endif

namespace ps2x::host {

// ---------------------------------------------------------------------------
// Pad button bits: PS2 DualShock 2 serial layout, ACTIVE-HIGH (bit set means
// "held"), i.e. the host reports pressed buttons and the engine inverts.
//
//   0x0001 SELECT      0x0100 L2
//   0x0002 L3          0x0200 R2
//   0x0004 R3          0x0400 L1
//   0x0008 START       0x0800 R1
//   0x0010 UP          0x1000 TRIANGLE
//   0x0020 RIGHT       0x2000 CIRCLE
//   0x0040 DOWN        0x4000 CROSS
//   0x0080 LEFT        0x8000 SQUARE
//
// The engine stores its own mask active-low (0xFFFF == nothing held), so a patch
// feeding the kernel pad state must invert:
//   engineMask = ps2x::host::PadActiveLowWord(hostButtons);
// ---------------------------------------------------------------------------
inline constexpr uint16_t PAD_SELECT   = 0x0001u;
inline constexpr uint16_t PAD_L3       = 0x0002u;
inline constexpr uint16_t PAD_R3       = 0x0004u;
inline constexpr uint16_t PAD_START    = 0x0008u;
inline constexpr uint16_t PAD_UP       = 0x0010u;
inline constexpr uint16_t PAD_RIGHT    = 0x0020u;
inline constexpr uint16_t PAD_DOWN     = 0x0040u;
inline constexpr uint16_t PAD_LEFT     = 0x0080u;
inline constexpr uint16_t PAD_L2       = 0x0100u;
inline constexpr uint16_t PAD_R2       = 0x0200u;
inline constexpr uint16_t PAD_L1       = 0x0400u;
inline constexpr uint16_t PAD_R1       = 0x0800u;
inline constexpr uint16_t PAD_TRIANGLE = 0x1000u;
inline constexpr uint16_t PAD_CIRCLE   = 0x2000u;
inline constexpr uint16_t PAD_CROSS    = 0x4000u;
inline constexpr uint16_t PAD_SQUARE   = 0x8000u;

inline constexpr uint16_t PAD_BUTTON_MASK = 0xFFFFu;

// Analog axes are -128..127 with 0 == centre (positive X right, positive Y down,
// as on the DualShock).  The engine converts to its 0x80-centred bytes itself;
// the host does not remap them.
inline constexpr int8_t PAD_AXIS_MIN    = -128;
inline constexpr int8_t PAD_AXIS_MAX    = 127;
inline constexpr int8_t PAD_AXIS_CENTER = 0;

// Helper for engine patches: active-high host word -> active-low engine mask.
[[nodiscard]] inline constexpr uint16_t PadActiveLowWord(uint16_t pressed) noexcept
{
    return static_cast<uint16_t>(~pressed);
}

// Pacing rate used by VsyncTick()/WaitFrame()/GameFrame().
inline constexpr int HOST_REFRESH_HZ = 60;

// ---------------------------------------------------------------------------
// Surface / window lifetime (consumed by the Vulkan GS backend on its own
// thread).  AttachWindow(nullptr) means "surface destroyed".
//
// AttachWindow() takes its own ANativeWindow reference; the caller keeps the
// reference it passes in.  NativeWindow() returns the attached window or
// nullptr; the reference stays owned by the host, so acquire it yourself for
// long-lived use and never release it.  Once AttachWindow(nullptr) returns the
// host holds no reference at all.
// ---------------------------------------------------------------------------
void AttachWindow(ANativeWindow* window);
[[nodiscard]] ANativeWindow* NativeWindow();

// Logical drawable size in pixels, as reported by the Java SurfaceView.
void SetSurfaceSize(int width, int height);
void GetSurfaceSize(int& width, int& height);

// ---------------------------------------------------------------------------
// Lifecycle.
//
// SetPaused(true) parks GameFrame()/WaitFrame() and freezes the vsync counter so
// resume does not burst catch-up frames.  The game thread lives across
// onPause/onResume; it either blocks in GameFrame() or polls IsPaused().
//
// RequestExit()/ShouldExit() are the orderly way out (back button -> confirm).
// RequestExit() unblocks GameFrame(), so the engine loop can observe ShouldExit()
// and return; the Java side joins the game thread afterwards.
// ---------------------------------------------------------------------------
void SetPaused(bool paused);
[[nodiscard]] bool IsPaused();

// Monotonically increasing 60 Hz tick counter; frozen while paused.
[[nodiscard]] uint64_t VsyncTick();

// Sleeps until the next 60 Hz tick boundary.  Returns early when paused, when
// exiting or when the host shuts down.  Call it from the game loop when nothing
// presents.
void WaitFrame();

// One Android game-loop iteration: unblocks on pause/exit, otherwise waits for
// the next vsync boundary.  The Vulkan backend presents on its own; the host
// never presents.  Equivalent to: if (!paused && !shouldExit) WaitFrame().
void GameFrame();

void RequestExit();
[[nodiscard]] bool ShouldExit();

// ---------------------------------------------------------------------------
// Input.  `pressed` is the active-high PAD_* word above; axes are -128..127 with
// 0 == centre.
// ---------------------------------------------------------------------------
void SetPadState(uint16_t buttons, int8_t lx, int8_t ly, int8_t rx, int8_t ry);
void GetPadState(uint16_t& pressed, int8_t& lx, int8_t& ly, int8_t& rx, int8_t& ry);

// 0..1 per motor; forwarded to the Java vibrator.
void SetRumble(float low, float high);

// ---------------------------------------------------------------------------
// Logging.  StartLog() creates <directory>/logs/session-<timestamp>.log, mirrors
// every line to logcat and installs the terminate/crash handlers.  Idempotent:
// calling it twice keeps the first session file.
// ---------------------------------------------------------------------------
void StartLog(const char* directory);
void Log(const char* tag, const char* fmt, ...) PS2X_HOST_PRINTF(2, 3);
[[nodiscard]] std::string LogFilePath();

// The app's files directory, as passed to nativeStart(); "" before that.
// GOW2_BOOT_ELF=<filesDir>/SCUS_974.81 and GOW2_ISO=<filesDir>/gow2.iso are
// published in the environment by nativeStart().
[[nodiscard]] const char* FilesDir();

// ---------------------------------------------------------------------------
// Disc handling: the engine reads disc images from a path, so the Java layer
// copies the SAF document to <filesDir>/gow2.iso.  ExtractDiscFromIso() runs the
// ISO9660 module (android-runner/native/iso) to pull the boot ELF into filesDir.
// Returns false and fills errorOut on failure.
// ---------------------------------------------------------------------------
bool ExtractDiscFromIso(const char* isoPath, char* errorOut, size_t errorCap);

// ---------------------------------------------------------------------------
// Storage helpers (kept for patches that prefer pread over stdio).
// ---------------------------------------------------------------------------
int DupFileDescriptor(int fd);
int64_t ReadAt(int fd, int64_t offset, void* dst, size_t bytes);

// ---------------------------------------------------------------------------
// Audio: AAudio stream, PCM s16, 2 channels, low latency, 768-frame capacity.
// `fill` writes frames*2 interleaved int16 samples into dst and runs on the
// AAudio realtime thread.  StartAudio() may be called again after a stream
// disconnect; it rebuilds the stream.  IsAudioReady() is true while a stream is
// running, i.e. while the engine should feed it.
// ---------------------------------------------------------------------------
bool StartAudio(int sampleRate, void (*fill)(void* dst, unsigned frames));
void StopAudio();
[[nodiscard]] bool IsAudioReady();

// Frames the output device has consumed (monotonic across restarts).
[[nodiscard]] uint64_t AudioFramesPlayed();

// ---------------------------------------------------------------------------
// Runtime facts.
// ---------------------------------------------------------------------------
void SetSdkInt(int sdk);
[[nodiscard]] int GetSdkInt();

} // namespace ps2x::host

#endif // PS2X_HOST_H
