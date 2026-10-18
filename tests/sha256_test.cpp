// SPDX-License-Identifier: MIT
#include "doctest/doctest.h"
#include "core/sha256.h"
#include "support/test_images.h"
#include <cstring>

using namespace harness;

TEST_CASE("sha256: abc")
{
    const char* s = "abc";
    CHECK(Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), std::strlen(s))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256: empty")
{
    CHECK(Sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256: FrameHash is hash of top-down RGB")
{
    auto v = harness::testimg::View3x2();
    auto rgb = ToRgbTopDown(v);
    CHECK(FrameHash(v) == Sha256Hex(rgb));
}
