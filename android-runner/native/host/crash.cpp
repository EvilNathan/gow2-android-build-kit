// Fatal-signal handler: backtrace + last log lines to <files>/logs/crash-*.txt.
//
// Everything reachable from the signal handler is allocation-free and uses
// async-signal-safe syscalls (open/write/close) where it matters; the unwind
// pass is best-effort by design.
#include "ps2x_host_internal.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

namespace ps2x::host::detail {
namespace {

constexpr size_t kPathLen = 512;
constexpr int kMaxFrames = 64;
constexpr size_t kAltStackSize = 64 * 1024;
constexpr size_t kRingDumpBytes = 12 * 1024;

alignas(16) char g_altStack[kAltStackSize];
bool g_altStackInstalled = false;

// --- tiny async-signal-safe writer ----------------------------------------
struct Writer
{
    int fd = -1;

    void String(const char* text) const
    {
        if (fd < 0 || text == nullptr)
            return;
        const size_t length = ::strlen(text);
        if (length != 0)
            (void)::write(fd, text, length);
    }

    void Bytes(const char* data, size_t length) const
    {
        if (fd >= 0 && data != nullptr && length != 0)
            (void)::write(fd, data, length);
    }

    void Char(char value) const
    {
        if (fd >= 0)
            (void)::write(fd, &value, 1);
    }

    void Decimal(uint64_t value) const
    {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        } while (value != 0 && count < sizeof digits);
        for (size_t i = 0; i < count; ++i)
            Char(digits[count - 1 - i]);
    }

    void Hex(uint64_t value) const
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        char digits[16];
        size_t count = 0;
        do {
            digits[count++] = kDigits[value & 0xFu];
            value >>= 4u;
        } while (value != 0 && count < sizeof digits);
        String("0x");
        for (size_t i = 0; i < count; ++i)
            Char(digits[count - 1 - i]);
    }
};

void AppendLine(Writer& out, const char* text)
{
    out.String(text);
    out.Char('\n');
}

// --- backtrace -------------------------------------------------------------
struct BacktraceState
{
    int fd = -1;
    int index = 0;
};

_Unwind_Reason_Code BacktraceCallback(_Unwind_Context* context, void* argument)
{
    auto* state = static_cast<BacktraceState*>(argument);
    if (state == nullptr || context == nullptr)
        return _URC_END_OF_STACK;

    const uintptr_t pc = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (pc != 0) {
        const Writer out{state->fd};
        out.String("  #");
        out.Decimal(static_cast<uint64_t>(state->index));
        out.String(" ");
        out.Hex(pc);
        out.Char('\n');
        ++state->index;
    }
    return state->index >= kMaxFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
}

const char* SignalName(int sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "signal";
    }
}

// <files>/logs/crash-<epoch>.txt.  Epoch seconds keep the handler free of
// localtime()/snprintf().
void CrashPath(char* out, size_t cap)
{
    if (out == nullptr || cap < 16)
        return;
    const char* directory = LogDirectory();
    if (directory == nullptr || directory[0] == '\0')
        directory = ".";

    char number[24];
    size_t digits = 0;
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seconds = static_cast<uint64_t>(ts.tv_sec);
    do {
        number[digits++] = static_cast<char>('0' + (seconds % 10u));
        seconds /= 10u;
    } while (seconds != 0 && digits < sizeof number);

    size_t used = 0;
    const size_t dirLength = ::strnlen(directory, kPathLen);
    if (dirLength + 1 >= cap)
        return; // no room even for the directory
    ::memcpy(out, directory, dirLength);
    used = dirLength;

    static constexpr char kPrefix[] = "/crash-";
    static constexpr char kSuffix[] = ".txt";
    if (used + sizeof kPrefix + digits + sizeof kSuffix >= cap)
        return;
    for (size_t i = 0; i + 1 < sizeof kPrefix; ++i)
        out[used++] = kPrefix[i];
    for (size_t i = 0; i < digits; ++i)
        out[used++] = number[digits - 1 - i];
    for (size_t i = 0; i + 1 < sizeof kSuffix; ++i)
        out[used++] = kSuffix[i];
    out[used] = '\0';
}

void WriteReport(int sig, const char* reason, uintptr_t faultAddress, bool includeRing)
{
    char path[kPathLen] = {0};
    CrashPath(path, sizeof path);
    if (path[0] == '\0')
        return;

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;

    Writer out{fd};
    AppendLine(out, "PS2X Android host crash report");
    out.String("signal: ");
    out.String(SignalName(sig));
    out.Char('\n');
    if (faultAddress != 0) {
        out.String("fault:  ");
        out.Hex(faultAddress);
        out.Char('\n');
    }
    if (reason != nullptr && reason[0] != '\0') {
        out.String("reason: ");
        out.String(reason);
        out.Char('\n');
    }

    AppendLine(out, "backtrace:");
    BacktraceState state{fd, 0};
    _Unwind_Backtrace(&BacktraceCallback, &state);

    if (includeRing) {
        AppendLine(out, "log ring (oldest first):");
        // Small buffer: this may run on the 64 KiB signal stack.
        char ring[kRingDumpBytes];
        const size_t used = LogRingDump(ring, sizeof ring);
        out.Bytes(ring, used);
        if (used != 0 && ring[used - 1] != '\n')
            out.Char('\n');
    }
    AppendLine(out, "---- end of report ----");
    (void)::close(fd);
}

void RestoreDefaultAndRaise(int sig)
{
    struct sigaction action;
    ::memset(&action, 0, sizeof action);
    action.sa_handler = SIG_DFL;
    ::sigemptyset(&action.sa_mask);
    ::sigaction(sig, &action, nullptr);
    ::raise(sig);
    // Should not be reached; make sure the process still dies.
    ::_exit(128 + sig);
}

void CrashHandler(int sig, siginfo_t* info, void* /*context*/)
{
    static volatile sig_atomic_t inHandler = 0;
    if (inHandler != 0) {
        // Crash inside the crash handler: get out immediately.
        RestoreDefaultAndRaise(sig);
        return;
    }
    inHandler = 1;

    const uintptr_t fault = (info != nullptr) ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    WriteReport(sig, "fatal signal", fault, true);
    RestoreDefaultAndRaise(sig);
}

} // namespace

void CrashInstall()
{
    if (g_altStackInstalled)
        return;

    stack_t stack;
    ::memset(&stack, 0, sizeof stack);
    stack.ss_sp = g_altStack;
    stack.ss_size = sizeof g_altStack;
    stack.ss_flags = 0;
    // Not fatal if it fails: without an alt stack only stack-overflow crashes
    // lose their report.
    (void)::sigaltstack(&stack, nullptr);
    g_altStackInstalled = true;

    struct sigaction action;
    ::memset(&action, 0, sizeof action);
    action.sa_sigaction = &CrashHandler;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;

    static constexpr int kSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    for (int sig : kSignals)
        (void)::sigaction(sig, &action, nullptr);
}

void CrashWriteReport(int sig, const char* reason)
{
    WriteReport(sig, reason, 0, true);
}

} // namespace ps2x::host::detail
