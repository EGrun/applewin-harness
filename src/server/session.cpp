#include "server/session.h"
#include "server/protocol.h"
#include "core/working_copy.h"
#include <stdexcept>
#include <utility>

// Task 2 stubs: implemented in Task 5.

namespace harness {

namespace {

[[noreturn]] void NotImplemented(const char* what)
{
    throw RpcError{kInternalError, std::string(what) + ": not implemented"};
}

}  // namespace

Session::Session(DisplayMode display, Emulator& emulator) : display_(display), emulator_(emulator)
{
}

Session::~Session() = default;

void Session::Post(ConnectionPtr, nlohmann::json)
{
    throw std::logic_error("Session::Post: not implemented");
}

int Session::RunMainLoop()
{
    throw std::logic_error("Session::RunMainLoop: not implemented");
}

bool Session::IsOperatorOnly(std::string_view)
{
    throw std::logic_error("Session::IsOperatorOnly: not implemented");
}

bool Session::DrainCommands()
{
    throw std::logic_error("Session::DrainCommands: not implemented");
}

void Session::Dispatch(const Command&)
{
    throw std::logic_error("Session::Dispatch: not implemented");
}

void Session::SendInputApplied()
{
    throw std::logic_error("Session::SendInputApplied: not implemented");
}

void Session::SendFrameReady()
{
    throw std::logic_error("Session::SendFrameReady: not implemented");
}

nlohmann::json Session::CreateSession(const nlohmann::json&) { NotImplemented("create_session"); }
nlohmann::json Session::CloseSession(const nlohmann::json&) { NotImplemented("close_session"); }
nlohmann::json Session::Pause(const nlohmann::json&) { NotImplemented("pause"); }
nlohmann::json Session::Resume(const nlohmann::json&) { NotImplemented("resume"); }
nlohmann::json Session::StepFrames(const nlohmann::json&) { NotImplemented("step_frames"); }
nlohmann::json Session::GetFrame(const nlohmann::json&) { NotImplemented("get_frame"); }
nlohmann::json Session::SubscribeFrames(const ConnectionPtr&, const nlohmann::json&) { NotImplemented("subscribe_frames"); }
nlohmann::json Session::TypeText(const ConnectionPtr&, const nlohmann::json&) { NotImplemented("type_text"); }
nlohmann::json Session::PressKey(const ConnectionPtr&, const nlohmann::json&) { NotImplemented("press_key"); }
nlohmann::json Session::SetJoystick(const ConnectionPtr&, const nlohmann::json&) { NotImplemented("set_joystick"); }

}  // namespace harness
