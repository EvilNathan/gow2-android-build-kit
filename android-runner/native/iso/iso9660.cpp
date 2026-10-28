#if defined(_WIN32)
#  define _CRT_SECURE_NO_WARNINGS 1  // _open/_read/_fstat64 are the Windows pread stand-in
#endif

#include "iso9660.h"

#include "sha1.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace ps2x::iso
{
namespace
{

constexpr uint32_t kSectorSize = 2048u;
constexpr uint32_t kMaxVolumeDescriptors = 64u;
constexpr int kMaxDirectoryDepth = 16;
constexpr char kExpectedBootElfSha1[] = "E6A62A0F3E781C29513F563F4625B9B3A9103C56";

struct Extent
{
    uint64_t lsn = 0;   // absolute 2048-byte sector in the image
    uint64_t size = 0;  // bytes in this extent
};

struct FileRecord
{
    Entry entry;
    std::vector<Extent> extents;
};

uint16_t Le16(const uint8_t* p)
{
    return static_cast<uint16_t>(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

uint32_t Le32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool PreadAll(int fd, uint64_t offset, void* buffer, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(buffer);
    while (size > 0)
    {
#if defined(_WIN32)
        // pread(2) does not exist on Windows; this path is only used by the
        // Windows self-test. Android/Linux always uses pread below.
        if (_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0)
            return false;
        const unsigned int chunk = static_cast<unsigned int>(size > (1u << 30) ? (1u << 30) : size);
        const int got = _read(fd, bytes, chunk);
        if (got <= 0)
            return false;
#else
        const size_t chunk = size > (size_t(1) << 30) ? (size_t(1) << 30) : size;
        const ssize_t got = ::pread(fd, bytes, chunk, static_cast<off_t>(offset));
        if (got < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
#endif
        bytes += got;
        offset += static_cast<uint64_t>(got);
        size -= static_cast<size_t>(got);
    }
    return true;
}

#if defined(_WIN32)
int RawOpen(const char* path)
{
    return _open(path, _O_RDONLY | _O_BINARY);
}

void RawClose(int fd)
{
    _close(fd);
}
#else
int RawOpen(const char* path)
{
    return ::open(path, O_RDONLY);
}

void RawClose(int fd)
{
    ::close(fd);
}
#endif

bool FdSize(int fd, uint64_t& size)
{
#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0 && st.st_size > 0)
    {
        size = static_cast<uint64_t>(st.st_size);
        return true;
    }
    const __int64 end = _lseeki64(fd, 0, SEEK_END);
    if (end < 0)
        return false;
    size = static_cast<uint64_t>(end);
    return true;
#else
    struct stat st;
    if (::fstat(fd, &st) == 0 && st.st_size > 0)
    {
        size = static_cast<uint64_t>(st.st_size);
        return true;
    }
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end < 0)
        return false;
    size = static_cast<uint64_t>(end);
    return true;
#endif
}

std::string ToUpper(std::string text)
{
    for (char& c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

std::string Trim(std::string text)
{
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin]))
        ++begin;
    size_t end = text.size();
    while (end > begin && isSpace(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::string NormalizeComponent(std::string name)
{
    const size_t semicolon = name.find(';');
    if (semicolon != std::string::npos)
        name.erase(semicolon);
    for (char& c : name)
    {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        else if (c == '\\')
            c = '/';
    }
    return name;
}

// "cdrom0:\\SCUS_974.81;1", "scus_974.81;1" and "SCUS_974.81" all normalize
// to "SCUS_974.81"; nested paths keep their '/' separators.
std::string NormalizePath(const std::string& input)
{
    std::string text;
    text.reserve(input.size());
    for (char c : input)
    {
        if (c == '\\')
            text.push_back('/');
        else if (c >= 'a' && c <= 'z')
            text.push_back(static_cast<char>(c - 'a' + 'A'));
        else
            text.push_back(c);
    }

    const size_t colon = text.find(':');
    if (colon != std::string::npos)
    {
        const std::string device = text.substr(0, colon);
        if (device == "CDROM0" || device == "CDROM1" || device == "CDROM" || device == "CD")
            text.erase(0, colon + 1);
    }

    std::string result;
    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t slash = text.find('/', pos);
        std::string component = (slash == std::string::npos) ? text.substr(pos) : text.substr(pos, slash - pos);
        const size_t semicolon = component.find(';');
        if (semicolon != std::string::npos)
            component.erase(semicolon);
        if (!component.empty())
        {
            if (!result.empty())
                result.push_back('/');
            result += component;
        }
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return result;
}

std::string DecodeAsciiName(const uint8_t* bytes, size_t length)
{
    std::string name;
    name.reserve(length);
    for (size_t i = 0; i < length; ++i)
        if (bytes[i] != 0)
            name.push_back(static_cast<char>(bytes[i]));
    return name;
}

// Joliet supplementary descriptors store UCS-2 big-endian names. PS2 discs
// use the plain primary descriptor, but some homebrew/other images only have
// a Joliet volume, so decode it when no primary descriptor exists.
std::string DecodeJolietName(const uint8_t* bytes, size_t length)
{
    std::string name;
    for (size_t i = 0; i + 1 < length; i += 2)
    {
        const uint32_t code = (uint32_t(bytes[i]) << 8) | uint32_t(bytes[i + 1]);
        if (code == 0)
            continue;
        if (code < 0x80)
        {
            name.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            name.push_back(static_cast<char>(0xC0 | (code >> 6)));
            name.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            name.push_back(static_cast<char>(0xE0 | (code >> 12)));
            name.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            name.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return name;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z')
            ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z')
            cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb)
            return false;
    }
    return true;
}

bool WriteWholeFile(const std::filesystem::path& path, const void* data, size_t size, std::string& error)
{
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            error = "cannot create directory " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    std::ofstream file(path.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        error = "cannot open for writing: " + path.string();
        return false;
    }

    const char* bytes = static_cast<const char*>(data);
    size_t left = size;
    constexpr size_t kChunk = size_t(64) << 20;
    while (left > 0)
    {
        const size_t take = left < kChunk ? left : kChunk;
        file.write(bytes, static_cast<std::streamsize>(take));
        if (!file)
        {
            error = "write failed: " + path.string();
            return false;
        }
        bytes += take;
        left -= take;
    }

    file.flush();
    if (!file)
    {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

// Parses "BOOT2 = cdrom0:\SCUS_974.81;1" (any leading spaces, any case for
// the device, optional quotes) out of SYSTEM.CNF.
bool ParseBoot2(const std::vector<uint8_t>& cnf, std::string& elfName, std::string& error)
{
    std::string text(reinterpret_cast<const char*>(cnf.data()), cnf.size());
    const size_t nul = text.find('\0');
    if (nul != std::string::npos)
        text.resize(nul);

    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t newline = text.find('\n', pos);
        std::string line = (newline == std::string::npos) ? text.substr(pos) : text.substr(pos, newline - pos);
        pos = (newline == std::string::npos) ? text.size() + 1 : newline + 1;
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        if (ToUpper(Trim(line.substr(0, equals))) != "BOOT2")
            continue;

        std::string value = Trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        const size_t colon = value.find(':');
        if (colon != std::string::npos)
        {
            const std::string device = ToUpper(value.substr(0, colon));
            if (device != "CDROM0" && device != "CDROM1" && device != "CDROM")
            {
                error = "SYSTEM.CNF BOOT2 uses an unsupported device: " + value;
                return false;
            }
            value = value.substr(colon + 1);
        }

        for (char& c : value)
            if (c == '\\')
                c = '/';
        const size_t slash = value.rfind('/');
        if (slash != std::string::npos)
            value = value.substr(slash + 1);
        const size_t semicolon = value.find(';');
        if (semicolon != std::string::npos)
            value.erase(semicolon);
        value = Trim(value);
        if (value.empty())
        {
            error = "SYSTEM.CNF has an empty BOOT2 file name";
            return false;
        }
        elfName = value;
        return true;
    }

    error = "SYSTEM.CNF does not contain a BOOT2 line";
    return false;
}

ExtractResult ExtractBootDataImpl(IsoImage& image, const char* outputDir)
{
    ExtractResult result;
    try
    {
        if (!outputDir || !*outputDir)
        {
            result.error = "output directory is empty";
            return result;
        }

        std::error_code ec;
        const std::filesystem::path outDir(outputDir);
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            result.error = "cannot create output directory " + outDir.string() + ": " + ec.message();
            return result;
        }

        const auto cnfEntry = image.Find("SYSTEM.CNF");
        if (!cnfEntry)
        {
            result.error = "SYSTEM.CNF was not found in the disc image (not a PlayStation 2 disc?)";
            return result;
        }
        std::vector<uint8_t> cnfData;
        if (!image.ReadFile(*cnfEntry, cnfData))
        {
            result.error = "cannot read SYSTEM.CNF from the disc image";
            return result;
        }

        std::string bootName;
        if (!ParseBoot2(cnfData, bootName, result.error))
            return result;

        const auto elfEntry = image.Find(bootName.c_str());
        if (!elfEntry)
        {
            result.error = "the BOOT2 ELF was not found in the disc image: " + bootName;
            return result;
        }
        std::vector<uint8_t> elfData;
        if (!image.ReadFile(*elfEntry, elfData))
        {
            result.error = "cannot read the boot ELF from the disc image: " + bootName;
            return result;
        }
        if (!WriteWholeFile(outDir / bootName, elfData.data(), elfData.size(), result.error))
            return result;

        const std::string elfNameText = bootName + "\n";
        if (!WriteWholeFile(outDir / "elf-name.txt", elfNameText.data(), elfNameText.size(), result.error))
            return result;

        // The game opens IOP modules as IOP_MOD/NAME.IRX even though they live
        // in the disc root, so extract them in both places.
        const std::filesystem::path iopModDir = outDir / "IOP_MOD";
        std::filesystem::create_directories(iopModDir, ec);
        if (ec)
        {
            result.error = "cannot create " + iopModDir.string() + ": " + ec.message();
            return result;
        }

        std::vector<std::string> moduleNames;
        for (const Entry& entry : image.Entries())
        {
            if (entry.directory)
                continue;
            if (entry.name.find('/') != std::string::npos)
                continue;
            if (entry.name.size() < 5 || entry.name.compare(entry.name.size() - 4, 4, ".IRX") != 0)
                continue;
            moduleNames.push_back(entry.name);
        }
        std::sort(moduleNames.begin(), moduleNames.end());

        for (const std::string& name : moduleNames)
        {
            const auto entry = image.Find(name.c_str());
            if (!entry)
            {
                result.error = "the disc directory listing changed while reading: " + name;
                return result;
            }
            std::vector<uint8_t> data;
            if (!image.ReadFile(*entry, data))
            {
                result.error = "cannot read IOP module from the disc image: " + name;
                return result;
            }
            if (!WriteWholeFile(outDir / name, data.data(), data.size(), result.error))
                return result;
            if (!WriteWholeFile(iopModDir / name, data.data(), data.size(), result.error))
                return result;
        }

        result.elfName = bootName;
        result.elfSha1 = Sha1::Hex(elfData.data(), elfData.size());
        result.sha1Matches = EqualsIgnoreCase(result.elfSha1, kExpectedBootElfSha1);
        result.ok = true;
        return result;
    }
    catch (const std::exception& ex)
    {
        result = ExtractResult{};
        result.error = std::string("unexpected error while extracting the disc image: ") + ex.what();
        return result;
    }
    catch (...)
    {
        result = ExtractResult{};
        result.error = "unexpected error while extracting the disc image";
        return result;
    }
}

} // namespace

struct IsoImage::Impl
{
    int fd = -1;
    bool ownsFd = false;
    uint64_t imageSize = 0;
    uint64_t sectorCount = 0;
    uint32_t layer0Sectors = 0;
    uint32_t layer1Base = 0;  // absolute LSN that layer-1 extent LSNs are relative to (0 = none)
    bool joliet = false;
    std::vector<FileRecord> records;
    std::vector<Entry> entries;

    void Close()
    {
        if (fd >= 0 && ownsFd)
            RawClose(fd);
        fd = -1;
        ownsFd = false;
        imageSize = 0;
        sectorCount = 0;
        layer0Sectors = 0;
        layer1Base = 0;
        joliet = false;
        records.clear();
        entries.clear();
    }

    bool ReadAt(uint64_t byteOffset, void* buffer, size_t size) const
    {
        if (fd < 0)
            return false;
        if (size == 0)
            return true;
        if (byteOffset > imageSize || size > imageSize - byteOffset)
            return false;
        return PreadAll(fd, byteOffset, buffer, size);
    }

    bool ReadSector(uint64_t lsn, uint8_t* buffer) const
    {
        if (lsn >= sectorCount)
            return false;
        return ReadAt(lsn * kSectorSize, buffer, kSectorSize);
    }

    bool OpenScan(std::string& error)
    {
        uint64_t size = 0;
        if (!FdSize(fd, size))
        {
            error = "cannot determine the size of the disc image";
            return false;
        }
        imageSize = size;
        sectorCount = imageSize / kSectorSize;
        if (sectorCount < 17)
        {
            error = "the file is too small to be an ISO9660 disc image";
            return false;
        }
        return Scan(error);
    }

    bool Scan(std::string& error)
    {
        records.clear();
        entries.clear();
        layer0Sectors = 0;
        layer1Base = 0;
        joliet = false;

        uint8_t primary[kSectorSize] = {};
        uint8_t supplementary[kSectorSize] = {};
        bool havePrimary = false;
        bool haveSupplementary = false;

        for (uint32_t lsn = 16; lsn < 16 + kMaxVolumeDescriptors; ++lsn)
        {
            uint8_t descriptor[kSectorSize];
            if (!ReadSector(lsn, descriptor))
                break;
            if (std::memcmp(descriptor + 1, "CD001", 5) != 0)
            {
                if (lsn == 16)
                {
                    error = "not an ISO9660 image (no CD001 volume descriptor at LSN 16)";
                    return false;
                }
                break;
            }
            const uint8_t type = descriptor[0];
            if (type == 255)  // volume descriptor set terminator
                break;
            if (type == 1 && !havePrimary)
            {
                std::memcpy(primary, descriptor, kSectorSize);
                havePrimary = true;
                break;
            }
            if (type == 2 && !haveSupplementary)
            {
                std::memcpy(supplementary, descriptor, kSectorSize);
                haveSupplementary = true;
            }
        }

        const uint8_t* pvd = nullptr;
        if (havePrimary)
        {
            pvd = primary;
        }
        else if (haveSupplementary)
        {
            pvd = supplementary;
            joliet = true;
        }
        if (pvd == nullptr)
        {
            error = "no ISO9660 primary volume descriptor was found";
            return false;
        }

        if (Le16(pvd + 128) != kSectorSize)
        {
            error = "unsupported ISO9660 logical block size (expected 2048)";
            return false;
        }

        layer0Sectors = Le32(pvd + 80);
        if (layer0Sectors < 17)
        {
            error = "the ISO9660 volume space size is invalid";
            return false;
        }
        if (layer0Sectors > sectorCount)
        {
            error = "the ISO9660 volume space is larger than the image file";
            return false;
        }

        uint32_t rootLsn = Le32(pvd + 156 + 2);
        uint32_t rootSize = Le32(pvd + 156 + 10);
        if (rootSize == 0)
        {
            error = "the ISO9660 root directory is empty";
            return false;
        }

        std::set<uint64_t> visited;
        if (!ScanDirectory(0, rootLsn, rootSize, std::string(), visited, 0, error))
            return false;

        // Dual-layer DVD: God of War II's image has a second primary volume
        // descriptor at LSN = layer-0 sector count. Its extents are relative
        // to (that LSN - 16), matching tools/iso_extract.py.
        if (layer0Sectors < sectorCount)
        {
            uint8_t descriptor[kSectorSize];
            if (ReadSector(layer0Sectors, descriptor) &&
                std::memcmp(descriptor + 1, "CD001", 5) == 0 &&
                descriptor[0] == 1 &&
                Le16(descriptor + 128) == kSectorSize)
            {
                const uint32_t layer1Sectors = Le32(descriptor + 80);
                if (layer1Sectors != 0)
                {
                    layer1Base = layer0Sectors - 16;
                    rootLsn = Le32(descriptor + 156 + 2);
                    rootSize = Le32(descriptor + 156 + 10);
                    if (rootSize != 0)
                    {
                        std::set<uint64_t> layer1Visited;
                        if (!ScanDirectory(layer1Base, rootLsn, rootSize, std::string(),
                                           layer1Visited, 0, error))
                            return false;
                    }
                }
            }
        }

        entries.reserve(records.size());
        for (const FileRecord& record : records)
            entries.push_back(record.entry);
        return true;
    }

    bool ScanDirectory(uint64_t base, uint32_t lsn, uint32_t size, const std::string& path,
                       std::set<uint64_t>& visited, int depth, std::string& error)
    {
        if (depth > kMaxDirectoryDepth)
        {
            error = "ISO9660 directory nesting is too deep";
            return false;
        }
        if (size == 0)
            return true;

        const uint64_t absoluteLsn = base + lsn;
        if (!visited.insert(absoluteLsn).second)
            return true;  // directory loop; already scanned

        std::vector<uint8_t> data(size);
        if (!ReadAt(absoluteLsn * kSectorSize, data.data(), data.size()))
        {
            error = "cannot read ISO9660 directory data at LSN " + std::to_string(absoluteLsn);
            return false;
        }

        std::vector<Extent> pendingExtents;
        std::string pendingName;
        bool havePending = false;

        const auto flushPending = [&]() {
            if (!havePending)
                return;
            havePending = false;

            uint64_t total = 0;
            for (const Extent& extent : pendingExtents)
                total += extent.size;

            FileRecord record;
            record.entry.name = pendingName;
            record.entry.lsn = static_cast<uint32_t>(pendingExtents.front().lsn);
            record.entry.size = static_cast<uint32_t>(total > 0xFFFFFFFFull ? 0xFFFFFFFFull : total);
            record.entry.directory = false;
            record.extents = pendingExtents;
            records.push_back(std::move(record));
            pendingExtents.clear();
        };

        size_t offset = 0;
        while (offset < data.size())
        {
            const uint8_t recordLength = data[offset];
            if (recordLength == 0)
            {
                // Records never span a sector boundary: skip the zero padding
                // up to the next sector.
                offset = (offset / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (recordLength < 34 || offset + recordLength > data.size())
            {
                error = "malformed ISO9660 directory record in " +
                        (path.empty() ? std::string("<root>") : path);
                return false;
            }

            const uint8_t* record = data.data() + offset;
            offset += recordLength;

            const uint32_t extentLsn = Le32(record + 2);
            const uint32_t extentSize = Le32(record + 10);
            const uint8_t flags = record[25];
            const uint8_t nameLength = record[32];
            if (nameLength == 0 || size_t(33) + nameLength > recordLength)
            {
                error = "malformed ISO9660 file name in " + (path.empty() ? std::string("<root>") : path);
                return false;
            }

            const uint8_t* nameBytes = record + 33;
            if (nameLength == 1 && (nameBytes[0] == 0 || nameBytes[0] == 1))
                continue;  // "." and ".."

            std::string name = joliet ? DecodeJolietName(nameBytes, nameLength)
                                      : DecodeAsciiName(nameBytes, nameLength);
            name = NormalizeComponent(name);
            if (name.empty())
                continue;
            if (name.find('/') != std::string::npos)
            {
                error = "invalid file name in the ISO9660 directory tree";
                return false;
            }
            const std::string fullName = path.empty() ? name : path + "/" + name;

            if ((flags & 0x02) != 0)
            {
                flushPending();
                if (!ScanDirectory(base, extentLsn, extentSize, fullName, visited, depth + 1, error))
                    return false;
                continue;
            }

            // Multi-extent files: consecutive records with the same name where
            // all but the last carry flag 0x80. Concatenate their extents.
            if (havePending && pendingName != fullName)
                flushPending();
            if (!havePending)
            {
                havePending = true;
                pendingName = fullName;
            }
            pendingExtents.push_back(Extent{base + extentLsn, extentSize});
            if ((flags & 0x80) == 0)
                flushPending();
        }
        flushPending();
        return true;
    }

    const FileRecord* Lookup(const Entry& entry) const
    {
        for (const FileRecord& record : records)
            if (!record.entry.directory && record.entry.name == entry.name && record.entry.lsn == entry.lsn)
                return &record;
        return nullptr;
    }
};

IsoImage::IsoImage()
    : impl_(new Impl())
{
}

IsoImage::~IsoImage() = default;

bool IsoImage::OpenPath(const char* path, std::string& error)
{
    error.clear();
    try
    {
        if (path == nullptr || *path == '\0')
        {
            error = "the disc image path is empty";
            return false;
        }
        if (!impl_)
            impl_.reset(new Impl());
        else
            impl_->Close();

        const int fd = RawOpen(path);
        if (fd < 0)
        {
            error = std::string("cannot open the disc image: ") + path;
            return false;
        }
        impl_->fd = fd;
        impl_->ownsFd = true;

        std::string scanError;
        if (!impl_->OpenScan(scanError))
        {
            error = scanError;
            impl_->Close();
            return false;
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        error = std::string("unexpected error while opening the disc image: ") + ex.what();
        if (impl_)
            impl_->Close();
        return false;
    }
    catch (...)
    {
        error = "unexpected error while opening the disc image";
        if (impl_)
            impl_->Close();
        return false;
    }
}

bool IsoImage::OpenFd(int fd, std::string& error)
{
    error.clear();
    try
    {
        if (fd < 0)
        {
            error = "invalid file descriptor for the disc image";
            return false;
        }
        if (!impl_)
            impl_.reset(new Impl());
        else
            impl_->Close();

        impl_->fd = fd;
        impl_->ownsFd = false;

        std::string scanError;
        if (!impl_->OpenScan(scanError))
        {
            error = scanError;
            impl_->fd = -1;
            impl_->ownsFd = false;
            return false;
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        error = std::string("unexpected error while opening the disc image: ") + ex.what();
        if (impl_)
        {
            impl_->fd = -1;
            impl_->ownsFd = false;
        }
        return false;
    }
    catch (...)
    {
        error = "unexpected error while opening the disc image";
        if (impl_)
        {
            impl_->fd = -1;
            impl_->ownsFd = false;
        }
        return false;
    }
}

std::optional<Entry> IsoImage::Find(const char* isoPath)
{
    try
    {
        if (!impl_ || impl_->fd < 0 || isoPath == nullptr)
            return std::nullopt;
        const std::string wanted = NormalizePath(isoPath);
        if (wanted.empty())
            return std::nullopt;
        for (const FileRecord& record : impl_->records)
            if (record.entry.name == wanted)
                return record.entry;
        return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool IsoImage::ReadFile(const Entry& entry, std::vector<uint8_t>& out)
{
    out.clear();
    try
    {
        if (!impl_ || impl_->fd < 0)
            return false;
        const FileRecord* record = impl_->Lookup(entry);
        if (record == nullptr)
            return false;

        uint64_t total = 0;
        for (const Extent& extent : record->extents)
        {
            if (extent.size > UINT64_MAX - total)
                return false;
            total += extent.size;
        }
        if (total > static_cast<uint64_t>(out.max_size()))
            return false;

        out.resize(static_cast<size_t>(total));
        size_t position = 0;
        for (const Extent& extent : record->extents)
        {
            const size_t size = static_cast<size_t>(extent.size);
            if (!impl_->ReadAt(extent.lsn * kSectorSize, out.data() + position, size))
            {
                out.clear();
                return false;
            }
            position += size;
        }
        return true;
    }
    catch (...)
    {
        out.clear();
        return false;
    }
}

uint32_t IsoImage::layer1Start() const
{
    if (!impl_ || impl_->fd < 0)
        return 0;
    return impl_->layer1Base;
}

const std::vector<Entry>& IsoImage::Entries() const
{
    static const std::vector<Entry> kEmpty;
    if (!impl_ || impl_->fd < 0)
        return kEmpty;
    return impl_->entries;
}

ExtractResult ExtractBootData(const char* isoPath, const char* outputDir)
{
    ExtractResult result;
    try
    {
        if (isoPath == nullptr || *isoPath == '\0')
        {
            result.error = "the disc image path is empty";
            return result;
        }
        IsoImage image;
        std::string error;
        if (!image.OpenPath(isoPath, error))
        {
            result.error = error.empty() ? "cannot open the disc image" : error;
            return result;
        }
        return ExtractBootDataImpl(image, outputDir);
    }
    catch (const std::exception& ex)
    {
        result = ExtractResult{};
        result.error = std::string("unexpected error while opening the disc image: ") + ex.what();
        return result;
    }
    catch (...)
    {
        result = ExtractResult{};
        result.error = "unexpected error while opening the disc image";
        return result;
    }
}

ExtractResult ExtractBootDataFromFd(int fd, const char* outputDir)
{
    ExtractResult result;
    try
    {
        IsoImage image;
        std::string error;
        if (!image.OpenFd(fd, error))
        {
            result.error = error.empty() ? "cannot open the disc image from the file descriptor" : error;
            return result;
        }
        return ExtractBootDataImpl(image, outputDir);
    }
    catch (const std::exception& ex)
    {
        result = ExtractResult{};
        result.error = std::string("unexpected error while opening the disc image: ") + ex.what();
        return result;
    }
    catch (...)
    {
        result = ExtractResult{};
        result.error = "unexpected error while opening the disc image";
        return result;
    }
}

} // namespace ps2x::iso
