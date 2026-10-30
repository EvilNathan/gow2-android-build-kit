// Files directory + file-descriptor helpers (pread for patches that prefer it).
#include "ps2x_host.h"

#include "ps2x_host_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

namespace ps2x::host {
namespace {

constexpr size_t kStoragePathLen = 512;

std::mutex g_filesMutex;
char g_filesDir[kStoragePathLen] = {0};

} // namespace

const char* FilesDir()
{
    std::lock_guard<std::mutex> lock(g_filesMutex);
    return g_filesDir;
}

int DupFileDescriptor(int fd)
{
    if (fd < 0)
        return -1;

    int copy = -1;
#if defined(F_DUPFD_CLOEXEC)
    copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
#endif
    if (copy < 0) {
        copy = ::dup(fd);
        if (copy >= 0) {
            const int flags = ::fcntl(copy, F_GETFD, 0);
            if (flags >= 0)
                ::fcntl(copy, F_SETFD, flags | FD_CLOEXEC);
        }
    }
    return copy;
}

int64_t ReadAt(int fd, int64_t offset, void* dst, size_t bytes)
{
    if (fd < 0 || dst == nullptr || offset < 0)
        return -1;

    auto* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t got = ::pread64(fd, out + done, bytes - done, static_cast<off_t>(offset + static_cast<int64_t>(done)));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            break; // EOF
        done += static_cast<size_t>(got);
    }
    return static_cast<int64_t>(done);
}

namespace detail {

void SetFilesDir(const char* directory)
{
    std::lock_guard<std::mutex> lock(g_filesMutex);
    if (directory == nullptr) {
        g_filesDir[0] = '\0';
        return;
    }
    std::snprintf(g_filesDir, sizeof g_filesDir, "%s", directory);
}

} // namespace detail

} // namespace ps2x::host
