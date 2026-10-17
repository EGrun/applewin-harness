#pragma once
#include "host/emulator.h"
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>

namespace harness::testsupport {

// Snapshot of the whole AppleWin registry subtree (HKCU, Software, AppleWin): whether the root exists, plus every subkey's
// relative path and last-write time. A value written anywhere below the root changes its key's time.
struct RegistryStamp {
    bool exists = false;
    std::map<std::wstring, unsigned long long> keys;   // relative path ("" = root) -> last-write as 64-bit
    bool operator==(const RegistryStamp& o) const { return exists == o.exists && keys == o.keys; }
};

inline void StampKey(HKEY key, const std::wstring& rel, RegistryStamp& s)
{
    FILETIME ft{};
    DWORD subKeys = 0, maxName = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeys, &maxName, nullptr, nullptr, nullptr, nullptr, nullptr, &ft) != ERROR_SUCCESS)
        return;
    s.keys[rel] = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    std::vector<wchar_t> name(maxName + 2);
    for (DWORD i = 0; i < subKeys; ++i) {
        DWORD len = DWORD(name.size());
        if (RegEnumKeyExW(key, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue;
        std::wstring child(name.data(), len);
        HKEY sub = nullptr;
        if (RegOpenKeyExW(key, child.c_str(), 0, KEY_READ, &sub) == ERROR_SUCCESS) {
            StampKey(sub, rel.empty() ? child : rel + L"\\" + child, s);
            RegCloseKey(sub);
        }
    }
}

inline RegistryStamp ReadAppleWinRegistryStamp()
{
    RegistryStamp s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AppleWin", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        s.exists = true;
        StampKey(key, L"", s);
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

// Realtime rate tolerance (ruling C8): the spec's +/-2% locally, +/-3% on CI's shared runners (GitHub sets CI).
inline double RealtimeRateTolerance()
{
    return GetEnvironmentVariableA("CI", nullptr, 0) > 0 ? 0.03 : 0.02;
}

inline void ShutdownAtExit() { Emulator::Instance().Shutdown(); }

// Lazily starts the shared headless emulator with %TEMP%\applewin-harness\tests.ini; Shutdown is registered with atexit.
inline Emulator& StartedEmulator()
{
    static bool started = false;
    if (!started) {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / "applewin-harness";
        std::filesystem::create_directories(dir);
        EmulatorConfig cfg;
        cfg.configIni = dir / "tests.ini";
        cfg.display = DisplayMode::Headless;   // the shared fixture stays headless (the spec default is Windowed)
        std::error_code ec;
        std::filesystem::remove(cfg.configIni, ec);   // AppleWin persists slot config here; start every run clean
        RegistryStampBeforeStartRef() = ReadAppleWinRegistryStamp();
        Emulator::Instance().Start(cfg);
        started = true;
        std::atexit(ShutdownAtExit);
    }
    return Emulator::Instance();
}

}  // namespace harness::testsupport
