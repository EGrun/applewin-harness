// SPDX-License-Identifier: MIT
// AudioCapture and EncodeWav unit tests (spec: docs/specs/m3.md, D14 "Capture" and Part B "get_audio"; plan: M3 Part B
// Task 1, "capture:" and "wav:").
#include "doctest/doctest.h"
#include "core/audio_capture.h"
#include "core/wav.h"
#include "support/audio_signals.h"

#pragma warning(push, 3)
#include "StdAfx.h"
#include <windows.h>
#include "Common.h"
#pragma warning(pop)

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using harness::AudioCapture;
using harness::EncodeWav;
using namespace harness::testsupport;

namespace {

uint64_t F2C(uint64_t frame) { return AudioCapture::FrameToCycle(frame); }

// Checks that timeline frames [from, to) of `stereo` (interleaved, starting at frame 0) are (left, right).
void CheckFrames(const std::vector<int16_t>& stereo, uint64_t from, uint64_t to, int left, int right)
{
    REQUIRE(stereo.size() >= 2 * to);
    uint64_t bad = 0, firstBad = 0;
    for (uint64_t k = from; k < to; ++k) {
        if (stereo[2 * k] != left || stereo[2 * k + 1] != right) {
            if (bad++ == 0) firstBad = k;
        }
    }
    INFO("frames [" << from << ", " << to << ") expected (" << left << ", " << right << "); first mismatch at frame "
                    << firstBad << " = (" << (bad ? stereo[2 * firstBad] : 0) << ", "
                    << (bad ? stereo[2 * firstBad + 1] : 0) << ")");
    CHECK(bad == 0);
}

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
uint16_t U16(const std::vector<uint8_t>& b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }
std::string Tag(const std::vector<uint8_t>& b, size_t at) { return std::string(b.begin() + at, b.begin() + at + 4); }

}  // namespace

// ---- Frame <-> cycle ----

TEST_CASE("capture: constants match AppleWin and the 30 s ring")
{
    CHECK(harness::kAudioBaseClockHz == CLK_6502_NTSC);
    CHECK(harness::kAudioSampleRate == 44100u);
    CHECK(harness::kAudioChannels == 2);
    CHECK(harness::kAudioRingFrames == 1323000u);
    CHECK(double(harness::kCyclesPerFrameNum) / double(harness::kCyclesPerFrameDen) ==
          doctest::Approx(CLK_6502_NTSC / 44100.0).epsilon(1e-12));
}

TEST_CASE("capture: frame/cycle conversions round-trip")
{
    CHECK(AudioCapture::FrameToCycle(0) == 0);
    CHECK(AudioCapture::FrameToCycle(1) == 24);              // ceil(23.1402)
    CHECK(AudioCapture::FrameToCycle(44100) == 1020485);     // ceil(1,020,484.32)
    CHECK(AudioCapture::CycleToFrame(0) == 0);
    CHECK(AudioCapture::CycleToFrame(23) == 0);
    CHECK(AudioCapture::CycleToFrame(24) == 1);
    CHECK(AudioCapture::CycleToFrame(1020484) == 44099);
    CHECK(AudioCapture::CycleToFrame(1020485) == 44100);

    uint64_t bad = 0;
    for (uint64_t k = 0; k < 200000; ++k) {
        const uint64_t c = AudioCapture::FrameToCycle(k);
        if (AudioCapture::CycleToFrame(c) != k) ++bad;
        if (k > 0 && AudioCapture::CycleToFrame(c - 1) != k - 1) ++bad;   // c is the first cycle of frame k
    }
    CHECK(bad == 0);
    // Large values (days of emulated time) stay exact.
    for (uint64_t k : {uint64_t(1) << 32, uint64_t(1000000000007ull), uint64_t(1) << 40}) {
        CAPTURE(k);
        CHECK(AudioCapture::CycleToFrame(AudioCapture::FrameToCycle(k)) == k);
    }
}

// ---- Placement and mixing ----

TEST_CASE("capture: an empty capture")
{
    AudioCapture c;
    CHECK(c.NewestCycle() == 0);
    CHECK(c.OldestCycle() == 0);
    CHECK(c.Read(0, 1000000).empty());
}

TEST_CASE("capture: two voices at different rates mix with saturation")
{
    AudioCapture c;
    // Voice 1, like AppleWin's speaker: mono, one sample per 23 cycles (~44,369 Hz), 441 samples per commit. +20000 for
    // the first 20 commits, then -20000.
    const uint64_t kSpkCycles = 441 * 23;
    for (int i = 0; i < 40; ++i) {
        const std::vector<int16_t> s = Constant(441, int16_t(i < 20 ? 20000 : -20000));
        c.Commit(1, s, 1, kSpkCycles * uint64_t(i + 1));
    }
    // Voice 2, like the Mockingboard: stereo at exactly 44,100 Hz (L = +20000, R = -20000), 735 frames per commit.
    for (int i = 0; i < 24; ++i) {
        std::vector<int16_t> s(735 * 2);
        for (size_t k = 0; k < 735; ++k) {
            s[2 * k] = 20000;
            s[2 * k + 1] = -20000;
        }
        c.Commit(2, s, 2, F2C(735 * uint64_t(i + 1)));
    }

    const uint64_t spkEnd = kSpkCycles * 40;    // 405,720
    const uint64_t mbEnd = F2C(735 * 24);       // frame 17,640
    CHECK(c.NewestCycle() == (std::max)(spkEnd, mbEnd));
    CHECK(c.OldestCycle() == 0);
    const std::vector<int16_t> all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 17640);

    const uint64_t flip = AudioCapture::CycleToFrame(kSpkCycles * 20);   // speaker +20000 -> -20000
    const uint64_t spkEndFrame = AudioCapture::CycleToFrame(spkEnd);
    CheckFrames(all, 2, flip - 2, 32767, 0);                    // L: 20000 + 20000 saturates; R: 20000 - 20000
    CheckFrames(all, flip + 2, spkEndFrame - 2, 0, -32768);     // L: -20000 + 20000; R saturates negative
    CheckFrames(all, spkEndFrame + 2, 17640, 20000, -20000);    // the Mockingboard alone
}

TEST_CASE("capture: uneven commits are placed by the interval between them")
{
    AudioCapture c;
    // First commit: 441 frames ending at frame 1441. It covers the 441 x CLK/44100 cycles before: frames [1000, 1441).
    c.Commit(0, Constant(441, 100), 1, F2C(1441));
    CHECK(c.NewestCycle() == F2C(1441));
    // Stretched: a 100-sample ramp (0, 10, ..., 990) over the 2,000 frames [1441, 3441).
    std::vector<int16_t> ramp(100);
    for (int i = 0; i < 100; ++i) ramp[size_t(i)] = int16_t(10 * i);
    c.Commit(0, ramp, 1, F2C(3441));
    // Compressed: 300 samples over the 150 frames [3441, 3591).
    c.Commit(0, Constant(300, 300), 1, F2C(3591));
    // An empty interval adds nothing.
    c.Commit(0, Constant(50, 400), 1, F2C(3591));
    c.Commit(0, Constant(100, 500), 1, F2C(3691));

    CHECK(c.NewestCycle() == F2C(3691));
    const std::vector<int16_t> all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 3691);
    CheckFrames(all, 0, 998, 0, 0);
    CheckFrames(all, 1002, 1439, 100, 100);
    CheckFrames(all, 3443, 3589, 300, 300);
    CheckFrames(all, 3593, 3689, 500, 500);

    // The ramp is interpolated linearly: frame 1441 + j holds 10 x (j x 100 / 2000) = j / 2 (within 3), never
    // decreases, and moves by at most 2 per frame (zero-order hold or nearest-neighbour would jump by 10).
    uint64_t bad = 0, decreasing = 0, jumps = 0;
    for (uint64_t k = 1443; k < 3439; ++k) {
        const double expected = 10.0 * double(k - 1441) * 100.0 / 2000.0;
        if (std::abs(all[2 * k] - expected) > 3.0 || all[2 * k] != all[2 * k + 1]) ++bad;
        if (all[2 * k] < all[2 * (k - 1)]) ++decreasing;
        if (all[2 * k] - all[2 * (k - 1)] > 2) ++jumps;
    }
    CHECK(bad == 0);
    CHECK(decreasing == 0);
    CHECK(jumps == 0);
}

TEST_CASE("capture: a stereo commit keeps its channels; a mono commit goes to both")
{
    AudioCapture c;
    std::vector<int16_t> s(441 * 2);
    for (size_t k = 0; k < 441; ++k) {
        s[2 * k] = 1000;
        s[2 * k + 1] = -1000;
    }
    c.Commit(0, s, 2, F2C(441));
    c.Commit(1, Constant(441, 7), 1, F2C(882));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 882);
    CheckFrames(all, 2, 439, 1000, -1000);
    CheckFrames(all, 443, 880, 7, 7);
}

TEST_CASE("capture: invalid commits throw std::invalid_argument")
{
    AudioCapture c;
    CHECK_THROWS_AS(c.Commit(0, Constant(6, 0), 3, 1000), std::invalid_argument);
    CHECK_THROWS_AS(c.Commit(0, Constant(5, 0), 2, 1000), std::invalid_argument);
    CHECK_THROWS_AS(c.Commit(0, Constant(4, 0), 0, 1000), std::invalid_argument);
}

// ---- Read ----

TEST_CASE("capture: Read is cut short at the newest sample")
{
    AudioCapture c;
    c.Commit(0, Constant(441, 5), 1, F2C(441));
    CHECK(c.Read(0, F2C(441) + 100000).size() == 2 * 441);
    CHECK(c.Read(F2C(100), F2C(200)).size() == 2 * 100);
    CHECK(c.Read(F2C(441) + 5, F2C(441) + 100000).empty());
    CHECK(c.Read(F2C(200), F2C(200)).empty());
}

TEST_CASE("capture: the ring keeps the newest 30 s and Read past it throws")
{
    AudioCapture c;
    // 30 s of one-second blocks, block s at value 100 x (s + 1), in 0.1 s commits.
    auto commitSecond = [&](uint64_t s) {
        for (uint64_t i = 0; i < 10; ++i)
            c.Commit(0, Constant(4410, int16_t(100 * (s + 1))), 1, F2C(s * 44100 + (i + 1) * 4410));
    };
    for (uint64_t s = 0; s < 30; ++s) commitSecond(s);
    CHECK(c.NewestCycle() == F2C(30 * 44100));
    CHECK(c.OldestCycle() == 0);   // exactly 1,323,000 frames: nothing dropped yet
    CHECK(c.Read(0, F2C(10)).size() == 20);

    commitSecond(30);   // the 31st second pushes the first one out
    CHECK(c.NewestCycle() == F2C(31 * 44100));
    CHECK(c.OldestCycle() == F2C(44100));
    const auto second2 = c.Read(F2C(44100), F2C(2 * 44100));
    REQUIRE(second2.size() == 2 * 44100);
    CheckFrames(second2, 2, 44100 - 2, 200, 200);
    const auto last = c.Read(F2C(30 * 44100), c.NewestCycle());
    REQUIRE(last.size() == 2 * 44100);
    CheckFrames(last, 2, 44100 - 2, 3100, 3100);
    CHECK(c.Read(c.OldestCycle(), c.NewestCycle()).size() == 2 * harness::kAudioRingFrames);

    CHECK_THROWS_AS(c.Read(c.OldestCycle() - 1, c.OldestCycle() + 1000), std::out_of_range);
    CHECK_THROWS_AS(c.Read(0, 10), std::out_of_range);
}

namespace {

// A sample value that changes every frame and doesn't repeat with the ring's period.
int16_t SeamValue(uint64_t frame) { return int16_t(int64_t((frame * 7) % 30011) - 15005); }

// Checks that the frames [from, to) read from the capture hold SeamValue(frame) (within 1).
void CheckSeamValues(const AudioCapture& c, uint64_t from, uint64_t to)
{
    const auto got = c.Read(F2C(from), F2C(to));
    REQUIRE(got.size() == 2 * (to - from));
    uint64_t bad = 0, firstBad = 0;
    for (uint64_t k = from; k < to; ++k) {
        const int want = SeamValue(k);
        const size_t i = size_t(k - from);
        if (std::abs(got[2 * i] - want) > 1 || std::abs(got[2 * i + 1] - want) > 1)
            if (bad++ == 0) firstBad = k;
    }
    INFO("frames [" << from << ", " << to << "): first mismatch at frame " << firstBad);
    CHECK(bad == 0);
}

}  // namespace

TEST_CASE("capture: commits that straddle the ring's seam keep every sample")
{
    // 4,409-frame commits (1,323,000 is not a multiple of 4,409), each placed one-to-one on the timeline, so commit 301
    // (frames [1,322,709, 1,327,118)) crosses the ring's physical end. 305 commits = 1,344,745 frames.
    AudioCapture c;
    const uint64_t chunk = 4409, total = 305 * chunk;
    std::vector<int16_t> s(chunk);
    for (uint64_t done = 0; done < total; done += chunk) {
        for (uint64_t i = 0; i < chunk; ++i) s[size_t(i)] = SeamValue(done + i);
        c.Commit(0, s, 1, F2C(done + chunk));
    }
    CHECK(c.NewestCycle() == F2C(total));
    CHECK(c.OldestCycle() == F2C(total - harness::kAudioRingFrames));
    CheckSeamValues(c, harness::kAudioRingFrames - 3000, harness::kAudioRingFrames + 3000);   // across the seam
    CheckSeamValues(c, total - harness::kAudioRingFrames, total - harness::kAudioRingFrames + 3000);   // oldest
    CheckSeamValues(c, total - 3000, total);   // newest
}

TEST_CASE("capture: a single commit longer than the ring keeps its newest 30 s")
{
    AudioCapture c;
    const uint64_t total = 1400000;
    std::vector<int16_t> s(total);
    for (uint64_t k = 0; k < total; ++k) s[size_t(k)] = SeamValue(k);
    // A first commit covers N x CLK/44100 cycles before its end: frames [0, 1,400,000).
    c.Commit(0, s, 1, F2C(total));
    CHECK(c.NewestCycle() == F2C(total));
    CHECK(c.OldestCycle() == F2C(total - harness::kAudioRingFrames));
    CheckSeamValues(c, total - harness::kAudioRingFrames + 2, total - harness::kAudioRingFrames + 3000);
    CheckSeamValues(c, total - 3000, total);
    CHECK_THROWS_AS(c.Read(F2C(total - harness::kAudioRingFrames) - 1, F2C(total)), std::out_of_range);
}

TEST_CASE("capture: a paused voice leaves a silent gap")
{
    AudioCapture c;
    c.Commit(0, Constant(441, 100), 1, F2C(441));
    // Paused for 2 s (88,200 frames), then 441 frames: they land in the 441 frames before the end, not stretched.
    const uint64_t end = 441 + 88200 + 441;
    c.Commit(0, Constant(441, 200), 1, F2C(end));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * end);
    CheckFrames(all, 2, 439, 100, 100);
    CheckFrames(all, 443, end - 443, 0, 0);
    CheckFrames(all, end - 439, end - 2, 200, 200);
}

TEST_CASE("capture: a gap just under the threshold is still stretched")
{
    AudioCapture c;
    c.Commit(0, Constant(441, 100), 1, F2C(441));
    // 441 frames over 4,000 frames (~90 ms late): under the 100 ms slack, so they fill the whole interval.
    c.Commit(0, Constant(441, 200), 1, F2C(441 + 4000));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 4441);
    CheckFrames(all, 443, 4439, 200, 200);
    // Just over: 441 frames over 4,500 frames (> 441 + 4,410) are silence, then the last 441.
    c.Commit(0, Constant(441, 300), 1, F2C(4441 + 4900));
    const auto more = c.Read(0, c.NewestCycle());
    CheckFrames(more, 4443, 4441 + 4900 - 443, 0, 0);
    CheckFrames(more, 4441 + 4900 - 439, 4441 + 4900 - 2, 300, 300);
}

TEST_CASE("capture: a 22,050 Hz voice's first commit spans N x CLK/22050 cycles")
{
    AudioCapture c;
    c.RegisterVoice(5, 22050);
    // 441 samples at 22,050 Hz last 10 ms = 882 timeline frames: [2000, 2882).
    c.Commit(5, Constant(441, 100), 1, F2C(2882));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 2882);
    CheckFrames(all, 0, 1998, 0, 0);
    CheckFrames(all, 2002, 2880, 100, 100);
    // After a 2 s pause the next commit is again 882 frames long, not 441.
    const uint64_t end = 2882 + 88200 + 882;
    c.Commit(5, Constant(441, 200), 1, F2C(end));
    const auto more = c.Read(0, c.NewestCycle());
    REQUIRE(more.size() == 2 * end);
    CheckFrames(more, 2884, end - 882 - 2, 0, 0);
    CheckFrames(more, end - 880, end - 2, 200, 200);
}

TEST_CASE("capture: Clear empties the timeline and forgets every voice")
{
    AudioCapture c;
    c.Commit(0, Constant(441, 100), 1, F2C(441));
    c.Commit(1, Constant(441, 100), 1, F2C(441));
    c.Clear();
    CHECK(c.NewestCycle() == 0);
    CHECK(c.OldestCycle() == 0);
    CHECK(c.Read(0, 10000000).empty());

    // Voice 0's next commit is a first commit again: frames [1000, 1441), not stretched from frame 441.
    c.Commit(0, Constant(441, 200), 1, F2C(1441));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 1441);
    CheckFrames(all, 0, 998, 0, 0);
    CheckFrames(all, 1002, 1439, 200, 200);
}

TEST_CASE("capture: ResetVoice makes the voice's next commit a first commit")
{
    AudioCapture c;
    c.Commit(0, Constant(441, 100), 1, F2C(441));
    c.ResetVoice(0);
    c.Commit(0, Constant(441, 200), 1, F2C(2441));
    const auto all = c.Read(0, c.NewestCycle());
    REQUIRE(all.size() == 2 * 2441);
    CheckFrames(all, 2, 439, 100, 100);
    CheckFrames(all, 443, 1998, 0, 0);
    CheckFrames(all, 2002, 2439, 200, 200);
}

// ---- WAV ----

TEST_CASE("wav: header fields and byte length")
{
    const std::vector<int16_t> s = {1, -2, 3, -4, 0x1234, -32768};
    const std::vector<uint8_t> w = EncodeWav(s, 44100, 2);
    REQUIRE(w.size() == 44 + 12);
    CHECK(Tag(w, 0) == "RIFF");
    CHECK(U32(w, 4) == 36 + 12);
    CHECK(Tag(w, 8) == "WAVE");
    CHECK(Tag(w, 12) == "fmt ");
    CHECK(U32(w, 16) == 16);
    CHECK(U16(w, 20) == 1);           // PCM
    CHECK(U16(w, 22) == 2);           // channels
    CHECK(U32(w, 24) == 44100);       // sample rate
    CHECK(U32(w, 28) == 44100 * 4);   // byte rate
    CHECK(U16(w, 32) == 4);           // block align
    CHECK(U16(w, 34) == 16);          // bits per sample
    CHECK(Tag(w, 36) == "data");
    CHECK(U32(w, 40) == 12);
    std::vector<int16_t> back(6);
    std::memcpy(back.data(), w.data() + 44, 12);   // little-endian host
    CHECK(back == s);
    CHECK(w[44] == 0x01);
    CHECK(w[45] == 0x00);
    CHECK(w[52] == 0x34);
    CHECK(w[53] == 0x12);
}

TEST_CASE("wav: mono, empty, and invalid input")
{
    const std::vector<uint8_t> m = EncodeWav(Constant(3, 9), 22050, 1);
    REQUIRE(m.size() == 44 + 6);
    CHECK(U16(m, 22) == 1);
    CHECK(U32(m, 24) == 22050);
    CHECK(U32(m, 28) == 44100);
    CHECK(U16(m, 32) == 2);
    CHECK(U32(m, 40) == 6);

    const std::vector<uint8_t> e = EncodeWav({}, 44100, 2);
    REQUIRE(e.size() == 44);
    CHECK(U32(e, 4) == 36);
    CHECK(U32(e, 40) == 0);

    CHECK_THROWS_AS(EncodeWav(Constant(3, 0), 44100, 2), std::invalid_argument);
    CHECK_THROWS_AS(EncodeWav(Constant(4, 0), 44100, 0), std::invalid_argument);
    CHECK_THROWS_AS(EncodeWav(Constant(6, 0), 44100, 3), std::invalid_argument);
    CHECK_THROWS_AS(EncodeWav(Constant(4, 0), 0, 2), std::invalid_argument);
}
