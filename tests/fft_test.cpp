// DominantHz unit tests (spec: docs/specs/m3.md, Part B "dominant_hz"; plan: M3 Part B Task 1, "fft:").
#include "doctest/doctest.h"
#include "core/fft.h"
#include "support/audio_signals.h"

#include <cmath>
#include <stdexcept>
#include <vector>

using harness::DominantHz;
using harness::testsupport::kPi;

namespace {

std::vector<float> Sine(size_t n, double rate, double hz, double amplitude = 10000.0)
{
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i)
        s[i] = float(amplitude * std::sin(2.0 * kPi * hz * double(i) / rate));
    return s;
}

}  // namespace

TEST_CASE("fft: pure sines come back within 0.5%")
{
    for (double hz : {440.0, 2000.0}) {
        CAPTURE(hz);
        CHECK(DominantHz(Sine(16384, 44100.0, hz), 44100.0) == doctest::Approx(hz).epsilon(0.005));
    }
}

TEST_CASE("fft: only the first 16,384 samples count")
{
    std::vector<float> s = Sine(16384, 44100.0, 440.0);
    const std::vector<float> later = Sine(20000, 44100.0, 3000.0, 30000.0);
    s.insert(s.end(), later.begin(), later.end());
    CHECK(DominantHz(s, 44100.0) == doctest::Approx(440.0).epsilon(0.005));
}

TEST_CASE("fft: a short input is zero-padded")
{
    CHECK(DominantHz(Sine(4410, 44100.0, 1000.0), 44100.0) == doctest::Approx(1000.0).epsilon(0.01));
}

TEST_CASE("fft: a DC offset does not win over the tone")
{
    std::vector<float> s = Sine(16384, 44100.0, 2000.0, 3000.0);
    for (float& v : s) v += 8000.0f;
    CHECK(DominantHz(s, 44100.0) == doctest::Approx(2000.0).epsilon(0.005));
}

TEST_CASE("fft: silence and empty input give 0; a bad rate throws")
{
    CHECK(DominantHz(std::vector<float>(16384, 0.0f), 44100.0) == 0.0);
    CHECK(DominantHz({}, 44100.0) == 0.0);
    CHECK_THROWS_AS(DominantHz(Sine(100, 44100.0, 440.0), 0.0), std::invalid_argument);
}
