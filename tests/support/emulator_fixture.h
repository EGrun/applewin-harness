#pragma once
#include "host/emulator.h"
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace harness::testsupport {

// Existence and last-write time of HKCU\Software\AppleWin.
struct RegistryStamp {
    bool exists = false;
    FILETIME lastWrite{};
    bool operator==(const RegistryStamp& o) const
    {
        return exists == o.exists && lastWrite.dwLowDateTime == o.lastWrite.dwLowDateTime &&
               lastWrite.dwHighDateTime == o.lastWrite.dwHighDateTime;
    }
};

inline RegistryStamp ReadAppleWinRegistryStamp()
{
    RegistryStamp s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AppleWin", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        s.exists = true;
        RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &s.lastWrite);
        RegCloseKey(key);
    }
    return s;
}

inline RegistryStamp& RegistryStampBeforeStartRef()
{
    static RegistryStamp stamp;
    return stamp;
}
inline const RegistryStamp& RegistryStampBeforeStart() { return RegistryStampBeforeStartRef(); }

inline void ShutdownAtExit() { Emulator::Instance().Shutdown(); }

// Lazily starts the shared emulator with %TEMP%\applewin-harness\tests.ini; Shutdown is registered with atexit.
inline Emulator& StartedEmulator()
{
    static bool started = false;
    if (!started) {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / "applewin-harness";
        std::filesystem::create_directories(dir);
        EmulatorConfig cfg;
        cfg.configIni = dir / "tests.ini";
        RegistryStampBeforeStartRef() = ReadAppleWinRegistryStamp();
        Emulator::Instance().Start(cfg);
        started = true;
        std::atexit(ShutdownAtExit);
    }
    return Emulator::Instance();
}

}  // namespace harness::testsupport
