// HeadlessFrame unit tests.
#include "doctest/doctest.h"
#include "host/headless_frame.h"
#include "resource.h"

using harness::HeadlessFrame;

TEST_CASE("headless: GetResource finds the embedded ROM")
{
    HeadlessFrame f;
    CHECK(f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 16384) != nullptr);
}

TEST_CASE("headless: GetResource unknown id returns null")
{
    HeadlessFrame f;
    CHECK(f.GetResource(0xFFFF, "ROM", 16384) == nullptr);
}

TEST_CASE("headless: GetResource wrong size returns null")
{
    HeadlessFrame f;
    CHECK(f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 1) == nullptr);
}

TEST_CASE("headless: FrameMessageBox records and returns IDOK")
{
    HeadlessFrame f;
    CHECK(f.FrameMessageBox("t", "c", MB_OK) == IDOK);
    REQUIRE(!f.messages().empty());
    CHECK(f.messages().back() == "c: t");
}

TEST_CASE("headless: CreateSoundBuffer returns null")
{
    HeadlessFrame f;
    CHECK(!f.CreateSoundBuffer(4096, 44100, 1, "x"));   // bool: SoundBuffer is incomplete, doctest cannot stringify it
}
