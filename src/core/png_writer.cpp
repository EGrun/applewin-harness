// SPDX-License-Identifier: MIT
#include "core/png_writer.h"
#include <zlib.h>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace harness {

namespace {

void PutU32(std::vector<uint8_t>& o, uint32_t v)
{
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void PutChunk(std::vector<uint8_t>& o, const char type[4], const uint8_t* data, size_t len)
{
    PutU32(o, static_cast<uint32_t>(len));
    const size_t typePos = o.size();
    o.insert(o.end(), type, type + 4);
    if (len)
        o.insert(o.end(), data, data + len);
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), o.data() + typePos, static_cast<uInt>(4 + len));
    PutU32(o, static_cast<uint32_t>(crc));
}

}  // namespace

std::vector<uint8_t> EncodePng(const FrameView& v)
{
    if (!v.topRow || v.width == 0 || v.height == 0)
        throw std::invalid_argument("EncodePng: empty frame");

    const size_t rowBytes = static_cast<size_t>(v.width) * 3;
    const std::vector<uint8_t> rgb = ToRgbTopDown(v);
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * v.height);
    for (uint32_t y = 0; y < v.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + y * rowBytes, rgb.begin() + (y + 1) * rowBytes);
    }

    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
        throw std::runtime_error("EncodePng: compress2 failed");

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, v.width);
    PutU32(ihdr, v.height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    PutChunk(out, "IHDR", ihdr.data(), ihdr.size());
    PutChunk(out, "IDAT", z.data(), zlen);
    PutChunk(out, "IEND", nullptr, 0);
    return out;
}

bool WritePngFile(const std::filesystem::path& path, const FrameView& v)
{
    const std::vector<uint8_t> png = EncodePng(v);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    f.flush();
    return static_cast<bool>(f);
}

}  // namespace harness
