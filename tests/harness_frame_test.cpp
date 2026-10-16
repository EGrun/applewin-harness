// HarnessFrame unit tests.
#include "doctest/doctest.h"
#include "host/harness_frame.h"
#include "resource.h"

using harness::HarnessFrame;

TEST_CASE("frame: GetResource finds the embedded ROM")
{
    HarnessFrame f;
    CHECK(f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 16384) != nullptr);
}

TEST_CASE("frame: GetResource unknown id returns null")
{
    HarnessFrame f;
    CHECK(f.GetResource(0xFFFF, "ROM", 16384) == nullptr);
}

TEST_CASE("frame: GetResource wrong size returns null")
{
    HarnessFrame f;
    CHECK(f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 1) == nullptr);
}

TEST_CASE("frame: FrameMessageBox records and returns IDOK")
{
    HarnessFrame f;
    CHECK(f.FrameMessageBox("t", "c", MB_OK) == IDOK);
    REQUIRE(!f.messages().empty());
    CHECK(f.messages().back() == "c: t");
}

TEST_CASE("frame: CreateSoundBuffer returns null")
{
    HarnessFrame f;
    CHECK(!f.CreateSoundBuffer(4096, 44100, 1, "x"));   // bool: SoundBuffer is incomplete, doctest cannot stringify it
}
