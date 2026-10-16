// Stub: references no AppleWin symbol (types in the header are fine).
#include "host/emulator.h"
#include <stdexcept>

namespace harness {

Emulator& Emulator::Instance() { throw std::runtime_error("not implemented"); }
void Emulator::Start(const EmulatorConfig&) { throw std::runtime_error("not implemented"); }
void Emulator::Shutdown() { throw std::runtime_error("not implemented"); }
void Emulator::PowerCycle() { throw std::runtime_error("not implemented"); }
void Emulator::RunFrames(uint32_t) { throw std::runtime_error("not implemented"); }
void Emulator::RunRealtime(std::chrono::milliseconds) { throw std::runtime_error("not implemented"); }
uint64_t Emulator::EmulatedCycles() const { throw std::runtime_error("not implemented"); }
uint64_t Emulator::PresentedFrames() const { throw std::runtime_error("not implemented"); }
FrameView Emulator::Frame(bool) const { throw std::runtime_error("not implemented"); }
std::string Emulator::ReadTextRow(int) const { throw std::runtime_error("not implemented"); }
const std::vector<std::string>& Emulator::Messages() const { throw std::runtime_error("not implemented"); }

}  // namespace harness
