#include "sha1.h"

#include <cstring>

namespace ps2x::iso
{
namespace
{

inline uint32_t Rotl(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

inline uint32_t LoadBigEndian32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

} // namespace

Sha1::Sha1()
{
    Reset();
}

void Sha1::Reset()
{
    state_[0] = 0x67452301u;
    state_[1] = 0xEFCDAB89u;
    state_[2] = 0x98BADCFEu;
    state_[3] = 0x10325476u;
    state_[4] = 0xC3D2E1F0u;
    total_ = 0;
    buffered_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha1::Transform(const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = LoadBigEndian32(block + i * 4);
    for (int i = 16; i < 80; ++i)
        w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];

    for (int i = 0; i < 80; ++i)
    {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t temp = Rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotl(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::Update(const void* data, size_t size)
{
    if (size == 0)
        return;

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    total_ += static_cast<uint64_t>(size);

    if (buffered_ != 0)
    {
        const size_t freeBytes = sizeof(buffer_) - buffered_;
        const size_t take = size < freeBytes ? size : freeBytes;
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == sizeof(buffer_))
        {
            Transform(buffer_);
            buffered_ = 0;
        }
    }

    while (size >= sizeof(buffer_))
    {
        Transform(bytes);
        bytes += sizeof(buffer_);
        size -= sizeof(buffer_);
    }

    if (size != 0)
    {
        std::memcpy(buffer_, bytes, size);
        buffered_ = size;
    }
}

void Sha1::Final(uint8_t digest[20])
{
    const uint64_t bitCount = total_ * 8u;

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = static_cast<uint8_t>(bitCount >> (56 - 8 * i));

    // 0x80 followed by zeros up to byte 56 of the final block, then the 64-bit
    // big-endian length. At most 64 padding bytes are needed.
    uint8_t padding[72];
    const size_t paddingLength = (buffered_ < 56) ? (56 - buffered_) : (120 - buffered_);
    std::memset(padding, 0, paddingLength);
    padding[0] = 0x80;

    Update(padding, paddingLength);
    Update(lengthBytes, 8);

    for (int i = 0; i < 5; ++i)
    {
        digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }

    Reset();
}

std::string Sha1::Hex(const void* data, size_t size)
{
    Sha1 sha;
    sha.Update(data, size);

    uint8_t digest[20];
    sha.Final(digest);

    static const char kDigits[] = "0123456789ABCDEF";
    std::string text(40, '0');
    for (int i = 0; i < 20; ++i)
    {
        text[static_cast<size_t>(i) * 2 + 0] = kDigits[digest[i] >> 4];
        text[static_cast<size_t>(i) * 2 + 1] = kDigits[digest[i] & 0x0F];
    }
    return text;
}

} // namespace ps2x::iso
