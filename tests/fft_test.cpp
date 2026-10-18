// SPDX-License-Identifier: MIT
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

TEST_CASE("fft: off-bin tones need the parabolic refinement and the Hann window")
{
    // Bin width 44100 / 16384 = 2.6917 Hz. 1234.5 Hz is 0.64 bins past bin 458 and 1233.449 Hz is bin 458.25. The
    // unrefined peak bin is off by 0.078% and 0.055%. Refined on a rectangular window the error is still 0.035-0.051%;
    // Hann-windowed and refined (on linear or log magnitude) it is at most 0.011%.
    for (double hz : {1234.5, 458.25 * 44100.0 / 16384.0}) {
        CAPTURE(hz);
        CHECK(DominantHz(Sine(16384, 44100.0, hz), 44100.0) == doctest::Approx(hz).epsilon(0.0002));
    }
}

TEST_CASE("fft: exactly the first 16,384 samples count")
{
    // [0, 4096): 440 Hz; [4096, 16384): 2000 Hz; then a louder 3000 Hz. A 16,384-point FFT sees mostly 2000 Hz; a
    // 4,096-point one would see only 440 Hz, and one over more than 16,384 samples would pick up the 3000 Hz.
    std::vector<float> s = Sine(4096, 44100.0, 440.0);
    std::vector<float> mid = Sine(16384, 44100.0, 2000.0);
    s.insert(s.end(), mid.begin() + 4096, mid.end());
    const std::vector<float> later = Sine(20000, 44100.0, 3000.0, 30000.0);
    s.insert(s.end(), later.begin(), later.end());
    CHECK(DominantHz(s, 44100.0) == doctest::Approx(2000.0).epsilon(0.01));
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
