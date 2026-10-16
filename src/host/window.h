#pragma once
#include <windows.h>
#include "core/frame_view.h"
#include <cstdint>
#include <string>
#include <vector>

namespace harness {

// The view-only presenter (D8): one top-level Win32 window owned by the calling thread.
// Keyboard and mouse messages go to DefWindowProc only. Nothing in Window calls AppleWin.
class Window {
public:
    // Registers the class AppleWinHarnessWindow once per process, creates a WS_OVERLAPPEDWINDOW with a 1120x768 client
    // area (the borderless frame at 2x) and shows it. Throws std::runtime_error on failure.
    explicit Window(const std::wstring& title);
    ~Window();   // destroys the window if it is still open
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Copies the frame into a top-down BGRA buffer and invalidates the client area. WM_PAINT draws that buffer scaled
    // to the client area with StretchDIBits (COLORONCOLOR).
    void Show(const FrameView&);
    const std::vector<uint8_t>& Pixels() const;   // width*height*4 bytes, top row first

    bool Pump();          // dispatches every pending message for the thread with PeekMessage; returns !Closed()
    bool Closed() const;  // true once WM_CLOSE has destroyed the window
    HWND Handle() const;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    bool closed_ = false;
    uint32_t width_ = 0, height_ = 0;
    std::vector<uint8_t> pixels_;
};

}  // namespace harness
