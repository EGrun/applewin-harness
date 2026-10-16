#include "core/pacing.h"
#include <cmath>
#include <stdexcept>
#include <windows.h>

namespace harness {

// carried_ holds the number of bursts handed out so far (exact in a double up to 2^53). Each burst is
// floor(n * perBurst) - floor((n-1) * perBurst), so N bursts sum to exactly floor(N * perBurst) with no
// accumulated rounding drift.
BurstBudget::BurstBudget(double clockHz, std::chrono::microseconds period)
{
    if (!(clockHz > 0.0)) throw std::invalid_argument("clockHz must be > 0");
    if (period.count() <= 0) throw std::invalid_argument("period must be > 0");
    cyclesPerBurst_ = clockHz * (static_cast<double>(period.count()) / 1e6);
}

uint32_t BurstBudget::Next()
{
    const double prev = std::floor(carried_ * cyclesPerBurst_);
    carried_ += 1.0;
    const double cur = std::floor(carried_ * cyclesPerBurst_);
    return static_cast<uint32_t>(cur - prev);
}

RealtimePacer::RealtimePacer()
{
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) throw std::runtime_error("CreateWaitableTimerExW failed");
}

RealtimePacer::~RealtimePacer()
{
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
}

namespace {
long long Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
long long QpcFreq() { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }
}

// A periodic waitable timer only fires at the system tick granularity (measured ~1.45 ms), so the 1 ms
// cadence is kept by re-arming the high-resolution timer one-shot to an absolute deadline each tick.
void RealtimePacer::Start()
{
    nextTick_ = Qpc() + QpcFreq() / 1000;
}

void RealtimePacer::WaitForTick()
{
    const long long freq = QpcFreq();
    const long long now = Qpc();
    long long remaining = nextTick_ - now;           // QPC counts
    if (remaining > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -(remaining * 10'000'000LL / freq);   // relative, 100 ns units
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (!SetWaitableTimer(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, FALSE))
            throw std::runtime_error("SetWaitableTimer failed");
        WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
    }
    nextTick_ += freq / 1000;
    // If we fell badly behind, resync rather than burst-firing to catch up.
    const long long t = Qpc();
    if (t - nextTick_ > freq / 20) nextTick_ = t + freq / 1000;
}

}  // namespace harness
