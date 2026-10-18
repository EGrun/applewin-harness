// SPDX-License-Identifier: MIT
#pragma once
// Synthetic signals and measurements for the agent-audio tests (M3 Part B).
#include "core/audio_capture.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace harness::testsupport {

inline constexpr double kPi = 3.14159265358979323846;

// Amplitude for a level in dBFS (full scale 32,768).
inline double AmplitudeForDb(double db) { return 32768.0 * std::pow(10.0, db / 20.0); }

// Mono mix (L + R) / 2 of interleaved stereo int16.
inline std::vector<float> MonoMix(std::span<const int16_t> stereo)
{
    std::vector<float> mono(stereo.size() / 2);
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = (float(stereo[2 * i]) + float(stereo[2 * i + 1])) / 2.0f;
    return mono;
}

// AC RMS (mean subtracted) in dBFS; -infinity for silence or an empty input.
inline double AcRmsDb(std::span<const float> mono)
{
    if (mono.empty()) return -std::numeric_limits<double>::infinity();
    double mean = 0;
    for (float v : mono) mean += v;
    mean /= double(mono.size());
    double sum = 0;
    for (float v : mono) sum += (v - mean) * (v - mean);
    const double rms = std::sqrt(sum / double(mono.size()));
    return rms > 0 ? 20.0 * std::log10(rms / 32768.0) : -std::numeric_limits<double>::infinity();
}

// Mono square wave: n frames at rate, frequency hz, +/-amplitude around dc, starting at phase 0 (high half first).
inline std::vector<int16_t> SquareWave(size_t n, double rate, double hz, double amplitude, double dc = 0.0)
{
    std::vector<int16_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double phase = std::fmod(double(i) * hz / rate, 1.0);
        out[i] = int16_t(std::lround(dc + (phase < 0.5 ? amplitude : -amplitude)));
    }
    return out;
}

inline std::vector<int16_t> Constant(size_t n, int16_t value) { return std::vector<int16_t>(n, value); }

// Commits a mono signal to the capture as one voice, in pieces of `chunk` frames placed exactly on the timeline:
// piece i ends at FrameToCycle(startFrame + frames so far). The first piece is the voice's first commit, so it covers
// the chunk x CLK/44100 cycles before its end, i.e. it starts at (about) startFrame.
inline uint64_t CommitOnTimeline(AudioCapture& capture, int voice, std::span<const int16_t> mono, uint64_t startFrame,
                                 size_t chunk = 441)
{
    size_t done = 0;
    while (done < mono.size()) {
        const size_t n = (std::min)(chunk, mono.size() - done);
        done += n;
        capture.Commit(voice, mono.subspan(done - n, n), 1, AudioCapture::FrameToCycle(startFrame + done));
    }
    return startFrame + done;
}

}  // namespace harness::testsupport
