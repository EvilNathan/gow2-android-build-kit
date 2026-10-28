// Windows self-test / acceptance proof for the in-app ISO9660 reader.
//
// Build (from the repo root):
//   g++ -std=c++17 -O2 -Wall -Wextra -o build\gscheck\iso_extract_test.exe ^
//       tools\gscheck\iso_extract_test.cpp android-runner\native\iso\iso9660.cpp ^
//       android-runner\native\iso\sha1.cpp -I android-runner\native
//   (or use cl.exe from a VS Developer Command Prompt with the same sources)
//
// Run against the real disc image:
//   build\gscheck\iso_extract_test.exe "C:\...\God of War II (USA).iso" ^
//       build\gscheck\real-out --compare build\iso-list-py.txt
// where build\iso-list-py.txt is produced by:
//   python -I tools\iso_extract.py "<iso>" --list > build\iso-list-py.txt
//
// Run the synthetic edge-case test (multi-extent file, dual-volume layout is
// covered by the real image, non-sector-aligned directory sizes, fd input):
//   build\gscheck\iso_extract_test.exe --self-test build\gscheck\selftest
//
// Exit code 0 = every check passed, 1 = at least one failure.
#if defined(_WIN32)
#  define _CRT_SECURE_NO_WARNINGS 1  // _open/_close are the Windows fd stand-in
#endif

#include "iso/iso9660.h"
#include "iso/sha1.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace
{

constexpr char kExpectedElfSha1[] = "E6A62A0F3E781C29513F563F4625B9B3A9103C56";
constexpr uint32_t kExpectedLayer1Start = 2083872u;  // God of War II USA v1.01

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& name, const std::string& detail = std::string())
{
    ++g_checks;
    if (condition)
    {
        std::cout << "[ok]   " << name;
    }
    else
    {
        ++g_failures;
        std::cout << "[FAIL] " << name;
    }
    if (!detail.empty())
        std::cout << " (" << detail << ")";
    std::cout << "\n";
}

std::vector<uint8_t> ReadLocalFile(const std::filesystem::path& path)
{
    std::ifstream file(path.string().c_str(), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string Trim(const std::string& text)
{
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
        ++begin;
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
        --end;
    return text.substr(begin, end - begin);
}

std::string Upper(std::string text)
{
    for (char& c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

void PutLe16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void PutBe16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

void PutLe32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

void PutBe32(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

std::vector<uint8_t> MakeDirectoryRecord(const std::vector<uint8_t>& name, uint32_t lsn, uint32_t size, uint8_t flags)
{
    const size_t length = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
    std::vector<uint8_t> record(length, 0);
    record[0] = static_cast<uint8_t>(length);
    PutLe32(&record[2], lsn);
    PutBe32(&record[6], lsn);
    PutLe32(&record[10], size);
    PutBe32(&record[14], size);
    record[25] = flags;
    PutLe16(&record[28], 1);
    PutBe16(&record[30], 1);
    record[32] = static_cast<uint8_t>(name.size());
    std::memcpy(&record[33], name.data(), name.size());
    return record;
}

std::vector<uint8_t> MakeDirectoryRecord(const std::string& nameWithVersion, uint32_t lsn, uint32_t size, uint8_t flags)
{
    return MakeDirectoryRecord(std::vector<uint8_t>(nameWithVersion.begin(), nameWithVersion.end()), lsn, size, flags);
}

// Builds a minimal 30-sector single-layer ISO9660 image with:
//   SYSTEM.CNF, ELF.BIN, a two-extent BIG.BIN (2148 bytes), a root TEST.IRX,
//   and SUBDIR/NESTED.IRX. The directory data lengths are NOT sector aligned,
//   which exercises the record-walking code.
std::vector<uint8_t> BuildSyntheticIso()
{
    constexpr uint32_t kSectors = 30;
    std::vector<uint8_t> image(size_t(kSectors) * 2048u, 0);

    const std::string cnf = "BOOT2 = cdrom0:\\ELF.BIN;1\r\nVER = 1.00\r\n";
    const std::vector<uint8_t> elf(512, 0x5A);
    std::vector<uint8_t> bigFirst(2048);
    for (size_t i = 0; i < bigFirst.size(); ++i)
        bigFirst[i] = static_cast<uint8_t>(i * 7u);
    std::vector<uint8_t> bigSecond(100, 0xC3);
    const std::vector<uint8_t> nested(50, 0x11);
    const std::vector<uint8_t> testIrx(64, 0x22);

    const auto appendRecord = [](std::vector<uint8_t>& directory, const std::vector<uint8_t>& record) {
        directory.insert(directory.end(), record.begin(), record.end());
    };

    std::vector<uint8_t> rootDirectory;
    const std::vector<uint8_t> dot{0};
    const std::vector<uint8_t> dotdot{1};
    appendRecord(rootDirectory, MakeDirectoryRecord(dot, 20, 0, 0x02));
    appendRecord(rootDirectory, MakeDirectoryRecord(dotdot, 20, 0, 0x02));
    appendRecord(rootDirectory, MakeDirectoryRecord("SYSTEM.CNF;1", 21, uint32_t(cnf.size()), 0));
    appendRecord(rootDirectory, MakeDirectoryRecord("ELF.BIN;1", 22, uint32_t(elf.size()), 0));
    // Multi-extent: same name twice, flag 0x80 on all but the last record.
    appendRecord(rootDirectory, MakeDirectoryRecord("BIG.BIN;1", 23, 2048, 0x80));
    appendRecord(rootDirectory, MakeDirectoryRecord("BIG.BIN;1", 25, uint32_t(bigSecond.size()), 0));
    appendRecord(rootDirectory, MakeDirectoryRecord("SUBDIR", 26, 0, 0x02));
    appendRecord(rootDirectory, MakeDirectoryRecord("TEST.IRX;1", 28, uint32_t(testIrx.size()), 0));
    // Fill the sizes of the "." and ".." records now that the directory is complete.
    for (size_t offset = 0; offset < rootDirectory.size();)
    {
        const size_t length = rootDirectory[offset];
        const uint8_t nameLength = rootDirectory[offset + 32];
        if (nameLength == 1 && (rootDirectory[offset + 33] == 0 || rootDirectory[offset + 33] == 1))
        {
            PutLe32(&rootDirectory[offset + 10], uint32_t(rootDirectory.size()));
            PutBe32(&rootDirectory[offset + 14], uint32_t(rootDirectory.size()));
        }
        offset += length;
    }

    std::vector<uint8_t> subDirectory;
    const std::vector<uint8_t> subDot{0};
    const std::vector<uint8_t> subDotdot{1};
    appendRecord(subDirectory, MakeDirectoryRecord(subDot, 26, 0, 0x02));
    appendRecord(subDirectory, MakeDirectoryRecord(subDotdot, 20, uint32_t(rootDirectory.size()), 0x02));
    appendRecord(subDirectory, MakeDirectoryRecord("NESTED.IRX;1", 27, uint32_t(nested.size()), 0));
    for (size_t offset = 0; offset < subDirectory.size();)
    {
        const size_t length = subDirectory[offset];
        const uint8_t nameLength = subDirectory[offset + 32];
        if (nameLength == 1 && (subDirectory[offset + 33] == 0 || subDirectory[offset + 33] == 1))
        {
            PutLe32(&subDirectory[offset + 10], uint32_t(subDirectory.size()));
            PutBe32(&subDirectory[offset + 14], uint32_t(subDirectory.size()));
        }
        offset += length;
    }
    // Patch the root "SUBDIR" record length to the real subdirectory size.
    for (size_t offset = 0; offset < rootDirectory.size();)
    {
        const size_t length = rootDirectory[offset];
        const std::string name(reinterpret_cast<const char*>(&rootDirectory[offset + 33]), rootDirectory[offset + 32]);
        if (name == "SUBDIR")
        {
            PutLe32(&rootDirectory[offset + 10], uint32_t(subDirectory.size()));
            PutBe32(&rootDirectory[offset + 14], uint32_t(subDirectory.size()));
        }
        offset += length;
    }

    // Primary volume descriptor at LSN 16.
    {
        uint8_t* pvd = &image[16 * 2048];
        pvd[0] = 1;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        PutLe32(pvd + 80, kSectors);
        PutBe32(pvd + 84, kSectors);
        PutLe16(pvd + 120, 1);
        PutBe16(pvd + 122, 1);
        PutLe16(pvd + 124, 1);
        PutBe16(pvd + 126, 1);
        PutLe16(pvd + 128, 2048);
        PutBe16(pvd + 130, 2048);
        const std::vector<uint8_t> rootRecord = MakeDirectoryRecord(dot, 20, uint32_t(rootDirectory.size()), 0x02);
        std::memcpy(pvd + 156, rootRecord.data(), rootRecord.size());
    }
    // Volume descriptor set terminator at LSN 17.
    image[17 * 2048] = 255;
    std::memcpy(&image[17 * 2048 + 1], "CD001", 5);
    image[17 * 2048 + 6] = 1;

    auto place = [&](uint32_t lsn, const std::vector<uint8_t>& data) {
        std::memcpy(&image[size_t(lsn) * 2048], data.data(), data.size());
    };
    place(20, rootDirectory);
    place(21, std::vector<uint8_t>(cnf.begin(), cnf.end()));
    place(22, elf);
    place(23, bigFirst);
    place(25, bigSecond);
    place(26, subDirectory);
    place(27, nested);
    place(28, testIrx);
    return image;
}

bool LoadPythonListing(const std::filesystem::path& path,
                       std::map<std::string, std::pair<uint32_t, uint32_t>>& listing,
                       std::string& error)
{
    std::ifstream file(path.string().c_str());
    if (!file)
    {
        error = "cannot open " + path.string();
        return false;
    }
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream stream(line);
        uint64_t lsn = 0;
        uint64_t size = 0;
        if (!(stream >> lsn >> size))
            continue;
        std::string name;
        std::getline(stream, name);
        name = Upper(Trim(name));
        if (name.empty())
            continue;
        listing[name] = {static_cast<uint32_t>(lsn), static_cast<uint32_t>(size)};
    }
    return true;
}

int RunSelfTest(const std::filesystem::path& outDir)
{
    std::cout << "== synthetic ISO9660 self-test\n";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // SHA1 known-answer test (RFC 3174).
    Check(ps2x::iso::Sha1::Hex("abc", 3) == "A9993E364706816ABA3E25717850C26C9CD0D89D",
          "SHA1 known-answer (abc)");

    const std::filesystem::path isoPath = outDir / "synthetic.iso";
    const std::vector<uint8_t> image = BuildSyntheticIso();
    {
        std::ofstream file(isoPath.string().c_str(), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!file)
        {
            std::cerr << "cannot write " << isoPath << "\n";
            return 1;
        }
    }
    Check(std::filesystem::file_size(isoPath) == image.size(), "synthetic image written");

    ps2x::iso::IsoImage iso;
    std::string error;
    Check(iso.OpenPath(isoPath.string().c_str(), error), "OpenPath(synthetic)", error);
    Check(iso.layer1Start() == 0, "single volume reports layer1Start() == 0");
    Check(iso.Entries().size() == 5, "entry count", "expected 5, got " + std::to_string(iso.Entries().size()));

    const auto big = iso.Find("big.bin;1");
    Check(big.has_value(), "Find is case/version insensitive");
    if (big)
    {
        Check(big->lsn == 23, "multi-extent first LSN", "got " + std::to_string(big->lsn));
        Check(big->size == 2148, "multi-extent total size", "got " + std::to_string(big->size));
        std::vector<uint8_t> data;
        Check(iso.ReadFile(*big, data), "ReadFile(multi-extent)");
        bool contentOk = data.size() == 2148;
        for (size_t i = 0; contentOk && i < 2048; ++i)
            contentOk = data[i] == static_cast<uint8_t>(i * 7u);
        for (size_t i = 0; contentOk && i < 100; ++i)
            contentOk = data[2048 + i] == 0xC3;
        Check(contentOk, "multi-extent data concatenated in order");
    }

    const auto nested = iso.Find("SUBDIR/NESTED.IRX");
    Check(nested.has_value(), "Find nested path");
    if (nested)
    {
        std::vector<uint8_t> data;
        Check(iso.ReadFile(*nested, data) && data.size() == 50, "ReadFile nested entry");
    }

    // ExtractBootData: synthetic ELF intentionally has the wrong SHA1, so a
    // mismatch must be reported without failing the extraction.
    const std::filesystem::path extractDir = outDir / "extracted";
    const auto result = ps2x::iso::ExtractBootData(isoPath.string().c_str(), extractDir.string().c_str());
    Check(result.ok, "ExtractBootData(synthetic) ok", result.error);
    Check(result.elfName == "ELF.BIN", "boot ELF name from SYSTEM.CNF", result.elfName);
    Check(!result.sha1Matches, "SHA1 mismatch is reported (non-fatal)");
    Check(result.elfSha1 == ps2x::iso::Sha1::Hex(std::vector<uint8_t>(512, 0x5A).data(), 512),
          "reported SHA1 matches the ELF bytes", result.elfSha1);
    Check(std::filesystem::exists(extractDir / "ELF.BIN") &&
              std::filesystem::file_size(extractDir / "ELF.BIN") == 512,
          "ELF extracted to <out>/NAME");
    {
        bool nameOk = false;
        if (std::filesystem::exists(extractDir / "elf-name.txt"))
        {
            const std::vector<uint8_t> nameBytes = ReadLocalFile(extractDir / "elf-name.txt");
            nameOk = Trim(std::string(nameBytes.begin(), nameBytes.end())) == "ELF.BIN";
        }
        Check(nameOk, "elf-name.txt content");
    }
    Check(std::filesystem::exists(extractDir / "TEST.IRX") &&
              std::filesystem::file_size(extractDir / "TEST.IRX") == 64,
          "root *.IRX extracted");
    Check(std::filesystem::exists(extractDir / "IOP_MOD" / "TEST.IRX") &&
              std::filesystem::file_size(extractDir / "IOP_MOD" / "TEST.IRX") == 64,
          "*.IRX copied to IOP_MOD/");
    Check(!std::filesystem::exists(extractDir / "IOP_MOD" / "NESTED.IRX"),
          "nested *.IRX is not treated as a root IOP module");
    Check(!std::filesystem::exists(extractDir / "NESTED.IRX"),
          "nested *.IRX is not extracted to the root");

    // File-descriptor input (the Android SAF code path).
    {
#if defined(_WIN32)
        const int fd = _open(isoPath.string().c_str(), _O_RDONLY | _O_BINARY);
#else
        const int fd = ::open(isoPath.string().c_str(), O_RDONLY);
#endif
        Check(fd >= 0, "open synthetic image as fd");
        if (fd >= 0)
        {
            const auto fdResult = ps2x::iso::ExtractBootDataFromFd(fd, (extractDir / "fd").string().c_str());
            Check(fdResult.ok && fdResult.elfName == "ELF.BIN", "ExtractBootDataFromFd", fdResult.error);
#if defined(_WIN32)
            _close(fd);
#else
            ::close(fd);
#endif
        }
    }

    // A file that is not an ISO must fail cleanly (no exception, clear error).
    {
        const std::filesystem::path bogus = outDir / "not-an-iso.bin";
        std::ofstream file(bogus.string().c_str(), std::ios::binary | std::ios::trunc);
        const char text[] = "not a disc image";
        file.write(text, sizeof(text) - 1);
        file.close();
        ps2x::iso::IsoImage bad;
        std::string badError;
        Check(!bad.OpenPath(bogus.string().c_str(), badError) && !badError.empty(),
              "non-ISO file is rejected with an error", badError);
        const auto badResult = ps2x::iso::ExtractBootData(bogus.string().c_str(), (outDir / "bogus").string().c_str());
        Check(!badResult.ok && !badResult.error.empty(), "ExtractBootData reports ok=false on a non-ISO");
    }

    std::cout << "\n" << (g_failures == 0 ? "SELF-TEST PASSED" : "SELF-TEST FAILED")
              << " (" << (g_checks - g_failures) << "/" << g_checks << " checks)\n";
    return g_failures == 0 ? 0 : 1;
}

int RunRealIso(const std::filesystem::path& isoPath, const std::filesystem::path& outDir,
               bool listEntries, const std::filesystem::path* compareListing)
{
    std::cout << "== real disc image: " << isoPath << "\n";
    std::cout << "image size: " << std::filesystem::file_size(isoPath) << " bytes\n";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    ps2x::iso::IsoImage iso;
    std::string error;
    const bool opened = iso.OpenPath(isoPath.string().c_str(), error);
    Check(opened, "OpenPath", error);
    if (!opened)
        return 1;

    std::cout << "layer1Start: " << iso.layer1Start()
              << " (expected " << kExpectedLayer1Start << ")\n";
    Check(iso.layer1Start() == kExpectedLayer1Start, "dual-layer base LSN",
          "got " + std::to_string(iso.layer1Start()));

    Check(iso.Entries().size() == 16, "disc file count", "got " + std::to_string(iso.Entries().size()));
    Check(iso.Find("SYSTEM.CNF;1").has_value(), "Find(\"SYSTEM.CNF;1\")");
    Check(iso.Find("scus_974.81;1").has_value(), "Find is case-insensitive");
    Check(iso.Find("cdrom0:\\SCUS_974.81;1").has_value(), "Find accepts a cdrom0 path");
    Check(iso.Find("PART2.PAK").has_value(), "Find layer-1 file PART2.PAK");

    const auto part2 = iso.Find("PART2.PAK");
    if (part2)
    {
        Check(part2->lsn == 2084139u, "PART2.PAK absolute LSN resolved through layer 1",
              "got " + std::to_string(part2->lsn));
        Check(part2->size == 4265340928u, "PART2.PAK size",
              "got " + std::to_string(part2->size));
    }

    if (listEntries)
    {
        std::cout << "\n-- listing --\n";
        for (const ps2x::iso::Entry& entry : iso.Entries())
            std::cout << (entry.directory ? "d " : "f ") << entry.lsn << " " << entry.size << " " << entry.name << "\n";
    }

    if (compareListing != nullptr)
    {
        std::map<std::string, std::pair<uint32_t, uint32_t>> expected;
        std::string listingError;
        if (!LoadPythonListing(*compareListing, expected, listingError))
        {
            Check(false, "read tools/iso_extract.py listing", listingError);
        }
        else
        {
            std::map<std::string, std::pair<uint32_t, uint32_t>> actual;
            for (const ps2x::iso::Entry& entry : iso.Entries())
                if (!entry.directory)
                    actual[entry.name] = {entry.lsn, entry.size};

            bool same = expected.size() == actual.size();
            if (same)
            {
                for (const auto& [name, info] : expected)
                {
                    const auto found = actual.find(name);
                    if (found == actual.end() || found->second != info)
                    {
                        same = false;
                        break;
                    }
                }
            }
            Check(same, "file list (LSN + size) matches tools/iso_extract.py",
                  std::to_string(expected.size()) + " expected, " + std::to_string(actual.size()) + " found");
            if (!same)
            {
                for (const auto& [name, info] : expected)
                {
                    const auto found = actual.find(name);
                    if (found == actual.end())
                        std::cout << "  missing: " << name << "\n";
                    else if (found->second != info)
                        std::cout << "  mismatch: " << name << " expected " << info.first << "/" << info.second
                                  << " got " << found->second.first << "/" << found->second.second << "\n";
                }
                for (const auto& [name, info] : actual)
                    if (expected.find(name) == expected.end())
                        std::cout << "  unexpected: " << name << "\n";
            }
        }
    }

    const std::filesystem::path extractDir = outDir / "extracted";
    const auto result = ps2x::iso::ExtractBootData(isoPath.string().c_str(), extractDir.string().c_str());
    Check(result.ok, "ExtractBootData", result.error);
    std::cout << "elfName: " << result.elfName << "\n";
    std::cout << "elfSha1: " << result.elfSha1 << "\n";
    if (result.ok)
    {
        Check(result.elfName == "SCUS_974.81", "boot ELF name", result.elfName);
        Check(result.elfSha1 == kExpectedElfSha1, "boot ELF SHA1", result.elfSha1);
        Check(result.sha1Matches, "sha1Matches flag");

        const std::filesystem::path elfPath = extractDir / result.elfName;
        Check(std::filesystem::exists(elfPath), "ELF extracted");
        if (std::filesystem::exists(elfPath))
        {
            const std::vector<uint8_t> elfOnDisk = ReadLocalFile(elfPath);
            Check(elfOnDisk.size() == 2125688, "ELF size", std::to_string(elfOnDisk.size()));
            Check(ps2x::iso::Sha1::Hex(elfOnDisk.data(), elfOnDisk.size()) == kExpectedElfSha1,
                  "extracted ELF SHA1 on disk");
        }
        const std::filesystem::path nameFile = extractDir / "elf-name.txt";
        if (std::filesystem::exists(nameFile))
        {
            const std::vector<uint8_t> nameBytes = ReadLocalFile(nameFile);
            Check(Trim(std::string(nameBytes.begin(), nameBytes.end())) == "SCUS_974.81",
                  "elf-name.txt content");
        }
        else
        {
            Check(false, "elf-name.txt exists");
        }

        size_t rootIrx = 0;
        size_t iopModIrx = 0;
        for (const auto& item : std::filesystem::directory_iterator(extractDir))
            if (item.is_regular_file() && item.path().extension() == ".IRX")
                ++rootIrx;
        if (std::filesystem::exists(extractDir / "IOP_MOD"))
            for (const auto& item : std::filesystem::directory_iterator(extractDir / "IOP_MOD"))
                if (item.is_regular_file() && item.path().extension() == ".IRX")
                    ++iopModIrx;
        // God of War II USA v1.01 has 10 root-level IOP modules (plus
        // IOPRP300.IMG, which is not an IRX and is not needed by the runner).
        Check(rootIrx == 10, "root *.IRX count", std::to_string(rootIrx));
        Check(iopModIrx == 10, "IOP_MOD/*.IRX count", std::to_string(iopModIrx));
        Check(std::filesystem::exists(extractDir / "IOP_MOD" / "SIO2MAN.IRX"),
              "IOP_MOD/SIO2MAN.IRX extracted");
    }

    // fd path on the real image (Android SAF equivalent): open separately and
    // verify the same ELF entry can be read back byte-for-byte.
    {
#if defined(_WIN32)
        const int fd = _open(isoPath.string().c_str(), _O_RDONLY | _O_BINARY);
#else
        const int fd = ::open(isoPath.string().c_str(), O_RDONLY);
#endif
        Check(fd >= 0, "open the real image as fd");
        if (fd >= 0)
        {
            ps2x::iso::IsoImage imageFromFd;
            std::string fdError;
            Check(imageFromFd.OpenFd(fd, fdError), "IsoImage::OpenFd", fdError);
            if (imageFromFd.Entries().size() == 16)
            {
                const auto elf = imageFromFd.Find("SCUS_974.81");
                std::vector<uint8_t> data;
                Check(elf && imageFromFd.ReadFile(*elf, data) && data.size() == 2125688,
                      "ReadFile via OpenFd");
                if (!data.empty())
                    Check(ps2x::iso::Sha1::Hex(data.data(), data.size()) == kExpectedElfSha1,
                          "OpenFd ELF SHA1");
            }
#if defined(_WIN32)
            _close(fd);
#else
            ::close(fd);
#endif
        }
    }

    std::cout << "\n" << (g_failures == 0 ? "ACCEPTANCE PASSED" : "ACCEPTANCE FAILED")
              << " (" << (g_checks - g_failures) << "/" << g_checks << " checks)\n";
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: iso_extract_test <iso> <outdir> [--list] [--compare <python-list.txt>]\n"
                     "       iso_extract_test --self-test <outdir>\n";
        return 2;
    }

    const std::string first = argv[1];
    if (first == "--self-test")
    {
        if (argc < 3)
        {
            std::cerr << "usage: iso_extract_test --self-test <outdir>\n";
            return 2;
        }
        return RunSelfTest(std::filesystem::path(argv[2]));
    }

    if (argc < 3)
    {
        std::cerr << "usage: iso_extract_test <iso> <outdir> [--list] [--compare <python-list.txt>]\n";
        return 2;
    }

    const std::filesystem::path isoPath(first);
    const std::filesystem::path outDir(argv[2]);
    if (!std::filesystem::exists(isoPath))
    {
        std::cerr << "disc image not found: " << isoPath << "\n";
        return 2;
    }

    bool listEntries = false;
    std::filesystem::path compareListing;
    bool hasCompare = false;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--list")
        {
            listEntries = true;
        }
        else if (arg == "--compare" && i + 1 < argc)
        {
            compareListing = std::filesystem::path(argv[++i]);
            hasCompare = true;
        }
    }

    return RunRealIso(isoPath, outDir, listEntries, hasCompare ? &compareListing : nullptr);
}
