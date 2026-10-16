#include "doctest/doctest.h"
#include "core/pacing.h"
#include <chrono>
#include <stdexcept>

using namespace harness;
using namespace std::chrono_literals;

TEST_CASE("burst: exact long-term total")
{
    BurstBudget b(1'020'484.32, 1000us);
    uint64_t sum = 0;
    for (int i = 0; i < 1000; ++i) {
        auto n = b.Next();
        CHECK((n == 1020 || n == 1021));
        sum += n;
    }
    CHECK(sum == 1'020'484);
}

TEST_CASE("burst: invalid")
{
    CHECK_THROWS_AS(BurstBudget(0, 1000us), std::invalid_argument);
    CHECK_THROWS_AS(BurstBudget(1e6, 0us), std::invalid_argument);
}

TEST_SUITE("realtime")
{
    TEST_CASE("pacer: 500 ticks ~= 500 ms")
    {
        RealtimePacer p;
        p.Start();
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 500; ++i) p.WaitForTick();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms >= 450);
        CHECK(ms <= 550);
    }
}
