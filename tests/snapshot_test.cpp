// M4 checkpoints in-process (spec: docs/specs/m4.md, pass conditions 1, 3, 5 and 6, "Restore purity"; plan Review Focus
// 1 and 3). Test prefix "ckpt:".
//
// The file is named snapshot_test.cpp, not checkpoint_test.cpp: doctest runs files in name order, and "emu:
// precondition" (dos_test.cpp) must run before anything starts the shared emulator. The restore error paths are in
// zz_restore_failure_test.cpp, so they run after every other in-process test.
#include "doctest/doctest.h"
#include "host/emulator.h"
#include "core/audio_capture.h"
#include "core/base64.h"
#include "core/sha256.h"
#include "core/working_copy.h"
#include "support/audio_signals.h"
#include "support/emulator_fixture.h"
#include "support/golden.h"
#include "support/paths.h"
#include "support/server_process.h"
#include "support/snapshot_support.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#pragma warning(push, 3)
#include "Memory.h"
#pragma warning(pop)

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;
using namespace harness;
using namespace harness::testsupport;

namespace {

constexpr uint64_t kCyclesPerFrame = 17030;
constexpr uint64_t kMaxInstructionCycles = 7;   // the longest 65C02 instruction: the most a cycle-exact run overshoots
constexpr double kBaseClockHz = (157500000.0 / 11.0) * 65.0 / 912.0;   // CLK_6502_NTSC

// The presented frame's BGRA32 rows, top row first: the bytes get_frame { format: "BGRA32" } sends.
std::vector<uint8_t> RawFrame(const FrameView& f)
{
    const size_t rowBytes = size_t(f.width) * 4;
    std::vector<uint8_t> pixels(rowBytes * f.height);
    for (uint32_t y = 0; y < f.height; ++y)
        std::memcpy(pixels.data() + y * rowBytes, f.topRow + ptrdiff_t(y) * f.strideBytes, rowBytes);
    return pixels;
}

std::string RawFrameHash(const FrameView& f) { return Sha256Hex(RawFrame(f)); }

// ---- Pass condition 1 in-process ----

struct Step {
    std::string checksum;
    uint64_t cycles = 0;
    std::string frame;
    bool operator==(const Step&) const = default;
};

struct ScriptRun {
    std::vector<Step> steps;
    std::vector<int16_t> audio;   // the timeline over the script's range
};

doctest::String toString(const Step& s)
{
    return (s.checksum + " @" + std::to_string(s.cycles) + " frame " + s.frame).c_str();
}

// PRINT PEEK(-16368) ($C010: any-key-down in bit 7), PEEK(-16287)/PEEK(-16286) (buttons 0 and 1), PDL(0), PDL(1).
constexpr const char* kPrintInputState =
    "PRINT PEEK(-16368);\" \";PEEK(-16287);\" \";PEEK(-16286);\" \";PDL(0);\" \";PDL(1)\r";

// Motor on ($C0E9), 20 x phases 3..0 on/off (head out to track 0), then 30000 speaker toggles ($C030): ~30 s of tone.
constexpr const char* kBusyProgram =
    "NEW\r"
    "10 X=PEEK(49385)\r"
    "20 FOR T=1 TO 20:FOR P=3 TO 0 STEP -1:X=PEEK(49377+2*P):X=PEEK(49376+2*P):NEXT:NEXT\r"
    "30 FOR I=1 TO 30000:X=PEEK(49200):NEXT\r"
    "RUN\r";

// The script of pass condition 1: input mixed with whole-frame and cycle-exact steps, a bell for audio, and a short disk
// read (CATALOG; ruling D4: both runs start from a restore, which reseeds rand() the same way, D2). Records the
// checksum, cycle and frame hash after every step. Its first statement prints the input state (ruling D6: any-key-down,
// buttons 0 and 1, paddles 0 and 1), so a key or joystick change leaked across the restore shows on screen and in RAM.
ScriptRun RunScript(Emulator& e, uint64_t fromCycle)
{
    ScriptRun r;
    auto record = [&] { r.steps.push_back(Step{e.StateChecksum(), e.EmulatedCycles(), RawFrameHash(e.Frame())}); };
    record();
    e.QueueText(kPrintInputState, 0);
    REQUIRE(e.RunFrames(30));
    record();
    e.QueueText("10 PRINT \"HI\"\r", 0);
    REQUIRE(e.RunFrames(40));
    record();
    e.RunCycles(12345);
    record();
    e.QueueText("PRINT CHR$(7)\r", 20);
    e.RunCycles(7 * kCyclesPerFrame + 3);
    record();
    REQUIRE(e.RunFrames(60));
    record();
    e.QueueKey(uint8_t('A'), 50);
    REQUIRE(e.RunFrames(30));
    record();
    e.QueueText("CATALOG\r", 0);
    REQUIRE(e.RunFrames(90));
    record();
    e.RunCycles(1);
    record();
    r.audio = e.Audio().Read(fromCycle, e.EmulatedCycles());
    e.TakeAppliedInputs();
    return r;
}

// Work between the restores that leaves the machine and the harness busy AT the restore (review I2): kBusyProgram turns
// the Disk II motor on, steps the head out to track 0 (run 1's CATALOG leaves it on the catalog track, 17) and then
// toggles the speaker in a long loop that is still running at the restore. Meanwhile joystick 0 is moved with button 0
// down, a key is held, and text is still queued behind the unread key.
void UnrelatedWork(Emulator& e)
{
    e.QueueText(kBusyProgram, 0);
    REQUIRE(e.RunFrames(240));
    JoystickChange j;
    j.x = uint8_t(0);
    j.y = uint8_t(255);
    j.button0 = true;
    e.QueueJoystick(j, 5000);
    e.QueueKey(uint8_t('Z'), 5000);
    e.QueueText("LIST\r", 500);
    REQUIRE(e.RunFrames(20));
    e.RunCycles(4321);
}

// Pass condition 5's speech program (tests/programs/ssi263_speech.bas, shared with the Python tests), in type_text's form
// (RETURN as "\n"). AppleWin's default slot-4 card is a Mockingboard C; its socket-1 SSI263 (SSI263AP) is the primary
// speech chip at $C440-$C444: reg 0 duration/phoneme, 1 inflection, 2 rate/inflection, 3 control/articulation/
// amplitude, 4 filter frequency. The program powers the chip down, sets rate, inflection and filter, writes a phoneme
// (duration-mode bits 00: A/!R interrupt disabled), clears CTL with amplitude 15 ("POKE S+3,127"), which plays it, then
// writes five more phonemes ~0.1 s apart and powers down: ~0.7 s of speech from ~1.1 s after typing starts.
std::string SpeechProgram()
{
    const std::vector<uint8_t> bytes = ReadAll(RepoRoot() / "tests" / "programs" / "ssi263_speech.bas");
    std::string s(bytes.begin(), bytes.end());
    std::erase(s, '\r');   // a CRLF checkout (core.autocrlf)
    REQUIRE_MESSAGE(s.find("POKE S+3,127") != std::string::npos, "tests/programs/ssi263_speech.bas is missing or changed");
    return s;
}

// The negative control: amplitude 0 when CTL is cleared, every other write the same. Silence there shows the speech
// run's sound is the SSI263's, not a side effect of the $C44x writes on the first 6522 or its AY-3-8910.
std::string MutedSpeechProgram()
{
    std::string s = SpeechProgram();
    s.replace(s.find("POKE S+3,127"), 12, "POKE S+3,112");
    return s;
}

// The in-process form of a type_text text: RETURN is "\r".
std::string InProcessText(std::string s)
{
    std::replace(s.begin(), s.end(), '\n', '\r');
    return s;
}

// Pass condition 5's script: the program typed and run, whole-frame and cycle-exact steps splitting the speech.
ScriptRun RunSpeech(Emulator& e, uint64_t fromCycle, const std::string& program)
{
    ScriptRun r;
    auto record = [&] { r.steps.push_back(Step{e.StateChecksum(), e.EmulatedCycles(), RawFrameHash(e.Frame())}); };
    record();
    e.QueueText(InProcessText(program), 0);
    REQUIRE(e.RunFrames(70));
    record();
    e.RunCycles(12345);
    record();
    REQUIRE(e.RunFrames(20));
    record();
    e.RunCycles(5 * kCyclesPerFrame + 777);
    record();
    REQUIRE(e.RunFrames(40));
    record();
    e.RunCycles(1);
    record();
    r.audio = e.Audio().Read(fromCycle, e.EmulatedCycles());
    e.TakeAppliedInputs();
    return r;
}

// A restore as the session does it: eject, put the checkpoint's disk copy back, load.
void Restore(Emulator& e, const fs::path& state, const SnapshotMeta& meta, const fs::path& diskCopy, const fs::path& working)
{
    e.EjectAll();
    fs::copy_file(diskCopy, working, fs::copy_options::overwrite_existing);
    e.LoadSnapshot(state, meta);
}

bool AllZero(const std::vector<int16_t>& v)
{
    for (const int16_t s : v)
        if (s != 0) return false;
    return true;
}

// ---- Servers for the cross-process tests ----

struct Server {
    std::unique_ptr<ServerProcess> process;
    Announcement announcement;
};

Server StartServer(const std::wstring& args)
{
    Server s;
    s.process = std::make_unique<ServerProcess>(args);
    REQUIRE(s.process->Launched());
    const auto line = s.process->WaitForLine(15s);
    REQUIRE_MESSAGE(line.has_value(), "no announcement line within 15 s; stderr: " << s.process->Stderr());
    const auto a = ParseAnnouncement(*line);
    REQUIRE_MESSAGE(a.has_value(), "announcement is not the expected JSON: " << *line);
    s.announcement = *a;
    return s;
}

std::unique_ptr<PipeClient> Connect(const Server& s, const std::string& role)
{
    auto c = std::make_unique<PipeClient>();
    REQUIRE(c->Open(s.announcement.pipe));
    const std::string& token = role == "agent" ? s.announcement.agentToken : s.announcement.operatorToken;
    const auto reply = c->Call("hello", {{"protocol_version", "0.3"}, {"role", role}, {"token", token}});
    REQUIRE_MESSAGE(reply.has_value(), "no reply to hello");
    REQUIRE_MESSAGE(reply->contains("result"), "hello failed: " << reply->dump());
    return c;
}

// The result of a call; REQUIREs that there is one.
json Result(PipeClient& c, const std::string& method, const json& params = json::object(),
            std::chrono::milliseconds timeout = 60s)
{
    const auto reply = c.Call(method, params, timeout);
    REQUIRE_MESSAGE(reply.has_value(), method << ": no reply");
    REQUIRE_MESSAGE(reply->contains("result"), method << " " << params.dump() << " failed: " << reply->dump());
    return (*reply)["result"];
}

std::string StringHash(const std::string& s)
{
    return Sha256Hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

// SHA-256 of a reply's base64 "data" text (equal text, equal bytes).
std::string DataHash(const json& reply) { return StringHash(reply["data"].get<std::string>()); }

// type_text's form of an in-process text: RETURN is sent as "\n".
std::string PipeText(std::string s)
{
    std::replace(s.begin(), s.end(), '\r', '\n');
    return s;
}

struct PipeRun {
    std::vector<std::string> records;   // "checksum @cycles frame-hash" after every step
    std::string audioHash;              // SHA-256 of get_audio's base64 PCM16 over the script's range
    bool audioSilent = false;           // every sample of that range is zero
    bool operator==(const PipeRun&) const = default;
};

// Pass condition 1's script over the pipe (type_text, step_frames, step_cycles), from the checkpoint at fromCycle.
PipeRun RunPipeScript(PipeClient& op, PipeClient& agent, uint64_t fromCycle)
{
    PipeRun r;
    auto record = [&] {
        const json cs = Result(op, "get_state_checksum");
        const json frame = Result(agent, "get_frame", {{"format", "BGRA32"}});
        r.records.push_back(cs["sha256"].get<std::string>() + " @" + std::to_string(cs["emulated_cycles"].get<uint64_t>()) +
                            " " + DataHash(frame));
    };
    record();
    Result(agent, "type_text", {{"text", PipeText(kPrintInputState)}, {"inter_key_delay_ms", 0}});
    Result(op, "step_frames", {{"frames", 30}});
    record();
    Result(agent, "type_text", {{"text", "10 PRINT \"HI\"\n"}, {"inter_key_delay_ms", 0}});
    Result(op, "step_frames", {{"frames", 40}});
    record();
    Result(op, "step_cycles", {{"cycles", 12345}});
    record();
    Result(agent, "type_text", {{"text", "PRINT CHR$(7)\n"}, {"inter_key_delay_ms", 20}});
    Result(op, "step_cycles", {{"cycles", 7 * kCyclesPerFrame + 3}});
    record();
    Result(op, "step_frames", {{"frames", 60}});
    record();
    Result(agent, "press_key", {{"key", "A"}, {"hold_ms", 50}});
    Result(op, "step_frames", {{"frames", 30}});
    record();
    Result(agent, "type_text", {{"text", "CATALOG\n"}, {"inter_key_delay_ms", 0}});
    Result(op, "step_frames", {{"frames", 90}});
    record();
    const uint64_t end = Result(op, "step_cycles", {{"cycles", 1}})["emulated_cycles"].get<uint64_t>();
    record();
    const uint64_t ms = uint64_t(std::ceil(double(end - fromCycle) * 1000.0 / kBaseClockHz)) + 1;
    r.audioHash = DataHash(Result(agent, "get_audio", {{"from_cycle", fromCycle}, {"duration_ms", ms}, {"format", "PCM16"}}));
    return r;
}

// In a fresh server (display mode from args), ruling D4: DOS booted stepped, a checkpoint, some frames, a restore, the
// script; unrelated work, a restore, the script again. Returns both runs (each starts from a restore).
std::pair<PipeRun, PipeRun> CheckpointReplayOverPipe(const std::wstring& args)
{
    Server s = StartServer(args);
    auto op = Connect(s, "operator");
    Result(*op, "create_session", {{"run_mode", "stepped"}, {"disks", json::array({{{"slot", 6}, {"drive", 1},
                                                                                   {"path", DosMasterImage().string()}}})}});
    Result(*op, "step_frames", {{"frames", 720}});
    auto agent = Connect(s, "agent");
    const json cp = Result(*op, "checkpoint", {{"name", "pc3"}});
    const uint64_t at = cp["emulated_cycles"].get<uint64_t>();
    const std::string atChecksum = Result(*op, "get_state_checksum")["sha256"].get<std::string>();
    Result(*op, "step_frames", {{"frames", 30}});
    Result(*op, "restore_checkpoint", {{"checkpoint_id", cp["checkpoint_id"]}});
    PipeRun a = RunPipeScript(*op, *agent, at);
    // Unrelated work as UnrelatedWork in-process, still busy at the restore: motor on, head at track 0, speaker
    // toggling, joystick moved with button 0 down, a key held, text queued, and an audio subscription mid-sound.
    Result(*agent, "subscribe_audio");
    Result(*agent, "type_text", {{"text", PipeText(kBusyProgram)}, {"inter_key_delay_ms", 0}});
    Result(*op, "step_frames", {{"frames", 240}});
    Result(*agent, "set_joystick", {{"x", 0}, {"y", 255}, {"buttons", {{"0", true}}}, {"duration_ms", 5000}});
    Result(*agent, "press_key", {{"key", "Z"}, {"hold_ms", 5000}});
    Result(*agent, "type_text", {{"text", "LIST\n"}, {"inter_key_delay_ms", 500}});
    Result(*op, "step_frames", {{"frames", 20}});
    Result(*op, "step_cycles", {{"cycles", 4321}});
    Result(*op, "restore_checkpoint", {{"checkpoint_id", cp["checkpoint_id"]}});
    PipeRun b = RunPipeScript(*op, *agent, at);
    // Each run's first record was taken straight after a restore: the checkpoint's own checksum.
    CHECK(a.records[0].rfind(atChecksum, 0) == 0);
    CHECK(b.records[0].rfind(atChecksum, 0) == 0);
    Result(*op, "close_session");
    return {a, b};
}

// Pass condition 5's script over the pipe, as RunSpeech in-process.
PipeRun RunPipeSpeech(PipeClient& op, PipeClient& agent, uint64_t fromCycle, const std::string& program)
{
    PipeRun r;
    auto record = [&] {
        const json cs = Result(op, "get_state_checksum");
        const json frame = Result(agent, "get_frame", {{"format", "BGRA32"}});
        r.records.push_back(cs["sha256"].get<std::string>() + " @" + std::to_string(cs["emulated_cycles"].get<uint64_t>()) +
                            " " + DataHash(frame));
    };
    record();
    Result(agent, "type_text", {{"text", program}, {"inter_key_delay_ms", 0}});
    Result(op, "step_frames", {{"frames", 70}});
    record();
    Result(op, "step_cycles", {{"cycles", 12345}});
    record();
    Result(op, "step_frames", {{"frames", 20}});
    record();
    Result(op, "step_cycles", {{"cycles", 5 * kCyclesPerFrame + 777}});
    record();
    Result(op, "step_frames", {{"frames", 40}});
    record();
    const uint64_t end = Result(op, "step_cycles", {{"cycles", 1}})["emulated_cycles"].get<uint64_t>();
    record();
    const uint64_t ms = uint64_t(std::ceil(double(end - fromCycle) * 1000.0 / kBaseClockHz)) + 1;
    const json audio = Result(agent, "get_audio", {{"from_cycle", fromCycle}, {"duration_ms", ms}, {"format", "PCM16"}});
    // Base64 of all-zero PCM is only 'A's (and '=' padding): any other character is a non-zero sample.
    r.audioSilent = audio["data"].get<std::string>().find_first_not_of("A=") == std::string::npos;
    r.audioHash = DataHash(audio);
    return r;
}

struct SpeechRuns {
    PipeRun first, second, muted;
};

// In a fresh server (display mode from args): DOS booted stepped, a checkpoint, some frames, a restore, the speech; the
// speech started again and still playing at the second restore, the speech; a third restore and the muted program.
SpeechRuns SpeechOverPipe(const std::wstring& args)
{
    Server s = StartServer(args);
    auto op = Connect(s, "operator");
    Result(*op, "create_session", {{"run_mode", "stepped"}, {"disks", json::array({{{"slot", 6}, {"drive", 1},
                                                                                   {"path", DosMasterImage().string()}}})}});
    Result(*op, "step_frames", {{"frames", 720}});
    auto agent = Connect(s, "agent");
    const json cp = Result(*op, "checkpoint", {{"name", "pc5"}});
    const uint64_t at = cp["emulated_cycles"].get<uint64_t>();
    Result(*op, "step_frames", {{"frames", 30}});
    Result(*op, "restore_checkpoint", {{"checkpoint_id", cp["checkpoint_id"]}});
    SpeechRuns r;
    r.first = RunPipeSpeech(*op, *agent, at, SpeechProgram());
    Result(*agent, "type_text", {{"text", "RUN\n"}, {"inter_key_delay_ms", 0}});
    Result(*op, "step_frames", {{"frames", 20}});
    Result(*op, "step_cycles", {{"cycles", 999}});
    Result(*op, "restore_checkpoint", {{"checkpoint_id", cp["checkpoint_id"]}});
    r.second = RunPipeSpeech(*op, *agent, at, SpeechProgram());
    Result(*op, "restore_checkpoint", {{"checkpoint_id", cp["checkpoint_id"]}});
    r.muted = RunPipeSpeech(*op, *agent, at, MutedSpeechProgram());
    Result(*op, "close_session");
    return r;
}

}  // namespace

// ---- Pass condition 1 (the restore-purity core) ----

TEST_CASE("ckpt: two restored runs replay the script exactly, whatever ran before each restore")
{
    // Ruling D4: both runs start from a restore of the same checkpoint, with very different work before each.
    auto& e = StartedEmulator();
    DosBoot boot(e, "replay");
    const fs::path state = boot.dir.path / "state.yaml";
    const fs::path diskCopy = boot.dir.path / "disk-copy.dsk";

    const uint64_t at = e.EmulatedCycles();
    const std::string checksumAt = e.StateChecksum();
    const SnapshotMeta meta = e.SaveSnapshot(state);
    CHECK(meta.emulatedCycles == at);
    CHECK(e.EmulatedCycles() == at);              // saving runs nothing
    CHECK(e.StateChecksum() == checksumAt);       // and changes nothing
    REQUIRE(fs::exists(state));
    e.FlushDisks();
    fs::copy_file(boot.copy.Path(), diskCopy);

    REQUIRE(e.RunFrames(30));                     // anything
    Restore(e, state, meta, diskCopy, boot.copy.Path());
    CHECK(e.EmulatedCycles() == at);              // the cycle origin comes from the checkpoint
    CHECK(e.StateChecksum() == checksumAt);
    const ScriptRun first = RunScript(e, at);
    REQUIRE(first.steps.size() == 9);
    CHECK_MESSAGE(!AllZero(first.audio), "the script's bell left no audio: the audio comparison would prove nothing");

    UnrelatedWork(e);
    Restore(e, state, meta, diskCopy, boot.copy.Path());
    CHECK(e.EmulatedCycles() == at);
    CHECK(e.StateChecksum() == checksumAt);
    const ScriptRun second = RunScript(e, at);

    REQUIRE(second.steps.size() == first.steps.size());
    for (size_t i = 0; i < first.steps.size(); ++i) {
        INFO("step " << i);
        CHECK(second.steps[i] == first.steps[i]);
    }
    CHECK(second.audio.size() == first.audio.size());
    CHECK_MESSAGE(second.audio == first.audio, "audio over the script's range differs between the runs");
}

// ---- Pass condition 5 in-process ----

TEST_CASE("ckpt: SSI263 speech replays identically across two restores; the muted program is silent")
{
    auto& e = StartedEmulator();
    DosBoot boot(e, "speech");
    const fs::path state = boot.dir.path / "state.yaml";
    const fs::path diskCopy = boot.dir.path / "disk-copy.dsk";
    const uint64_t at = e.EmulatedCycles();
    const std::string checksumAt = e.StateChecksum();
    const SnapshotMeta meta = e.SaveSnapshot(state);
    e.FlushDisks();
    fs::copy_file(boot.copy.Path(), diskCopy);

    REQUIRE(e.RunFrames(30));
    Restore(e, state, meta, diskCopy, boot.copy.Path());
    const ScriptRun first = RunSpeech(e, at, SpeechProgram());
    const double db = AcRmsDb(MonoMix(first.audio));
    INFO("speech AC RMS " << db << " dBFS");
    CHECK_MESSAGE(db > -40.0, "the speech program left no audio");

    // The speech started again and still playing at the restore: the SSI263 mid-phoneme with its amplitude up.
    e.QueueText("RUN\r", 0);
    REQUIRE(e.RunFrames(20));
    e.RunCycles(999);
    Restore(e, state, meta, diskCopy, boot.copy.Path());
    CHECK(e.StateChecksum() == checksumAt);
    const ScriptRun second = RunSpeech(e, at, SpeechProgram());
    REQUIRE(second.steps.size() == first.steps.size());
    for (size_t i = 0; i < first.steps.size(); ++i) {
        INFO("step " << i);
        CHECK(second.steps[i] == first.steps[i]);
    }
    CHECK_MESSAGE(second.audio == first.audio, "speech audio differs between the restored runs");

    Restore(e, state, meta, diskCopy, boot.copy.Path());
    const ScriptRun muted = RunSpeech(e, at, MutedSpeechProgram());
    CHECK(muted.audio.size() == first.audio.size());
    CHECK_MESSAGE(AllZero(muted.audio), "the muted program is not silent: the speech run's audio may not be the SSI263's");
}

TEST_CASE("ckpt: LoadSnapshot discards pending input and reports each id as discarded (D1)")
{
    auto& e = StartedEmulator();
    ScratchDir dir("discard");
    EjectAndPowerCycle guard{e};
    e.PowerCycle();   // no disk: the boot ROM never reads the keyboard, so typed keys stay queued
    REQUIRE(e.RunFrames(10));
    const SnapshotMeta meta = e.SaveSnapshot(dir.path / "state.yaml");
    e.TakeAppliedInputs();

    const uint64_t text = e.QueueText("ABC", 0);
    const uint64_t key = e.QueueKey(uint8_t('Q'), 100);
    JoystickChange j;
    j.x = uint8_t(5);
    const uint64_t joy = e.QueueJoystick(j, 1000);   // queued behind the text and the key
    REQUIRE(e.RunFrames(1));                         // "A" is applied; "B" then waits for the strobe
    CHECK(e.TakeAppliedInputs().empty());

    e.LoadSnapshot(dir.path / "state.yaml", meta);
    std::vector<InputApplied> applied = e.TakeAppliedInputs();
    std::vector<uint64_t> discarded;
    for (const InputApplied& a : applied) {
        CHECK(a.discarded);
        discarded.push_back(a.inputId);
    }
    std::sort(discarded.begin(), discarded.end());
    CHECK(discarded == std::vector<uint64_t>{text, key, joy});

    REQUIRE(e.RunFrames(120));   // nothing from before the restore takes effect later
    CHECK(e.TakeAppliedInputs().empty());
}

TEST_CASE("ckpt: FLASH text replays identically after restores, whatever ran before (full speed included)")
{
    // The text-flash phase (NTSC.cpp's flash counter) is in no save state; the restore sets it from the restored cycle.
    // The Applesoft cursor blinks in software, so pass condition 1's script does not show it: FLASH text does.
    auto& e = StartedEmulator();
    DosBoot boot(e, "flash");
    e.QueueText("HOME:FLASH:PRINT \"FLASHING TEXT\":NORMAL\r", 0);
    REQUIRE(e.RunFrames(60));
    const fs::path state = boot.dir.path / "state.yaml";
    const SnapshotMeta meta = e.SaveSnapshot(state);
    auto frames = [&] {
        std::vector<std::string> hashes;
        for (int i = 0; i < 40; ++i) {   // more than one flash period (32 frames)
            REQUIRE(e.RunFrames(1));
            hashes.push_back(RawFrameHash(e.Frame()));
        }
        return hashes;
    };
    e.EjectAll();
    e.LoadSnapshot(state, meta);
    const std::vector<std::string> first = frames();
    // A flash phase that differs from the first restore's: 7 frames, a realtime run at full speed (whole-screen
    // redraws) and one at normal speed, ending mid-frame.
    REQUIRE(e.RunFrames(7));
    e.SetFullSpeedOverride(true);
    REQUIRE(e.RunRealtime(std::chrono::milliseconds(120)));
    e.SetFullSpeedOverride(false);
    REQUIRE(e.RunRealtime(std::chrono::milliseconds(37)));
    e.EjectAll();
    e.LoadSnapshot(state, meta);
    const std::vector<std::string> second = frames();
    CHECK_MESSAGE(std::count(first.begin(), first.end(), first.front()) < 40, "no flashing seen: the test proves nothing");
    for (size_t i = 0; i < first.size(); ++i) {
        INFO("frame " << i + 1);
        CHECK(second[i] == first[i]);
    }
}

// AppleWin's save state stores disk image names as UTF-8 and its load hands those bytes to the ANSI file API
// (YamlSaveHelper::SaveString, Disk2InterfaceCard::LoadSnapshotFloppy). Unless the ANSI code page is UTF-8, a non-ASCII
// path cannot come back: the checkpoint is refused unless the image went in through its ASCII 8.3 short path (D10, M6),
// and a save state naming a non-ASCII path is refused before loading (no file dialog, machine unchanged, ruling D7).
TEST_CASE("ckpt: a non-ASCII disk path is refused at checkpoint, or restores; a save state naming one never opens a dialog")
{
    auto& e = StartedEmulator();
    const bool utf8Acp = GetACP() == CP_UTF8;
    const std::wstring accented = L"awh-ckpt-café-" + std::to_wstring(GetCurrentProcessId());
    const fs::path dir = fs::temp_directory_path() / accented;
    struct RemoveDir {
        fs::path p;
        ~RemoveDir() { std::error_code ec; fs::remove_all(p, ec); }
    } removeDir{dir};
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::create_directories(dir);
    BOOL usedDefault = FALSE;
    const std::wstring w = dir.wstring();
    const bool ansiRepresentable =
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &usedDefault) > 0 && !usedDefault;
    REQUIRE_MESSAGE(ansiRepresentable, "pick a character this machine's ANSI code page has (InsertDisk needs one)");

    // 1. Checkpoint with the working copy under the accented directory.
    {
        WorkingCopy copy(DosMasterImage(), dir / "disk");
        e.InsertDisk(0, copy.Path());
        EjectAndPowerCycle guard{e};
        e.PowerCycle();
        REQUIRE(e.RunFrames(60));
        const std::string before = e.StateChecksum();
        const fs::path state = dir / "state.yaml";
        // D10 (M6): the working copy went in through its ASCII 8.3 short path when the volume has one for it.
        wchar_t shortBuf[MAX_PATH] = {};
        const DWORD shortLen = GetShortPathNameW(copy.Path().c_str(), shortBuf, MAX_PATH);
        const bool hasAsciiPath = shortLen > 0 && shortLen < MAX_PATH &&
                                  std::all_of(shortBuf, shortBuf + shortLen, [](wchar_t c) { return c < 0x80; });
        if (utf8Acp || hasAsciiPath) {
            const SnapshotMeta meta = e.SaveSnapshot(state);
            e.EjectAll();
            e.LoadSnapshot(state, meta);
            CHECK(e.StateChecksum() == before);
        } else {
            CHECK_THROWS_AS(e.SaveSnapshot(state), std::runtime_error);
            CHECK_FALSE(fs::exists(state));
            CHECK(e.StateChecksum() == before);
        }
    }

    // 2. A save state that names an image under the accented directory (an ASCII checkpoint, edited): LoadSnapshot makes
    // AppleWin's own lookups on the same bytes before loading.
    ScratchDir ascii("ansi");
    WorkingCopy copy(DosMasterImage(), ascii.path / "disk");
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle guard{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(60));
    const fs::path state = ascii.path / "state.yaml";
    const SnapshotMeta meta = e.SaveSnapshot(state);
    const std::string before = e.StateChecksum();
    fs::create_directories(dir / "disk");
    fs::copy_file(copy.Path(), dir / "disk" / copy.Path().filename(), fs::copy_options::overwrite_existing);
    const std::u8string from = ascii.path.filename().u8string(), to = fs::path(accented).u8string();
    std::vector<uint8_t> bytes = ReadAll(state);
    std::string text(bytes.begin(), bytes.end());
    const std::string fromS(from.begin(), from.end()), toS(to.begin(), to.end());
    size_t replaced = 0;
    for (size_t pos = 0; (pos = text.find(fromS, pos)) != std::string::npos; pos += toS.size(), ++replaced)
        text.replace(pos, fromS.size(), toS);
    REQUIRE(replaced > 0);
    WriteAll(state, std::vector<uint8_t>(text.begin(), text.end()));

    e.EjectAll();
    if (utf8Acp) {
        e.LoadSnapshot(state, meta);
        CHECK(e.StateChecksum() == before);
    } else {
        std::string what;
        try {
            e.LoadSnapshot(state, meta);
        } catch (const std::runtime_error& ex) {
            what = ex.what();
        }
        CHECK_MESSAGE(what.find("not found") != std::string::npos, "expected the pre-load refusal, got: " << what);
        e.ReinsertAll();   // ruling D7: the restore is abandoned, the machine unchanged
        CHECK(e.StateChecksum() == before);
        REQUIRE(e.RunFrames(2));
    }
}

// D10 (M6): with no ASCII short path for a non-ANSI-safe working path (8.3 names off), the checkpoint is still refused,
// with the D10 reason, writes nothing and leaves the machine alone. The seam forces that on every machine.
TEST_CASE("ckpt: with no ASCII short path a non-ANSI-safe disk path is refused at checkpoint (D10 seam)")
{
    auto& e = StartedEmulator();
    struct Seam {
        Seam() { Emulator::SetShortPathsDisabledForTest(true); }
        ~Seam() { Emulator::SetShortPathsDisabledForTest(false); }
    } seam;
    const fs::path dir = fs::temp_directory_path() / (L"awh-d10-café-" + std::to_wstring(GetCurrentProcessId()));
    struct RemoveDir {
        fs::path p;
        ~RemoveDir() { std::error_code ec; fs::remove_all(p, ec); }
    } removeDir{dir};
    fs::create_directories(dir);
    if (GetACP() == CP_UTF8) {
        MESSAGE("ANSI code page is UTF-8: every path survives the save state, nothing to refuse");
        return;
    }
    BOOL usedDefault = FALSE;
    const std::wstring w = dir.wstring();
    REQUIRE(WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &usedDefault) > 0);
    REQUIRE_FALSE(usedDefault);

    WorkingCopy copy(DosMasterImage(), dir / "disk");
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle guard{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(30));
    const std::string before = e.StateChecksum();
    ScratchDir out("d10seam");
    const fs::path state = out.path / "state.yaml";
    try {
        e.SaveSnapshot(state);
        FAIL("the checkpoint should have been refused");
    } catch (const std::runtime_error& ex) {
        CHECK_MESSAGE(std::string(ex.what()).find("cannot bring back") != std::string::npos, ex.what());
    }
    CHECK_FALSE(fs::exists(state));
    CHECK(e.StateChecksum() == before);
    REQUIRE(e.RunFrames(2));
}

// ---- Pass condition 6 ----

TEST_CASE("ckpt: frames after PowerCycle equal a fresh process's through the DOS boot")
{
    // The reference comes from a fresh AppleWinHarness.exe --serve --headless (a fresh Start with the same disk), read
    // every 10 frames with get_frame BGRA32; one process gives all 72 samples (the CLI would need one per sample).
    constexpr int kSamples = 72;   // 720 frames
    std::vector<std::string> reference;
    {
        Server s = StartServer(L"--serve --headless");
        auto op = Connect(s, "operator");
        Result(*op, "create_session", {{"run_mode", "stepped"}, {"disks", json::array({{{"slot", 6}, {"drive", 1},
                                                                                       {"path", DosMasterImage().string()}}})}});
        for (int i = 0; i < kSamples; ++i) {
            Result(*op, "step_frames", {{"frames", 10}});
            reference.push_back(DataHash(Result(*op, "get_frame", {{"format", "BGRA32"}})));
        }
        Result(*op, "close_session");
    }

    auto& e = StartedEmulator();
    ScratchDir dir("powercycle");
    WorkingCopy copy(DosMasterImage(), dir.path / "disk");
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle guard{e};
    // Leave the machine mid-boot and mid-frame first (a realtime run ends at an arbitrary cycle, not on a frame
    // boundary): PowerCycle must not depend on it. RunFrames alone would keep the video scanner frame-aligned, and the
    // test would then pass even with a stale scanner phase.
    REQUIRE(e.RunFrames(37));
    REQUIRE(e.RunRealtime(std::chrono::milliseconds(30)));
    e.PowerCycle();
    for (int i = 0; i < kSamples; ++i) {
        REQUIRE(e.RunFrames(10));
        const std::string got = StringHash(Base64Encode(RawFrame(e.Frame())));
        INFO("frame " << (i + 1) * 10);
        CHECK(got == reference[size_t(i)]);
        if (got != reference[size_t(i)]) break;   // the first difference is the useful one
    }
}

// ---- Checksum and RunCycles ----

TEST_CASE("ckpt: StateChecksum is stable and changes with one byte of RAM")
{
    auto& e = StartedEmulator();
    EjectAndPowerCycle guard{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(30));
    const std::string a = e.StateChecksum();
    CHECK(a.size() == 64);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(e.StateChecksum() == a);
    (void)e.Frame();
    (void)e.Disks();
    CHECK(e.StateChecksum() == a);   // reading frames and disks is not machine state

    uint8_t byte = 0;
    CopyBytesFromMemoryPage(&byte, 0x0300, 1);
    WriteByteToMemory(0x0300, uint8_t(byte ^ 0x01));
    const std::string b = e.StateChecksum();
    CHECK(b != a);
    WriteByteToMemory(0x0300, byte);
    CHECK(e.StateChecksum() == a);
}

TEST_CASE("ckpt: RunCycles runs exactly the cycles asked for, give or take the last instruction")
{
    auto& e = StartedEmulator();
    EjectAndPowerCycle guard{e};
    e.PowerCycle();
    REQUIRE(e.RunFrames(5));
    for (const uint64_t n : {uint64_t(1), uint64_t(7), uint64_t(12345), kCyclesPerFrame - 1, 3 * kCyclesPerFrame + 7,
                             uint64_t(1000000)}) {
        INFO("cycles " << n);
        const uint64_t before = e.EmulatedCycles();
        e.RunCycles(n);
        const uint64_t ran = e.EmulatedCycles() - before;
        CHECK(ran >= n);
        CHECK(ran <= n + kMaxInstructionCycles);
    }
}

TEST_CASE("ckpt: Disks lists the inserted working copies; EjectAll and ReinsertAll round-trip")
{
    auto& e = StartedEmulator();
    EjectAndPowerCycle guard{e};
    e.PowerCycle();
    CHECK(e.Disks().empty());

    ScratchDir dir("disks");
    WorkingCopy copy(DosMasterImage(), dir.path / "disk");
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle ejectFirst{e};   // destroyed before copy
    std::vector<DiskInfo> disks = e.Disks();
    REQUIRE(disks.size() == 1);
    CHECK(disks[0].drive == 0);
    CHECK(fs::equivalent(disks[0].workingPath, copy.Path()));
    CHECK_FALSE(disks[0].writeProtected);
    CHECK(disks[0].format == "DO");

    e.EjectAll();
    CHECK(e.Disks().empty());
    e.ReinsertAll();
    disks = e.Disks();
    REQUIRE(disks.size() == 1);
    CHECK(fs::equivalent(disks[0].workingPath, copy.Path()));
}

// ---- Windowed (pass conditions 3 and 5), launched as servers ----

TEST_SUITE("windowed")
{
    TEST_CASE("ckpt: windowed and headless servers replay the checkpoint script identically")
    {
        const auto headless = CheckpointReplayOverPipe(L"--serve --headless");
        const auto windowed = CheckpointReplayOverPipe(L"--serve");
        CHECK(headless.first == headless.second);
        CHECK(windowed.first == windowed.second);
        REQUIRE(windowed.first.records.size() == headless.first.records.size());
        for (size_t i = 0; i < headless.first.records.size(); ++i) {
            INFO("step " << i);
            CHECK(windowed.first.records[i] == headless.first.records[i]);
        }
        CHECK(windowed.first.audioHash == headless.first.audioHash);
    }

    TEST_CASE("ckpt: SSI263 speech replays identically in both display modes")
    {
        const SpeechRuns headless = SpeechOverPipe(L"--serve --headless");
        const SpeechRuns windowed = SpeechOverPipe(L"--serve");
        for (const SpeechRuns* r : {&headless, &windowed}) {
            CHECK_MESSAGE(!r->first.audioSilent, "the speech program left no audio");
            CHECK(r->second == r->first);
            CHECK_MESSAGE(r->muted.audioSilent, "the muted program is not silent");
        }
        REQUIRE(windowed.first.records.size() == headless.first.records.size());
        for (size_t i = 0; i < headless.first.records.size(); ++i) {
            INFO("step " << i);
            CHECK(windowed.first.records[i] == headless.first.records[i]);
        }
        CHECK(windowed.first.audioHash == headless.first.audioHash);
    }
}
