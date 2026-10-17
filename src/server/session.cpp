#include "server/session.h"
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
constexpr uint64_t kMaxMs = UINT32_MAX;   // inter_key_delay_ms, hold_ms, duration_ms

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
           method == "set_speed" || method == "get_speed";
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
                    std::cerr << "error: " << fatal << std::endl;
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
        std::cerr << "error: " << e.what() << std::endl;
        rc = 1;
    } catch (...) {
        std::cerr << "error: unknown exception" << std::endl;
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

void Session::Dispatch(const Command& command)
{
    const ConnectionPtr& conn = command.connection;
    json reply;
    try {
        if (!command.params.is_null() && !command.params.is_object())
            throw RpcError{kInvalidParams, "params must be an object"};
        const json params = command.params.is_object() ? command.params : json::object();
        const std::string& m = command.method;
        json result;
        if (m == "create_session") result = CreateSession(params);
        else if (m == "close_session") result = CloseSession(params);
        else if (m == "pause") result = Pause(params);
        else if (m == "resume") result = Resume(params);
        else if (m == "step_frames") result = StepFrames(params);
        else if (m == "get_frame") result = GetFrame(params);
        else if (m == "subscribe_frames") result = SubscribeFrames(conn, params);
        else if (m == "type_text") result = TypeText(conn, params);
        else if (m == "press_key") result = PressKey(conn, params);
        else if (m == "set_joystick") result = SetJoystick(conn, params);
        else if (m == "set_speed") result = SetSpeed(params);
        else if (m == "get_speed") result = GetSpeed(params);
        else if (m == "get_audio") result = GetAudio(params);
        else if (m == "subscribe_audio") result = SubscribeAudio(conn, params);
        else throw RpcError{kMethodNotFound, "method not found: " + m};
        reply = MakeResult(command.id, std::move(result));
    } catch (const RpcError& e) {
        reply = MakeError(command.id, e.code, e.message);
    } catch (const std::exception& e) {
        reply = MakeError(command.id, kInternalError, e.what());
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
            conn->Send(MakeNotification("input_applied", json{{"input_id", a.inputId},
                                                              {"first_cycle", a.firstCycle},
                                                              {"last_cycle", a.lastCycle}}));
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
    if (GetBool(params, "allow_text_screen", false))
        throw RpcError{kNotSupported, "allow_text_screen is not supported"};
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

json Session::TypeText(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    std::string text = GetString(params, "text", std::nullopt);
    for (char& c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\n') c = '\r';   // RETURN
        else if (u < 0x20 || u > 0x7E) InvalidParams("text may only contain 0x20-0x7E and \\n");
    }
    if (text.size() > kMaxTextChars) InvalidParams("text is limited to 4096 characters");
    const uint64_t delay = GetUint(params, "inter_key_delay_ms", 0, kMaxMs,
                                   kDefaultInterKeyDelayMs);
    const uint64_t id = emulator_.QueueText(text, uint32_t(delay));
    inputOwners_[id] = connection;
    return InputReply(id, emulator_.ScheduledCycle(id));
}

json Session::PressKey(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
    const KeySpec key = ParseKey(params);
    const uint64_t hold = GetUint(params, "hold_ms", 0, kMaxMs, kDefaultHoldMs);
    const uint64_t id = emulator_.QueueKey(key, uint32_t(hold));
    inputOwners_[id] = connection;
    return InputReply(id, emulator_.ScheduledCycle(id));
}

json Session::SetJoystick(const ConnectionPtr& connection, const json& params)
{
    if (state_ == State::NoSession) throw RpcError{kNoSession, "no session"};
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
    const uint64_t duration = GetUint(params, "duration_ms", 0, kMaxMs, 0);
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
    const uint64_t newest = audio.NewestCycle(), oldest = audio.OldestCycle();
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
                                AudioSubscription{connection, LevelDetector(thresholdDb, uint32_t(minSilenceMs),
                                                                            emulator_.EmulatedCycles())});
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
        if (!logged) { logged = true; std::cerr << "warning: sound events: " << e.what() << std::endl; }
    } catch (...) {
        static bool logged = false;
        if (!logged) { logged = true; std::cerr << "warning: sound events: unknown exception" << std::endl; }
    }
}

}  // namespace harness
