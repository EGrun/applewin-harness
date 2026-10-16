// DominantHz: Hann-windowed radix-2 FFT with parabolic peak refinement (spec: docs/specs/m3.md, Part B).
#include "core/fft.h"
#include "core/level_detector.h"   // kDominantFftPoints

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace harness {

namespace {

// In-place iterative radix-2 FFT; a.size() is a power of two.
void Fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * std::numbers::pi / double(len);
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> w = std::polar(1.0, ang * double(k));
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

}  // namespace

double DominantHz(std::span<const float> mono, double rate)
{
    if (!(rate > 0.0)) throw std::invalid_argument("DominantHz: rate must be > 0");
    const size_t n = (std::min)(mono.size(), size_t(kDominantFftPoints));
    if (n == 0) return 0.0;
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += mono[i];
    mean /= double(n);

    std::vector<std::complex<double>> a(kDominantFftPoints);
    for (size_t i = 0; i < n; ++i) {
        const double w = n > 1 ? 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n - 1)) : 1.0;
        a[i] = (double(mono[i]) - mean) * w;
    }
    Fft(a);

    const size_t half = kDominantFftPoints / 2;
    std::vector<double> mag(half + 1);
    for (size_t k = 0; k <= half; ++k) mag[k] = std::abs(a[k]);
    size_t peak = 1;
    for (size_t k = 2; k < half; ++k)
        if (mag[k] > mag[peak]) peak = k;
    if (!(mag[peak] > 0.0)) return 0.0;

    double delta = 0.0;
    if (mag[peak - 1] > 0.0 && mag[peak + 1] > 0.0) {
        const double l = std::log(mag[peak - 1]), c = std::log(mag[peak]), r = std::log(mag[peak + 1]);
        const double denom = l - 2.0 * c + r;
        if (denom < 0.0) delta = std::clamp(0.5 * (l - r) / denom, -0.5, 0.5);
    }
    return (double(peak) + delta) * rate / double(kDominantFftPoints);
}

}  // namespace harness
