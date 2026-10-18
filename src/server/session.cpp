#include "server/session.h"
#include "host/log.h"
#include "server/protocol.h"
#include "core/audio_capture.h"
#include "core/base64.h"
#include "core/level_detector.h"
#include "core/wav.h"
#include "core/png_writer.h"
#include "core/working_copy.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace harness {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr uint32_t kMaxTextChars = 4096;
constexpr uint32_t kDefaultInterKeyDelayMs = 30;
constexpr uint32_t kDefaultHoldMs = 50;
constexpr uint32_t kMaxStepFrames = 100000;
constexpr uint32_t kMaxEveryNFrames = 600;
constexpr DWORD kIdleWaitMs = 50;
constexpr uint64_t kMaxHoldMs = 10000;            // press_key.hold_ms
constexpr uint64_t kMaxDurationMs = 60000;        // set_joystick.duration_ms
constexpr uint64_t kMaxInterKeyDelayMs = 1000;    // type_text.inter_key_delay_ms
constexpr uint64_t kMaxPendingInputEvents = 4096;
constexpr size_t kMaxStringUnits = 32767;         // any string parameter, in UTF-16 code units

[[noreturn]] void InvalidParams(const std::string& message)
{
    throw RpcError{kInvalidParams, message};
}

// The field, or nullptr when it is absent or null.
const json* Field(const json& params, const char* name)
{
    const auto it = params.find(name);
    if (it == params.end() || it->is_null()) return nullptr;
    return &*it;
}

bool GetBool(const json& params, const char* name, bool fallback)
{
    const json* f = Field(params, name);
    if (!f) return fallback;
    if (!f->is_boolean()) InvalidParams(std::string(name) + " must be a boolean");
    return f->get<bool>();
}

// An integer in [lo, hi]; fallback when absent (nullopt fallback: required).
uint64_t GetUint(const json& params, const char* name, uint64_t lo, uint64_t hi, std::optional<uint64_t> fallback)
{
    const json* f = Field(params, name);
    if (!f) {
        if (!fallback) InvalidParams(std::string(name) + " is required");
        return *fallback;
    }
    if (f->is_number_unsigned()) {
        const uint64_t v = f->get<uint64_t>();
        if (v >= lo && v <= hi) return v;
    } else if (f->is_number_integer()) {
        const int64_t v = f->get<int64_t>();
        if (v >= 0 && uint64_t(v) >= lo && uint64_t(v) <= hi) return uint64_t(v);
    } else {
        InvalidParams(std::string(name) + " must be an integer");
    }
    InvalidParams(std::string(name) + " must be in " + std::to_string(lo) + ".." + std::to_string(hi));
}

// The length of a UTF-8 string in UTF-16 code units: a 4-byte sequence is a surrogate pair, any other lead byte is one.
size_t Utf16Units(const std::string& s)
{
    size_t n = 0;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((u & 0xC0) == 0x80) continue;   // continuation byte
        n += u >= 0xF0 ? 2 : 1;
    }
    return n;
}

// Every string in the params, at any depth, is at most kMaxStringUnits UTF-16 code units (-32602).
void CheckStringLengths(const json& v)
{
    if (v.is_string()) {
        if (Utf16Units(v.get_ref<const std::string&>()) > kMaxStringUnits)
            InvalidParams("a string parameter is limited to 32767 UTF-16 code units");
    } else if (v.is_structured()) {
        for (const json& child : v) CheckStringLengths(child);
    }
}

std::string GetString(const json& params, const char* name, std::optional<std::string> fallback)
{
    const json* f = Field(params, name);
    if (!f) {
        if (!fallback) InvalidParams(std::string(name) + " is required");
        return *fallback;
    }
    if (!f->is_string()) InvalidParams(std::string(name) + " must be a string");
    return f->get<std::string>();
}

fs::path Utf8Path(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string PathUtf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

// press_key's key: one character 0x20-0x7E, an int 0..127, or a name (spec: Part A "Input queue").
KeySpec ParseKey(const json& params)
{
    const json* f = Field(params, "key");
    if (!f) InvalidParams("key is required");
    if (f->is_number_integer()) {
        const int64_t v = f->is_number_unsigned() ? int64_t(std::min<uint64_t>(f->get<uint64_t>(), 1000))
                                                  : f->get<int64_t>();
        if (v < 0 || v > 127) InvalidParams("key code must be in 0..127");
        return KeySpec{uint8_t(v)};
    }
    if (!f->is_string()) InvalidParams("key must be a string or an integer");
    const std::string k = f->get<std::string>();
    if (k.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(k[0]);
        if (c >= 0x20 && c <= 0x7E) return KeySpec{uint8_t(c)};
        InvalidParams("key character must be 0x20-0x7E");
    }
    if (k == "RETURN") return KeySpec{uint8_t(0x0D)};
    if (k == "ESC") return KeySpec{uint8_t(0x1B)};
    if (k == "TAB") return KeySpec{uint8_t(0x09)};
    if (k == "DELETE") return KeySpec{uint8_t(0x7F)};
    if (k == "LEFT") return KeySpec{ArrowKey::Left};
    if (k == "RIGHT") return KeySpec{ArrowKey::Right};
    if (k == "UP") return KeySpec{ArrowKey::Up};
    if (k == "DOWN") return KeySpec{ArrowKey::Down};
    if (k == "RESET") return KeySpec{ResetKey{}};
    if (k.size() == 6 && k.compare(0, 5, "CTRL-") == 0 && k[5] >= 'A' && k[5] <= 'Z')
        return KeySpec{uint8_t(k[5] - 'A' + 1)};
    InvalidParams("unknown key: " + k);
}

json InputReply(uint64_t id, std::optional<uint64_t> at)
{
    return json{{"input_id", id}, {"applied_at_cycle", at ? json(*at) : json(nullptr)}};
}

}  // namespace

Session::Session(DisplayMode display, Emulator& emulator) : display_(display), emulator_(emulator)
{
    queueEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!queueEvent_) throw std::system_error(int(GetLastError()), std::system_category(), "CreateEventW");
}

Session::~Session()
{
    Cleanup();
    if (queueEvent_) CloseHandle(static_cast<HANDLE>(queueEvent_));
}

void Session::SetFatalErrorSource(std::function<std::string()> fatalError)
{
    fatalError_ = std::move(fatalError);
}

void Session::SetParentProcess(void* processHandle)
{
    parent_ = processHandle;
}

void Session::Post(ConnectionPtr connection, json request)
{
    // Connection thread. Never throws into the transport: a request that cannot be queued is answered here.
    try {
        Command c;
        c.connection = connection;
        c.id = request.contains("id") ? request["id"] : json(nullptr);
        c.method = request.value("method", std::string());
        if (const auto it = request.find("params"); it != request.end()) c.params = std::move(*it);
        {
            std::lock_guard lock(queueMutex_);
            queue_.push_back(std::move(c));
        }
        SetEvent(static_cast<HANDLE>(queueEvent_));
    } catch (const std::exception& e) {
        try {
            if (connection) connection->Send(MakeError(request.value("id", json()), kInternalError, e.what()));
        } catch (...) {
        }
    }
}

bool Session::IsOperatorOnly(std::string_view method)
{
    return method == "create_session" || method == "close_session" || method == "pause" || method == "resume" ||
           method == "step_frames" ||
           method == "set_speed" || method == "get_speed" ||
           method == "checkpoint" || method == "restore_checkpoint" || method == "list_checkpoints" ||
           method == "delete_checkpoint" || method == "get_state_checksum" || method == "step_cycles" ||
           method == "list_disks" || method == "export_disk" ||
           method == "insert_disk" || method == "eject_disk" ||
           method == "subscribe_disk_writes" || method == "unsubscribe_disk_writes";
}

int Session::RunMainLoop()
{
    int rc = 0;
    emulator_.SetPresentObserver([this] { SendFrameReady(); SendSoundEvents(); });
    try {
        for (;;) {
            if (fatalError_) {
                const std::string fatal = fatalError_();
                if (!fatal.empty()) {
                    log::Error(log::Component::Session, fatal);
                    rc = 1;
                    break;
                }
            }
            const HANDLE parent = static_cast<HANDLE>(parent_);
            if (state_ == State::Running) {
                // One realtime step, the window pumped after it, then the commands that arrived meanwhile.
                const bool open = emulator_.RunRealtimeStep();
                SendInputApplied();
                SendSoundEvents();
                if (!open) break;   // window closed: exit 0
                if (!emulator_.PumpWindow()) break;
                if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) break;   // launcher exited: exit 0
            } else {
                HANDLE waits[2] = {static_cast<HANDLE>(queueEvent_), parent};
                const DWORD count = parent ? 2 : 1;
                const DWORD w = MsgWaitForMultipleObjects(count, waits, FALSE, kIdleWaitMs, QS_ALLINPUT);
                if (parent && w == WAIT_OBJECT_0 + 1) break;   // launcher exited: exit 0
                if (state_ != State::NoSession && !emulator_.PumpWindow()) break;   // window closed: exit 0
            }
            if (!DrainCommands()) break;
        }
    } catch (const std::exception& e) {
        log::Error(log::Component::Session, e.what());
        rc = 1;
    } catch (...) {
        log::Error(log::Component::Session, "unknown exception");
        rc = 1;
    }
    Cleanup();
    return rc;
}

bool Session::DrainCommands()
{
    std::deque<Command> batch;
    {
        std::lock_guard lock(queueMutex_);
        batch.swap(queue_);
    }
    for (const Command& c : batch) {
        Dispatch(c);
        if (exitRequested_) return false;
    }
    return true;
}

namespace {

// Method names are logged only when this dispatcher knows them: a client-chosen name is client text, and is never logged.
bool IsKnownMethod(const std::string& m)
{
    static const char* const kNames[] = {
        "create_session", "close_session", "pause", "resume", "step_frames", "get_frame", "get_text_screen",
        "subscribe_frames", "type_text", "press_key", "set_joystick", "set_speed", "get_speed", "get_audio",
        "subscribe_audio", "checkpoint", "restore_checkpoint", "list_checkpoints", "delete_checkpoint",
        "get_state_checksum", "step_cycles", "list_disks", "export_disk", "insert_disk", "eject_disk",
        "subscribe_disk_writes", "unsubscribe_disk_writes"};
    for (const char* n : kNames)
        if (m == n) return true;
    return false;
}

// The info lines the spec lists: session create/close, checkpoint/restore, disk insert/eject/export. Only the method
// is named; no parameter (a path, a name, typed text) is ever part of a log line.
void LogSuccess(const std::string& method)
{
    if (method == "create_session") log::Info(log::Component::Session, "session created");
    else if (method == "close_session") log::Info(log::Component::Session, "session closed");
    else if (method == "checkpoint") log::Info(log::Component::Session, "checkpoint saved");
    else if (method == "restore_checkpoint") log::Info(log::Component::Session, "checkpoint restored");
    else if (method == "insert_disk") log::Info(log::Component::Session, "disk inserted");
    else if (method == "eject_disk") log::Info(log::Component::Session, "disk ejected");
    else if (method == "export_disk") log::Info(log::Component::Session, "disk exported");
}

// A rejected request (-32602, -32010: warning) or a failed one (-32603: error). Other errors (no session yet, wrong
// state, unknown method) are the client's to handle, not logged. Rate limited per connection by the logger.
void LogFailure(const std::string& method, int code, uint64_t connection)
{
    const bool invalid = code == kInvalidParams, full = code == kInputQueueFull, internal = code == kInternalError;
    if (!invalid && !full && !internal) return;
    const std::string what = IsKnownMethod(method) ? method : "request";
    const std::string text = what + (invalid ? " rejected: invalid params (" : full ? " rejected: input queue full (" : " failed: internal error (") +
                             std::to_string(code) + ")";
    if (invalid || full) log::Warning(log::Component::Session, text, connection);
    else log::Error(log::Component::Session, text, connection);
}

}  // namespace

void Session::Dispatch(const Command& command)
{
    const ConnectionPtr& conn = command.connection;
    const uint64_t connId = conn ? conn->id() : 0;
    json reply;
    if (state_ != State::NoSession) log::SetEmulatedMs(double(emulator_.EmulatedCycles()) * 1000.0 / kAudioBaseClockHz);
    try {
        if (!command.params.is_null() && !command.params.is_object())
            throw RpcError{kInvalidParams, "params must be an object"};
        const json params = command.params.is_object() ? command.params : json::object();
        CheckStringLengths(params);
        const std::string& m = command.method;
        json result;
        if (m == "create_session") result = CreateSession(params);
        else if (m == "close_session") result = CloseSession(params);
        else if (m == "pause") result = Pause(params);
        else if (m == "resume") result = Resume(params);
        else if (m == "step_frames") result = StepFrames(params);
        else if (m == "get_frame") result = GetFrame(params);
        else if (m == "get_text_screen") result = GetTextScreen(params);
        else if (m == "subscribe_frames") result = SubscribeFrames(conn, params);
        else if (m == "type_text") result = TypeText(conn, params);
        else if (m == "press_key") result = PressKey(conn, params);
        else if (m == "set_joystick") result = SetJoystick(conn, params);
        else if (m == "set_speed") result = SetSpeed(params);
        else if (m == "get_speed") result = GetSpeed(params);
        else if (m == "get_audio") result = GetAudio(params);
        else if (m == "subscribe_audio") result = SubscribeAudio(conn, params);
        else if (m == "checkpoint") result = Checkpoint(params);
        else if (m == "restore_checkpoint") result = RestoreCheckpoint(params);
        else if (m == "list_checkpoints") result = ListCheckpoints(params);
        else if (m == "delete_checkpoint") result = DeleteCheckpoint(params);
        else if (m == "get_state_checksum") result = GetStateChecksum(params);
        else if (m == "step_cycles") result = StepCycles(params);
        else if (m == "list_disks") result = ListDisks(params);
        else if (m == "export_disk") result = ExportDisk(params);
        else if (m == "insert_disk") result = InsertDisk(params);
        else if (m == "eject_disk") result = EjectDisk(params);
        else if (m == "subscribe_disk_writes" || m == "unsubscribe_disk_writes")
            throw RpcError{kInternalError, "not implemented"};   // stub until M6 Task 4
        else throw RpcError{kMethodNotFound, "method not found: " + m};
        reply = MakeResult(command.id, std::move(result));
        LogSuccess(command.method);
    } catch (const RpcError& e) {
        reply = MakeError(command.id, e.code, e.message, e.data);
        LogFailure(command.method, e.code, connId);
    } catch (const std::exception& e) {
        reply = MakeError(command.id, kInternalError, e.what());
        LogFailure(command.method, kInternalError, connId);
    }
    try {
        conn->Send(reply);
    } catch (const std::exception& e) {   // a result over 16 MiB
        try { conn->Send(MakeError(command.id, kInternalError, e.what())); } catch (...) {}
    }
}

void Session::SendInputApplied()
{
    for (const InputApplied& a : emulator_.TakeAppliedInputs()) {
        const auto it = inputOwners_.find(a.inputId);
        if (it == inputOwners_.end()) continue;
        const ConnectionPtr conn = it->second;
        inputOwners_.erase(it);
        try {
            json params{{"input_id", a.inputId}};
            if (a.discarded) {   // ruling D1
                params["first_cycle"] = nullptr;
                params["last_cycle"] = nullptr;
                params["discarded"] = true;
            } else {
                params["first_cycle"] = a.firstCycle;
                params["last_cycle"] = a.lastCycle;
            }
            conn->Send(MakeNotification("input_applied", params));
        } catch (...) {
        }
    }
}

void Session::SendFrameReady()
{
    // Present observer: runs inside a run on the main thread and must not throw.
    try {
        if (frameSubscriptions_.empty()) return;
        bool haveFrame = false;
        json params;
        const bool fullSpeed = emulator_.FullSpeedStepping();
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, sub] : frameSubscriptions_) {
            if (++sub.presentsSeen % sub.everyNFrames != 0) continue;
            if (fullSpeed) {   // C4: at most one frame_ready per 16 ms of wall time while stepping at full speed
                if (now - sub.lastFullSpeedSend < std::chrono::milliseconds(16)) continue;
                sub.lastFullSpeedSend = now;
            }
            if (!haveFrame) {
                const FrameView f = emulator_.Frame(false);
                params = json{{"frame_id", emulator_.PresentedFrames()},
                              {"emulated_cycles", emulator_.EmulatedCycles()},
                              {"width", f.width},
                              {"height", f.height}};
                haveFrame = true;
            }
            sub.connection->Send(MakeNotification("frame_ready", params));
        }
    } catch (...) {
    }
}

bool Session::PauseRunning()
{
    if (state_ != State::Running) return true;
    const uint64_t before = emulator_.PresentedFrames();
    bool open = true;
    while (open && emulator_.PresentedFrames() == before)
        open = emulator_.RunRealtimeStep();
    emulator_.StopRealtime();
    state_ = State::Paused;
    SendInputApplied();
    SendSoundEvents();
    return open;
}

void Session::Cleanup() noexcept
{
    try { emulator_.SetPresentObserver({}); } catch (...) {}
    if (state_ != State::NoSession) {
        try { emulator_.Shutdown(); } catch (...) {}   // ejects the disks, so the working copies can be deleted
        state_ = State::NoSession;
    }
    copies_.clear();   // each WorkingCopy deletes its file
    checkpoints_.reset();
    std::error_code ec;
    if (!workDir_.empty()) fs::remove_all(workDir_, ec);
    if (!ini_.empty()) fs::remove(ini_, ec);
    frameSubscriptions_.clear();
    audioSubscriptions_.clear();
    inputOwners_.clear();
}

// ---- Operator methods ----

json Session::CreateSession(const json& params)
{
    if (state_ != State::NoSession || exitRequested_) throw RpcError{kInvalidState, "a session already exists"};

    // Options deferred to later milestones (R4) and Part C.
    const json* speed = Field(params, "speed");
    if (speed) ValidateSpeed(*speed, true);
    const std::string model = GetString(params, "model", std::string("apple2ee"));
    if (model != "apple2ee") throw RpcError{kNotSupported, "model " + model + " is not supported"};
    const bool allowTextScreen = GetBool(params, "allow_text_screen", false);
    if (GetBool(params, "shared_memory_frames", false))
        throw RpcError{kNotSupported, "shared_memory_frames is not supported"};
    if (Field(params, "hard_disks")) throw RpcError{kNotSupported, "hard disks are not supported"};
    const std::string runMode = GetString(params, "run_mode", std::string("realtime"));
    if (runMode != "realtime" && runMode != "stepped") InvalidParams("run_mode must be \"realtime\" or \"stepped\"");

    fs::path images[2];
    if (const json* disks = Field(params, "disks")) {
        if (!disks->is_array()) InvalidParams("disks must be an array");
        for (const json& d : *disks) {
            if (!d.is_object()) InvalidParams("each disk must be an object");
            const uint64_t slot = GetUint(d, "slot", 0, 7, std::nullopt);
            if (slot != 6) InvalidParams("disk slot must be 6");
            const uint64_t drive = GetUint(d, "drive", 1, 2, std::nullopt);
            if (!images[drive - 1].empty()) InvalidParams("drive " + std::to_string(drive) + " is given twice");
            const fs::path p = Utf8Path(GetString(d, "path", std::nullopt));
            if (!p.is_absolute()) InvalidParams("disk path must be absolute: " + PathUtf8(p));
            images[drive - 1] = p;
        }
        for (const fs::path& p : images) {
            std::error_code ec;
            if (!p.empty() && !fs::is_regular_file(p, ec)) throw RpcError{kDiskNotFound, "disk not found: " + PathUtf8(p)};
        }
    }

    const std::string pid = std::to_string(GetCurrentProcessId());
    const fs::path dir = fs::temp_directory_path() / "applewin-harness";
    fs::create_directories(dir);
    workDir_ = fs::absolute(dir / pid);
    ini_ = fs::absolute(dir / (pid + ".ini"));

    // Working copies (D5). Two images with the same file name keep apart: drive 2's then goes in a subdirectory.
    fs::path copyPaths[2];
    try {
        for (int i = 0; i < 2; ++i) {
            if (images[i].empty()) continue;
            fs::path where = workDir_;
            if (i == 1 && !images[0].empty() && images[0].filename() == images[1].filename()) where /= "drive2";
            copies_.push_back(std::make_unique<WorkingCopy>(images[i], where));
            copyPaths[i] = copies_.back()->Path();
        }
        EmulatorConfig cfg;
        cfg.configIni = ini_;
        cfg.display = display_;
        cfg.disk1 = copyPaths[0];
        emulator_.Start(cfg);
        try {
            if (!copyPaths[1].empty()) emulator_.InsertDisk(1, copyPaths[1]);
            if (speed) ApplySpeed(*speed, true);
        } catch (...) {
            emulator_.Shutdown();
            throw;
        }
    } catch (const std::exception& e) {
        // Name the user's images, not the working copies, and leave nothing behind.
        std::string what = e.what();
        for (int i = 0; i < 2; ++i)
            if (!copyPaths[i].empty()) ReplaceAll(what, copyPaths[i].string(), images[i].string());
        copies_.clear();   // Start and the drive-2 insert shut the emulator down on failure
        std::error_code ec;
        fs::remove_all(workDir_, ec);
        fs::remove(ini_, ec);
        throw RpcError{kInternalError, what};
    }

    originals_.clear();
    swapCopies_ = 0;
    for (int i = 0; i < 2; ++i)
        if (!copyPaths[i].empty()) originals_.emplace_back(copyPaths[i], images[i]);
    checkpoints_ = std::make_unique<CheckpointStore>(workDir_);
    unusable_ = false;
    allowTextScreen_ = allowTextScreen;
    audioFloor_ = 0;
    realtime_ = runMode == "realtime";
    state_ = realtime_ ? State::Running : State::Paused;
    return json{{"session_id", "1"}};   // one session per process (P6)
}

json Session::CloseSession(const json&)
{
    exitRequested_ = true;   // R6: also valid with no session. The reply is sent, then the loop exits (code 0).
    return json::object();
}

json Session::Pause(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    if (!PauseRunning()) {
        exitRequested_ = true;
        throw RpcError{kInvalidState, "the window was closed"};
    }
    return json{{"emulated_cycles", emulator_.EmulatedCycles()}};
}

json Session::Resume(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    if (!realtime_) throw RpcError{kInvalidState, "resume is only valid in a realtime session"};
    if (state_ == State::Paused) {
        emulator_.StopRealtime();   // the next step starts a fresh paced run
        state_ = State::Running;
    }
    return json::object();
}

json Session::StepFrames(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t frames = GetUint(params, "frames", 1, kMaxStepFrames, std::nullopt);
    bool open = PauseRunning();
    if (open) open = emulator_.RunFrames(uint32_t(frames), Pacing::Stepped);
    SendInputApplied();
    SendSoundEvents();
    if (!open) {
        exitRequested_ = true;
        throw RpcError{kInvalidState, "the window was closed"};
    }
    return json{{"emulated_cycles", emulator_.EmulatedCycles()}};
}

// ---- Agent methods ----

// Validates the set_speed fields without touching the emulator. create_session also accepts video_refresh: 60.
void Session::ValidateSpeed(const json& p, bool create)
{
    if (!p.is_object()) InvalidParams("speed must be an object");
    if (create) {
        if (const json* v = Field(p, "video_refresh")) {
            if (!v->is_number_integer()) InvalidParams("video_refresh must be 50 or 60");
            const int64_t r = v->get<int64_t>();
            if (r == 50) throw RpcError{kNotSupported, "video_refresh 50 (PAL) is not supported"};
            if (r != 60) InvalidParams("video_refresh must be 50 or 60");
        }
    }
    else if (Field(p, "video_refresh")) InvalidParams("video_refresh can only be set in create_session");
    const json* ms = Field(p, "machine_speed");
    const json* cm = Field(p, "clock_multiplier");
    if (ms && cm) InvalidParams("machine_speed and clock_multiplier cannot be sent together");
    if (ms) {
        if (ms->is_string()) {
            const std::string v = ms->get<std::string>();
            if (v != "authentic" && v != "max") InvalidParams("machine_speed must be 0..40, \"authentic\" or \"max\"");
        } else {
            GetUint(p, "machine_speed", 0, 40, std::nullopt);
        }
    }
    if (cm) {
        if (!cm->is_number()) InvalidParams("clock_multiplier must be a number");
        const double d = cm->get<double>();
        if (!std::isfinite(d) || d <= 0) InvalidParams("clock_multiplier must be a finite number above 0");
    }
    GetBool(p, "enhance_disk_speed", false);
    GetBool(p, "full_speed_override", false);
}

void Session::ApplySpeed(const json& p, bool create)
{
    ValidateSpeed(p, create);
    if (const json* ms = Field(p, "machine_speed")) {
        uint32_t v;
        if (ms->is_string()) v = ms->get<std::string>() == "max" ? 40 : 10;
        else v = uint32_t(GetUint(p, "machine_speed", 0, 40, std::nullopt));
        emulator_.SetMachineSpeed(v);
    }
    if (const json* cm = Field(p, "clock_multiplier")) emulator_.UseClockMultiplier(cm->get<double>());
    if (Field(p, "enhance_disk_speed")) emulator_.SetEnhanceDiskSpeed(GetBool(p, "enhance_disk_speed", true));
    if (Field(p, "full_speed_override")) emulator_.SetFullSpeedOverride(GetBool(p, "full_speed_override", false));
}

json Session::SetSpeed(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    ApplySpeed(params, false);
    return GetSpeed(params);
}

json Session::GetSpeed(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const SpeedSettings s = emulator_.Speed();
    const bool max = s.machineSpeed == 40;
    const double eff = emulator_.EffectiveClockHz();
    json reasons = json::array();
    for (const std::string& r : emulator_.FullSpeedReasons()) reasons.push_back(r);
    return json{{"machine_speed", s.machineSpeed},
                {"clock_multiplier", max ? json(nullptr) : json(eff / kAudioBaseClockHz)},
                {"video_refresh", 60},
                {"base_clock_hz", kAudioBaseClockHz},
                {"effective_clock_hz", max ? json(nullptr) : json(eff)},
                {"enhance_disk_speed", s.enhanceDisk},
                {"full_speed_override", s.fullSpeedOverride},
                {"full_speed_active", state_ == State::Running && emulator_.FullSpeedStepping()},
                {"full_speed_reasons", reasons}};
}

json Session::GetTextScreen(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    if (!allowTextScreen_) throw RpcError{kNotSupported, "get_text_screen needs a session created with allow_text_screen"};
    const TextScreen t = emulator_.ReadTextScreen();
    json lines = json::array();
    for (const std::string& l : t.lines) lines.push_back(l);
    return json{{"mode", t.mode}, {"lines", lines}, {"emulated_cycles", t.emulatedCycles}};
}

json Session::GetFrame(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const std::string format = GetString(params, "format", std::string("PNG"));
    if (format != "PNG" && format != "BGRA32") InvalidParams("format must be \"PNG\" or \"BGRA32\"");
    const bool borders = GetBool(params, "borders", false);
    const FrameView f = emulator_.Frame(borders);
    json result{{"frame_id", emulator_.PresentedFrames()},
                {"emulated_cycles", emulator_.EmulatedCycles()},
                {"width", f.width},
                {"height", f.height},
                {"format", format}};
    if (format == "PNG") {
        result["stride"] = f.width * 3;   // the PNG's 8-bit RGB rows
        result["data"] = Base64Encode(EncodePng(f));
    } else {
        const size_t rowBytes = size_t(f.width) * 4;
        std::vector<uint8_t> pixels(rowBytes * f.height);
        for (uint32_t y = 0; y < f.height; ++y)
            std::memcpy(pixels.data() + y * rowBytes, f.topRow + ptrdiff_t(y) * f.strideBytes, rowBytes);
        result["stride"] = rowBytes;
        result["data"] = Base64Encode(pixels);
    }
    return result;
}

json Session::SubscribeFrames(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const uint64_t n = GetUint(params, "every_n_frames", 0, kMaxEveryNFrames, 1);
    if (n == 0) {
        frameSubscriptions_.erase(connection->id());
    } else {
        frameSubscriptions_[connection->id()] = FrameSubscription{connection, uint32_t(n), 0};
    }
    return json::object();
}

// All or nothing: throws -32010 unless `events` more input events fit in the queue.
void Session::RequireInputRoom(uint64_t events)
{
    const uint64_t pending = emulator_.PendingInputEvents();
    if (pending + events > kMaxPendingInputEvents)
        throw RpcError{kInputQueueFull, "Input queue full", json{{"pending", pending}, {"limit", kMaxPendingInputEvents}}};
}

json Session::TypeText(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    std::string text = GetString(params, "text", std::nullopt);
    for (char& c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\n') c = '\r';   // RETURN
        else if (u < 0x20 || u > 0x7E) InvalidParams("text may only contain 0x20-0x7E and \\n");
    }
    if (text.size() > kMaxTextChars) InvalidParams("text is limited to 4096 characters");
    const uint64_t delay = GetUint(params, "inter_key_delay_ms", 0, kMaxInterKeyDelayMs, kDefaultInterKeyDelayMs);
    RequireInputRoom(std::max<size_t>(1, text.size()));   // an empty text still occupies a slot
    const uint64_t id = emulator_.QueueText(text, uint32_t(delay));
    inputOwners_[id] = connection;
    return InputReply(id, emulator_.ScheduledCycle(id));
}

json Session::PressKey(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const KeySpec key = ParseKey(params);
    const uint64_t hold = GetUint(params, "hold_ms", 0, kMaxHoldMs, kDefaultHoldMs);
    RequireInputRoom(1);
    const uint64_t id = emulator_.QueueKey(key, uint32_t(hold));
    inputOwners_[id] = connection;
    return InputReply(id, emulator_.ScheduledCycle(id));
}

json Session::SetJoystick(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t joystick = GetUint(params, "joystick", 0, 1, 0);
    if (joystick == 1) throw RpcError{kNotSupported, "joystick 1 is not supported"};
    JoystickChange change;
    if (Field(params, "x")) change.x = uint8_t(GetUint(params, "x", 0, 255, std::nullopt));
    if (Field(params, "y")) change.y = uint8_t(GetUint(params, "y", 0, 255, std::nullopt));
    if (const json* buttons = Field(params, "buttons")) {
        if (!buttons->is_object()) InvalidParams("buttons must be an object");
        for (const auto& [name, value] : buttons->items()) {
            if (name == "2") throw RpcError{kNotSupported, "button 2 is not supported"};
            if (name != "0" && name != "1") InvalidParams("unknown button: " + name);
            if (value.is_null()) continue;
            if (!value.is_boolean()) InvalidParams("button " + name + " must be a boolean");
            (name == "0" ? change.button0 : change.button1) = value.get<bool>();
        }
    }
    const uint64_t duration = GetUint(params, "duration_ms", 0, kMaxDurationMs, 0);
    RequireInputRoom(duration > 0 ? 2 : 1);
    const uint64_t id = emulator_.QueueJoystick(change, uint32_t(duration));
    inputOwners_[id] = connection;
    return InputReply(id, emulator_.ScheduledCycle(id));
}

// ---- Agent audio (M3 Part B). ----

json Session::GetAudio(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const json* fc = Field(params, "from_cycle");
    const uint64_t fromCycle = fc ? GetUint(params, "from_cycle", 0, UINT64_MAX, 0) : 0;
    const uint64_t durationMs = GetUint(params, "duration_ms", 1, 30000, 1000);
    bool wav = true;
    if (const json* f = Field(params, "format")) {
        if (!f->is_string() || (f->get<std::string>() != "WAV" && f->get<std::string>() != "PCM16"))
            InvalidParams("format must be \"WAV\" or \"PCM16\"");
        wav = f->get<std::string>() == "WAV";
    }

    const AudioCapture& audio = emulator_.Audio();
    const uint64_t span = uint64_t(double(durationMs) * kAudioBaseClockHz / 1000.0);
    const uint64_t newest = audio.NewestCycle(), oldest = (std::max)(audio.OldestCycle(), audioFloor_);
    uint64_t start, end;
    if (fc) {
        start = fromCycle;
        end = start + span;
    } else {
        end = newest;
        start = end > span ? end - span : 0;
        start = (std::max)(start, oldest);   // rounding at the ring's start must not fail a full-ring request
    }
    if (start < oldest) throw RpcError{kAudioNotBuffered, "audio before cycle " + std::to_string(oldest) + " is no longer buffered"};
    end = (std::max)(start, (std::min)(end, newest));

    const std::vector<int16_t> pcm = audio.Read(start, end);
    std::vector<uint8_t> bytes;
    if (wav) {
        bytes = EncodeWav(pcm, kAudioSampleRate, kAudioChannels);
    } else {
        bytes.resize(pcm.size() * 2);
        for (size_t i = 0; i < pcm.size(); ++i) {
            bytes[2 * i] = uint8_t(uint16_t(pcm[i]));
            bytes[2 * i + 1] = uint8_t(uint16_t(pcm[i]) >> 8);
        }
    }
    return json{{"start_cycle", start},
                {"end_cycle", end},
                {"sample_rate", kAudioSampleRate},
                {"channels", kAudioChannels},
                {"format", wav ? "WAV" : "PCM16"},
                {"data", Base64Encode(bytes)}};
}

json Session::SubscribeAudio(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    double thresholdDb = -50.0;
    if (const json* v = Field(params, "threshold_db")) {
        if (!v->is_number()) InvalidParams("threshold_db must be a number");
        thresholdDb = v->get<double>();
        if (!(thresholdDb >= -120.0 && thresholdDb <= 0.0)) InvalidParams("threshold_db must be between -120 and 0");
    }
    const uint64_t minSilenceMs = GetUint(params, "min_silence_ms", 10, 10000, 100);
    // Only sound from now on is reported; re-subscribing replaces the old subscription.
    audioSubscriptions_.erase(connection->id());
    audioSubscriptions_.emplace(connection->id(),
                                AudioSubscription{connection,
                                                  LevelDetector(thresholdDb, uint32_t(minSilenceMs),
                                                                emulator_.EmulatedCycles()),
                                                  thresholdDb, uint32_t(minSilenceMs)});
    return json::object();
}

void Session::SendSoundEvents()
{
    // Also called from the present observer inside a run: must not throw.
    try {
        if (audioSubscriptions_.empty()) return;
        const AudioCapture& audio = emulator_.Audio();
        // Hold back 110 ms behind the newest committed audio (B9): a voice can commit up to ~64 ms (a Mockingboard
        // Timer1 period) after another, and a measured block is never revisited. Matches the capture's gap threshold.
        constexpr uint64_t kHoldBackCycles = uint64_t(0.110 * kAudioBaseClockHz);
        const uint64_t newest = audio.NewestCycle();
        const uint64_t upto = (std::min)(emulator_.EmulatedCycles(), newest > kHoldBackCycles ? newest - kHoldBackCycles : 0);
        for (auto& [id, sub] : audioSubscriptions_) {
            for (const SoundEvent& e : sub.detector.Feed(audio, upto)) {
                if (e.kind == SoundEvent::Started) {
                    sub.connection->Send(MakeNotification(
                        "sound_started", json{{"start_cycle", e.startCycle},
                                              {"emulated_ms", double(e.startCycle) * 1000.0 / kAudioBaseClockHz}}));
                } else {
                    sub.connection->Send(MakeNotification(
                        "sound_stopped",
                        json{{"start_cycle", e.startCycle},
                             {"end_cycle", e.endCycle},
                             {"duration_ms", double(e.endCycle - e.startCycle) * 1000.0 / kAudioBaseClockHz},
                             {"peak_db", e.peakDb},
                             {"dominant_hz", e.dominantHz}}));
                }
            }
        }
    } catch (const std::exception& e) {
        static bool logged = false;
        if (!logged) { logged = true; log::Warning(log::Component::Audio, std::string("sound events: ") + e.what()); }
    } catch (...) {
        static bool logged = false;
        if (!logged) { logged = true; log::Warning(log::Component::Audio, "sound events: unknown exception"); }
    }
}

// ---- M4: checkpoints, state checksum, step_cycles, disks (docs/specs/m4.md). ----

namespace {

constexpr uint64_t kMaxStepCycles = 17030000;
constexpr size_t kMaxCheckpointNameChars = CheckpointStore::kMaxNameChars;

// The checkpoint name: a string of up to 64 printable characters (UTF-8 code points; no control characters).
std::string ParseCheckpointName(const json& params)
{
    const json* f = Field(params, "name");
    if (!f) return {};
    if (!f->is_string()) InvalidParams("name must be a string");
    const std::string s = f->get<std::string>();
    size_t chars = 0;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) InvalidParams("name must be printable characters");
        if ((u & 0xC0) != 0x80) ++chars;   // not a UTF-8 continuation byte
    }
    if (chars > kMaxCheckpointNameChars) InvalidParams("name is limited to 64 characters");
    return s;
}

// Lower-cased, absolute, normal form for path comparison on Windows (case-insensitive).
std::wstring Canonical(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec) c = p.lexically_normal();
    std::wstring w = c.wstring();
    for (wchar_t& ch : w) ch = wchar_t(towlower(ch));
    return w;
}

bool IsInside(const fs::path& child, const fs::path& dir)
{
    const std::wstring c = Canonical(child);
    std::wstring d = Canonical(dir);
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return c.size() > d.size() && c.compare(0, d.size(), d) == 0 && (c[d.size()] == L'\\' || c[d.size()] == L'/');
}

// Review M-2/M-3: a Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9 and their superscript-digit forms,
// CONIN$, CONOUT$), with or without an extension and with trailing spaces or dots, as Win32 treats it in any path
// segment.
bool IsDeviceName(std::wstring segment)
{
    const size_t dot = segment.find(L'.');
    if (dot != std::wstring::npos) segment.resize(dot);
    while (!segment.empty() && (segment.back() == L' ' || segment.back() == L'.')) segment.pop_back();
    for (wchar_t& ch : segment) ch = wchar_t(towupper(ch));
    if (segment == L"CON" || segment == L"PRN" || segment == L"AUX" || segment == L"NUL" || segment == L"CONIN$" ||
        segment == L"CONOUT$")
        return true;
    if (segment.size() == 4 && (segment.compare(0, 3, L"COM") == 0 || segment.compare(0, 3, L"LPT") == 0)) {
        const wchar_t d = segment[3];
        return (d >= L'1' && d <= L'9') || d == L'\u00B9' || d == L'\u00B2' || d == L'\u00B3';
    }
    return false;
}

// export_disk's dest_path names an ordinary file: no \\?\ or \\.\ prefix, no ':' after the drive (an NTFS alternate
// data stream), and no device name in any segment.
void RequirePlainFilePath(const fs::path& dest)
{
    const std::wstring& w = dest.native();
    std::wstring slashed = w;   // Win32 treats '/' and '\' alike, so "/\.\x" is also a device path
    std::replace(slashed.begin(), slashed.end(), L'/', L'\\');
    if (slashed.rfind(L"\\\\?\\", 0) == 0 || slashed.rfind(L"\\\\.\\", 0) == 0)
        InvalidParams("dest_path must not use a \\\\?\\ or \\\\.\\ prefix");
    const size_t afterDrive = dest.has_root_name() && dest.root_name().native().size() == 2 &&
                                      dest.root_name().native()[1] == L':'
                                  ? 2
                                  : 0;
    if (w.find(L':', afterDrive) != std::wstring::npos)
        InvalidParams("dest_path must not name an alternate data stream (':')");
    for (const fs::path& segment : dest.relative_path())
        if (IsDeviceName(segment.native())) InvalidParams("dest_path names a device: " + PathUtf8(segment));
}

}  // namespace

void Session::RequireUsable() const
{
    if (unusable_)
        throw RpcError{kInvalidState, "the session is unusable after a failed restore_checkpoint; close_session"};
}

json Session::Checkpoint(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const std::string name = ParseCheckpointName(params);
    if (checkpoints_->List().size() >= CheckpointStore::kMaxCheckpoints)
        throw RpcError{kCheckpointFailed, "checkpoint limit reached (256)"};
    if (!PauseRunning()) {   // M4-7: pause at the next present first
        exitRequested_ = true;
        throw RpcError{kInvalidState, "the window was closed"};
    }
    try {
        const SnapshotMeta meta = emulator_.SaveSnapshot(checkpoints_->PendingStateFile());
        emulator_.FlushDisks();
        const CheckpointStore::Entry e = checkpoints_->Create(name, meta, emulator_.Disks());
        state_ = State::Paused;   // SaveSnapshot ended the realtime run
        return json{{"checkpoint_id", e.id}, {"emulated_cycles", meta.emulatedCycles}};
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(checkpoints_->PendingStateFile(), ec);
        throw RpcError{kCheckpointFailed, e.what()};
    }
}

json Session::RestoreCheckpoint(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const std::string id = GetString(params, "checkpoint_id", std::nullopt);
    CheckpointStore::Entry entry;
    try {
        entry = checkpoints_->Get(id);
    } catch (const std::out_of_range&) {
        InvalidParams("unknown checkpoint: " + id);
    }
    if (!PauseRunning()) {
        exitRequested_ = true;
        throw RpcError{kInvalidState, "the window was closed"};
    }

    // Before the load (ruling D7): a failure leaves the session usable with its machine state unchanged. Review I-3:
    // every check that needs no ejected drive runs before EjectAll, so those failures leave the drives untouched.
    try {
        checkpoints_->PrepareRestore(entry);
        std::vector<fs::path> images;
        for (const CheckpointStore::DiskCopy& c : entry.disks) images.push_back(c.workingPath);
        emulator_.CheckSnapshot(entry.StateFile(), images);
    } catch (const std::exception& e) {
        throw RpcError{kCheckpointFailed, e.what()};
    }

    // After EjectAll a failure before the load puts the working copies and the drives back (re-inserting may reset a
    // drive's rotational position: docs/api_spec.md). If that fails too, the drives no longer hold what they held and
    // the session is unusable (review I-2): -32004 now, -32008 for the gated methods after.
    auto abandon = [&](std::string what, bool disksRestored) -> RpcError {
        if (disksRestored) {
            try {
                emulator_.ReinsertAll();
                return RpcError{kCheckpointFailed, what};
            } catch (const std::exception& e) {
                what += "; re-inserting the disks failed: " + std::string(e.what());
            }
        }
        unusable_ = true;
        state_ = State::Paused;
        return RpcError{kCheckpointFailed, what + "; the session is unusable, close_session"};
    };
    try {
        emulator_.EjectAll();
        checkpoints_->RestoreDisks(entry);
    } catch (const RestoreRollbackError& e) {
        throw abandon(e.what(), false);
    } catch (const std::exception& e) {
        throw abandon(e.what(), true);
    }
    try {
        emulator_.LoadSnapshot(entry.StateFile(), entry.meta);
    } catch (const SnapshotPreLoadError& e) {   // the machine is untouched: put the disks and drives back
        const std::string rollback = checkpoints_->RollbackRestore(entry);
        if (!rollback.empty()) throw abandon(std::string(e.what()) + "; " + rollback, false);
        throw abandon(e.what(), true);
    } catch (const std::exception& e) {   // in or after the load: the restored disks stay, the session is unusable
        checkpoints_->CommitRestore(entry);
        unusable_ = true;
        state_ = State::Paused;
        SendInputApplied();   // the discarded inputs
        throw RpcError{kCheckpointFailed, e.what()};
    }
    checkpoints_->CommitRestore(entry);

    // Restore purity owned by the session: discarded inputs reported (D1), detectors reset to the restored cycle.
    SendInputApplied();
    const uint64_t at = emulator_.EmulatedCycles();
    audioFloor_ = at;   // audio from before the restore is gone (get_audio: -32009)
    for (auto& [cid, sub] : audioSubscriptions_)
        sub.detector = LevelDetector(sub.thresholdDb, sub.minSilenceMs, at);
    state_ = State::Paused;   // both run modes; resume starts a fresh paced run
    return json{{"emulated_cycles", at}};
}

json Session::ListCheckpoints(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    json list = json::array();
    for (const CheckpointStore::Entry& e : checkpoints_->List())
        list.push_back(json{{"checkpoint_id", e.id},
                            {"name", e.name.empty() ? json(nullptr) : json(e.name)},
                            {"created_at", e.createdAt},
                            {"emulated_cycles", e.meta.emulatedCycles}});
    return list;
}

json Session::DeleteCheckpoint(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const std::string id = GetString(params, "checkpoint_id", std::nullopt);
    try {
        checkpoints_->Delete(id);
    } catch (const std::out_of_range&) {
        InvalidParams("unknown checkpoint: " + id);
    } catch (const std::exception& e) {
        throw RpcError{kCheckpointFailed, e.what()};
    }
    return json::object();
}

json Session::GetStateChecksum(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    return json{{"sha256", emulator_.StateChecksum()}, {"emulated_cycles", emulator_.EmulatedCycles()}};
}

json Session::StepCycles(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t cycles = GetUint(params, "cycles", 1, kMaxStepCycles, std::nullopt);
    bool open = PauseRunning();
    if (open) open = emulator_.RunCycles(cycles);
    SendInputApplied();
    SendSoundEvents();
    if (!open) {
        exitRequested_ = true;
        throw RpcError{kInvalidState, "the window was closed"};
    }
    return json{{"emulated_cycles", emulator_.EmulatedCycles()}};
}

json Session::ListDisks(const json&)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    json list = json::array();
    for (const DiskInfo& d : emulator_.Disks())
        list.push_back(json{{"slot", 6},
                            {"drive", d.drive + 1},
                            {"original_path", PathUtf8(OriginalOf(d.workingPath))},
                            {"working_path", PathUtf8(d.workingPath)},
                            {"write_protected", d.writeProtected},
                            {"format", d.format}});
    return list;
}

json Session::ExportDisk(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t slot = GetUint(params, "slot", 0, 7, std::nullopt);
    if (slot != 6) InvalidParams("slot must be 6");
    const uint64_t drive = GetUint(params, "drive", 1, 2, std::nullopt);
    const fs::path dest = Utf8Path(GetString(params, "dest_path", std::nullopt));
    if (!dest.is_absolute()) InvalidParams("dest_path must be absolute");
    RequirePlainFilePath(dest);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(dest, ec))) InvalidParams("dest_path already exists");
    for (const auto& [working, original] : originals_)
        if (Canonical(original) == Canonical(dest))
            InvalidParams("dest_path is a session's original image");
    const fs::path harnessDir = workDir_.parent_path();   // %TEMP%\applewin-harness
    if (IsInside(dest, harnessDir)) InvalidParams("dest_path is inside the harness work directory");
    if (!fs::is_directory(dest.parent_path(), ec)) InvalidParams("the directory of dest_path does not exist");

    const std::vector<DiskInfo> disks = emulator_.Disks();
    const DiskInfo* found = nullptr;
    for (const DiskInfo& d : disks)
        if (d.drive == int(drive) - 1) found = &d;
    if (!found) throw RpcError{kDiskNotFound, "drive " + std::to_string(drive) + " is empty"};

    emulator_.FlushDisks();
    fs::copy_file(found->workingPath, dest, fs::copy_options::none, ec);
    if (ec) throw RpcError{kInternalError, "cannot write " + PathUtf8(dest) + ": " + ec.message()};
    return json::object();
}

// ---- M5: disk swapping (docs/specs/m5.md). ----

fs::path Session::OriginalOf(const fs::path& workingCopy) const
{
    for (const auto& [working, original] : originals_)
        if (_wcsicmp(working.c_str(), workingCopy.c_str()) == 0) return original;
    return {};
}

json Session::InsertDisk(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t slot = GetUint(params, "slot", 0, 7, std::nullopt);
    if (slot != 6) InvalidParams("slot must be 6");
    const int drive = int(GetUint(params, "drive", 1, 2, std::nullopt)) - 1;
    const fs::path original = Utf8Path(GetString(params, "path", std::nullopt));
    if (!original.is_absolute()) InvalidParams("disk path must be absolute: " + PathUtf8(original));
    const bool writeProtected = GetBool(params, "write_protected", false);
    std::error_code ec;
    if (!fs::is_regular_file(original, ec)) throw RpcError{kDiskNotFound, "disk not found: " + PathUtf8(original)};

    // A copy of its own in a numbered subdirectory: the same image inserted twice, or in both drives, never collides,
    // and a replaced disk's copy (which a checkpoint may name) stays until close_session.
    std::unique_ptr<WorkingCopy> copy;
    try {
        copy = std::make_unique<WorkingCopy>(original, workDir_ / ("insert-" + std::to_string(++swapCopies_)));
    } catch (const std::exception& e) {
        throw RpcError{kInternalError, e.what()};
    }

    std::optional<DiskInfo> previous;
    for (const DiskInfo& d : emulator_.Disks())
        if (d.drive == drive) previous = d;
    try {
        if (previous) emulator_.EjectDisk(drive);   // flushes the track first
        emulator_.InsertDisk(drive, copy->Path(), writeProtected);
    } catch (const std::exception& e) {
        // AppleWin's own image-open result said no: leave the drive as it was and name the user's image.
        std::string what = e.what();
        ReplaceAll(what, copy->Path().string(), original.string());
        if (previous) {
            try {
                emulator_.InsertDisk(drive, previous->workingPath, previous->writeProtected);
            } catch (const std::exception& e2) {
                what += "; putting the previous disk back failed: " + std::string(e2.what());
            }
        }
        throw RpcError{kDiskNotFound, what};   // the new copy is deleted as `copy` goes out of scope
    }
    originals_.emplace_back(copy->Path(), original);
    copies_.push_back(std::move(copy));
    return json::object();
}

json Session::EjectDisk(const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    RequireUsable();
    const uint64_t slot = GetUint(params, "slot", 0, 7, std::nullopt);
    if (slot != 6) InvalidParams("slot must be 6");
    const int drive = int(GetUint(params, "drive", 1, 2, std::nullopt)) - 1;
    bool occupied = false;
    for (const DiskInfo& d : emulator_.Disks())
        if (d.drive == drive) occupied = true;
    if (!occupied) throw RpcError{kDiskNotFound, "drive " + std::to_string(drive + 1) + " is empty"};
    emulator_.EjectDisk(drive);   // flushes the track first; the working copy stays until close_session
    return json::object();
}

}  // namespace harness
