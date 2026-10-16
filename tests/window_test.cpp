// Window unit tests (spec: docs/specs/m2.md, "src/host/window.h"). They need a desktop session.
#include "doctest/doctest.h"
#include "host/window.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using harness::FrameView;
using harness::Window;

namespace {

// 2x2 BGRA, bottom-up storage. Visual top row = A, B; visual bottom row = C, D.
constexpr std::array<uint8_t, 16> kBottomUp2x2 = {
    0x31, 0x32, 0x33, 0x00,   0x41, 0x42, 0x43, 0x00,   // buffer row 0 = visual bottom: C, D
    0x11, 0x12, 0x13, 0x00,   0x21, 0x22, 0x23, 0x00,   // buffer row 1 = visual top: A, B
};
const std::vector<uint8_t> kTopDown2x2 = {
    0x11, 0x12, 0x13, 0x00,   0x21, 0x22, 0x23, 0x00,   // A, B
    0x31, 0x32, 0x33, 0x00,   0x41, 0x42, 0x43, 0x00,   // C, D
};

FrameView BottomUp2x2() { return FrameView{kBottomUp2x2.data() + 8, 2, 2, -8}; }

}  // namespace

TEST_SUITE("windowed") {

TEST_CASE("window: construction gives a visible AppleWinHarnessWindow with a 1120x768 client area")
{
    Window w(L"window test");
    HWND h = w.Handle();
    REQUIRE(h != nullptr);
    CHECK(IsWindowVisible(h));
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, int(std::size(cls)));
    CHECK(std::wstring(cls) == L"AppleWinHarnessWindow");
    RECT rc{};
    REQUIRE(GetClientRect(h, &rc));
    CHECK(rc.right - rc.left == 1120);
    CHECK(rc.bottom - rc.top == 768);
    CHECK(!w.Closed());
}

TEST_CASE("window: Show copies a bottom-up frame top row first")
{
    Window w(L"window test");
    w.Show(BottomUp2x2());
    CHECK(w.Pixels() == kTopDown2x2);
}

TEST_CASE("window: keyboard and mouse messages change nothing")
{
    Window w(L"window test");
    w.Show(BottomUp2x2());
    HWND h = w.Handle();
    SendMessageW(h, WM_CHAR, WPARAM('A'), 0);
    SendMessageW(h, WM_KEYDOWN, WPARAM('A'), 0);
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    CHECK(w.Pump());
    CHECK(!w.Closed());
    CHECK(IsWindow(h));
    CHECK(w.Pixels() == kTopDown2x2);
}

TEST_CASE("window: WM_CLOSE closes it")
{
    Window w(L"window test");
    REQUIRE(PostMessageW(w.Handle(), WM_CLOSE, 0, 0));
    CHECK(!w.Pump());
    CHECK(w.Closed());
}

}  // TEST_SUITE("windowed")
