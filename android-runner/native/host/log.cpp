// Session logging: one file per session, logcat mirror, 256-line crash ring.
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <android/log.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace ps2x::host {
namespace {

constexpr size_t kRingLines = 256;
constexpr size_t kRingLineLen = 224;
constexpr size_t kPathLen = 512;

char g_ring[kRingLines][kRingLineLen];
std::atomic<uint32_t> g_ringCount{0};

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
char g_logPath[kPathLen] = {0};
char g_logDir[kPathLen] = {0};

// "HH:MM:SS.mmm" — never throws, no allocation.
void FormatClock(char* out, size_t cap)
{
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    ::localtime_r(&ts.tv_sec, &local);
    std::snprintf(out, cap, "%02d:%02d:%02d.%03d",
        local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<int>(ts.tv_nsec / 1000000));
}

// Whole-second local timestamp for file names; no allocation.
void FormatFileStamp(char* out, size_t cap)
{
    const time_t now = ::time(nullptr);
    tm local{};
    ::localtime_r(&now, &local);
    std::snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec);
}

// mkdir -p for the two levels we need (files dir already exists).
void EnsureDirectory(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return;
    char buffer[kPathLen];
    std::snprintf(buffer, sizeof buffer, "%s", path);
    for (char* p = buffer + 1; *p != '\0'; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        ::mkdir(buffer, 0770);
        *p = '/';
    }
    ::mkdir(buffer, 0770);
}

int LogcatPriority(const char* tag)
{
    if (tag == nullptr || tag[0] == '\0')
        return ANDROID_LOG_INFO;
    switch (tag[0]) {
    case 'E':
        return ANDROID_LOG_ERROR;
    case 'W':
        return ANDROID_LOG_WARN;
    case 'D':
        return ANDROID_LOG_DEBUG;
    default:
        return ANDROID_LOG_INFO;
    }
}

// Log() body; assumes g_logMutex is held.
void WriteLineLocked(const char* tag, const char* message)
{
    char stamp[24];
    FormatClock(stamp, sizeof stamp);
    const char* safeTag = (tag != nullptr) ? tag : "host";
    const char* safeMessage = (message != nullptr) ? message : "";

    detail::LogRingPush(safeMessage);

    char logcatTag[64];
    std::snprintf(logcatTag, sizeof logcatTag, "ps2x/%s", safeTag);
    __android_log_print(LogcatPriority(safeTag), logcatTag, "%s", safeMessage);

    if (g_logFile == nullptr)
        return;
    const pid_t tid = ::gettid();
    std::fprintf(g_logFile, "%s %5d [%s] %s\n", stamp, static_cast<int>(tid), safeTag, safeMessage);
    std::fflush(g_logFile);
}

[[noreturn]] void OnTerminate()
{
    try {
        Log("E/host", "std::terminate called");
        detail::CrashWriteReport(0, "std::terminate");
    } catch (...) {
        // Nothing we can do while unwinding a terminate.
    }
    std::abort();
}

} // namespace

void StartLog(const char* directory)
{
    try {
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (directory != nullptr && directory[0] != '\0') {
                std::snprintf(g_logDir, sizeof g_logDir, "%s/logs", directory);
            } else {
                std::snprintf(g_logDir, sizeof g_logDir, ".");
            }
            EnsureDirectory(g_logDir);

            if (g_logFile != nullptr) {
                // Idempotent: keep the first session file.
                return;
            }

            char stamp[32];
            FormatFileStamp(stamp, sizeof stamp);
            std::snprintf(g_logPath, sizeof g_logPath, "%s/session-%s.log", g_logDir, stamp);
            g_logFile = std::fopen(g_logPath, "a");
        }

        std::set_terminate(OnTerminate);
        detail::CrashInstall();

        Log("host", "---- session start ----");
        Log("host", "sdk=%d pid=%d log=%s", GetSdkInt(), static_cast<int>(::getpid()), g_logPath);
    } catch (...) {
        // Logging must never take the process down.
    }
}

void Log(const char* tag, const char* fmt, ...)
{
    try {
        char message[768];
        message[0] = '\0';
        int written = 0;
        if (fmt != nullptr) {
            va_list args;
            va_start(args, fmt);
            written = std::vsnprintf(message, sizeof message, fmt, args);
            va_end(args);
        }
        if (written < 0)
            return;

        std::lock_guard<std::mutex> lock(g_logMutex);
        WriteLineLocked(tag, message);
    } catch (...) {
        // Never throw out of a log call.
    }
}

std::string LogFilePath()
{
    try {
        std::lock_guard<std::mutex> lock(g_logMutex);
        return std::string(g_logPath);
    } catch (...) {
        return std::string();
    }
}

namespace detail {

void LogRingPush(const char* line)
{
    if (line == nullptr)
        return;
    char stamp[24];
    FormatClock(stamp, sizeof stamp);
    const uint32_t sequence = g_ringCount.load(std::memory_order_relaxed);
    char* slot = g_ring[sequence % kRingLines];
    std::snprintf(slot, kRingLineLen, "%s %s", stamp, line);
    g_ringCount.store(sequence + 1, std::memory_order_release);
}

size_t LogRingDump(char* dst, size_t cap)
{
    if (dst == nullptr || cap == 0)
        return 0;
    // Lock-free read: the writer may be racing us, a torn line is acceptable in
    // a crash dump (this also runs from a signal handler).
    const uint32_t total = g_ringCount.load(std::memory_order_acquire);
    const uint32_t first = total > kRingLines ? total - static_cast<uint32_t>(kRingLines) : 0u;
    size_t used = 0;
    for (uint32_t i = first; i < total && used + 1 < cap; ++i) {
        const char* slot = g_ring[i % kRingLines];
        if (slot[0] == '\0')
            continue;
        const size_t length = strnlen(slot, kRingLineLen);
        if (used + length + 1 >= cap)
            break;
        std::memcpy(dst + used, slot, length);
        used += length;
        dst[used++] = '\n';
    }
    if (used < cap)
        dst[used] = '\0';
    return used;
}

const char* LogDirectory()
{
    return g_logDir;
}

} // namespace detail

} // namespace ps2x::host
