// Minimal, dependency-free SHA-1 (RFC 3174).
//
// The Android app verifies the boot ELF it extracts from the user's own disc
// image, and it must not depend on OpenSSL or any other external library.
// This is a plain public-domain-style implementation; it is not hardened
// against timing side channels because it only hashes local game data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2x::iso
{

class Sha1
{
public:
    Sha1();

    void Reset();
    void Update(const void* data, size_t size);
    void Final(uint8_t digest[20]);

    // Uppercase hex digest (matches Get-FileHash -Algorithm SHA1).
    static std::string Hex(const void* data, size_t size);

private:
    void Transform(const uint8_t block[64]);

    uint32_t state_[5];
    uint64_t total_;
    uint8_t buffer_[64];
    size_t buffered_;
};

} // namespace ps2x::iso
