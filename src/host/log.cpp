// SPDX-License-Identifier: MIT
#include "host/log.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace harness::log {
namespace {

constexpr std::chrono::seconds kWindow{1};
constexpr size_t kMaxKeys = 256;   // bounds the rate-limit table: older entries are dropped when it grows past this

struct Key {
    Level level;
    Component component;
    uint64_t connection;
    std::string message;
    bool operator<(const Key& o) const
    {
        return std::tie(level, component, connection, message) < std::tie(o.level, o.component, o.connection, o.message);
    }
};

struct Entry {
    std::chrono::steady_clock::time_point windowStart;
    uint64_t suppressed = 0;
};

std::mutex g_mutex;   // the rate-limit table, the format and the stderr write
Format g_format = Format::Text;
std::map<Key, Entry> g_entries;
std::shared_ptr<const OperatorSink> g_sink;   // guarded by g_mutex
std::atomic<double> g_emulatedMs{0.0};

const char* ComponentName(Component c)
{
    switch (c) {
    case Component::Main: return "main";
    case Component::Server: return "server";
    case Component::Session: return "session";
    case Component::Emulator: return "emulator";
    case Component::Audio: return "audio";
    }
    return "main";
}

std::string Timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms.count()));
    return buf;
}

}  // namespace

const char* LevelName(Level level)
{
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    }
    return "info";
}

bool ParseFormat(std::string_view name, Format& out)
{
    if (name == "text") out = Format::Text;
    else if (name == "json") out = Format::Json;
    else return false;
    return true;
}

void SetFormat(Format format)
{
    std::lock_guard lock(g_mutex);
    g_format = format;
}

void SetOperatorSink(OperatorSink sink)
{
    std::shared_ptr<const OperatorSink> p = sink ? std::make_shared<const OperatorSink>(std::move(sink)) : nullptr;
    std::lock_guard lock(g_mutex);
    g_sink = std::move(p);
}

void SetEmulatedMs(double ms)
{
    g_emulatedMs.store(ms);
}

void Write(Level level, Component component, std::string_view messageView, uint64_t connection)
{
    std::string message(messageView);
    std::shared_ptr<const OperatorSink> sink;
    {
        std::lock_guard lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();
        Key key{level, component, connection, message};
        auto it = g_entries.find(key);
        if (it != g_entries.end() && now - it->second.windowStart < kWindow) {
            ++it->second.suppressed;
            return;
        }
        uint64_t suppressed = 0;
        if (it != g_entries.end()) {
            suppressed = it->second.suppressed;
            it->second = Entry{now, 0};
        } else {
            if (g_entries.size() >= kMaxKeys)
                for (auto e = g_entries.begin(); e != g_entries.end();)
                    e = now - e->second.windowStart >= kWindow ? g_entries.erase(e) : std::next(e);
            g_entries.emplace(std::move(key), Entry{now, 0});
        }
        if (suppressed) message += " (" + std::to_string(suppressed) + " similar suppressed)";

        std::string line;
        if (g_format == Format::Json) {
            nlohmann::ordered_json j;
            j["ts"] = Timestamp();
            j["level"] = LevelName(level);
            j["msg"] = message;
            j["component"] = ComponentName(component);
            if (connection) j["connection"] = connection;
            line = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        } else {
            line = std::string(LevelName(level)) + ": " + message;
        }
        line.push_back('\n');
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
        if (level == Level::Warning || level == Level::Error) sink = g_sink;
    }
    if (sink && *sink) {
        try {
            (*sink)(level, message, g_emulatedMs.load());
        } catch (...) {
        }
    }
}

}  // namespace harness::log
