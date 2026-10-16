#include "server/pipe_server.h"
#include "server/protocol.h"
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace harness {

namespace {

constexpr DWORD kMaxInstances = 8;
constexpr DWORD kPipeBufferBytes = 64 * 1024;

std::string Win32Message(const char* what, DWORD err)
{
    return std::string(what) + " failed (error " + std::to_string(err) + ")";
}

std::string Hex(const uint8_t* p, size_t n)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kDigits[p[i] >> 4]);
        s.push_back(kDigits[p[i] & 0xF]);
    }
    return s;
}

std::string RandomHex(size_t bytes)
{
    std::vector<uint8_t> buf(bytes);
    const NTSTATUS st = BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) throw std::runtime_error("BCryptGenRandom failed");
    return Hex(buf.data(), buf.size());
}

// Constant-time in the expected token's length: no early exit on the first differing byte.
bool TokenEquals(const std::string& expected, const std::string& given)
{
    unsigned diff = expected.size() == given.size() ? 0u : 1u;
    for (size_t i = 0; i < expected.size(); ++i) {
        const unsigned char g = i < given.size() ? static_cast<unsigned char>(given[i]) : static_cast<unsigned char>(0);
        diff |= static_cast<unsigned char>(expected[i]) ^ g;
    }
    return diff == 0 && !expected.empty();
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE v = nullptr)
    {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = v;
    }
    HANDLE release() { return std::exchange(h, nullptr); }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

HANDLE NewEvent()
{
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!e) throw std::runtime_error(Win32Message("CreateEventW", GetLastError()));
    return e;
}

// Waits for an issued overlapped operation, or cancels it if `cancel` is signalled first. Returns the bytes
// transferred, or nullopt on failure or cancellation. Never returns while the operation is still in flight.
std::optional<DWORD> FinishOverlapped(HANDLE file, OVERLAPPED& ov, HANDLE cancel)
{
    const HANDLE waits[2] = {ov.hEvent, cancel};
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0) CancelIoEx(file, &ov);
    DWORD n = 0;
    const BOOL ok = GetOverlappedResult(file, &ov, &n, TRUE);   // completes (or aborts) the operation either way
    if (!ok || w != WAIT_OBJECT_0) return std::nullopt;
    return n;
}

// One accepted pipe instance. Owned jointly by its thread and whoever keeps the ConnectionPtr (the session); the
// pipe handle is closed by the thread when the connection ends, the events when the last owner lets go.
class PipeConnection final : public Connection {
public:
    PipeConnection(HANDLE pipe, uint64_t id) : pipe_(pipe), id_(id), cancel_(NewEvent()), writeEvent_(NewEvent()) {}

    ~PipeConnection() override { CloseHandleLocked(); }

    Role role() const override { return role_.load(); }
    uint64_t id() const override { return id_; }

    void Send(const nlohmann::json& message) override
    {
        const std::vector<uint8_t> wire = EncodeMessage(message);
        std::lock_guard lock(writeMutex_);
        if (closed_.load() || !pipe_) return;
        size_t done = 0;
        while (done < wire.size()) {
            OVERLAPPED ov{};
            ov.hEvent = writeEvent_.h;
            ResetEvent(writeEvent_.h);
            const DWORD chunk = static_cast<DWORD>(wire.size() - done);   // <= 16 MiB + 4
            DWORD n = 0;
            if (WriteFile(pipe_.h, wire.data() + done, chunk, &n, &ov)) {
                // Completed synchronously; the event and GetOverlappedResult agree with n.
                if (!GetOverlappedResult(pipe_.h, &ov, &n, TRUE)) return MarkClosed();
            } else if (GetLastError() == ERROR_IO_PENDING) {
                const auto r = FinishOverlapped(pipe_.h, ov, cancel_.h);
                if (!r) return MarkClosed();
                n = *r;
            } else {
                return MarkClosed();
            }
            if (n == 0) return MarkClosed();
            done += n;
        }
    }

    // Reads exactly n bytes; false on disconnect, error or cancellation.
    bool ReadExact(uint8_t* p, size_t n, HANDLE readEvent)
    {
        size_t got = 0;
        while (got < n) {
            if (closed_.load()) return false;
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD r = 0;
            const DWORD want = static_cast<DWORD>(n - got);
            if (ReadFile(pipe_.h, p + got, want, &r, &ov)) {
                if (!GetOverlappedResult(pipe_.h, &ov, &r, TRUE)) return false;
            } else if (GetLastError() == ERROR_IO_PENDING) {
                const auto res = FinishOverlapped(pipe_.h, ov, cancel_.h);
                if (!res) return false;
                r = *res;
            } else {
                return false;
            }
            if (r == 0) return false;
            got += r;
        }
        return true;
    }

    // Any thread: stops pending and future I/O. The handle stays open until Finish.
    void Cancel()
    {
        closed_.store(true);
        SetEvent(cancel_.h);
    }

    // The connection thread, last: waits for an in-flight Send, then closes the handle. Data already written stays
    // readable by the client (no DisconnectNamedPipe, which would discard it).
    void Finish()
    {
        Cancel();
        CloseHandleLocked();
    }

    void SetRole(Role r) { role_.store(r); }

private:
    void MarkClosed() { closed_.store(true); }

    void CloseHandleLocked()
    {
        std::lock_guard lock(writeMutex_);
        pipe_.reset();
    }

    Handle pipe_;
    const uint64_t id_;
    std::atomic<Role> role_{Role::Agent};
    std::atomic<bool> closed_{false};
    Handle cancel_;       // manual-reset; set once, never reset
    Handle writeEvent_;   // used only under writeMutex_
    std::mutex writeMutex_;
};

std::atomic<uint64_t> g_nextConnectionId{1};

}  // namespace

struct PipeServer::Impl {
    std::wstring name;
    Tokens tokens;
    RequestHandler onRequest;
    MethodPredicate isOperatorOnly;
    DisplayMode display;

    // Security: one allow ACE for the token user.
    std::vector<uint8_t> tokenUser;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};

    Handle stopEvent;
    Handle slotEvent;   // auto-reset: a connection ended
    std::thread acceptor;
    bool started = false;
    bool stopped = false;

    struct Record {
        std::shared_ptr<PipeConnection> conn;
        std::thread thread;
        std::atomic<bool> done{false};
    };
    std::mutex recordsMutex;
    std::list<Record> records;
    std::atomic<DWORD> active{0};

    ~Impl()
    {
        if (acl) LocalFree(acl);
    }

    void BuildSecurity()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            throw std::runtime_error(Win32Message("OpenProcessToken", GetLastError()));
        Handle tokenHandle(token);
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0) throw std::runtime_error(Win32Message("GetTokenInformation", GetLastError()));
        tokenUser.resize(size);
        if (!GetTokenInformation(token, TokenUser, tokenUser.data(), size, &size))
            throw std::runtime_error(Win32Message("GetTokenInformation", GetLastError()));
        PSID sid = reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid;

        EXPLICIT_ACCESS_W ea{};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        const DWORD err = SetEntriesInAclW(1, &ea, nullptr, &acl);
        if (err != ERROR_SUCCESS) throw std::runtime_error(Win32Message("SetEntriesInAclW", err));
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
            throw std::runtime_error(Win32Message("InitializeSecurityDescriptor", GetLastError()));
        if (!SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE))
            throw std::runtime_error(Win32Message("SetSecurityDescriptorDacl", GetLastError()));
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
    }

    HANDLE CreateInstance(bool first)
    {
        DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        return CreateNamedPipeW(name.c_str(), openMode,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                kMaxInstances, kPipeBufferBytes, kPipeBufferBytes, 0, &sa);
    }

    bool Stopping() const { return WaitForSingleObject(stopEvent.h, 0) == WAIT_OBJECT_0; }

    // Joins and drops the connections whose threads have finished.
    void Reap()
    {
        std::list<Record> finished;
        {
            std::lock_guard lock(recordsMutex);
            for (auto it = records.begin(); it != records.end();) {
                auto next = std::next(it);
                if (it->done.load()) finished.splice(finished.end(), records, it);
                it = next;
            }
        }
        for (auto& r : finished)
            if (r.thread.joinable()) r.thread.join();
    }

    void Spawn(HANDLE pipe)
    {
        std::shared_ptr<PipeConnection> conn;
        try {
            conn = std::make_shared<PipeConnection>(pipe, g_nextConnectionId.fetch_add(1));
        } catch (...) {
            CloseHandle(pipe);
            return;
        }
        active.fetch_add(1);
        std::lock_guard lock(recordsMutex);
        Record& rec = records.emplace_back();
        rec.conn = conn;
        try {
            rec.thread = std::thread([this, conn, &rec] { Serve(conn, rec.done); });
        } catch (...) {
            conn->Finish();
            records.pop_back();
            active.fetch_sub(1);
        }
    }

    void AcceptLoop(HANDLE firstInstance)
    {
        Handle instance(firstInstance);
        Handle connectEvent;
        try {
            connectEvent.reset(NewEvent());
        } catch (...) {
            return;
        }
        for (;;) {
            if (!instance) {
                // Wait for a free slot (the listening instance counts as one of the 8).
                while (active.load() >= kMaxInstances) {
                    const HANDLE waits[2] = {stopEvent.h, slotEvent.h};
                    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) return;
                    Reap();
                }
                if (Stopping()) return;
                instance.reset(CreateInstance(false));
                if (!instance) {
                    instance.release();   // INVALID_HANDLE_VALUE: nothing to close
                    if (WaitForSingleObject(stopEvent.h, 100) == WAIT_OBJECT_0) return;
                    continue;
                }
            }

            OVERLAPPED ov{};
            ov.hEvent = connectEvent.h;
            ResetEvent(connectEvent.h);
            bool connected = false;
            if (ConnectNamedPipe(instance.h, &ov)) {
                connected = true;
            } else {
                const DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (err == ERROR_IO_PENDING) {
                    const auto r = FinishOverlapped(instance.h, ov, stopEvent.h);
                    connected = r.has_value();
                    if (!connected && Stopping()) return;   // instance closes with `instance`
                }
                // ERROR_NO_DATA (the client already left) or another error: drop this instance, make another.
            }
            if (connected)
                Spawn(instance.release());
            else
                instance.reset();
            Reap();
            if (Stopping()) return;
        }
    }

    // ---- Per connection ----

    void Serve(std::shared_ptr<PipeConnection> conn, std::atomic<bool>& done)
    {
        Handle readEvent;
        try {
            readEvent.reset(NewEvent());
            bool authed = false;
            std::string body;
            for (;;) {
                std::array<uint8_t, 4> prefix{};
                if (!conn->ReadExact(prefix.data(), prefix.size(), readEvent.h)) break;
                const auto len = ParseLengthPrefix(std::span<const uint8_t, 4>(prefix));
                if (!len) break;
                body.resize(*len);
                if (!conn->ReadExact(reinterpret_cast<uint8_t*>(body.data()), body.size(), readEvent.h)) break;
                if (!HandleMessage(conn, body, authed)) break;
            }
        } catch (...) {
            // Out of memory or similar: drop just this connection.
        }
        conn->Finish();
        readEvent.reset();
        done.store(true);
        active.fetch_sub(1);
        SetEvent(slotEvent.h);
    }

    // One message. Returns false when the connection must close (after any reply has been written).
    bool HandleMessage(const std::shared_ptr<PipeConnection>& conn, const std::string& body, bool& authed)
    {
        using nlohmann::json;
        const json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded()) {
            conn->Send(MakeError(nullptr, kParseError, "parse error"));
            return authed;   // before hello, only a hello request is accepted
        }

        // A JSON-RPC 2.0 request object: jsonrpc "2.0", method a string, id (if present) a string, number or null,
        // params (if present) an object or array.
        const bool isObject = msg.is_object();
        const bool hasId = isObject && msg.contains("id");
        const bool idValid = hasId && (msg["id"].is_string() || msg["id"].is_number() || msg["id"].is_null());
        const json id = idValid ? msg["id"] : json(nullptr);
        const bool valid = isObject && msg.contains("jsonrpc") && msg["jsonrpc"] == "2.0" && msg.contains("method") &&
                           msg["method"].is_string() && (!hasId || idValid) &&
                           (!msg.contains("params") || msg["params"].is_object() || msg["params"].is_array());
        if (!valid) {
            conn->Send(MakeError(id, kInvalidRequest, "invalid request"));
            return authed;
        }
        const std::string method = msg["method"].get<std::string>();

        if (!authed) {
            if (!hasId || method != "hello") {
                conn->Send(MakeError(id, kUnauthorized, "hello required first"));
                return false;
            }
            const json params = msg.value("params", json::object());
            if (!params.is_object()) {
                conn->Send(MakeError(id, kUnauthorized, "unauthorized"));
                return false;
            }
            const auto version = params.find("protocol_version");
            if (version == params.end() || !version->is_string() || version->get<std::string>() != kProtocolVersion) {
                conn->Send(MakeError(id, kInvalidRequest, "unsupported protocol_version; expected 0.3"));
                return false;
            }
            const auto roleIt = params.find("role");
            const auto tokenIt = params.find("token");
            const std::string roleName = roleIt != params.end() && roleIt->is_string() ? roleIt->get<std::string>() : "";
            const std::string token = tokenIt != params.end() && tokenIt->is_string() ? tokenIt->get<std::string>() : "";
            bool ok = false;
            Role role = Role::Agent;
            if (roleName == "agent") {
                ok = TokenEquals(tokens.agent, token);
                role = Role::Agent;
            } else if (roleName == "operator") {
                ok = TokenEquals(tokens.operatorToken, token);
                role = Role::Operator;
            }
            if (!ok) {
                conn->Send(MakeError(id, kUnauthorized, "unauthorized"));
                return false;
            }
            conn->SetRole(role);
            authed = true;
            conn->Send(MakeResult(id, json{{"protocol_version", kProtocolVersion},
                                           {"role", roleName},
                                           {"display_mode", display == DisplayMode::Headless ? "headless" : "windowed"}}));
            return true;
        }

        if (!hasId) return true;   // client notifications are ignored
        if (method == "hello") {
            conn->Send(MakeError(id, kInvalidRequest, "hello already completed"));
            return true;
        }
        if (conn->role() == Role::Agent && isOperatorOnly && isOperatorOnly(method)) {
            conn->Send(MakeError(id, kUnauthorized, "operator role required"));
            return true;
        }
        try {
            if (onRequest) onRequest(conn, msg);
            else conn->Send(MakeError(id, kMethodNotFound, "method not found"));
        } catch (const std::exception& e) {
            conn->Send(MakeError(id, kInternalError, e.what()));
        } catch (...) {
            conn->Send(MakeError(id, kInternalError, "unknown error"));
        }
        return true;
    }
};

PipeServer::PipeServer(std::wstring name, Tokens tokens, RequestHandler onRequest, MethodPredicate isOperatorOnly,
                       DisplayMode display)
    : impl_(std::make_unique<Impl>())
{
    impl_->name = std::move(name);
    impl_->tokens = std::move(tokens);
    impl_->onRequest = std::move(onRequest);
    impl_->isOperatorOnly = std::move(isOperatorOnly);
    impl_->display = display;
}

PipeServer::~PipeServer()
{
    Stop();
}

void PipeServer::Start()
{
    Impl& d = *impl_;
    if (d.started) throw std::logic_error("PipeServer::Start: already started");
    if (d.tokens.agent.empty() || d.tokens.operatorToken.empty())
        throw std::runtime_error("PipeServer::Start: empty token");
    d.BuildSecurity();
    d.stopEvent.reset(NewEvent());
    HANDLE slot = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!slot) throw std::runtime_error(Win32Message("CreateEventW", GetLastError()));
    d.slotEvent.reset(slot);
    HANDLE first = d.CreateInstance(true);
    if (first == INVALID_HANDLE_VALUE) throw std::runtime_error(Win32Message("CreateNamedPipeW", GetLastError()));
    try {
        d.acceptor = std::thread([&d, first] { d.AcceptLoop(first); });
    } catch (...) {
        CloseHandle(first);
        throw;
    }
    d.started = true;
}

void PipeServer::Stop()
{
    if (!impl_) return;
    Impl& d = *impl_;
    if (!d.started || d.stopped) return;
    d.stopped = true;
    SetEvent(d.stopEvent.h);
    if (d.acceptor.joinable()) d.acceptor.join();   // no new connections after this
    std::list<Impl::Record> all;
    {
        std::lock_guard lock(d.recordsMutex);
        all.swap(d.records);
    }
    for (auto& r : all) r.conn->Cancel();
    for (auto& r : all)
        if (r.thread.joinable()) r.thread.join();
}

std::wstring PipeServer::RandomPipeName()
{
    const std::string id = RandomHex(16);
    return L"\\\\.\\pipe\\applewin-harness-" + std::wstring(id.begin(), id.end());
}

std::string PipeServer::RandomToken()
{
    return RandomHex(32);
}

}  // namespace harness
