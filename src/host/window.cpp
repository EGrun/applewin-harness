#include "host/window.h"

#include <stdexcept>

namespace harness {

// Task 2 stubs: the interface is final, the behaviour arrives in a later task.

Window::Window(const std::wstring&) { throw std::logic_error("not implemented"); }

Window::~Window() = default;

void Window::Show(const FrameView&) { throw std::logic_error("not implemented"); }

const std::vector<uint8_t>& Window::Pixels() const { throw std::logic_error("not implemented"); }

bool Window::Pump() { throw std::logic_error("not implemented"); }

bool Window::Closed() const { throw std::logic_error("not implemented"); }

HWND Window::Handle() const { throw std::logic_error("not implemented"); }

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace harness
