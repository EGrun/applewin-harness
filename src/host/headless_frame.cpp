// Stub: not yet part of any vcxproj (Task 5 adds it together with the host code it links against).
#include "host/headless_frame.h"
#include <stdexcept>

namespace harness {

void HeadlessFrame::Initialize(bool) { throw std::runtime_error("not implemented"); }
void HeadlessFrame::Destroy() { throw std::runtime_error("not implemented"); }
void HeadlessFrame::VideoPresentScreen() { throw std::runtime_error("not implemented"); }
int HeadlessFrame::FrameMessageBox(LPCSTR, LPCSTR, UINT) { throw std::runtime_error("not implemented"); }
void HeadlessFrame::GetBitmap(WORD, LONG, LPVOID) { throw std::runtime_error("not implemented"); }
std::shared_ptr<NetworkBackend> HeadlessFrame::CreateNetworkBackend(const std::string&) { return nullptr; }
std::shared_ptr<SoundBuffer> HeadlessFrame::CreateSoundBuffer(uint32_t, uint32_t, int, const char*) { return nullptr; }
BYTE* HeadlessFrame::GetResource(WORD, LPCSTR, uint32_t) { throw std::runtime_error("not implemented"); }
void HeadlessFrame::Restart() { throw std::runtime_error("not implemented"); }

}  // namespace harness
