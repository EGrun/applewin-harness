// SPDX-License-Identifier: MIT
#pragma once
// The harness's single logger (spec: docs/specs/m6.md, "Structured logging"). Every diagnostic line goes through
// here: to stderr as `level: message` (text) or one JSON object per line, and, for warning and error, to the operator
// sink (the `log` notification). Thread-safe: called from the emulator thread and from the pipe threads.
//
// Rate limit: for each distinct (level, component, connection, message) at most one line per second is written. The
// others are counted, and the next line that passes carries " (N similar suppressed)". A client therefore cannot make
// the server log (to stderr or to operators) without bound. Never pass a token or client-typed text as a message.
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace harness::log {

enum class Level { Debug, Info, Warning, Error };
enum class Component { Main, Server, Session, Emulator, Audio };
enum class Format { Text, Json };

// "text" or "json" (exact, lower case); false for anything else.
bool ParseFormat(std::string_view name, Format& out);
void SetFormat(Format format);

// Writes one line. connection: the pipe connection the line is about (0 for none); it is part of the rate-limit key
// and a JSON field, never of the text.
void Write(Level level, Component component, std::string_view message, uint64_t connection = 0);
inline void Info(Component c, std::string_view m, uint64_t connection = 0) { Write(Level::Info, c, m, connection); }
inline void Warning(Component c, std::string_view m, uint64_t connection = 0) { Write(Level::Warning, c, m, connection); }
inline void Error(Component c, std::string_view m, uint64_t connection = 0) { Write(Level::Error, c, m, connection); }

// The operator fan-out: called (outside the logger's lock) for each warning and error that passes the rate limit, with
// the final message. It must not block. An empty function removes it.
using OperatorSink = std::function<void(Level level, const std::string& message, double emulatedMs)>;
void SetOperatorSink(OperatorSink sink);

// The emulated time (ms since power-on) reported in `log` notifications; the session updates it.
void SetEmulatedMs(double ms);

const char* LevelName(Level level);

}  // namespace harness::log
