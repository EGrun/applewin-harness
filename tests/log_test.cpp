// SPDX-License-Identifier: MIT
// The logger's rate-limit table and the one-line-per-record rule (docs/specs/m6.md, "Structured logging").
#include "doctest/doctest.h"
#include "host/log.h"
#include <cstdio>
#include <fstream>
#include <io.h>
#include <sstream>
#include <string>

namespace {

// Runs fn with stderr redirected to a file; returns what it wrote.
template <class F>
std::string CaptureStderr(F fn)
{
    char path[L_tmpnam_s];
    tmpnam_s(path);
    std::fflush(stderr);
    const int saved = _dup(_fileno(stderr));
    std::FILE* f = nullptr;
    freopen_s(&f, path, "w", stderr);
    fn();
    std::fflush(stderr);
    _dup2(saved, _fileno(stderr));
    _close(saved);
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    std::remove(path);
    return ss.str();
}

}  // namespace

TEST_CASE("log: text mode writes one line per record, escaping newlines")
{
    harness::log::SetFormat(harness::log::Format::Text);
    const std::string out = CaptureStderr([] {
        harness::log::Warning(harness::log::Component::Main, "first line\nsecond line\r\nthird");
    });
    CHECK(out.size() > 2);
    // stderr is a text-mode stream, so the line ends in CR LF: compare up to the first real line break.
    const std::string line = out.substr(0, out.find_first_of("\r\n"));
    CHECK(line == "warning: first line\\nsecond line\\r\\nthird");
    CHECK(out.find_first_of("\r\n") == out.size() - (out.back() == '\n' && out[out.size() - 2] == '\r' ? 2 : 1));
}

TEST_CASE("log: the rate-limit key table stays within 256 entries")
{
    harness::log::SetFormat(harness::log::Format::Text);
    harness::log::SetGlobalCapForTest(100000);   // so that every distinct key is admitted within one window
    CaptureStderr([] {
        for (int i = 0; i < 1000; ++i)
            harness::log::Info(harness::log::Component::Main, "distinct key " + std::to_string(i));
    });
    CHECK(harness::log::KeyCountForTest() <= 256);
    CHECK(harness::log::KeyCountForTest() > 0);
    harness::log::SetGlobalCapForTest(20);
}
