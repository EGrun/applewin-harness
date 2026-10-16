#pragma once
#include "StdAfx.h"
#include <windows.h>
#include "SoundBuffer.h"
#include <cstdint>
#include <vector>

namespace harness::testsupport {

// In-memory SoundBuffer with settable play/write cursors. Records every call, and every SetVolume value.
class FakeSoundBuffer final : public SoundBuffer {
public:
    explicit FakeSoundBuffer(uint32_t bytes) : data(bytes) {}

    // Settable state
    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    LONG volume = 0;
    DWORD status = 0;

    // Recorded calls
    std::vector<uint8_t> data;
    std::vector<LONG> setVolumes;
    int setCurrentPositionCalls = 0, getCurrentPositionCalls = 0, lockCalls = 0, unlockCalls = 0;
    int stopCalls = 0, playCalls = 0, setVolumeCalls = 0, getVolumeCalls = 0, getStatusCalls = 0, restoreCalls = 0;
    DWORD lastNewPosition = 0;
    DWORD lastLockOffset = 0, lastLockBytes = 0, lastLockFlags = 0;
    DWORD lastUnlockBytes1 = 0, lastUnlockBytes2 = 0;
    DWORD lastPlayFlags = 0;

    HRESULT SetCurrentPosition(DWORD dwNewPosition) override
    {
        ++setCurrentPositionCalls;
        lastNewPosition = dwNewPosition;
        return S_OK;
    }

    HRESULT GetCurrentPosition(LPDWORD play, LPDWORD write) override
    {
        ++getCurrentPositionCalls;
        if (play) *play = playCursor;
        if (write) *write = writeCursor;
        return S_OK;
    }

    // Like DirectSound: the region may wrap into a second part; DSBLOCK_ENTIREBUFFER locks the whole buffer.
    HRESULT Lock(DWORD offset, DWORD bytes, LPVOID* p1, DWORD* n1, LPVOID* p2, DWORD* n2, DWORD flags) override
    {
        ++lockCalls;
        lastLockOffset = offset;
        lastLockBytes = bytes;
        lastLockFlags = flags;
        const DWORD size = DWORD(data.size());
        if (flags & DSBLOCK_ENTIREBUFFER) {
            offset = 0;
            bytes = size;
        }
        if (offset >= size || bytes > size)
            return E_INVALIDARG;
        const DWORD first = (bytes < size - offset) ? bytes : size - offset;
        *p1 = data.data() + offset;
        *n1 = first;
        if (p2) *p2 = (bytes > first) ? data.data() : nullptr;
        if (n2) *n2 = bytes - first;
        return S_OK;
    }

    HRESULT Unlock(LPVOID, DWORD n1, LPVOID, DWORD n2) override
    {
        ++unlockCalls;
        lastUnlockBytes1 = n1;
        lastUnlockBytes2 = n2;
        return S_OK;
    }

    HRESULT Stop() override { ++stopCalls; return S_OK; }

    HRESULT Play(DWORD, DWORD, DWORD flags) override
    {
        ++playCalls;
        lastPlayFlags = flags;
        return S_OK;
    }

    HRESULT SetVolume(LONG v) override
    {
        ++setVolumeCalls;
        setVolumes.push_back(v);
        volume = v;
        return S_OK;
    }

    HRESULT GetVolume(LONG* v) override
    {
        ++getVolumeCalls;
        if (v) *v = volume;
        return S_OK;
    }

    HRESULT GetStatus(LPDWORD s) override
    {
        ++getStatusCalls;
        if (s) *s = status;
        return S_OK;
    }

    HRESULT Restore() override { ++restoreCalls; return S_OK; }
};

}  // namespace harness::testsupport
