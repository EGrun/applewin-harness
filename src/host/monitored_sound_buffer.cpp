#include "host/monitored_sound_buffer.h"

#include <stdexcept>
#include <utility>

namespace harness {

// Task 2 stubs: the interface is final, the behaviour arrives in a later task.

MonitoredSoundBuffer::MonitoredSoundBuffer(std::shared_ptr<SoundBuffer> inner, uint32_t bufferBytes)
    : inner_(std::move(inner)), bufferBytes_(bufferBytes)
{
}

HRESULT MonitoredSoundBuffer::SetCurrentPosition(DWORD) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::GetCurrentPosition(LPDWORD, LPDWORD) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::Lock(DWORD, DWORD, LPVOID*, DWORD*, LPVOID*, DWORD*, DWORD)
{
    throw std::logic_error("not implemented");
}

HRESULT MonitoredSoundBuffer::Unlock(LPVOID, DWORD, LPVOID, DWORD) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::Stop() { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::Play(DWORD, DWORD, DWORD) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::SetVolume(LONG) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::GetVolume(LONG*) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::GetStatus(LPDWORD) { throw std::logic_error("not implemented"); }

HRESULT MonitoredSoundBuffer::Restore() { throw std::logic_error("not implemented"); }

void MonitoredSoundBuffer::SetMuted(bool) { throw std::logic_error("not implemented"); }

uint64_t MonitoredSoundBuffer::Underruns() const { throw std::logic_error("not implemented"); }

}  // namespace harness
