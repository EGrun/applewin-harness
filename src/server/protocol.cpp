#include "server/protocol.h"
#include <stdexcept>
#include <utility>

namespace harness {

nlohmann::json MakeResult(const nlohmann::json& id, nlohmann::json result)
{
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json MakeError(const nlohmann::json& id, int code, std::string message, nlohmann::json data)
{
    nlohmann::json error{{"code", code}, {"message", std::move(message)}};
    if (!data.is_null()) error["data"] = std::move(data);
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

nlohmann::json MakeNotification(std::string method, nlohmann::json params)
{
    return nlohmann::json{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

std::optional<uint32_t> ParseLengthPrefix(std::span<const uint8_t, 4> prefix, uint32_t maxBytes)
{
    const uint32_t n = uint32_t(prefix[0]) | (uint32_t(prefix[1]) << 8) | (uint32_t(prefix[2]) << 16) |
                       (uint32_t(prefix[3]) << 24);
    if (n == 0 || n > maxBytes) return std::nullopt;
    return n;
}

std::vector<uint8_t> EncodeMessage(const nlohmann::json& message)
{
    // Invalid UTF-8 in a string is replaced (U+FFFD) rather than throwing: the wire carries UTF-8 JSON only.
    const std::string text = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (text.size() > kMaxMessageBytes) throw std::length_error("EncodeMessage: message exceeds 16 MiB");
    const auto n = static_cast<uint32_t>(text.size());
    std::vector<uint8_t> wire;
    wire.reserve(text.size() + 4);
    wire.push_back(uint8_t(n & 0xFF));
    wire.push_back(uint8_t((n >> 8) & 0xFF));
    wire.push_back(uint8_t((n >> 16) & 0xFF));
    wire.push_back(uint8_t((n >> 24) & 0xFF));
    wire.insert(wire.end(), text.begin(), text.end());
    return wire;
}

}  // namespace harness
