// HarnessFrame unit tests.
#include "doctest/doctest.h"
#include "host/harness_frame.h"
#include "support/emulator_fixture.h"
#include "resource.h"

#pragma warning(push, 3)
#include "Interface.h"
#pragma warning(pop)

#include <cstddef>
#include <cstring>
#include <vector>

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

// ---- M2: presented frame, present hook, sound (spec: docs/specs/m2.md, "src/host/harness_frame.h") ----

namespace {

// The shared emulator's frame. VideoPresentScreen reads the global video buffer, which only exists after Start.
HarnessFrame& SharedFrame()
{
    harness::testsupport::StartedEmulator();
    return static_cast<HarnessFrame&>(GetFrame());
}

// Row y (0 = top) of a view.
const uint8_t* Row(const harness::FrameView& v, uint32_t y) { return v.topRow + ptrdiff_t(y) * v.strideBytes; }

}  // namespace

TEST_CASE("frame: PresentedFrame is all zero before the first present")
{
    HarnessFrame f;
    auto b = f.PresentedFrame(true);
    REQUIRE(b.width == 600);
    REQUIRE(b.height == 420);
    CHECK(b.strideBytes == -2400);
    bool allZero = true;
    for (uint32_t y = 0; y < b.height; ++y)
        for (uint32_t i = 0; i < b.width * 4; ++i)
            allZero = allZero && Row(b, y)[i] == 0;
    CHECK(allZero);
    auto v = f.PresentedFrame(false);
    CHECK(v.width == 560);
    CHECK(v.height == 384);
    CHECK(v.strideBytes == -2400);
}

TEST_CASE("frame: PresentedFrame is a snapshot of the video buffer at the last present")
{
    HarnessFrame& f = SharedFrame();
    Video& video = GetVideo();
    const uint32_t w = video.GetFrameBufferWidth(), h = video.GetFrameBufferHeight();
    uint8_t* live = video.GetFrameBuffer();   // bottom-up: row 0 is the bottom row
    REQUIRE(live != nullptr);
    const size_t bytes = size_t(w) * h * 4;
    std::vector<uint8_t> pattern(bytes);
    for (size_t i = 0; i < bytes; ++i)
        pattern[i] = uint8_t(i * 7 + 3);
    std::memcpy(live, pattern.data(), bytes);

    f.VideoPresentScreen();
    std::memset(live, 0xAB, bytes);   // the next frame starts scanning; the snapshot must not follow it

    auto b = f.PresentedFrame(true);
    REQUIRE(b.width == w);
    REQUIRE(b.height == h);
    REQUIRE(b.strideBytes == -int32_t(w * 4));
    bool same = true;
    for (uint32_t y = 0; y < h; ++y)
        same = same && std::memcmp(Row(b, y), pattern.data() + size_t(h - 1 - y) * w * 4, size_t(w) * 4) == 0;
    CHECK(same);

    // Borderless view: same snapshot, moved down borderH (18) rows and right borderW (20) pixels.
    auto v = f.PresentedFrame(false);
    CHECK(v.width == 560);
    CHECK(v.height == 384);
    CHECK(v.strideBytes == b.strideBytes);
    CHECK(v.topRow == b.topRow + 18 * b.strideBytes + 20 * 4);
}

TEST_CASE("frame: the present hook is called once per present")
{
    HarnessFrame& f = SharedFrame();
    int calls = 0;
    f.SetPresentHook([&calls] { ++calls; });
    struct ClearHook {
        HarnessFrame& f;
        ~ClearHook() { try { f.SetPresentHook({}); } catch (...) {} }
    } clear{f};
    f.VideoPresentScreen();
    CHECK(calls == 1);
    f.VideoPresentScreen();
    f.VideoPresentScreen();
    CHECK(calls == 3);
}

TEST_CASE("frame: with sound disabled CreateSoundBuffer returns null and no buffer is counted")
{
    HarnessFrame f;   // sound is off by default
    CHECK(!f.CreateSoundBuffer(4096, 44100, 1, "x"));
    CHECK(f.SoundBuffersCreated() == 0);
    CHECK(f.SoundUnderruns() == 0);
}
