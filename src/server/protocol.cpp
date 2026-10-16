#include "server/protocol.h"
#include <stdexcept>

// Task 2 stubs: implemented in Task 4.

namespace harness {

nlohmann::json MakeResult(const nlohmann::json&, nlohmann::json)
{
    throw std::logic_error("MakeResult: not implemented");
}

nlohmann::json MakeError(const nlohmann::json&, int, std::string)
{
    throw std::logic_error("MakeError: not implemented");
}

nlohmann::json MakeNotification(std::string, nlohmann::json)
{
    throw std::logic_error("MakeNotification: not implemented");
}

std::optional<uint32_t> ParseLengthPrefix(std::span<const uint8_t, 4>)
{
    throw std::logic_error("ParseLengthPrefix: not implemented");
}

std::vector<uint8_t> EncodeMessage(const nlohmann::json&)
{
    throw std::logic_error("EncodeMessage: not implemented");
}

}  // namespace harness
