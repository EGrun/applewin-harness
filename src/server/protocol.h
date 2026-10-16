#pragma once
// JSON-RPC 2.0 helpers and constants for the harness protocol (spec: docs/specs/m3.md, Part A "Transport" and "Errors";
// error codes from docs/api_spec.md).
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace harness {

inline constexpr const char* kProtocolVersion = "0.3";
inline constexpr uint32_t kMaxMessageBytes = 16u * 1024u * 1024u;   // 16 MiB; a length prefix above this closes the connection

// Standard JSON-RPC 2.0 codes.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;   // message carries the C++ exception's what()

// Harness codes (api_spec.md, "Errors").
inline constexpr int kHarnessError = -32000;
inline constexpr int kUnauthorized = -32001;      // bad token or role, method before hello, operator method from an agent
inline constexpr int kNoSession = -32002;
inline constexpr int kResourceLocked = -32003;
inline constexpr int kCheckpointFailed = -32004;
inline constexpr int kDiskNotFound = -32005;
inline constexpr int kNotSupported = -32006;
inline constexpr int kBusy = -32007;
inline constexpr int kInvalidState = -32008;
inline constexpr int kAudioNotBuffered = -32009;

// A JSON-RPC error a method handler reports; method handlers throw it and the dispatcher turns it into MakeError.
struct RpcError {
    int code;
    std::string message;
};

// {"jsonrpc":"2.0","id":id,"result":result}
nlohmann::json MakeResult(const nlohmann::json& id, nlohmann::json result);
// {"jsonrpc":"2.0","id":id,"error":{"code":code,"message":message}}; id is null when the request's id is unknown.
nlohmann::json MakeError(const nlohmann::json& id, int code, std::string message);
// {"jsonrpc":"2.0","method":method,"params":params} (no id).
nlohmann::json MakeNotification(std::string method, nlohmann::json params);

// The 4-byte little-endian length prefix. nullopt for 0 or anything over kMaxMessageBytes (the connection closes).
std::optional<uint32_t> ParseLengthPrefix(std::span<const uint8_t, 4> prefix);
// The whole wire message: 4-byte little-endian length, then the UTF-8 JSON text (message.dump()).
std::vector<uint8_t> EncodeMessage(const nlohmann::json& message);

}  // namespace harness
