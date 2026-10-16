# M3 Part B — Agent Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents hear the Apple. `get_audio` and `subscribe_audio` work over the pipe, and capture is deterministic in every mode. AppleWin stays unpatched, because the harness supplies virtual sound buffers (D14). Windowed realtime mode still plays sound with 0 underruns.

**Architecture:**
- `virtual_sound.cpp` replaces AppleWin's `Windows/DXSoundBuffer.cpp` translation unit.
- `HarnessFrame` always hands AppleWin a `VirtualSoundBuffer`.
- Every commit feeds `AudioCapture`, which places the samples on a 44,100 Hz stereo timeline by emulated cycle.
- `AudioOutput` (windowed only) streams that timeline to a harness-owned DirectSound buffer and supplies the realtime burst feedback.
- `LevelDetector` turns the timeline into `sound_started` and `sound_stopped` events.
- **Tests first (Task 1).**

**Tech Stack:** C++20, MSVC v143, DirectSound (delay-loaded, harness-owned), doctest, Python 3 standard library.

**Spec:** `docs/specs/m3.md`. The binding parts are D14 and Part B, plus Part A's server, which this plan extends.

## Global Constraints
- **AppleWin.** No file under `external/AppleWin` may be modified. `build/AppleWinCore.vcxproj` stops compiling `source\Windows\DXSoundBuffer.cpp`. That is the only change to the AppleWinCore source list.
- **Timeline.** 44,100 Hz, 2 channels, int16. The ring buffer holds 30 s (1,323,000 frames). Output frame *k* is at emulated cycle `k × CLK_6502_NTSC / 44100`.
- **Placing commits.** A commit of *N* sample frames covers the emulated cycles from the end of the voice's previous commit up to `g_nCumulativeCycles` at this commit. The very first commit of a voice covers the *N* × cycles-per-sample before it. The *N* frames are spread linearly over the timeline frames in that interval, and voices mix by saturating addition. A voice that commits more than 100 ms later than its *N* samples need (the gap exceeds *N* × CLK_6502_NTSC/44100 cycles by over 0.1 × CLK_6502_NTSC) was paused: the gap is silence, and the commit is placed like a first commit.
  - **Why:** AppleWin's speaker makes one sample per `(UINT)(clk/44100)` = 23 cycles, which is about 44,369 Hz. A fixed rate would drift.
- **`get_audio`.** 1–30,000 ms, formats `WAV` and `PCM16`. The WAV header is the canonical 44 bytes. `-32009` is returned for ranges older than the ring.
- **`subscribe_audio`.** 10 ms blocks of 441 frames. Level is AC RMS (mean subtracted) in dBFS of the mono mix (L+R)/2, with full scale 32,768. Defaults are -50 dB and 100 ms. The FFT is Hann-windowed over 16,384 points with parabolic peak refinement.
- **Determinism.** Captured bytes over a cycle range are identical across repeated runs and across display modes in stepped mode.
- **Regressions.** These must stay green: the M2 golden, headless mode not loading `dsound.dll`, the M2 `audio` suite (0 underruns, now counted by `AudioOutput`), and all of Part A's tests.
- **Test suites.** `realtime`, `windowed` and `audio`, as before. CI runs the Python tests headless.

## Review Focus
1. **AppleWin's buffer correction drifts.** If the virtual cursors are slightly wrong, Speaker's and the Mockingboard's error correction adds or drops samples, and determinism breaks. Expected: `g_nCpuCyclesFeedback` is always 0, and the Mockingboard's `m_numSamplesError` stays 0. *(Task 1 test: run 600 stepped frames with a speaker tone and assert `g_nCpuCyclesFeedback == 0` after every step.)*
2. **Speed changes the sample rate in the middle of capture.** Part C changes `g_fCurrentCLK6502`. Expected: the timeline stays in emulated time, because placement uses the interval between commits. *(Task 1 test: a 1 kHz tone captured at speed 10 and then speed 20 (realtime) gives the same `dominant_hz` within ±2%. It goes in suite `realtime` and is skipped until Part C. The plan's Part C reuses it.)*
3. **A silent DC level counts as sound.** The speaker idles at a non-zero DC level. Expected: no `sound_started` while idle. *(Task 1 test: boot DOS and idle for 5 s; no events.)*
4. **`get_audio` at the ring's edges.** Expected:
   - a `from_cycle` just older than the ring returns `-32009`;
   - a range past the newest sample is cut short;
   - `duration_ms` 30,001 returns `-32602`.

   *(Task 1 Python tests.)*
5. **Playback stops working after the rework.** Expected: windowed realtime still plays sound, and the 60-second tone test reports 0 underruns. *(Existing `audio` suite, plus a manual bell check at the end.)*

---

### Task 1: Interfaces, stubs, and the full failing Part B test suite
**Files:**
- **Create:**
  - `src/host/virtual_sound.{h,cpp}` (`VirtualSoundBuffer`, plus the replacement DSInit/DSUninit/DSAvailable/GetDefaultAudioDeviceName/`DXSoundBuffer::create`);
  - `src/core/audio_capture.{h,cpp}`, `src/core/level_detector.{h,cpp}`, `src/core/wav.{h,cpp}`, `src/core/fft.{h,cpp}`;
  - `src/host/audio_output.{h,cpp}`;
  - tests `tests/audio_capture_test.cpp`, `tests/level_detector_test.cpp`, `tests/virtual_sound_test.cpp`, `tests/fft_test.cpp`.
- **Modify:** `tests/python/test_pipe.py` (add an audio class), `tests/cli_test.cpp` (only if the M2 audio test needs its expectations restated, which it shouldn't), and the vcxproj files.
- Leave the AppleWinCore source list unchanged in this task, so the stubs don't break linking.

**Interfaces (Produces):**
- **`AudioCapture`:**
  - `void Commit(int voiceId, std::span<const int16_t> interleaved, int channels, uint64_t endCycle)`
  - `void ResetVoice(int voiceId)`
  - `uint64_t NewestCycle() const`, `uint64_t OldestCycle() const`
  - `std::vector<int16_t> Read(uint64_t fromCycle, uint64_t toCycle) const`, which returns interleaved stereo and throws `std::out_of_range` when `fromCycle < OldestCycle()`
  - `void Clear()`, called on PowerCycle
  - `static uint64_t FrameToCycle(uint64_t)` and `static uint64_t CycleToFrame(uint64_t)`
- **`LevelDetector`:**
  - `LevelDetector(double thresholdDb, uint32_t minSilenceMs)`
  - `std::vector<SoundEvent> Feed(const AudioCapture&, uint64_t uptoCycle)`
  - `SoundEvent { enum { Started, Stopped } kind; uint64_t startCycle, endCycle; double peakDb, dominantHz; }`
- **Helpers:**
  - `std::vector<uint8_t> EncodeWav(std::span<const int16_t>, uint32_t rate, uint16_t channels)`
  - `double DominantHz(std::span<const float> mono, double rate)`
- **`VirtualSoundBuffer : SoundBuffer`:** `VirtualSoundBuffer(uint32_t bytes, uint32_t rate, int channels, int voiceId, AudioCapture*)`.
- **`AudioOutput`:**
  - `static std::unique_ptr<AudioOutput> Open(HWND)`, which returns null if there is no device
  - `void Pump(const AudioCapture&, uint64_t uptoCycle)`
  - `int FeedbackCycles() const`
  - `void SetPlaying(bool)`
  - `uint64_t Underruns() const`

- [ ] **Step 1: Write the tests.**
  - **`capture:`**
    - Two voices at different rates (23 cycles per sample, and 44,100 Hz exactly) mixed with saturation.
    - Interval placement over a sequence of uneven commits.
    - Ring wrap at 30 s.
    - `Read` past the ring throws.
    - `Clear` empties the buffer.
    - Frame/cycle conversions round-trip.
  - **`level:`**
    - A synthetic 1 kHz square wave at -20 dBFS gives Started then Stopped, with `dominantHz` 1,000 ±1% and `peakDb` -20 ±1.
    - A DC offset alone gives no events (Review Focus 3).
    - `min_silence_ms` bridges short gaps.
  - **`fft:`** pure sines at 440 Hz and 2,000 Hz come back within ±0.5%.
  - **`wav:`** the header fields and byte length are correct.
  - **`vsound:`**
    - The cursors keep AppleWin's fill check neutral: play = commitEnd − ⅜·size.
    - A whole-buffer commit isn't captured.
    - The feedback stays 0 over a stepped tone run (Review Focus 1). This is an in-process test once Task 2 wires it; until then it fails.
  - **Python `TestAudio`:**
    - Pass condition 7 (the headless bell): `subscribe_audio`, type `PRINT CHR$(7)\n`, step 120 frames. Expect `sound_started`, then `sound_stopped` with `duration_ms` 50–200 and `dominant_hz` within ±10% of `BELL_HZ`. `BELL_HZ` is a constant in the test; Task 4 records its measured value. `get_audio` at the start gives AC RMS > -30 dBFS.
    - `-32009` after 31 s of stepped frames (stepping is fast).
    - Pass condition 8 (deterministic audio): the same script run twice in two processes gives byte-identical `get_audio` over the same cycles. A windowed copy runs only when `HARNESS_WINDOWED=1`.
    - The Review Focus 4 edge cases.
    - Idle DOS gives no events (Review Focus 3).
  - **`realtime` suite** (C++): the speed-change `dominant_hz` test (Review Focus 2), marked `doctest::skip()` until Part C.
- [ ] **Step 2:** Build with 0 warnings. Old tests pass; new tests fail. Commit: `test: M3B failing audio test suite and interface stubs`.

### Task 2: Virtual sound buffers replace DXSoundBuffer.cpp
**Files:** `build/AppleWinCore.vcxproj` (remove `DXSoundBuffer.cpp`), `src/host/virtual_sound.cpp`, `src/host/harness_frame.{h,cpp}`, `src/host/emulator.cpp`. Delete `src/host/monitored_sound_buffer.*` and its tests.
- [ ] **Step 1:** Implement the replacement TU and `VirtualSoundBuffer` as D14 describes.
  - `HarnessFrame::CreateSoundBuffer` always returns a `VirtualSoundBuffer`. The voice id is a per-frame counter.
  - `Emulator` owns the `AudioCapture`, passes it into `HarnessFrame`, and clears it on PowerCycle and Start.
  - `DSInit` is called in every mode, and `g_bDisableDirectSound` is always false.
  - The windowed DirectSound init from M2 (window → DSInit) is removed, because playback moves to `AudioOutput` (Task 5).
  - Between Task 2 and Task 5, windowed mode is silent. The `audio` suite is red, and that is expected and ledgered.
- [ ] **Step 2:** Run the full suite with `-tse=windowed,audio`. Expected:
  - the M1 and M2 goldens are unchanged;
  - headless mode doesn't load `dsound.dll` (the delay-load check);
  - `vsound:*` passes.

  If any golden changes (for example, because the speaker was muted before and now runs), stop and report it with evidence.
- [ ] **Step 3:** Commit: `feat(host): virtual sound buffers (D14); AppleWin's DXSoundBuffer.cpp replaced`.

### Task 3: AudioCapture, WAV, and `get_audio`
**Files:** `src/core/audio_capture.cpp`, `src/core/wav.cpp`, `src/server/session.cpp`.
- [ ] Implement them. `get_audio` resolves `from_cycle` against `EmulatedCycles` (since power-on), which is the timeline's origin. Run `-tc="capture:*,wav:*"`, then the Python audio tests that use only `get_audio`. Commit: `feat: audio capture timeline and get_audio`.

### Task 4: LevelDetector, FFT, and `subscribe_audio`
**Files:** `src/core/level_detector.cpp`, `src/core/fft.cpp`, `src/server/session.cpp`.
- [ ] **Step 1:** Implement them.
  - The session feeds the detector after every step for each subscribed agent connection, and sends the events as notifications.
  - Measure the //e bell's pitch from a captured `PRINT CHR$(7)`, set `BELL_HZ` in the Python test, and report the value. The controller ledgers it as a ruling.
- [ ] **Step 2:** Run `-tc="level:*,fft:*"` and the full Python suite. Commit: `feat: sound events (subscribe_audio)`.

### Task 5: AudioOutput (windowed playback, feedback, underruns)
**Files:** `src/host/audio_output.cpp`, `src/host/emulator.cpp`.
- [ ] **Step 1:** Implement it.
  - **Opening:** windowed Start calls `AudioOutput::Open(window)`. If it returns null, print the M2 warning (`warning: no audio device; running without sound`) and continue without sound.
  - **Buffer:** a 44,100 Hz stereo DirectSound buffer the same size as AppleWin's speaker buffer. Start writing ⅜ of the way in.
  - **`Pump`:** called after every realtime step. It copies new timeline frames, and computes the fill and feedback with AppleWin's rule from `Spkr_SubmitWaveBuffer`:
    - a fill below ¼ adds `SoundCore_GetErrorInc()`, a fill above ½ subtracts it, otherwise the error resets to 0;
    - the error is clamped to ±`SoundCore_GetErrorMax()`;
    - the feedback in cycles is error × `clk/44100`.
  - **Underruns:** counted once playback has started, whenever the play cursor has passed the written end.
  - **`SetPlaying(false)`** writes silence. It applies outside realtime runs, and it replaces M2's mute.
  - **Wiring:** the realtime burst becomes `budget.Next() + AudioOutput::FeedbackCycles()`. `SoundActive()` means `AudioOutput` exists, and `SoundUnderruns()` reads it.
- [ ] **Step 2:** Run `-ts=windowed`, `-ts=audio` twice (0 underruns each time), and the full suite in Debug and Release. Also run the M2 bell command, which must exit 0 and print the `audio underruns` line. Commit: `feat(host): harness-owned audio output with burst feedback (D14)`.

### Task 6: Docs
**Files:** `docs/decisions.md`, `docs/api_spec.md`, `project.md`, `README.md`.
- [ ] Update the docs. Add D14 in full. Update D8 (remove the SSI263 exception), D9 (feedback now comes from `AudioOutput`), and D12 (no patches; capture via virtual buffers; timeline placement by interval). Bring the `api_spec.md` Sound section up to the spec and record the measured bell pitch. Update the M3 acceptance in `project.md`, and change the D12 risk line from "AppleWin patches" to "replaced TU must track AppleWin's DXSoundBuffer interface on submodule updates". Commit: `docs: D14 virtual sound; audio API`.
