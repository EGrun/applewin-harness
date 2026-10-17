// VirtualSoundBuffer tests (spec: docs/specs/m3.md, D14 "Virtual buffers" and "Capture"; plan: M3 Part B Task 1,
// "vsound:" and Review Focus 1 and 2).
#include "doctest/doctest.h"
#include "host/virtual_sound.h"
#include "host/emulator.h"
#include "core/audio_capture.h"
#include "core/fft.h"
#include "core/working_copy.h"
#include "support/audio_signals.h"
#include "support/emulator_fixture.h"
#include "support/paths.h"

#pragma warning(push, 3)
#include "Common.h"
#include "Core.h"
#include "SoundCore.h"
#pragma warning(pop)

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using harness::AudioCapture;
using harness::VirtualSoundBuffer;
using namespace harness;
using namespace harness::testsupport;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kBytes = 4096;
uint64_t F2C(uint64_t frame) { return AudioCapture::FrameToCycle(frame); }

struct Rig {
    AudioCapture capture;
    uint64_t now = 0;   // the buffer's clock
    std::unique_ptr<VirtualSoundBuffer> buf;

    explicit Rig(int channels = 1, uint32_t bytes = kBytes)
        : buf(std::make_unique<VirtualSoundBuffer>(bytes, 44100, channels, 7, &capture, [this] { return now; }))
    {
    }
};

// One committed write as AppleWin does it: Lock(offset, bytes), fill both parts, Unlock with what Lock returned.
// The samples written are `first` for the part before the ring's end and `second` for the part after it.
void CommitSplit(SoundBuffer& b, DWORD offset, DWORD bytes, int16_t first, int16_t second, DWORD flags = 0)
{
    LPVOID p1 = nullptr, p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    REQUIRE(b.Lock(offset, bytes, &p1, &n1, &p2, &n2, flags) == S_OK);
    REQUIRE(p1 != nullptr);
    for (DWORD i = 0; i < n1 / 2; ++i) static_cast<int16_t*>(p1)[i] = first;
    if (p2)
        for (DWORD i = 0; i < n2 / 2; ++i) static_cast<int16_t*>(p2)[i] = second;
    REQUIRE(b.Unlock(p1, n1, p2, n2) == S_OK);
}
void Commit(SoundBuffer& b, DWORD offset, DWORD bytes, int16_t value, DWORD flags = 0)
{
    CommitSplit(b, offset, bytes, value, value, flags);
}

void Cursors(SoundBuffer& b, DWORD& play, DWORD& write)
{
    REQUIRE(b.GetCurrentPosition(&play, &write) == S_OK);
}

void CheckFrames(const std::vector<int16_t>& stereo, uint64_t from, uint64_t to, int value)
{
    REQUIRE(stereo.size() >= 2 * to);
    uint64_t bad = 0, firstBad = 0;
    for (uint64_t k = from; k < to; ++k)
        if (stereo[2 * k] != value || stereo[2 * k + 1] != value)
            if (bad++ == 0) firstBad = k;
    INFO("frames [" << from << ", " << to << ") expected " << value << "; first mismatch at frame " << firstBad);
    CHECK(bad == 0);
}

}  // namespace

// ---- Cursors (Review Focus 1) ----

TEST_CASE("vsound: the cursors keep AppleWin's fill check neutral")
{
    for (int channels : {1, 2}) {
        CAPTURE(channels);
        Rig r(channels);
        const DWORD frameBytes = DWORD(2 * channels);
        DWORD play = 0, write = 0;
        Cursors(*r.buf, play, write);
        CHECK(play == kBytes * 5 / 8);                       // commit end 0, minus 3/8
        CHECK(write == 0);                                   // the commit end (ruling B6)
        CHECK(write == (play + kBytes / 8 * 3) % kBytes);

        // AppleWin's speaker loop (Spkr_SubmitWaveBuffer): the first offset is play + 3/8, then each commit follows the
        // last. Uneven commit sizes, wrapping the ring many times.
        uint32_t offset = (play + kBytes / 8 * 3) % kBytes;
        CHECK(offset == 0);
        int alignFired = 0, outOfBand = 0, cursorWrong = 0;
        for (int i = 0; i < 300; ++i) {
            const DWORD bytes = frameBytes * DWORD(1 + (i * 37) % 300);
            r.now += 23 * bytes / frameBytes;
            Commit(*r.buf, offset, bytes, int16_t(i));
            offset = (offset + bytes) % kBytes;

            Cursors(*r.buf, play, write);
            if (play != (offset + kBytes - kBytes / 8 * 3) % kBytes || write != offset)
                ++cursorWrong;
            uint32_t check = offset;
            if (SoundCore_ValidateAndAlignWriteOffset(check, play, write)) ++alignFired;
            int remaining = int(offset) - int(play);
            if (remaining < 0) remaining += int(kBytes);
            if (remaining < int(kBytes / 4) || remaining > int(kBytes / 2)) ++outOfBand;   // error would change
        }
        CHECK(cursorWrong == 0);
        CHECK(alignFired == 0);
        CHECK(outOfBand == 0);
    }
}

// Ruling B6: the Mockingboard's (and SSI263's) first update starts writing at the write cursor
// (MockingboardCardManager::GenerateAllSoundData: m_byteOffset = dwCurrentWriteCursor), then measures the bytes queued
// as offset - play. With write = commit end that is exactly 3/8 of the buffer, so nNumSamplesError stays 0.
TEST_CASE("vsound: a voice that starts at the write cursor sees exactly 3/8 queued")
{
    for (int channels : {1, 2}) {
        CAPTURE(channels);
        Rig r(channels, 8192);
        const DWORD frameBytes = DWORD(2 * channels);
        Commit(*r.buf, 0, 8192, 0);   // DSZeroVoiceBuffer: a whole-buffer commit first
        DWORD play = 0, write = 0;
        uint32_t offset = 0;
        int wrong = 0;
        for (int i = 0; i < 50; ++i) {
            Cursors(*r.buf, play, write);
            if (i == 0)
                offset = write;   // first call: start at the write cursor
            uint32_t queued = (offset + 8192 - play) % 8192;
            if (queued != 8192 / 8 * 3) ++wrong;
            uint32_t check = offset;
            if (SoundCore_ValidateAndAlignWriteOffset(check, play, write)) ++wrong;
            const DWORD bytes = frameBytes * DWORD(17 + (i * 29) % 400);
            r.now += 23 * bytes / frameBytes;
            Commit(*r.buf, offset, bytes, int16_t(i));
            offset = (offset + bytes) % 8192;
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE("vsound: Unlock rejects a region the last Lock did not return, and changes nothing")
{
    Rig r(1);
    DWORD play0 = 0, write0 = 0, play = 0, write = 0;
    Cursors(*r.buf, play0, write0);
    LPVOID p1 = nullptr, p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    REQUIRE(r.buf->Lock(4000, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    CHECK(r.buf->Unlock(static_cast<uint8_t*>(p1) - 2, n1, p2, n2) == DSERR_INVALIDPARAM);   // wrong pointer
    REQUIRE(r.buf->Lock(4000, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    CHECK(r.buf->Unlock(p1, n1 + 2, p2, n2) == DSERR_INVALIDPARAM);                         // longer than locked
    REQUIRE(r.buf->Lock(4000, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    CHECK(r.buf->Unlock(p1, n1, nullptr, n2) == DSERR_INVALIDPARAM);                       // second part missing
    REQUIRE(r.buf->Lock(100, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    REQUIRE(r.buf->Unlock(p1, n1, p2, n2) == S_OK);
    CHECK(r.buf->Unlock(p1, n1, p2, n2) == DSERR_INVALIDPARAM);                            // no Lock outstanding
    Cursors(*r.buf, play, write);
    CHECK(write == 300);   // only the valid commit moved the commit end
}

// ---- Capture ----

TEST_CASE("vsound: commits are captured in order at the clock's cycle")
{
    Rig r(1);
    r.now = F2C(441);
    Commit(*r.buf, 0, 882, 1000);               // 441 mono frames
    CHECK(r.capture.NewestCycle() == F2C(441));
    r.now = F2C(882);
    Commit(*r.buf, 882, 882, -500);
    CHECK(r.capture.NewestCycle() == F2C(882));
    // A commit that wraps the ring's end: 50 frames of 7, then 50 frames of 9, over timeline frames [882, 982).
    r.now = F2C(982);
    CommitSplit(*r.buf, kBytes - 100, 200, 7, 9);
    CHECK(r.capture.NewestCycle() == F2C(982));

    const auto all = r.capture.Read(0, r.capture.NewestCycle());
    REQUIRE(all.size() == 2 * 982);
    CheckFrames(all, 2, 439, 1000);
    CheckFrames(all, 443, 880, -500);
    CheckFrames(all, 884, 930, 7);
    CheckFrames(all, 934, 980, 9);
}

TEST_CASE("vsound: a whole-buffer commit is not captured and resets the voice")
{
    Rig r(1);
    DWORD play0 = 0, write0 = 0, play = 0, write = 0;
    Cursors(*r.buf, play0, write0);

    r.now = F2C(441);
    Commit(*r.buf, 0, 0, 5000, DSBLOCK_ENTIREBUFFER);   // DSZeroVoiceBuffer's lock
    Commit(*r.buf, 0, kBytes, 5000);                    // DSZeroVoiceWritableBuffer's lock
    CHECK(r.capture.NewestCycle() == 0);
    Cursors(*r.buf, play, write);
    CHECK(play == play0);   // the commit end didn't move
    CHECK(write == write0);

    // After a partial commit, a whole-buffer commit makes the next commit a first commit again.
    Commit(*r.buf, 0, 882, 100);   // timeline frames [0, 441)
    Commit(*r.buf, 0, kBytes, 0);
    r.now = F2C(2441);
    Commit(*r.buf, 882, 882, 200);   // first commit: [2000, 2441), not stretched over [441, 2441)
    const auto all = r.capture.Read(0, r.capture.NewestCycle());
    REQUIRE(all.size() == 2 * 2441);
    CheckFrames(all, 2, 439, 100);
    CheckFrames(all, 443, 1998, 0);
    CheckFrames(all, 2002, 2439, 200);
}

TEST_CASE("vsound: destroying a buffer resets its voice")
{
    AudioCapture capture;
    uint64_t now = F2C(441);
    {
        VirtualSoundBuffer a(kBytes, 44100, 1, 3, &capture, [&] { return now; });
        Commit(a, 0, 882, 100);
    }
    now = F2C(2441);
    VirtualSoundBuffer b(kBytes, 44100, 1, 3, &capture, [&] { return now; });
    Commit(b, 0, 882, 200);
    const auto all = capture.Read(0, capture.NewestCycle());
    REQUIRE(all.size() == 2 * 2441);
    CheckFrames(all, 443, 1998, 0);
    CheckFrames(all, 2002, 2439, 200);
}

TEST_CASE("vsound: a null capture is allowed")
{
    VirtualSoundBuffer b(kBytes, 44100, 2, 0, nullptr, [] { return uint64_t(1000); });
    Commit(b, 0, 400, 1);
    DWORD play = 0, write = 0;
    Cursors(b, play, write);
    CHECK(play == (400 + kBytes - kBytes / 8 * 3) % kBytes);
}

// ---- DirectSound-like behaviour ----

TEST_CASE("vsound: Lock splits at the end of the ring and rejects regions outside it")
{
    Rig r(1);
    LPVOID p1 = nullptr, p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    REQUIRE(r.buf->Lock(4000, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    CHECK(n1 == 96);
    CHECK(n2 == 104);
    REQUIRE(p2 != nullptr);
    CHECK(static_cast<uint8_t*>(p1) - static_cast<uint8_t*>(p2) == 4000);
    REQUIRE(r.buf->Unlock(p1, n1, p2, n2) == S_OK);

    REQUIRE(r.buf->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER) == S_OK);
    CHECK(n1 == kBytes);
    CHECK(n2 == 0);
    REQUIRE(r.buf->Unlock(p1, n1, p2, n2) == S_OK);

    CHECK(r.buf->Lock(kBytes, 2, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM);
    CHECK(r.buf->Lock(0, kBytes + 2, &p1, &n1, &p2, &n2, 0) == DSERR_INVALIDPARAM);
}

TEST_CASE("vsound: status, volume and the methods with no effect")
{
    Rig r(2);
    DWORD status = 123;
    REQUIRE(r.buf->GetStatus(&status) == S_OK);
    CHECK(status == 0);
    REQUIRE(r.buf->Play(0, 0, DSBPLAY_LOOPING) == S_OK);
    REQUIRE(r.buf->GetStatus(&status) == S_OK);
    CHECK(status == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING));
    REQUIRE(r.buf->Stop() == S_OK);
    REQUIRE(r.buf->GetStatus(&status) == S_OK);
    CHECK(status == 0);
    REQUIRE(r.buf->Play(0, 0, 0) == S_OK);
    REQUIRE(r.buf->GetStatus(&status) == S_OK);
    CHECK(status == DSBSTATUS_PLAYING);

    LONG v = 1;
    REQUIRE(r.buf->GetVolume(&v) == S_OK);
    CHECK(v == DSBVOLUME_MAX);
    REQUIRE(r.buf->SetVolume(-500) == S_OK);
    REQUIRE(r.buf->GetVolume(&v) == S_OK);
    CHECK(v == -500);

    DWORD play0 = 0, write0 = 0, play = 0, write = 0;
    Cursors(*r.buf, play0, write0);
    CHECK(r.buf->SetCurrentPosition(100) == S_OK);
    CHECK(r.buf->Restore() == S_OK);
    Cursors(*r.buf, play, write);
    CHECK(play == play0);
    CHECK(write == write0);
}

// ---- In process: AppleWin driving virtual buffers (needs Task 2's wiring) ----

namespace {

// Ejects the disk and power-cycles, so the tests that follow start from the M1 state. Never throws.
struct EjectAndPowerCycle {
    Emulator& e;
    ~EjectAndPowerCycle()
    {
        try { e.EjectDisk(0); } catch (...) {}
        try { e.PowerCycle(); } catch (...) {}
    }
};

// A ~1,004 Hz square on the speaker: the monitor program at $300
//   LDA $C030 / LDY #$64 / DEY / BNE -3 / JMP $300   = 508 cycles per toggle (1,020,484 / 1,016 = 1,004.4 Hz).
constexpr double kToneHz = CLK_6502_NTSC / (2.0 * 508.0);

void BootDosAndStartTone(Emulator& e)
{
    e.PowerCycle();
    REQUIRE(e.RunFrames(720));
    REQUIRE(e.TypeText("CALL -151\r"));
    REQUIRE(e.RunFrames(30));
    REQUIRE(e.TypeText("300:AD 30 C0 A0 64 88 D0 FD 4C 00 03\r"));
    REQUIRE(e.RunFrames(30));
    REQUIRE(e.TypeText("300G\r"));
    REQUIRE(e.RunFrames(10));
}

// DominantHz and AC RMS of the newest 16,384 frames of the emulator's capture.
std::pair<double, double> MeasureNewest(const Emulator& e)
{
    const AudioCapture& a = e.Audio();
    const uint64_t newest = a.NewestCycle();
    REQUIRE(newest > F2C(16384));
    const std::vector<int16_t> stereo = a.Read(newest - F2C(16384), newest);
    const std::vector<float> mono = MonoMix(stereo);
    return {DominantHz(mono, 44100.0), AcRmsDb(mono)};
}

}  // namespace

TEST_CASE("vsound: AppleWin's speaker feedback stays 0 over a stepped tone (Review Focus 1)")
{
    auto& e = StartedEmulator();
    const auto work = std::filesystem::temp_directory_path() / ("awh-vsound-test-" + std::to_string(GetCurrentProcessId()));
    WorkingCopy copy(DosMasterImage(), work);
    e.InsertDisk(0, copy.Path());
    EjectAndPowerCycle restore{e};   // declared after copy: ejects before the copy is deleted

    BootDosAndStartTone(e);
    int nonZero = 0, firstNonZeroFrame = -1, firstValue = 0;
    for (int i = 0; i < 600; ++i) {
        REQUIRE(e.RunFrames(1));
        if (g_nCpuCyclesFeedback != 0 && nonZero++ == 0) {
            firstNonZeroFrame = i;
            firstValue = g_nCpuCyclesFeedback;
        }
    }
    INFO("first non-zero g_nCpuCyclesFeedback: " << firstValue << " after frame " << firstNonZeroFrame);
    CHECK(nonZero == 0);

    // The speaker really ran through a virtual buffer: the capture holds the tone, up to (about) now.
    const AudioCapture& a = e.Audio();
    CHECK(a.NewestCycle() <= e.EmulatedCycles());
    CHECK(a.NewestCycle() + 2 * 17030 >= e.EmulatedCycles());
    const auto [hz, db] = MeasureNewest(e);
    CHECK(db > -30.0);
    CHECK(hz == doctest::Approx(kToneHz).epsilon(0.02));
}

TEST_SUITE("realtime")
{
    // Review Focus 4 (M3 Part C): the clock changes through Emulator::SetMachineSpeed.
    TEST_CASE("vsound: a tone keeps its pitch when the clock speed changes")
    {
        auto& e = StartedEmulator();
        const auto work =
            std::filesystem::temp_directory_path() / ("awh-vsound-speed-" + std::to_string(GetCurrentProcessId()));
        WorkingCopy copy(DosMasterImage(), work);
        e.InsertDisk(0, copy.Path());
        EjectAndPowerCycle restore{e};
        struct RestoreSpeed {
            ~RestoreSpeed()
            {
                g_dwSpeed = SPEED_NORMAL;
                SetCurrentCLK6502();
            }
        } restoreSpeed;

        BootDosAndStartTone(e);
        double hz[2] = {};
        const uint32_t speeds[2] = {10, 20};
        for (int i = 0; i < 2; ++i) {
            e.SetMachineSpeed(speeds[i]);
            REQUIRE(e.RunRealtime(1500ms));
            const auto [h, db] = MeasureNewest(e);
            CAPTURE(speeds[i]);
            CHECK(db > -30.0);
            hz[i] = h;
        }
        CHECK(hz[0] == doctest::Approx(kToneHz).epsilon(0.02));
        CHECK(hz[1] == doctest::Approx(hz[0]).epsilon(0.02));
    }
}
