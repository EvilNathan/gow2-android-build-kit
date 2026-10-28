// ISO9660 / DVD-ROM reader for the Android God of War II runner.
//
// The app receives the user's own disc image through the Storage Access
// Framework as a file descriptor, so the reader works on a caller-owned fd
// with pread(2) (POSIX) / _lseeki64+_read (Windows test build) instead of
// std::ifstream. Nothing here is Android-specific; the same code is used by
// the Windows self-test in tools/gscheck.
//
// The disc image is a plain 2048-byte-per-sector DVD image (8,533,671,936
// bytes for God of War II USA v1.01). PS2 discs use plain ISO9660 names such
// as "SYSTEM.CNF;1" and "SCUS_974.81;1"; Rock Ridge and UDF are not needed.
//
// Dual-layer handling: God of War II's image is not one flat ISO9660 volume.
// The primary volume descriptor at LSN 16 describes layer 0 only (volume
// space size 2,083,888 sectors). A second volume descriptor sits at
// LSN = layer-0 sector count and describes layer 1 with LSNs relative to
// (that sector - 16). This reader scans both volumes, resolves every extent
// to an absolute LSN in the image and therefore only ever preads absolute
// offsets. Multi-extent files (ISO9660 flag 0x80, used for >4 GiB files) are
// concatenated in ReadFile().
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iso
{

// Result of ExtractBootData(). ok == false means nothing usable was written
// and error explains why; a SHA1 mismatch is reported through sha1Matches
// without failing the extraction (custom/modded discs are allowed).
struct ExtractResult
{
    bool ok = false;
    std::string error;
    std::string elfName;   // "SCUS_974.81"
    std::string elfSha1;   // uppercase hex of the extracted ELF
    bool sha1Matches = false;
};

// A file or directory found on the disc. name is the normalized path
// ("SYSTEM.CNF", "IOP_MOD/SIO2MAN.IRX"), lsn is the absolute 2048-byte
// sector of the first extent, size is the file size in bytes (clamped to
// UINT32_MAX for the theoretical >4 GiB multi-extent file; ReadFile still
// returns the whole file).
struct Entry
{
    std::string name;
    uint32_t lsn = 0;
    uint32_t size = 0;
    bool directory = false;
};

class IsoImage
{
public:
    IsoImage();
    ~IsoImage();

    // The object owns its fd and closes it in the destructor.
    IsoImage(const IsoImage&) = delete;
    IsoImage& operator=(const IsoImage&) = delete;

    // Opens a disc image by path. Returns false and fills error on failure.
    bool OpenPath(const char* path, std::string& error);

    // Uses a caller-owned, seekable file descriptor (Android SAF: detachFd()
    // or ParcelFileDescriptor.getFd()). The descriptor is never closed by
    // IsoImage. Reads use pread(2)/pread64, so they do not disturb the
    // caller's file offset. Returns false and fills error on failure.
    bool OpenFd(int fd, std::string& error);

    // Case-insensitive lookup; a leading "cdrom0:\", a leading slash and
    // ";"-versions are ignored, so all of these find the same entry:
    //   "SCUS_974.81", "scus_974.81;1", "cdrom0:\\SCUS_974.81;1".
    std::optional<Entry> Find(const char* isoPath);

    // Reads every extent of the entry (concatenating multi-extent files) into
    // out. Returns false on I/O errors or when entry was not produced by this
    // image.
    bool ReadFile(const Entry& entry, std::vector<uint8_t>& out);

    // Absolute LSN that layer-1 extent LSNs are relative to (0 on a
    // single-layer image). Informational: Find()/ReadFile() already resolve
    // extents to absolute LSNs.
    uint32_t layer1Start() const;

    // Test/tooling helper (not needed by the app): every file and directory
    // found on both volumes, in traversal order.
    const std::vector<Entry>& Entries() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Opens the image at isoPath, reads SYSTEM.CNF, parses BOOT2=cdrom0:\NAME;1,
// extracts the boot ELF to <outputDir>/NAME, every root-level *.IRX to
// <outputDir>/ plus a copy in <outputDir>/IOP_MOD/, writes
// <outputDir>/elf-name.txt and verifies the ELF SHA1 against
// E6A62A0F3E781C29513F563F4625B9B3A9103C56. Never throws: failures are
// reported through result.ok/result.error.
ExtractResult ExtractBootData(const char* isoPath, const char* outputDir);

// Same as ExtractBootData() but for a caller-owned file descriptor, which is
// how the Android app receives the ISO from the SAF picker. The descriptor is
// not closed.
ExtractResult ExtractBootDataFromFd(int fd, const char* outputDir);

} // namespace ps2x::iso
