// JSON-RPC helpers (spec: docs/specs/m3.md, Part A "Transport" - framing; api_spec.md "Errors").
#include "doctest/doctest.h"
#include "server/protocol.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace harness;
using nlohmann::json;

namespace {

std::array<uint8_t, 4> Le32(uint32_t v)
{
    return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}

std::optional<uint32_t> Parse(uint32_t v)
{
    const auto bytes = Le32(v);
    return ParseLengthPrefix(std::span<const uint8_t, 4>(bytes));
}

}  // namespace

TEST_CASE("protocol: error codes match api_spec.md")
{
    CHECK(kParseError == -32700);
    CHECK(kInvalidRequest == -32600);
    CHECK(kMethodNotFound == -32601);
    CHECK(kInvalidParams == -32602);
    CHECK(kInternalError == -32603);
    CHECK(kHarnessError == -32000);
    CHECK(kUnauthorized == -32001);
    CHECK(kNoSession == -32002);
    CHECK(kResourceLocked == -32003);
    CHECK(kCheckpointFailed == -32004);
    CHECK(kDiskNotFound == -32005);
    CHECK(kNotSupported == -32006);
    CHECK(kBusy == -32007);
    CHECK(kInvalidState == -32008);
    CHECK(kAudioNotBuffered == -32009);
    CHECK(kInputQueueFull == -32010);
    CHECK(std::string(kProtocolVersion) == "0.3");
    CHECK(kMaxMessageBytes == 16777216u);
    CHECK(kMaxRequestBytes == 1048576u);
}

TEST_CASE("protocol: length prefix is little-endian and rejects 0 and over 16 MiB")
{
    CHECK(Parse(1) == std::optional<uint32_t>(1));
    CHECK(Parse(0x00ABCDEF) == std::optional<uint32_t>(0x00ABCDEF));   // under 16 MiB, byte order matters
    CHECK(Parse(16777216) == std::optional<uint32_t>(16777216));
    CHECK(Parse(0) == std::nullopt);
    CHECK(Parse(16777217) == std::nullopt);
    CHECK(Parse(0xFFFFFFFFu) == std::nullopt);
}

TEST_CASE("protocol: the request limit is 1 MiB and an error can carry data")
{
    const auto parse = [](uint32_t v) {
        const auto bytes = Le32(v);
        return ParseLengthPrefix(std::span<const uint8_t, 4>(bytes), kMaxRequestBytes);
    };
    CHECK(parse(1048576) == std::optional<uint32_t>(1048576));
    CHECK(parse(1048577) == std::nullopt);
    CHECK(parse(0) == std::nullopt);
    CHECK(MakeError(1, kInputQueueFull, "Input queue full", json{{"pending", 4090}, {"limit", 4096}}) ==
          json{{"jsonrpc", "2.0"}, {"id", 1},
               {"error", {{"code", -32010}, {"message", "Input queue full"}, {"data", {{"pending", 4090}, {"limit", 4096}}}}}});
}

TEST_CASE("protocol: message builders")
{
    CHECK(MakeResult(7, json{{"a", 1}}) == json{{"jsonrpc", "2.0"}, {"id", 7}, {"result", {{"a", 1}}}});
    CHECK(MakeResult("x", json::object()) == json{{"jsonrpc", "2.0"}, {"id", "x"}, {"result", json::object()}});
    CHECK(MakeError(3, kNoSession, "no session") ==
          json{{"jsonrpc", "2.0"}, {"id", 3}, {"error", {{"code", -32002}, {"message", "no session"}}}});
    CHECK(MakeError(nullptr, kParseError, "parse error") ==
          json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}});
    const json n = MakeNotification("frame_ready", json{{"frame_id", 5}});
    CHECK(n == json{{"jsonrpc", "2.0"}, {"method", "frame_ready"}, {"params", {{"frame_id", 5}}}});
    CHECK(!n.contains("id"));
}

TEST_CASE("protocol: EncodeMessage prefixes the JSON text with its length")
{
    const json m = MakeResult(1, json::object());
    const std::string text = m.dump();
    const std::vector<uint8_t> wire = EncodeMessage(m);
    REQUIRE(wire.size() == text.size() + 4);
    const uint32_t len = uint32_t(wire[0]) | (uint32_t(wire[1]) << 8) | (uint32_t(wire[2]) << 16) | (uint32_t(wire[3]) << 24);
    CHECK(len == text.size());
    CHECK(std::string(wire.begin() + 4, wire.end()) == text);
}
