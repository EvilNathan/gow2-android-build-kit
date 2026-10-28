// ISO9660 boot-data extraction, wrapped for Java (nativeExtractDisc).
//
// The real implementation lives in android-runner/native/iso/iso9660.h (owned by
// the iso-tools teammate).  This file only adapts it to the host API and keeps a
// compiling stub so the app builds before that module lands.
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <cstdio>

#if defined(__has_include)
#if __has_include("iso9660.h")
#include "iso9660.h"
#define PS2X_HOST_HAVE_ISO9660 1
#endif
#endif

namespace ps2x::host {
namespace {

detail::DiscExtractInfo g_lastExtract;

void ClearInfo()
{
    g_lastExtract.ok = false;
    g_lastExtract.sha1Matches = false;
    g_lastExtract.error[0] = '\0';
    g_lastExtract.elfName[0] = '\0';
    g_lastExtract.elfSha1[0] = '\0';
}

void SetError(const char* message)
{
    ClearInfo();
    std::snprintf(g_lastExtract.error, sizeof g_lastExtract.error, "%s", (message != nullptr) ? message : "");
}

void CopyErrorOut(char* errorOut, size_t errorCap)
{
    if (errorOut != nullptr && errorCap > 0)
        std::snprintf(errorOut, errorCap, "%s", g_lastExtract.error);
}

} // namespace

bool ExtractDiscFromIso(const char* isoPath, char* errorOut, size_t errorCap)
{
    if (errorOut != nullptr && errorCap > 0)
        errorOut[0] = '\0';

    if (isoPath == nullptr || isoPath[0] == '\0') {
        SetError("ISO path is empty");
        CopyErrorOut(errorOut, errorCap);
        return false;
    }

    const char* outDir = FilesDir();
    if (outDir == nullptr || outDir[0] == '\0') {
        SetError("files directory is not set; call nativeStart first");
        CopyErrorOut(errorOut, errorCap);
        return false;
    }

#ifdef PS2X_HOST_HAVE_ISO9660
    const auto result = ps2x::iso::ExtractBootData(isoPath, outDir);
    if (!result.ok) {
        SetError(result.error.c_str());
        CopyErrorOut(errorOut, errorCap);
        Log("E/host/disc", "boot data extraction failed: %s", g_lastExtract.error);
        return false;
    }

    ClearInfo();
    g_lastExtract.ok = true;
    g_lastExtract.sha1Matches = result.sha1Matches;
    std::snprintf(g_lastExtract.elfName, sizeof g_lastExtract.elfName, "%s", result.elfName.c_str());
    std::snprintf(g_lastExtract.elfSha1, sizeof g_lastExtract.elfSha1, "%s", result.elfSha1.c_str());
    Log("host/disc", "boot ELF extracted: %s sha1=%s match=%d", g_lastExtract.elfName, g_lastExtract.elfSha1,
        g_lastExtract.sha1Matches ? 1 : 0);
    return true;
#else
    // iso9660.h has not been added to the tree yet: report that instead of failing
    // to link, so the Java UI can show a useful message.
    SetError("iso9660 module missing (android-runner/native/iso)");
    CopyErrorOut(errorOut, errorCap);
    Log("E/host/disc", "ExtractDiscFromIso: %s", g_lastExtract.error);
    return false;
#endif
}

namespace detail {

const DiscExtractInfo& LastDiscExtract()
{
    return g_lastExtract;
}

} // namespace detail

} // namespace ps2x::host
