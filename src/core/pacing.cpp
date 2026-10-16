#include "core/pacing.h"
#include <stdexcept>

namespace harness {

BurstBudget::BurstBudget(double, std::chrono::microseconds)
{
    throw std::runtime_error("not implemented");
}

uint32_t BurstBudget::Next()
{
    throw std::runtime_error("not implemented");
}

RealtimePacer::RealtimePacer()
{
    throw std::runtime_error("not implemented");
}

RealtimePacer::~RealtimePacer() = default;

void RealtimePacer::Start()
{
    throw std::runtime_error("not implemented");
}

void RealtimePacer::WaitForTick()
{
    throw std::runtime_error("not implemented");
}

}  // namespace harness
