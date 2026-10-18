// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace harness::testsupport {

inline std::filesystem::path TestExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// <exe dir>/../../../tests/golden/<name>, first line trimmed; empty if missing.
inline std::string ReadGoldenTrimmed(const char* name)
{
    std::ifstream f(TestExeDir() / ".." / ".." / ".." / "tests" / "golden" / name);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b);
}

}  // namespace harness::testsupport
