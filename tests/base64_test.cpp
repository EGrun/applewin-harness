#include "doctest/doctest.h"
#include "core/base64.h"
#include <cstring>
#include <nlohmann/json.hpp>
#include <vector>

using namespace harness;

namespace {
std::string Enc(const char* s)
{
    return Base64Encode(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s), std::strlen(s)));
}
}  // namespace

TEST_CASE("base64: RFC 4648 vectors")
{
    CHECK(Enc("") == "");
    CHECK(Enc("f") == "Zg==");
    CHECK(Enc("fo") == "Zm8=");
    CHECK(Enc("foo") == "Zm9v");
    CHECK(Enc("foob") == "Zm9vYg==");
    CHECK(Enc("fooba") == "Zm9vYmE=");
    CHECK(Enc("foobar") == "Zm9vYmFy");
}

TEST_CASE("base64: 3 MB length and content")
{
    std::vector<uint8_t> v(3 * 1024 * 1024);
    for (size_t i = 0; i < v.size(); ++i) v[i] = uint8_t(i * 31);
    const std::string s = Base64Encode(v);
    CHECK(s.size() == v.size() / 3 * 4);
    CHECK(s.find('=') == std::string::npos);
    v.push_back(1);
    CHECK(Base64Encode(v).size() == (v.size() + 2) / 3 * 4);
}

TEST_CASE("json: round-trips an object")
{
    nlohmann::json j = {{"a", 1}, {"b", "x"}};
    const auto p = nlohmann::json::parse(j.dump());
    CHECK(p["a"] == 1);
    CHECK(p["b"] == "x");
}
