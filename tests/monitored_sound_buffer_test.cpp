// MonitoredSoundBuffer unit tests (spec: docs/specs/m2.md, "src/host/monitored_sound_buffer.h").
#include "doctest/doctest.h"
#include "host/monitored_sound_buffer.h"
#include "support/fake_sound_buffer.h"
#include <memory>

using harness::MonitoredSoundBuffer;
using harness::testsupport::FakeSoundBuffer;

namespace {

constexpr uint32_t kBytes = 4096;

struct Rig {
    std::shared_ptr<FakeSoundBuffer> fake = std::make_shared<FakeSoundBuffer>(kBytes);
    MonitoredSoundBuffer buf{fake, kBytes};
};

// One committed write as AppleWin does it: Lock(offset, bytes), then Unlock with the parts Lock returned.
void Commit(SoundBuffer& b, DWORD offset, DWORD bytes, DWORD flags = 0)
{
    LPVOID p1 = nullptr, p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    REQUIRE(b.Lock(offset, bytes, &p1, &n1, &p2, &n2, flags) == S_OK);
    REQUIRE(b.Unlock(p1, n1, p2, n2) == S_OK);
}

void Poll(SoundBuffer& b)
{
    DWORD play = 0, write = 0;
    REQUIRE(b.GetCurrentPosition(&play, &write) == S_OK);
}

}  // namespace

TEST_CASE("sound: calls are forwarded to the inner buffer")
{
    Rig r;
    CHECK(r.buf.SetCurrentPosition(123) == S_OK);
    CHECK(r.fake->setCurrentPositionCalls == 1);
    CHECK(r.fake->lastNewPosition == 123);

    r.fake->playCursor = 10;
    r.fake->writeCursor = 20;
    DWORD play = 0, write = 0;
    CHECK(r.buf.GetCurrentPosition(&play, &write) == S_OK);
    CHECK(r.fake->getCurrentPositionCalls == 1);
    CHECK(play == 10);
    CHECK(write == 20);

    LPVOID p1 = nullptr, p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    CHECK(r.buf.Lock(4000, 200, &p1, &n1, &p2, &n2, 0) == S_OK);
    CHECK(r.fake->lockCalls == 1);
    CHECK(r.fake->lastLockOffset == 4000);
    CHECK(r.fake->lastLockBytes == 200);
    CHECK(p1 == r.fake->data.data() + 4000);
    CHECK(n1 == 96);
    CHECK(p2 == r.fake->data.data());
    CHECK(n2 == 104);
    CHECK(r.buf.Unlock(p1, n1, p2, n2) == S_OK);
    CHECK(r.fake->unlockCalls == 1);
    CHECK(r.fake->lastUnlockBytes1 == 96);
    CHECK(r.fake->lastUnlockBytes2 == 104);

    CHECK(r.buf.Stop() == S_OK);
    CHECK(r.fake->stopCalls == 1);
    CHECK(r.buf.Play(0, 0, DSBPLAY_LOOPING) == S_OK);
    CHECK(r.fake->playCalls == 1);
    CHECK(r.fake->lastPlayFlags == DWORD(DSBPLAY_LOOPING));

    CHECK(r.buf.SetVolume(-700) == S_OK);
    CHECK(r.fake->volume == -700);
    LONG v = 0;
    CHECK(r.buf.GetVolume(&v) == S_OK);
    CHECK(v == -700);

    r.fake->status = DSBSTATUS_PLAYING;
    DWORD status = 0;
    CHECK(r.buf.GetStatus(&status) == S_OK);
    CHECK(r.fake->getStatusCalls == 1);
    CHECK(status == DWORD(DSBSTATUS_PLAYING));

    CHECK(r.buf.Restore() == S_OK);
    CHECK(r.fake->restoreCalls == 1);
}

TEST_CASE("sound: mute sets DSBVOLUME_MIN on the inner buffer")
{
    Rig r;
    r.buf.SetMuted(true);
    REQUIRE(!r.fake->setVolumes.empty());
    CHECK(r.fake->setVolumes.back() == DSBVOLUME_MIN);
    CHECK(r.fake->volume == DSBVOLUME_MIN);
}

TEST_CASE("sound: SetVolume while muted is recorded, not forwarded")
{
    Rig r;
    r.buf.SetMuted(true);
    const size_t before = r.fake->setVolumes.size();
    CHECK(r.buf.SetVolume(-500) == S_OK);
    CHECK(r.fake->setVolumes.size() == before);
    CHECK(r.fake->volume == DSBVOLUME_MIN);
    LONG v = 0;
    CHECK(r.buf.GetVolume(&v) == S_OK);
    CHECK(v == -500);
}

TEST_CASE("sound: unmute restores the recorded volume")
{
    Rig r;
    r.buf.SetMuted(true);
    r.buf.SetVolume(-500);
    r.buf.SetMuted(false);
    REQUIRE(!r.fake->setVolumes.empty());
    CHECK(r.fake->setVolumes.back() == -500);
    CHECK(r.fake->volume == -500);
}

TEST_CASE("sound: the recorded volume starts as the inner volume at construction")
{
    auto fake = std::make_shared<FakeSoundBuffer>(kBytes);
    fake->volume = -300;
    MonitoredSoundBuffer buf(fake, kBytes);
    buf.SetMuted(true);
    buf.SetMuted(false);
    CHECK(fake->volume == -300);
}

TEST_CASE("sound: commit ending between play and write cursors is an underrun")
{
    Rig r;
    r.fake->playCursor = 100;
    r.fake->writeCursor = 200;
    Commit(r.buf, 0, 150);   // end 150 lies in (100, 200)
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 1);
}

TEST_CASE("sound: commit ending past the write cursor is not an underrun")
{
    Rig r;
    r.fake->playCursor = 100;
    r.fake->writeCursor = 200;
    Commit(r.buf, 0, 250);   // end 250 lies outside (100, 200)
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 0);
}

TEST_CASE("sound: wrap-around unsafe zone counts an underrun")
{
    Rig r;
    r.fake->playCursor = 4000;
    r.fake->writeCursor = 100;
    Commit(r.buf, 4046, 100);   // end (4046 + 100) mod 4096 = 50, inside the zone |xxW----Pxx|
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 1);
}

TEST_CASE("sound: an underrun is counted once until the next commit")
{
    Rig r;
    r.fake->playCursor = 100;
    r.fake->writeCursor = 200;
    Commit(r.buf, 0, 150);
    Poll(r.buf);
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 1);
    Commit(r.buf, 0, 150);
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 2);
}

TEST_CASE("sound: a whole-buffer commit stops tracking")
{
    Rig r;
    r.fake->playCursor = 4000;
    r.fake->writeCursor = 100;
    Commit(r.buf, 0, 150);   // partial commit: tracking on, end 150 is safe
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 0);
    // AppleWin's DSZeroVoiceWritableBuffer: Lock(0, 0, DSBLOCK_ENTIREBUFFER) and Unlock of all 4096 bytes.
    // The end (0) would lie in the unsafe zone if it were tracked.
    Commit(r.buf, 0, 0, DSBLOCK_ENTIREBUFFER);
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 0);
    // A plain 4096-byte commit covers the whole buffer too.
    Commit(r.buf, 0, 150);
    Commit(r.buf, 50, kBytes);
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 0);
}

TEST_CASE("sound: no underruns counted while muted, or after unmuting until the next commit")
{
    Rig r;
    r.fake->playCursor = 100;
    r.fake->writeCursor = 200;
    r.buf.SetMuted(true);
    Commit(r.buf, 0, 150);
    Poll(r.buf);
    CHECK(r.buf.Underruns() == 0);

    Rig r2;
    r2.fake->playCursor = 100;
    r2.fake->writeCursor = 200;
    Commit(r2.buf, 0, 150);   // would be an underrun at the next poll ...
    r2.buf.SetMuted(true);
    r2.buf.SetMuted(false);   // ... but unmuting stops tracking until the next commit
    Poll(r2.buf);
    CHECK(r2.buf.Underruns() == 0);
    Commit(r2.buf, 0, 150);
    Poll(r2.buf);
    CHECK(r2.buf.Underruns() == 1);
}
