// SPDX-License-Identifier: MIT
#pragma once
#include "doctest/doctest.h"
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <windows.h>
#include "zlib.h"

namespace harness::testsupport {

struct PngInfo {
    uint32_t width = 0, height = 0;
    int bitDepth = 0, colorType = 0;
    bool allCrcsValid = false;
    std::vector<std::vector<uint8_t>> rgbRows;   // each row with the filter byte stripped
};

inline uint32_t BE32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Checks signature, walks chunks, verifies each CRC with zlib crc32, inflates IDAT, strips filter bytes.
// Fails the test (FAIL_CHECK) if any filter byte is not 0 or the structure is malformed.
inline PngInfo ReadPng(std::span<const uint8_t> png)
{
    PngInfo info;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 8 || std::memcmp(png.data(), sig, 8) != 0) {
        FAIL_CHECK("bad PNG signature");
        return info;
    }
    bool crcsOk = true;
    std::vector<uint8_t> idat;
    size_t pos = 8;
    bool sawIend = false;
    while (pos + 12 <= png.size()) {
        uint32_t len = BE32(&png[pos]);
        if (pos + 12 + size_t(len) > png.size()) { FAIL_CHECK("truncated chunk"); return info; }
        const uint8_t* type = &png[pos + 4];
        const uint8_t* data = &png[pos + 8];
        uint32_t stored = BE32(&png[pos + 8 + len]);
        uLong c = crc32(0L, Z_NULL, 0);
        c = crc32(c, type, 4 + len);   // CRC covers type + data
        if (uint32_t(c) != stored) crcsOk = false;
        if (std::memcmp(type, "IHDR", 4) == 0 && len == 13) {
            info.width = BE32(data);
            info.height = BE32(data + 4);
            info.bitDepth = data[8];
            info.colorType = data[9];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            sawIend = true;
        }
        pos += 12 + size_t(len);
    }
    info.allCrcsValid = crcsOk && sawIend;
    if (info.width == 0 || info.height == 0 || info.colorType != 2 || info.bitDepth != 8) return info;

    const size_t rowBytes = size_t(info.width) * 3;
    std::vector<uint8_t> raw((rowBytes + 1) * info.height);
    uLongf rawLen = uLongf(raw.size());
    int rc = uncompress(raw.data(), &rawLen, idat.data(), uLong(idat.size()));
    if (rc != Z_OK || rawLen != raw.size()) { FAIL_CHECK("IDAT inflate failed or wrong size"); return info; }
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint8_t* row = &raw[y * (rowBytes + 1)];
        if (row[0] != 0) FAIL_CHECK("row filter byte is not 0");
        info.rgbRows.emplace_back(row + 1, row + 1 + rowBytes);
    }
    return info;
}

}  // namespace harness::testsupport
