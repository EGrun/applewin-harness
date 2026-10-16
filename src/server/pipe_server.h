#pragma once
// The ACL'd, overlapped named-pipe transport (spec: docs/specs/m3.md, Part A "Transport"; D3).
#include "host/emulator.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace harness {

enum class Role { Agent, Operator };

// One token per role: 64 lowercase hex characters each (32 bytes from BCryptGenRandom).
struct Tokens {
    std::string agent;
    std::string operatorToken;
};

// One accepted, authenticated pipe connection. Its role is fixed by the hello handshake.
struct Connection {
    virtual ~Connection() = default;
    virtual Role role() const = 0;
    // Frames and writes one whole message under the connection's mutex. Thread-safe. Does nothing once the
    // connection has closed (a client that went away never makes Send throw).
    virtual void Send(const nlohmann::json& message) = 0;
    virtual uint64_t id() const = 0;   // unique within the process; never 0
};
using ConnectionPtr = std::shared_ptr<Connection>;

// Creates up to 8 instances of \\.\pipe\<name>, each served by its own thread with overlapped I/O.
// Per connection, before onRequest:
// - framing: 4-byte little-endian length; 0 or over 16 MiB closes the connection; a truncated message closes it;
// - the handshake: the first message must be hello { protocol_version: "0.3", role, token }. A wrong token or role,
//   or any other method first: -32001 and close. Another protocol_version: -32600 and close. The reply is
//   { protocol_version: "0.3", role, display_mode: "windowed" | "headless" };
// - parse errors: -32700 (the connection stays open); not a JSON-RPC 2.0 request object: -32600; client
//   notifications (no id) are ignored;
// - roles: an agent connection calling a method for which isOperatorOnly returns true gets -32001 and stays open.
// Every other request is passed to onRequest on the connection's thread. Replies to it are the handler's job.
class PipeServer {
public:
    using RequestHandler = std::function<void(ConnectionPtr, nlohmann::json request)>;
    using MethodPredicate = std::function<bool(std::string_view method)>;

    // name: the full pipe name, \\.\pipe\applewin-harness-<32 hex>.
    PipeServer(std::wstring name, Tokens tokens, RequestHandler onRequest, MethodPredicate isOperatorOnly,
               DisplayMode display);
    ~PipeServer();   // Stop()
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    // Creates the first instance (FILE_FLAG_FIRST_PIPE_INSTANCE, a DACL with a single allow ACE for the current
    // user, PIPE_REJECT_REMOTE_CLIENTS) and starts accepting. Throws std::runtime_error on failure.
    void Start();
    // Cancels all I/O, closes every connection and joins the threads. Safe to call twice.
    void Stop();

    static std::wstring RandomPipeName();   // \\.\pipe\applewin-harness-<32 random hex>
    static std::string RandomToken();       // 64 hex characters from 32 BCryptGenRandom bytes

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace harness
