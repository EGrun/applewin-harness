// SPDX-License-Identifier: MIT
#include "host/window.h"

#include <cstring>
#include <mutex>
#include <stdexcept>

namespace harness {

namespace {

constexpr wchar_t kClassName[] = L"AppleWinHarnessWindow";
constexpr int kClientWidth = 1120;
constexpr int kClientHeight = 768;

[[noreturn]] void ThrowLastError(const char* what)
{
    throw std::runtime_error(std::string(what) + " failed, GetLastError=" + std::to_string(GetLastError()));
}

}  // namespace

Window::Window(const std::wstring& title)
{
    static std::once_flag once;   // lambda inside a member: it may name the private WndProc
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Window::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            ThrowLastError("RegisterClassExW");
    });

    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc{0, 0, kClientWidth, kClientHeight};
    if (!AdjustWindowRectEx(&rc, style, FALSE, 0)) ThrowLastError("AdjustWindowRectEx");

    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) ThrowLastError("CreateWindowExW");

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

Window::~Window()
{
    if (hwnd_ && !closed_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);   // no callbacks into a half-destroyed object
        DestroyWindow(hwnd_);
    }
}

void Window::Show(const FrameView& f)
{
    width_ = f.width;
    height_ = f.height;
    const size_t rowBytes = size_t(f.width) * 4;
    pixels_.resize(rowBytes * f.height);
    for (uint32_t y = 0; y < f.height; ++y)
        std::memcpy(pixels_.data() + y * rowBytes, f.topRow + ptrdiff_t(y) * f.strideBytes, rowBytes);
    if (hwnd_ && !closed_) InvalidateRect(hwnd_, nullptr, FALSE);
}

const std::vector<uint8_t>& Window::Pixels() const { return pixels_; }

bool Window::Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !Closed();
}

bool Window::Closed() const { return closed_; }

HWND Window::Handle() const { return hwnd_; }

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (self->pixels_.empty() || self->width_ == 0 || self->height_ == 0) {
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        } else {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = LONG(self->width_);
            bi.bmiHeader.biHeight = -LONG(self->height_);   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, int(self->width_),
                          int(self->height_), self->pixels_.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        self->closed_ = true;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace harness
