#include "doctest/doctest.h"
#include "core/png_writer.h"
#include "support/png_reader.h"
#include "support/test_images.h"
#include <fstream>
#include <iterator>
#include <stdexcept>

using namespace harness;
using namespace harness::testsupport;
using harness::testimg::View3x2;
using harness::testimg::TopDownView3x2;

namespace {
std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("png: header")
{
    auto p = ReadPng(EncodePng(View3x2()));
    CHECK(p.width == 3);
    CHECK(p.height == 2);
    CHECK(p.bitDepth == 8);
    CHECK(p.colorType == 2);
}

TEST_CASE("png: crcs")
{
    CHECK(ReadPng(EncodePng(View3x2())).allCrcsValid);
}

TEST_CASE("png: pixels top-down")
{
    auto p = ReadPng(EncodePng(View3x2()));
    REQUIRE(p.rgbRows.size() == 2);
    CHECK(p.rgbRows[0] == std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255});
    CHECK(p.rgbRows[1] == std::vector<uint8_t>{255, 255, 255, 0, 0, 0, 128, 128, 128});
}

TEST_CASE("png: positive stride equals negative stride of same image")
{
    CHECK(EncodePng(TopDownView3x2()) == EncodePng(View3x2()));
}

TEST_CASE("png: invalid input")
{
    const uint8_t buf[24] = {};
    FrameView nullTop{nullptr, 3, 2, 12};
    FrameView zeroWidth{buf, 0, 2, 12};
    FrameView zeroHeight{buf, 3, 0, 12};
    CHECK_THROWS_AS(EncodePng(nullTop), std::invalid_argument);
    CHECK_THROWS_AS(EncodePng(zeroWidth), std::invalid_argument);
    CHECK_THROWS_AS(EncodePng(zeroHeight), std::invalid_argument);
}

TEST_CASE("png: file write")
{
    auto tmp = std::filesystem::temp_directory_path() / "applewin-harness";
    std::filesystem::create_directories(tmp);
    auto file = tmp / "a.png";
    std::filesystem::remove(file);
    CHECK(WritePngFile(file, View3x2()));
    CHECK(ReadFileBytes(file) == EncodePng(View3x2()));
    CHECK_FALSE(WritePngFile(tmp / "no_such_dir" / "a.png", View3x2()));
}

TEST_CASE("frame: ToRgbTopDown")
{
    auto rgb = ToRgbTopDown(View3x2());
    CHECK(rgb.size() == 18);
    REQUIRE(!rgb.empty());
    CHECK(rgb[0] == 255);
    CHECK(rgb == std::vector<uint8_t>{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 128, 128, 128});
}
