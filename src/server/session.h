#pragma once
// The session loop and the agent and operator methods (spec: docs/specs/m3.md, Part A "Threading and the session
// loop", "Agent methods", "Operator methods"). One session per process (P6).
#include "core/level_detector.h"
#include "host/emulator.h"
#include "server/pipe_server.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace harness {

class WorkingCopy;

class Session {
public:
    // The emulator is started by create_session, in the given display mode (fixed for the process).
    Session(DisplayMode display, Emulator& emulator);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Thread-safe; called on connection threads (PipeServer's onRequest). Queues Command { connection, id, method,
    // params } for the main thread and wakes it. The request has already passed the transport's checks.
    void Post(ConnectionPtr connection, nlohmann::json request);

    // Runs on the main thread until close_session has been answered or the window is closed (returns 0), or a fatal
    // error (prints "error: ..." to stderr, returns 1). The emulator is shut down and the working copies and the INI
    // are removed before it returns, on every path.
    int RunMainLoop();

    // Polled by RunMainLoop (PipeServer::FatalError): a non-empty result is a fatal error (exit 1). Set before
    // RunMainLoop.
    void SetFatalErrorSource(std::function<std::string()> fatalError);

    // A handle (SYNCHRONIZE) to the process that launched the server, owned by the caller and valid until RunMainLoop
    // returns. When it becomes signalled the loop ends as for close_session (exit 0, normal cleanup). Set before
    // RunMainLoop; null means no watch.
    void SetParentProcess(void* processHandle);

    // The operator-only methods (create_session, close_session, pause, resume, step_frames, set_speed, get_speed, and
    // M4's checkpoint, restore_checkpoint, list_checkpoints, delete_checkpoint, get_state_checksum, step_cycles,
    // list_disks, export_disk); PipeServer's isOperatorOnly.
    static bool IsOperatorOnly(std::string_view method);

private:
    enum class State { NoSession, Paused, Running };   // Running exists only in realtime sessions

    struct Command {
        ConnectionPtr connection;
        nlohmann::json id;
        std::string method;
        nlohmann::json params;
    };

    struct FrameSubscription {
        ConnectionPtr connection;
        uint32_t everyNFrames = 0;
        uint64_t presentsSeen = 0;   // presents since the subscription was (re)set
        std::chrono::steady_clock::time_point lastFullSpeedSend{};   // frame_ready cap at full speed (C4)
    };

    struct AudioSubscription {
        ConnectionPtr connection;
        LevelDetector detector;
    };

    // Main thread.
    bool DrainCommands();                    // false once the process must exit
    void Dispatch(const Command& command);   // replies with the result, an RpcError, or -32603 with what()
    void SendInputApplied();                 // input_applied for every entry of Emulator::TakeAppliedInputs()
    void SendSoundEvents();                  // feed each audio subscriber's detector; sound_started / sound_stopped
    void SendFrameReady();                   // frame_ready to the subscribers that are due (the present observer)
    bool PauseRunning();                     // Running: run until the next present, then Paused; false if the window closed
    void Cleanup() noexcept;                 // emulator shut down, working copies, work dir and INI removed

    // Operator methods. Each returns the result object or throws RpcError.
    nlohmann::json CreateSession(const nlohmann::json& params);
    nlohmann::json CloseSession(const nlohmann::json& params);
    nlohmann::json Pause(const nlohmann::json& params);
    nlohmann::json Resume(const nlohmann::json& params);
    nlohmann::json StepFrames(const nlohmann::json& params);

    // Agent methods (-32002 before a session exists).
    nlohmann::json GetFrame(const nlohmann::json& params);
    nlohmann::json SubscribeFrames(const ConnectionPtr& connection, const nlohmann::json& params);
    nlohmann::json TypeText(const ConnectionPtr& connection, const nlohmann::json& params);
    nlohmann::json PressKey(const ConnectionPtr& connection, const nlohmann::json& params);
    nlohmann::json SetJoystick(const ConnectionPtr& connection, const nlohmann::json& params);
    nlohmann::json SetSpeed(const nlohmann::json& params);
    nlohmann::json GetSpeed(const nlohmann::json& params);
    // Applies the set_speed fields (shared with create_session.speed); throws RpcError.
    static void ValidateSpeed(const nlohmann::json& p, bool create);
    void ApplySpeed(const nlohmann::json& params, bool create);
    nlohmann::json GetAudio(const nlohmann::json& params);
    nlohmann::json SubscribeAudio(const ConnectionPtr& connection, const nlohmann::json& params);

    // M4 operator methods (docs/specs/m4.md, "API").
    nlohmann::json Checkpoint(const nlohmann::json& params);
    nlohmann::json RestoreCheckpoint(const nlohmann::json& params);
    nlohmann::json ListCheckpoints(const nlohmann::json& params);
    nlohmann::json DeleteCheckpoint(const nlohmann::json& params);
    nlohmann::json GetStateChecksum(const nlohmann::json& params);
    nlohmann::json StepCycles(const nlohmann::json& params);
    nlohmann::json ListDisks(const nlohmann::json& params);
    nlohmann::json ExportDisk(const nlohmann::json& params);

    DisplayMode display_;
    Emulator& emulator_;

    std::mutex queueMutex_;
    std::deque<Command> queue_;
    void* parent_ = nullptr;       // watched parent process handle, or null
    void* queueEvent_ = nullptr;   // auto-reset event set by Post (a Win32 HANDLE)

    State state_ = State::NoSession;
    bool realtime_ = false;
    bool exitRequested_ = false;
    std::function<std::string()> fatalError_;
    std::filesystem::path workDir_;   // %TEMP%\applewin-harness\<pid>
    std::filesystem::path ini_;       // %TEMP%\applewin-harness\<pid>.ini
    std::vector<std::unique_ptr<WorkingCopy>> copies_;
    std::map<uint64_t, ConnectionPtr> inputOwners_;   // input_id -> the connection that receives input_applied
    std::map<uint64_t, FrameSubscription> frameSubscriptions_;   // by connection id
    std::map<uint64_t, AudioSubscription> audioSubscriptions_;   // by connection id
};

}  // namespace harness
