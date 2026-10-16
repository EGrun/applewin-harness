// AppleWinHarness.exe --serve (spec: docs/specs/m3.md, Part A "Launch", "Transport", pass conditions 2, 3 and 6).
#include "doctest/doctest.h"
#include "support/golden.h"
#include "support/paths.h"
#include "support/server_process.h"
#include <windows.h>
#include <aclapi.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using nlohmann::json;
using namespace harness::testsupport;

namespace {

fs::path HarnessTempDir() { return fs::temp_directory_path() / "applewin-harness"; }

// A launched server and its parsed announcement. REQUIREs both.
struct Server {
    std::unique_ptr<ServerProcess> process;
    Announcement announcement;
    std::string line;
};

Server StartServer(const std::wstring& args = L"--serve --headless")
{
    Server s;
    s.process = std::make_unique<ServerProcess>(args);
    REQUIRE(s.process->Launched());
    const auto line = s.process->WaitForLine(15s);
    REQUIRE_MESSAGE(line.has_value(), "no announcement line within 15 s; stdout: " << s.process->Stdout()
                                                                                  << " stderr: " << s.process->Stderr());
    s.line = *line;
    const auto a = ParseAnnouncement(*line);
    REQUIRE_MESSAGE(a.has_value(), "announcement is not the expected JSON: " << *line);
    s.announcement = *a;
    return s;
}

json HelloParams(const std::string& role, const std::string& token, const std::string& version = "0.3")
{
    return {{"protocol_version", version}, {"role", role}, {"token", token}};
}

// Opens a connection and says hello; REQUIREs a successful reply.
std::unique_ptr<PipeClient> Connect(const Server& s, const std::string& role)
{
    auto c = std::make_unique<PipeClient>();
    REQUIRE(c->Open(s.announcement.pipe));
    const std::string& token = role == "agent" ? s.announcement.agentToken : s.announcement.operatorToken;
    const auto reply = c->Call("hello", HelloParams(role, token));
    REQUIRE_MESSAGE(reply.has_value(), "no reply to hello");
    REQUIRE_MESSAGE(reply->contains("result"), "hello failed: " << reply->dump());
    return c;
}

// The error code of a reply, or 0 if it has none.
int ErrorCode(const std::optional<json>& reply)
{
    if (!reply || !reply->contains("error") || !(*reply)["error"].contains("code")) return 0;
    return (*reply)["error"]["code"].get<int>();
}

std::string Dump(const std::optional<json>& reply) { return reply ? reply->dump() : std::string("<no reply>"); }

// A fresh connection can still say hello: the server survived whatever the test did.
void CheckServerStillServes(const Server& s)
{
    CHECK(s.process->Running());
    PipeClient c;
    REQUIRE(c.Open(s.announcement.pipe));
    const auto reply = c.Call("hello", HelloParams("operator", s.announcement.operatorToken));
    CHECK_MESSAGE(reply.has_value(), "no reply to hello on a new connection");
    if (reply) CHECK(reply->contains("result"));
}

void SendPrefix(PipeClient& c, uint32_t n)
{
    const uint8_t prefix[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
    REQUIRE(c.WriteRaw(prefix, 4));
}

std::string DosImageUtf8() { return DosMasterImage().string(); }

}  // namespace

// ---- Launch ----

TEST_CASE("server: the announcement is one JSON line with the pipe, both tokens and the pid")
{
    Server s = StartServer();
    // Spec format: {"pipe":"\\\\.\\pipe\\applewin-harness-<32 hex>","agent_token":"<64 hex>","operator_token":"<64 hex>","pid":<n>}
    static const std::regex kFormat(
        R"(\{"pipe":"\\\\\\\\\.\\\\pipe\\\\applewin-harness-[0-9a-f]{32}","agent_token":"[0-9a-f]{64}","operator_token":"[0-9a-f]{64}","pid":[1-9][0-9]*\})");
    CHECK_MESSAGE(std::regex_match(s.line, kFormat), "announcement: " << s.line);

    const json j = json::parse(s.line, nullptr, false);
    REQUIRE(j.is_object());
    CHECK(j.size() == 4);
    CHECK(s.announcement.pid == s.process->Pid());
    CHECK(s.announcement.agentToken != s.announcement.operatorToken);

    // Nothing else is written to stdout.
    std::this_thread::sleep_for(500ms);
    const std::string out = s.process->Stdout();
    CHECK_MESSAGE((out == s.line + "\n" || out == s.line + "\r\n"), "stdout: " << out);

    PipeClient c;
    CHECK(c.Open(s.announcement.pipe));
    CHECK(c.ServerPid() == s.announcement.pid);   // the pipe is served by the announced process
}

TEST_CASE("server: --serve with CLI-mode options exits 2")
{
    const std::wstring dos = L"\"" + DosMasterImage().wstring() + L"\"";
    const std::wstring cases[] = {
        L"--serve --disk1 " + dos,
        L"--serve --headless --disk1 " + dos,
        L"--serve --realtime",
        L"--serve --png out.png",
        L"--serve --frames 1",
        L"--serve --headless --type A",
        L"--serve --serve",
    };
    for (const auto& args : cases) {
        INFO("args: " << fs::path(args).string());
        ServerProcess p(args);
        REQUIRE(p.Launched());
        const auto code = p.WaitExit(30s);
        REQUIRE_MESSAGE(code.has_value(), "did not exit within 30 s");
        CHECK(*code == 2);
    }
}

// ---- Security (pass condition 2) ----

TEST_CASE("server: the pipe's DACL has exactly one allow ACE, for the current user")
{
    Server s = StartServer();
    PipeClient c;
    REQUIRE(c.Open(s.announcement.pipe));

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(GetSecurityInfo(c.Handle(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                            &sd) == ERROR_SUCCESS);
    struct FreeSd {
        PSECURITY_DESCRIPTOR p;
        ~FreeSd() { LocalFree(p); }
    } freeSd{sd};
    REQUIRE(dacl != nullptr);   // a null DACL would allow everyone
    REQUIRE(dacl->AceCount == 1);
    void* ace = nullptr;
    REQUIRE(GetAce(dacl, 0, &ace));
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    REQUIRE(header->AceType == ACCESS_ALLOWED_ACE_TYPE);
    const auto* allow = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
    const ACCESS_MASK m = allow->Mask;
    const bool generic = (m & GENERIC_READ) && (m & GENERIC_WRITE);
    const bool mapped = (m & FILE_READ_DATA) && (m & FILE_WRITE_DATA);
    CHECK_MESSAGE((generic || mapped), "mask " << m);

    HANDLE token = nullptr;
    REQUIRE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> buf(size);
    const BOOL got = GetTokenInformation(token, TokenUser, buf.data(), size, &size);
    CloseHandle(token);
    REQUIRE(got);
    const PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    CHECK(EqualSid(const_cast<DWORD*>(&allow->SidStart), user));
}

TEST_CASE("server: hello replies with the protocol version, role and display mode")
{
    Server s = StartServer();
    PipeClient op, agent;
    REQUIRE(op.Open(s.announcement.pipe));
    REQUIRE(agent.Open(s.announcement.pipe));   // two connections at once
    const auto r1 = op.Call("hello", HelloParams("operator", s.announcement.operatorToken));
    const auto r2 = agent.Call("hello", HelloParams("agent", s.announcement.agentToken));
    CHECK(Dump(r1) == json({{"jsonrpc", "2.0"}, {"id", 1},
                            {"result", {{"protocol_version", "0.3"}, {"role", "operator"}, {"display_mode", "headless"}}}})
                          .dump());
    CHECK(Dump(r2) == json({{"jsonrpc", "2.0"}, {"id", 1},
                            {"result", {{"protocol_version", "0.3"}, {"role", "agent"}, {"display_mode", "headless"}}}})
                          .dump());
}

TEST_CASE("server: a wrong token or role gets -32001 and the connection closes")
{
    Server s = StartServer();
    const std::string zeros(64, '0');
    const json cases[] = {
        HelloParams("operator", zeros),
        HelloParams("operator", s.announcement.agentToken),   // the agent's token does not make an operator
        HelloParams("agent", s.announcement.operatorToken),
        HelloParams("admin", s.announcement.operatorToken),
        json{{"protocol_version", "0.3"}, {"role", "operator"}},   // no token
    };
    for (const auto& params : cases) {
        INFO("hello params: " << params.dump());
        PipeClient c;
        REQUIRE(c.Open(s.announcement.pipe));
        const auto reply = c.Call("hello", params, 10s);
        CHECK_MESSAGE(ErrorCode(reply) == -32001, Dump(reply));
        CHECK(c.WaitClosed());
    }
    CheckServerStillServes(s);
}

TEST_CASE("server: any other method before hello gets -32001 and the connection closes")
{
    Server s = StartServer();
    for (const char* method : {"get_frame", "step_frames", "create_session"}) {
        INFO("method: " << method);
        PipeClient c;
        REQUIRE(c.Open(s.announcement.pipe));
        const auto reply = c.Call(method, json::object(), 10s);
        CHECK_MESSAGE(ErrorCode(reply) == -32001, Dump(reply));
        CHECK(c.WaitClosed());
    }
    CheckServerStillServes(s);
}

TEST_CASE("server: protocol_version 0.2 gets -32600 and the connection closes")
{
    Server s = StartServer();
    PipeClient c;
    REQUIRE(c.Open(s.announcement.pipe));
    const auto reply = c.Call("hello", HelloParams("operator", s.announcement.operatorToken, "0.2"), 10s);
    CHECK_MESSAGE(ErrorCode(reply) == -32600, Dump(reply));
    CHECK(c.WaitClosed());
}

TEST_CASE("server: an agent calling an operator method gets -32001 and stays open")
{
    Server s = StartServer();
    auto agent = Connect(s, "agent");
    for (const char* method : {"create_session", "close_session", "pause", "resume", "step_frames"}) {
        INFO("method: " << method);
        const auto reply = agent->Call(method, method == std::string("step_frames") ? json{{"frames", 1}} : json::object(), 10s);
        CHECK_MESSAGE(ErrorCode(reply) == -32001, Dump(reply));
    }
    CHECK(agent->StaysOpen());
    CHECK(s.process->Running());   // close_session from an agent did nothing
    CHECK(ErrorCode(agent->Call("pause", json::object(), 10s)) == -32001);
}

// ---- Framing (pass condition 3) ----

TEST_CASE("server: a zero length prefix closes the connection")
{
    Server s = StartServer();
    auto c = Connect(s, "operator");
    SendPrefix(*c, 0);
    CHECK(c->WaitClosed());
    CheckServerStillServes(s);
}

TEST_CASE("server: a length prefix over 16 MiB closes the connection")
{
    Server s = StartServer();
    auto c = Connect(s, "operator");
    SendPrefix(*c, 16u * 1024u * 1024u + 1u);
    CHECK(c->WaitClosed());
    CheckServerStillServes(s);

    PipeClient beforeHello;   // also before hello
    REQUIRE(beforeHello.Open(s.announcement.pipe));
    SendPrefix(beforeHello, 0xFFFFFFFFu);
    CHECK(beforeHello.WaitClosed());
    CheckServerStillServes(s);
}

TEST_CASE("server: a connection that has not said hello within 10 s is closed, freeing its slot")
{
    Server s = StartServer();
    std::vector<std::unique_ptr<PipeClient>> idle;   // all 8 instances held by clients that never speak
    for (int i = 0; i < 8; ++i) {
        auto c = std::make_unique<PipeClient>();
        REQUIRE(c->Open(s.announcement.pipe));
        idle.push_back(std::move(c));
    }
    const auto start = std::chrono::steady_clock::now();
    for (auto& c : idle) CHECK_MESSAGE(c->WaitClosed(14s), "an idle connection was not closed within 14 s");
    CHECK(std::chrono::steady_clock::now() - start < 14s);
    CHECK(s.process->Running());
    auto op = Connect(s, "operator");   // the slots are free again
    CHECK(op->StaysOpen(200ms));
}

TEST_CASE("server: a truncated message closes only that connection")
{
    Server s = StartServer();
    auto op = Connect(s, "operator");
    {
        auto agent = Connect(s, "agent");
        SendPrefix(*agent, 100);
        const char half[] = "{\"jsonrpc\"";
        REQUIRE(agent->WriteRaw(half, sizeof(half) - 1));
        agent->Close();   // the client goes away mid-message
    }
    std::this_thread::sleep_for(200ms);
    CHECK(op->StaysOpen(200ms));
    REQUIRE(op->SendBody("{not json"));
    CHECK(ErrorCode(op->ReadAnyReply()) == -32700);   // the other connection is still served
    CheckServerStillServes(s);
}

TEST_CASE("server: a client that never reads does not block the server and is closed at a 64 MiB backlog")
{
    Server s = StartServer();
    auto other = Connect(s, "operator");
    auto stalled = Connect(s, "agent");
    // Each invalid request echoes its 1 MiB string id in a -32600 reply, which this client never reads. A server
    // whose sends block stops reading after the first reply fills the pipe, and these writes then time out.
    const std::vector<uint8_t> frame =
        PipeClient::Frame(json{{"jsonrpc", "2.0"}, {"id", std::string(1024 * 1024, 'x')}}.dump());
    enum class W { Ok, Timeout, Closed };
    const auto write = [&](std::chrono::milliseconds timeout) {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.hEvent = ev;
        W result = W::Ok;
        DWORD done = 0;
        if (!WriteFile(stalled->Handle(), frame.data(), DWORD(frame.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            result = W::Closed;
        } else if (WaitForSingleObject(ev, DWORD(timeout.count())) != WAIT_OBJECT_0) {
            CancelIoEx(stalled->Handle(), &ov);
            GetOverlappedResult(stalled->Handle(), &ov, &done, TRUE);
            result = W::Timeout;
        } else if (!GetOverlappedResult(stalled->Handle(), &ov, &done, FALSE) || done != frame.size()) {
            result = W::Closed;
        }
        CloseHandle(ev);
        return result;
    };
    W last = W::Ok;
    int sent = 0;
    for (; sent < 100 && last == W::Ok; ++sent) {
        last = write(5s);
        if (sent == 32) {   // about 32 MiB queued for the stalled client: the other connection is served promptly
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(ErrorCode(other->Call("no_such_method", json::object(), 2s)) == -32601);
            CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        }
    }
    INFO("requests written: " << sent);
    CHECK(last != W::Timeout);   // the server kept reading
    CHECK((last == W::Closed || stalled->WaitClosed(10s)));
    CHECK(sent > 60);   // not closed before the backlog reached 64 MiB
    CheckServerStillServes(s);
}

TEST_CASE("server: malformed JSON gets -32700 and the connection stays open")
{
    Server s = StartServer();
    auto c = Connect(s, "operator");
    for (const std::string body : {std::string("{not json"), std::string("[1,"), std::string("{\"a\":\"\xff\"}")}) {
        INFO("body: " << body);
        REQUIRE(c->SendBody(body));
        const auto reply = c->ReadAnyReply();
        CHECK_MESSAGE(ErrorCode(reply) == -32700, Dump(reply));
        if (reply) CHECK((*reply)["id"].is_null());
        CHECK(c->StaysOpen());
    }
}

TEST_CASE("server: a message that is not a JSON-RPC 2.0 request gets -32600")
{
    Server s = StartServer();
    auto c = Connect(s, "operator");
    const json cases[] = {
        json{{"id", 5}, {"method", "pause"}},                         // no "jsonrpc"
        json{{"jsonrpc", "1.0"}, {"id", 6}, {"method", "pause"}},     // wrong version
        json{{"jsonrpc", "2.0"}, {"id", 7}},                          // no method
        json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", 42}},          // method not a string
        json::array({1, 2}),                                          // not an object
    };
    for (const auto& m : cases) {
        INFO("message: " << m.dump());
        REQUIRE(c->Send(m));
        const auto reply = c->ReadAnyReply();
        CHECK_MESSAGE(ErrorCode(reply) == -32600, Dump(reply));
    }
    CHECK(c->StaysOpen());
}

TEST_CASE("server: notifications from clients are ignored")
{
    Server s = StartServer();
    auto c = Connect(s, "operator");
    REQUIRE(c->Send(json{{"jsonrpc", "2.0"}, {"method", "pause"}}));          // no id: a notification
    REQUIRE(c->Send(json{{"jsonrpc", "2.0"}, {"method", "no_such_method"}}));
    REQUIRE(c->SendBody("{not json"));
    const auto reply = c->ReadAnyReply();   // the first reply is the parse error's: nothing answered the notifications
    CHECK_MESSAGE(ErrorCode(reply) == -32700, Dump(reply));
    CHECK(c->StaysOpen());
}

// ---- Methods (need the session: red until Task 5) ----

TEST_CASE("server: an unknown method gets -32601")
{
    Server s = StartServer();
    auto op = Connect(s, "operator");
    auto agent = Connect(s, "agent");
    CHECK(ErrorCode(op->Call("no_such_method", json::object(), 10s)) == -32601);
    CHECK(ErrorCode(agent->Call("no_such_method", json::object(), 10s)) == -32601);
    CHECK(op->StaysOpen());
    CHECK(agent->StaysOpen());
}

TEST_CASE("server: close_session replies {} and the process exits 0 without leftovers")
{
    Server s = StartServer();
    auto op = Connect(s, "operator");
    const json disks = json::array({{{"slot", 6}, {"drive", 1}, {"path", DosImageUtf8()}}});
    const auto created = op->Call("create_session", {{"disks", disks}, {"run_mode", "stepped"}});
    REQUIRE_MESSAGE((created && created->contains("result")), Dump(created));
    const fs::path work = HarnessTempDir() / std::to_string(s.process->Pid());
    CHECK(fs::exists(work / DosMasterImage().filename()));   // the working copy

    const auto closed = op->Call("close_session", json::object());
    REQUIRE_MESSAGE((closed && closed->contains("result")), Dump(closed));
    CHECK((*closed)["result"] == json::object());
    const auto code = s.process->WaitExit(15s);
    REQUIRE_MESSAGE(code.has_value(), "did not exit within 15 s of close_session");
    CHECK(*code == 0);
    CHECK(!fs::exists(work));
    CHECK(!fs::exists(HarnessTempDir() / (std::to_string(s.process->Pid()) + ".ini")));
}

// ---- Windowed: needs a desktop session (pass condition 6) ----

TEST_SUITE("windowed")
{
    TEST_CASE("server: windowed, closing the window after create_session exits 0 and removes the work dir")
    {
        Server s = StartServer(L"--serve");
        auto op = Connect(s, "operator");
        const json disks = json::array({{{"slot", 6}, {"drive", 1}, {"path", DosImageUtf8()}}});
        const auto created = op->Call("create_session", {{"disks", disks}, {"run_mode", "realtime"}});
        REQUIRE_MESSAGE((created && created->contains("result")), Dump(created));
        const DWORD pid = s.process->Pid();
        const fs::path work = HarnessTempDir() / std::to_string(pid);
        CHECK(fs::exists(work));

        HWND h = nullptr;
        for (auto deadline = std::chrono::steady_clock::now() + 10s; std::chrono::steady_clock::now() < deadline;) {
            h = FindHarnessWindowOf(pid);
            if (h && IsWindowVisible(h)) break;
            h = nullptr;
            std::this_thread::sleep_for(50ms);
        }
        REQUIRE_MESSAGE(h != nullptr, "no visible AppleWinHarnessWindow owned by the server within 10 s");
        REQUIRE(PostMessageW(h, WM_CLOSE, 0, 0));
        const auto code = s.process->WaitExit(30s);
        REQUIRE_MESSAGE(code.has_value(), "did not exit within 30 s of WM_CLOSE");
        CHECK(*code == 0);
        CHECK(!fs::exists(work));
        CHECK(!fs::exists(HarnessTempDir() / (std::to_string(pid) + ".ini")));
    }
}
