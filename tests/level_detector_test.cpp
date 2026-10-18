// SPDX-License-Identifier: MIT
// LevelDetector unit tests (spec: docs/specs/m3.md, Part B "subscribe_audio"; plan: M3 Part B Task 1, "level:"; Review
// Focus 3).
#include "doctest/doctest.h"
#include "core/audio_capture.h"
#include "core/level_detector.h"
#include "support/audio_signals.h"

#include <cstdint>
#include <string>
#include <vector>

using harness::AudioCapture;
using harness::LevelDetector;
using harness::SoundEvent;
using namespace harness::testsupport;

namespace {

constexpr double kRate = 44100.0;

size_t Frames(int ms) { return size_t(ms) * 441 / 10; }
uint64_t F2C(uint64_t frame) { return AudioCapture::FrameToCycle(frame); }
constexpr uint64_t kBlockCycles = 10206;   // one 10 ms block (FrameToCycle(441) = 10,205) plus a cycle of slack

// Appends a section to a mono signal.
void Append(std::vector<int16_t>& s, const std::vector<int16_t>& part) { s.insert(s.end(), part.begin(), part.end()); }

std::string Describe(const std::vector<SoundEvent>& events)
{
    std::string out;
    for (const auto& e : events)
        out += std::string(e.kind == SoundEvent::Started ? "Started" : "Stopped") + "(" + std::to_string(e.startCycle) +
               ", " + std::to_string(e.endCycle) + ", " + std::to_string(e.peakDb) + " dB, " +
               std::to_string(e.dominantHz) + " Hz) ";
    return out;
}

// 0.5 s silence, 0.3 s of a 1 kHz square at -20 dBFS, 0.5 s silence; returns the tone's first and end frames.
std::pair<uint64_t, uint64_t> BellLikeSignal(AudioCapture& c)
{
    std::vector<int16_t> s = Constant(Frames(500), 0);
    Append(s, SquareWave(Frames(300), kRate, 1000.0, AmplitudeForDb(-20.0)));
    Append(s, Constant(Frames(500), 0));
    CommitOnTimeline(c, 0, s, 0);
    return {Frames(500), Frames(800)};
}

}  // namespace

TEST_CASE("level: a 1 kHz square at -20 dBFS gives Started then Stopped")
{
    AudioCapture c;
    const auto [toneStart, toneEnd] = BellLikeSignal(c);
    LevelDetector d(-50.0, 100);
    const std::vector<SoundEvent> ev = d.Feed(c, c.NewestCycle());
    INFO(Describe(ev));
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == SoundEvent::Started);
    CHECK(ev[0].startCycle + kBlockCycles >= F2C(toneStart));
    CHECK(ev[0].startCycle <= F2C(toneStart) + kBlockCycles);
    CHECK(ev[1].kind == SoundEvent::Stopped);
    CHECK(ev[1].startCycle == ev[0].startCycle);
    CHECK(ev[1].endCycle + kBlockCycles >= F2C(toneEnd));
    CHECK(ev[1].endCycle <= F2C(toneEnd) + kBlockCycles);
    CHECK(ev[1].dominantHz == doctest::Approx(1000.0).epsilon(0.01));
    CHECK(ev[1].peakDb == doctest::Approx(-20.0).epsilon(0.05));   // +/-1 dB
}

TEST_CASE("level: feeding in pieces gives the same events as one Feed")
{
    AudioCapture c;
    BellLikeSignal(c);
    LevelDetector whole(-50.0, 100), pieces(-50.0, 100);
    const std::vector<SoundEvent> one = whole.Feed(c, c.NewestCycle());
    std::vector<SoundEvent> many;
    for (uint64_t upto = 17030; upto < c.NewestCycle() + 17030; upto += 17030) {   // one video frame at a time
        const auto ev = pieces.Feed(c, upto);
        many.insert(many.end(), ev.begin(), ev.end());
    }
    INFO("one: " << Describe(one) << " pieces: " << Describe(many));
    REQUIRE(one.size() == 2);
    REQUIRE(many.size() == one.size());
    for (size_t i = 0; i < one.size(); ++i) {
        CHECK(many[i].kind == one[i].kind);
        CHECK(many[i].startCycle == one[i].startCycle);
        CHECK(many[i].endCycle == one[i].endCycle);
        CHECK(many[i].peakDb == doctest::Approx(one[i].peakDb));
        CHECK(many[i].dominantHz == doctest::Approx(one[i].dominantHz));
    }
}

TEST_CASE("level: a DC offset alone is not sound (Review Focus 3)")
{
    AudioCapture c;
    // 1 s at a constant +8000 (the speaker's idle level), then a -20 dBFS tone on top of it, then the DC level again.
    std::vector<int16_t> s = Constant(Frames(1000), 8000);
    Append(s, SquareWave(Frames(300), kRate, 1000.0, AmplitudeForDb(-20.0), 8000.0));
    Append(s, Constant(Frames(500), 8000));
    // The detector starts at block 2, past the timeline's first frames.
    LevelDetector d(-50.0, 100, F2C(2 * 441));

    CommitOnTimeline(c, 0, std::span<const int16_t>(s).first(Frames(1000)), 0);
    const std::vector<SoundEvent> idle = d.Feed(c, c.NewestCycle());
    INFO("idle: " << Describe(idle));
    CHECK(idle.empty());

    // Positive control: the same detector reports the tone, starting where the tone starts, not at the DC.
    CommitOnTimeline(c, 0, std::span<const int16_t>(s).subspan(Frames(1000)), Frames(1000));
    const std::vector<SoundEvent> ev = d.Feed(c, c.NewestCycle());
    INFO("tone: " << Describe(ev));
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == SoundEvent::Started);
    CHECK(ev[0].startCycle + kBlockCycles >= F2C(Frames(1000)));
    CHECK(ev[0].startCycle <= F2C(Frames(1000)) + kBlockCycles);
    CHECK(ev[1].kind == SoundEvent::Stopped);
    CHECK(ev[1].peakDb == doctest::Approx(-20.0).epsilon(0.05));
}

TEST_CASE("level: min_silence_ms bridges shorter gaps")
{
    // 0.2 s silence, 0.2 s tone, a 50 ms gap, 0.2 s tone, 0.5 s silence.
    std::vector<int16_t> s = Constant(Frames(200), 0);
    Append(s, SquareWave(Frames(200), kRate, 1000.0, AmplitudeForDb(-20.0)));
    Append(s, Constant(Frames(50), 0));
    Append(s, SquareWave(Frames(200), kRate, 1000.0, AmplitudeForDb(-20.0)));
    Append(s, Constant(Frames(500), 0));
    AudioCapture c;
    CommitOnTimeline(c, 0, s, 0);

    SUBCASE("100 ms: one sound across the gap")
    {
        LevelDetector d(-50.0, 100);
        const auto ev = d.Feed(c, c.NewestCycle());
        INFO(Describe(ev));
        REQUIRE(ev.size() == 2);
        CHECK(ev[0].kind == SoundEvent::Started);
        CHECK(ev[1].kind == SoundEvent::Stopped);
        CHECK(ev[1].endCycle + kBlockCycles >= F2C(Frames(650)));
        CHECK(ev[1].endCycle <= F2C(Frames(650)) + kBlockCycles);
    }
    SUBCASE("20 ms: two sounds")
    {
        LevelDetector d(-50.0, 20);
        const auto ev = d.Feed(c, c.NewestCycle());
        INFO(Describe(ev));
        REQUIRE(ev.size() == 4);
        CHECK(ev[0].kind == SoundEvent::Started);
        CHECK(ev[1].kind == SoundEvent::Stopped);
        CHECK(ev[2].kind == SoundEvent::Started);
        CHECK(ev[3].kind == SoundEvent::Stopped);
        CHECK(ev[1].endCycle <= F2C(Frames(400)) + kBlockCycles);
        CHECK(ev[2].startCycle + kBlockCycles >= F2C(Frames(450)));
    }
}

TEST_CASE("level: the defaults are -50 dB and 100 ms")
{
    // A -45 dB tone with a 90 ms gap (bridged by 100 ms, not by 80 ms), then a -55 dB tone (below -50, above -60).
    std::vector<int16_t> s = Constant(Frames(200), 0);
    Append(s, SquareWave(Frames(200), kRate, 1000.0, AmplitudeForDb(-45.0)));
    Append(s, Constant(Frames(90), 0));
    Append(s, SquareWave(Frames(200), kRate, 1000.0, AmplitudeForDb(-45.0)));
    Append(s, Constant(Frames(500), 0));
    Append(s, SquareWave(Frames(300), kRate, 1000.0, AmplitudeForDb(-55.0)));
    Append(s, Constant(Frames(500), 0));
    AudioCapture c;
    CommitOnTimeline(c, 0, s, 0);
    LevelDetector d;
    const auto ev = d.Feed(c, c.NewestCycle());
    INFO(Describe(ev));
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].kind == SoundEvent::Started);
    CHECK(ev[0].startCycle + kBlockCycles >= F2C(Frames(200)));
    CHECK(ev[0].startCycle <= F2C(Frames(200)) + kBlockCycles);
    CHECK(ev[1].kind == SoundEvent::Stopped);
    CHECK(ev[1].endCycle + kBlockCycles >= F2C(Frames(690)));
    CHECK(ev[1].endCycle <= F2C(Frames(690)) + kBlockCycles);
    CHECK(ev[1].peakDb == doctest::Approx(-45.0).epsilon(0.025));
}

TEST_CASE("level: a tone below the threshold is not sound")
{
    std::vector<int16_t> s = Constant(Frames(200), 0);
    Append(s, SquareWave(Frames(300), kRate, 1000.0, AmplitudeForDb(-60.0)));
    Append(s, Constant(Frames(500), 0));
    AudioCapture c;
    CommitOnTimeline(c, 0, s, 0);
    LevelDetector quiet(-50.0, 100);
    CHECK(quiet.Feed(c, c.NewestCycle()).empty());
    LevelDetector sensitive(-70.0, 100);   // positive control
    const auto ev = sensitive.Feed(c, c.NewestCycle());
    INFO(Describe(ev));
    REQUIRE(ev.size() == 2);
    CHECK(ev[1].peakDb == doctest::Approx(-60.0).epsilon(0.02));
}

TEST_CASE("level: a sound still going gives only Started")
{
    std::vector<int16_t> s = Constant(Frames(200), 0);
    Append(s, SquareWave(Frames(500), kRate, 1000.0, AmplitudeForDb(-20.0)));
    AudioCapture c;
    CommitOnTimeline(c, 0, s, 0);
    LevelDetector d(-50.0, 100);
    const auto ev = d.Feed(c, c.NewestCycle());
    INFO(Describe(ev));
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == SoundEvent::Started);
}

TEST_CASE("level: a slow DC ramp is not sound, but a tone riding on it is (B8 detrend)")
{
    // AppleWin's speaker idles at -32768 and ramps to 0 at one unit per sample (about -48 dBFS mean-only AC RMS).
    std::vector<int16_t> ramp;
    for (int i = 0; i < 32768; ++i) ramp.push_back(int16_t(-32768 + i));
    Append(ramp, Constant(Frames(300), 0));
    {
        AudioCapture c;
        CommitOnTimeline(c, 0, ramp, 0);
        LevelDetector d;
        const auto ev = d.Feed(c, c.NewestCycle());
        INFO(Describe(ev));
        CHECK(ev.empty());
    }
    {
        // Positive control: a 1 kHz -45 dB tone on the same ramp.
        const std::vector<int16_t> tone = SquareWave(Frames(300), kRate, 1000.0, AmplitudeForDb(-45.0));
        std::vector<int16_t> r2 = Constant(Frames(100), -16000);
        for (size_t i = 0; i < Frames(300); ++i) r2.push_back(int16_t(-16000 + int(i) + tone[i]));
        Append(r2, Constant(Frames(500), -16000));
        AudioCapture c;
        CommitOnTimeline(c, 0, r2, 0);
        LevelDetector d;
        const auto ev = d.Feed(c, c.NewestCycle());
        INFO(Describe(ev));
        CHECK(ev.size() == 2);
    }
}
