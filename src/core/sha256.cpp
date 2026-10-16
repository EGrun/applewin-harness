#include "core/sha256.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>

namespace harness {

std::string Sha256Hex(std::span<const uint8_t> data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    UCHAR digest[32] = {};
    static const UCHAR kEmpty = 0;
    const NTSTATUS st = BCryptHash(alg, nullptr, 0,
                                   const_cast<PUCHAR>(data.empty() ? &kEmpty : data.data()),
                                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0)
        throw std::runtime_error("BCryptHash failed");
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (UCHAR b : digest) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 15]);
    }
    return s;
}

std::string FrameHash(const FrameView& v)
{
    return Sha256Hex(ToRgbTopDown(v));
}

}  // namespace harness
