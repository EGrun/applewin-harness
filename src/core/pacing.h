#pragma once
#include <chrono>
#include <cstdint>

namespace harness {

// Hands out a cycle count per burst; carries the fractional remainder forward so N bursts add up to
// exactly floor(N * clockHz * period). Throws std::invalid_argument if clockHz <= 0 or period <= 0.
class BurstBudget {
public:
    BurstBudget(double clockHz, std::chrono::microseconds period);
    uint32_t Next();
private:
    double cyclesPerBurst_ = 0.0;
    double carried_ = 0.0;
};

// Periodic 1 ms high-resolution waitable timer (CreateWaitableTimerExW with
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS). Throws std::runtime_error if the timer
// cannot be created.
class RealtimePacer {
public:
    RealtimePacer();
    ~RealtimePacer();
    RealtimePacer(const RealtimePacer&) = delete;
    RealtimePacer& operator=(const RealtimePacer&) = delete;
    void Start();
    void WaitForTick();
private:
    void* timer_ = nullptr;   // HANDLE
    long long nextTick_ = 0;  // QPC count of the next 1 ms deadline
};

}  // namespace harness
