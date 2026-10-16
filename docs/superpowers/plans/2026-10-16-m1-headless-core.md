# M1 Headless Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Link AppleWin's core into the harness, boot an Apple //e Enhanced headless, run it frame-exact or in real time, and save the screen as a PNG.

**Architecture:**
- AppleWin's sources, minus `Windows/AppleWin.cpp`, build unchanged into a static lib.
- The harness supplies the frontend: a `HeadlessFrame`, plus the `GetFrame`, `GetVideo` and `GetPropertySheet` factories.
- The harness owns the run loop.
- **The whole test suite is written first (Task 2), against interface headers with stub bodies.** The implementation tasks then turn it green.

**Tech Stack:** C++20 (harness code), MSVC v143, MSBuild, doctest 2.4.11, zlib (from the AppleWin submodule), Windows BCrypt.

**Spec:** `docs/specs/m1.md`. Read it with this plan; the interfaces and startup order there are binding.

## Global Constraints
- x64 only, with Debug and Release configurations. Platform toolset v143.
- Harness code is C++20 at warning level 4. AppleWin sources compile with AppleWin's own settings: warning level 3, its language standard, `StdAfx.h` precompiled header, and its defines `WIN32;_WINDOWS;_CRT_SECURE_NO_DEPRECATE;NO_DSHOW_STRSAFE;YAML_DECLARE_STATIC`.
- **No file under `external/AppleWin` may be modified.** If a patch seems unavoidable, stop and report it.
- New dependencies: only `third_party/doctest/doctest.h` (v2.4.11).
- Nothing may show modal UI or write to `HKCU\Software\AppleWin`.
- Golden hash file: `tests/golden/m1_boot_120.sha256`.
- Cycles per frame at 60 Hz: 17,030. NTSC clock: 1,020,484 Hz (integer part).

## Review Focus
1. **The exe built without `Applewin.rc`, so ROMs are missing.** `HeadlessFrame::GetResource` must return null, and `Start` must throw a `runtime_error` that includes the AppleWin message. It must not crash or hang. *(Test in Task 2: `GetResource` for a known ROM is non-null with size 16384, and for an unknown id is null.)*
2. **A modal dialog during startup**, e.g. from AppleWin code that calls `MessageBox` directly, which `Joystick.cpp` does. Expected behaviour: nothing blocks. *(Test in Task 2: `Start` completes and `Messages()` is empty for the default config.)*
3. **Upside-down images** caused by AppleWin's bottom-up framebuffer. *(Test in Task 2: a negative-stride PNG round trip. Task 7 adds a human visual check before the golden hash is recorded.)*
4. **Calling methods out of order:** `RunFrames` before `Start`, or `Start` twice. Expected: `std::logic_error`. *(Test in Task 2.)*
5. **Writing to the user's registry.** Expected: the last-write time of `HKCU\Software\AppleWin` is unchanged. *(Test in Task 2.)*

**Build command** (used in every task):
```bash
MSB=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | head -1)
"$MSB" AppleWinHarness.sln -m -nologo -v:minimal -p:Configuration=Debug -p:Platform=x64
```
**Test command:** `bin/x64/Debug/HarnessTests.exe` (all tests) or `bin/x64/Debug/HarnessTests.exe -tce="*" -ts=<suite>`.

---

### Task 1: Build scaffolding

**Files:**
- Create: `build/AppleWinCore.vcxproj`, `build/HarnessCore.vcxproj`, `tests/HarnessTests.vcxproj`, `tests/main.cpp`, `tests/smoke_test.cpp`, `third_party/doctest/doctest.h`
- Modify: `AppleWinHarness.sln` (add the projects above plus AppleWin's `zlib`, `minizip` and `yaml` projects from the submodule), `AppleWinHarness.vcxproj` (link the libraries, compile `external/AppleWin/resource/Applewin.rc`)

**Interfaces:**
- Produces: the `HarnessCore` lib, with include roots `src/` and the AppleWin include directories (`external/AppleWin/source`, `source/cpu`, `source/debugger`, `zlib`, `libyaml/include`, `resource`). Both exes link everything listed under "Build layout" in the spec. Output goes to `bin/x64/<Config>/`.

- [ ] **Step 1: Write the smoke test.** `tests/main.cpp` holds `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and includes doctest. `tests/smoke_test.cpp` has `TEST_CASE("smoke") { CHECK(1 + 1 == 2); }`. Download doctest from `https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h`.
- [ ] **Step 2: Create `build/AppleWinCore.vcxproj`.** Copy the `ClCompile` list from `external/AppleWin/AppleWin-VS2022.vcxproj`, with paths prefixed by `..\external\AppleWin\`, and drop `source\Windows\AppleWin.cpp`. Mark `StdAfx.cpp` as PCH `Create` and the rest as `Use`. Copy AppleWin's x64 compile settings.
- [ ] **Step 3: Build.** Expected: `AppleWinCore.lib`, `HarnessTests.exe` and `AppleWinHarness.exe` build with 0 errors. `HarnessTests.exe` reports `1 passed`. `AppleWinHarness.exe` still prints its hello line.
- [ ] **Step 4: Commit.** Message: `build: add AppleWinCore lib, HarnessCore lib, doctest test exe`.

### Task 2: Interface headers, stubs, and the full failing test suite (spec-driven)

The implementer of this task writes tests from `docs/specs/m1.md` only.

**Files:**
- Create the headers: `src/core/frame_view.h`, `src/core/png_writer.h`, `src/core/sha256.h`, `src/core/pacing.h`, `src/host/headless_frame.h`, `src/host/emulator.h`. Signatures are copied exactly from the spec.
- Create stub `.cpp` files next to each header. Every function throws `std::runtime_error("not implemented")`, except `HeadlessFrame`'s trivial no-op members.
- Create the tests: `tests/png_writer_test.cpp`, `tests/sha256_test.cpp`, `tests/pacing_test.cpp`, `tests/headless_frame_test.cpp`, `tests/emulator_test.cpp`, `tests/cli_test.cpp`, `tests/support/emulator_fixture.h`, `tests/support/png_reader.h`.
- Delete: `tests/smoke_test.cpp`.

**Interfaces:**
- Produces: all the headers listed above (taken from the spec), plus these test helpers:
  - `harness::Emulator& StartedEmulator()`: lazily calls `Start` with `%TEMP%\applewin-harness\tests.ini` and registers `Shutdown` with `atexit`. Immediately before `Start`, it records `RegistryStampBeforeStart()`, the existence and last-write `FILETIME` of `HKCU\Software\AppleWin`.
  - `RegistryStamp ReadAppleWinRegistryStamp()`: reads that stamp now.
  - `PngInfo ReadPng(std::span<const uint8_t>)`: returns `{width, height, bitDepth, colorType, allCrcsValid, rgbRows}`. It checks the signature, walks the chunks, verifies each CRC with zlib `crc32`, inflates IDAT with zlib `uncompress`, and strips each row's filter byte. It fails the test if any filter byte isn't 0.

- [ ] **Step 1: Write `png_writer_test.cpp`**
```cpp
// 3x2 image, bottom-up BGRA buffer (stride -12): visual top row = red, green, blue; bottom row = white, black, gray(0x80)
TEST_CASE("png: header")         { auto p = ReadPng(EncodePng(view3x2)); CHECK(p.width==3); CHECK(p.height==2); CHECK(p.bitDepth==8); CHECK(p.colorType==2); }
TEST_CASE("png: crcs")           { CHECK(ReadPng(EncodePng(view3x2)).allCrcsValid); }
TEST_CASE("png: pixels top-down"){ auto p = ReadPng(EncodePng(view3x2));
    CHECK(p.rgbRows[0] == std::vector<uint8_t>{255,0,0, 0,255,0, 0,0,255});
    CHECK(p.rgbRows[1] == std::vector<uint8_t>{255,255,255, 0,0,0, 128,128,128}); }
TEST_CASE("png: positive stride equals negative stride of same image") { CHECK(EncodePng(topDownView) == EncodePng(view3x2)); }
TEST_CASE("png: invalid input")  { CHECK_THROWS_AS(EncodePng({nullptr,3,2,12}), std::invalid_argument);
                                   CHECK_THROWS_AS(EncodePng({buf,0,2,12}), std::invalid_argument); }
TEST_CASE("png: file write")     { CHECK(WritePngFile(tmp/"a.png", view3x2)); CHECK(readFile(tmp/"a.png") == EncodePng(view3x2));
                                   CHECK_FALSE(WritePngFile(tmp/"no_such_dir"/"a.png", view3x2)); }
TEST_CASE("frame: ToRgbTopDown") { CHECK(ToRgbTopDown(view3x2).size()==18); CHECK(ToRgbTopDown(view3x2)[0]==255); }
```
- [ ] **Step 2: Write `sha256_test.cpp`.**
  - `Sha256Hex("abc")` must equal `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`.
  - `Sha256Hex({})` must equal `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
  - `FrameHash(view3x2) == Sha256Hex(ToRgbTopDown(view3x2))`.
- [ ] **Step 3: Write `pacing_test.cpp`**
```cpp
TEST_CASE("burst: exact long-term total") { BurstBudget b(1'020'484.32, 1000us); uint64_t sum=0;
    for (int i=0;i<1000;++i){ auto n=b.Next(); CHECK((n==1020||n==1021)); sum+=n; } CHECK(sum==1'020'484); }
TEST_CASE("burst: invalid") { CHECK_THROWS_AS(BurstBudget(0,1000us), std::invalid_argument); CHECK_THROWS_AS(BurstBudget(1e6,0us), std::invalid_argument); }
TEST_SUITE("realtime") { TEST_CASE("pacer: 500 ticks ~= 500 ms") { RealtimePacer p; p.Start(); auto t0=steady_clock::now();
    for(int i=0;i<500;++i) p.WaitForTick(); auto ms=duration_cast<milliseconds>(steady_clock::now()-t0).count(); CHECK(ms>=450); CHECK(ms<=550); } }
```
- [ ] **Step 4: Write `headless_frame_test.cpp`.** Use a local `HeadlessFrame f;`, without starting the emulator.
  - `f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 16384) != nullptr`
  - `f.GetResource(0xFFFF, "ROM", 16384) == nullptr`
  - `f.GetResource(IDR_APPLE2E_ENHANCED_ROM, "ROM", 1) == nullptr`
  - After `f.FrameMessageBox("t","c",MB_OK)`, it returns `IDOK` and `f.messages().back() == "c: t"`.
  - `f.CreateSoundBuffer(4096,44100,1,"x") == nullptr`
- [ ] **Step 5: Write `emulator_test.cpp`**
```cpp
// doctest's default order is by file name, then line. No earlier file starts the emulator, so this case runs first.
TEST_CASE("emu: precondition") { CHECK_THROWS_AS(Emulator::Instance().RunFrames(1), std::logic_error); }
TEST_CASE("emu: start twice")  { auto& e=StartedEmulator(); CHECK_THROWS_AS(e.Start({}), std::logic_error); CHECK(e.Messages().empty()); }
TEST_CASE("emu: boot text")    { auto& e=StartedEmulator(); e.PowerCycle(); e.RunFrames(120); CHECK(e.ReadTextRow(0).find("Apple //e") != std::string::npos); }
TEST_CASE("emu: frame geometry"){ auto& e=StartedEmulator(); auto v=e.Frame(); CHECK(v.width==560); CHECK(v.height==384); CHECK(v.strideBytes==-600*4);
                                  auto b=e.Frame(true); CHECK(b.width==600); CHECK(b.height==420); }
TEST_CASE("emu: frame-exact cycles") { auto& e=StartedEmulator(); auto c0=e.EmulatedCycles(); auto f0=e.PresentedFrames(); e.RunFrames(10);
    auto d=e.EmulatedCycles()-c0; CHECK(d>=170'300); CHECK(d<=170'300+7); auto df=e.PresentedFrames()-f0; CHECK(df>=9); CHECK(df<=11);
    auto c1=e.EmulatedCycles(); e.RunFrames(0); CHECK(e.EmulatedCycles()==c1); }
TEST_CASE("emu: deterministic boot") { auto& e=StartedEmulator(); e.PowerCycle(); e.RunFrames(120); auto a=FrameHash(e.Frame());
    e.PowerCycle(); e.RunFrames(120); CHECK(FrameHash(e.Frame())==a); }
TEST_CASE("emu: golden boot") { auto& e=StartedEmulator(); e.PowerCycle(); e.RunFrames(120);
    auto golden=readTrimmed("tests/golden/m1_boot_120.sha256"); REQUIRE_MESSAGE(!golden.empty(), "run Task 7 to record the golden hash"); CHECK(FrameHash(e.Frame())==golden); }
TEST_CASE("emu: no registry writes") { auto& e=StartedEmulator(); e.RunFrames(60);
    CHECK(ReadAppleWinRegistryStamp() == RegistryStampBeforeStart()); }   // stamp = {exists, last-write FILETIME} via RegQueryInfoKeyW
TEST_SUITE("realtime") { TEST_CASE("emu: realtime rate") { auto& e=StartedEmulator(); auto c0=e.EmulatedCycles(); e.RunRealtime(2000ms);
    double d=double(e.EmulatedCycles()-c0); CHECK(d>=2*1'020'484*0.98); CHECK(d<=2*1'020'484*1.02); } }
```
  Locate the golden file relative to the test exe: `<exe dir>\..\..\..\tests\golden`.
- [ ] **Step 6: Write `cli_test.cpp`.** Start `AppleWinHarness.exe` with `CreateProcessW`, from the same folder as the test exe, capturing stdout.
  - `--boot-frames 120 --png <tmp>\boot.png`: exit code 0, the file exists, and stdout matches the regex `^sha256 [0-9a-f]{64}\r?\n$`.
  - No arguments: exit code 2.
  - `--boot-frames abc`: exit code 2.
- [ ] **Step 7: Build and run the tests to confirm they fail.** Expected: a clean build. Every test fails with "not implemented" except the `HeadlessFrame` `CreateSoundBuffer` check and the precondition test, which may pass. Record the failure count in the commit message.
- [ ] **Step 8: Commit.** Message: `test: M1 interface headers, stubs and failing spec tests`.

### Task 3: FrameView, PNG writer, SHA-256

**Files:** Modify `src/core/frame_view.cpp`, `src/core/png_writer.cpp`, `src/core/sha256.cpp`.
**Interfaces:** Consumes the Task 2 headers. Produces working `ToRgbTopDown`, `EncodePng`, `WritePngFile`, `Sha256Hex` and `FrameHash`.

- [ ] **Step 1: Run the tests and confirm they fail.** Run `HarnessTests.exe -tc="png:*,frame:*,sha*"`. Expected: they fail with "not implemented".
- [ ] **Step 2: Implement.**
  - PNG: signature, then IHDR (big-endian width and height, depth 8, colour type 2, compression 0, filter 0, interlace 0), then a single IDAT holding `compress2(Z_DEFAULT_COMPRESSION)` of the rows, each prefixed by a 0 filter byte, then IEND.
  - CRCs: zlib `crc32` over the chunk type and data.
  - SHA-256: `BCryptOpenAlgorithmProvider(BCRYPT_SHA256_ALGORITHM)` and `BCryptHash`.
- [ ] **Step 3: Run the tests and confirm they pass.** Same filter. Expected: all pass.
- [ ] **Step 4: Commit.** Message: `feat(core): PNG writer, SHA-256, frame view`.

### Task 4: Burst budget and realtime pacer (can run in parallel with Task 3)

**Files:** Modify `src/core/pacing.cpp`.
**Interfaces:** Produces `BurstBudget::Next()` and `RealtimePacer::Start/WaitForTick`.

- [ ] **Step 1: Run the tests and confirm they fail.** Run `HarnessTests.exe -tc="burst:*,pacer:*"`. Expected: they fail.
- [ ] **Step 2: Implement.**
  - `BurstBudget`: keeps a `double` accumulator. `Next()` adds `clockHz × period`, returns `floor(acc)`, and subtracts what it returned.
  - `RealtimePacer`: uses `CreateWaitableTimerExW` with high resolution and `SetWaitableTimer(due = -10'000, period = 1 ms)`. `WaitForTick` calls `WaitForSingleObject`.
- [ ] **Step 3: Run the tests and confirm they pass.** Same filter. Expected: they pass (the realtime test within ±10%).
- [ ] **Step 4: Commit.** Message: `feat(core): burst budget and high-resolution realtime pacer`.

### Task 5: HeadlessFrame, AppleWin host, and Emulator (except realtime)

**Files:**
- Modify: `src/host/headless_frame.cpp`, `src/host/emulator.cpp`.
- Create: `src/host/applewin_host.cpp`.

**Interfaces:**
- Consumes: `FrameView` and `FrameHash` (Task 3); the AppleWin APIs named in the spec.
- Produces: everything on `Emulator` except `RunRealtime`.

- [ ] **Step 1: Run the tests and confirm they fail.** Run `HarnessTests.exe -tc="emu:*,headless*" -tce="*realtime*,emu: golden*"`. Expected: they fail.
- [ ] **Step 2: Implement `HeadlessFrame`** exactly as the table in the spec describes. Copy `GetResource` from `Win32Frame::GetResource` (`external/AppleWin/source/Windows/Win32Frame.cpp:676`).
- [ ] **Step 3: Write `applewin_host.cpp`.**
  - Define `GetFrame`, `GetVideo` and `GetPropertySheet` as statics.
  - Build. For every unresolved external the linker reports, copy its definition and initial value from `external/AppleWin/source/Windows/AppleWin.cpp`.
  - Repeat until the build links.
  - List the copied symbols in a comment at the top of the file.
- [ ] **Step 4: Implement `Emulator`.** Follow the spec's startup order, boot sequence, `RunFrames` loop, `Frame` geometry, `ReadTextRow` and preconditions.
  - `ReadTextRow`: row addresses for text page 1 are `0x400 + 0x80×(row % 8) + 0x28×(row / 8)`, read with `MemGetMainPtr`.
- [ ] **Step 5: Run the tests and confirm they pass.** Same filter. Expected: all pass. If `boot text` fails, save the frame as a PNG and look at it before changing anything.
- [ ] **Step 6: Confirm no AppleWin changes.** Run `git -C external/AppleWin status --porcelain`. Expected: empty output.
- [ ] **Step 7: Commit.** Message: `feat(host): headless AppleWin frontend and frame-exact emulator`.

### Task 6: Realtime run loop

**Files:** Modify `src/host/emulator.cpp`.
**Interfaces:** Consumes `BurstBudget` and `RealtimePacer` (Task 4), and the per-step logic from Task 5. Produces `Emulator::RunRealtime`.

- [ ] **Step 1: Run the test and confirm it fails.** Run `HarnessTests.exe -ts=realtime -tc="emu:*"`. Expected: it fails.
- [ ] **Step 2: Implement `RunRealtime`.** Share the per-step function with `RunFrames`, with no duplicated loop body.
- [ ] **Step 3: Run the test and confirm it passes.** Same filter. Expected: it passes within ±2%.
- [ ] **Step 4: Commit.** Message: `feat(host): realtime paced run loop`.

### Task 7: Command line, golden hash, and docs

**Files:**
- Modify: `src/main.cpp`, `project.md` (mark M1 ✅ and record the patch list, which is expected to be empty), `docs/decisions.md` (set D2's status to Accepted, with the spike outcome), `README.md` (Run section).
- Create: `tests/golden/m1_boot_120.sha256`.

**Interfaces:** Consumes `Emulator`, `WritePngFile` and `FrameHash`.

- [ ] **Step 1: Run the CLI tests and confirm they fail.** Run `HarnessTests.exe -tc="cli:*"`. Expected: they fail.
- [ ] **Step 2: Implement `main.cpp`.** Follow the spec's command-line section: argument parsing, the temporary INI, exit codes 0/2/1, and the stdout format.
- [ ] **Step 3: Run the CLI tests and confirm they pass.** Same filter. Expected: they pass.
- [ ] **Step 4: Have a person check the boot screen.**
  1. Run `bin/x64/Debug/AppleWinHarness.exe --boot-frames 120 --png boot.png`.
  2. **Stop and ask your human partner to look at `boot.png`.** It should show the //e boot screen, the right way up, with "Apple //e" at the top.
  3. Only after they confirm, write the printed hash into `tests/golden/m1_boot_120.sha256`.
- [ ] **Step 5: Run the full test suite, Debug and Release.** Build both configurations and run `HarnessTests.exe`. Expected: all tests pass.
- [ ] **Step 6: Update the docs** listed under Files.
- [ ] **Step 7: Commit.** Message: `feat: M1 CLI, golden boot hash; mark M1 done`.
