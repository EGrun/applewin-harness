#include "server/pipe_server.h"
#include "host/log.h"
#include "host/harness_version.h"
#include "server/protocol.h"
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <deque>
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
constexpr size_t kMaxBacklogBytes = 64u * 1024u * 1024u;   // queued outbound bytes per connection

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
std::optional<DWORD> FinishOverlapped(HANDLE file, OVERLAPPED& ov, HANDLE cancel, DWORD timeoutMs = INFINITE)
{
    const HANDLE waits[2] = {ov.hEvent, cancel};
    const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);   // a timeout cancels like `cancel`
    if (w != WAIT_OBJECT_0) CancelIoEx(file, &ov);
    DWORD n = 0;
    const BOOL ok = GetOverlappedResult(file, &ov, &n, TRUE);   // completes (or aborts) the operation either way
    if (!ok || w != WAIT_OBJECT_0) return std::nullopt;
    return n;
}

// One accepted pipe instance. Owned jointly by its two threads (reader and writer) and whoever keeps the ConnectionPtr
// (the session). Send never blocks: it queues the encoded message and the writer thread writes it. The pipe handle is
// closed by the reader thread once the writer has stopped; the events when the last owner lets go.
class PipeConnection final : public Connection {
public:
    PipeConnection(HANDLE pipe, uint64_t id)
        : pipe_(pipe), id_(id), cancel_(NewEvent()), writeEvent_(NewEvent()), sendReady_(NewAutoEvent())
    {
    }

    Role role() const override { return role_.load(); }
    uint64_t id() const override { return id_; }
    bool closed() const override { return closed_.load(); }

    // Queues one whole message and returns at once. A backlog over kMaxBacklogBytes closes the connection (the client
    // is not reading). Throws std::length_error for a message over 16 MiB (EncodeMessage).
    void Send(const nlohmann::json& message) override
    {
        std::vector<uint8_t> wire = EncodeMessage(message);
        bool overflow = false;
        {
            std::lock_guard lock(queueMutex_);
            if (closed_.load() || closing_) return;
            if (backlog_ + wire.size() > kMaxBacklogBytes) {
                overflow = true;
            } else {
                backlog_ += wire.size();
                queue_.push_back(std::move(wire));
            }
        }
        if (overflow) Cancel();
        else SetEvent(sendReady_.h);
    }

    // The writer thread: writes queued messages, whole and in order, until cancelled, a write fails, or the connection
    // is closing and the queue is empty.
    void WriterLoop()
    {
        for (;;) {
            std::vector<uint8_t> wire;
            {
                std::lock_guard lock(queueMutex_);
                if (closed_.load()) return;
                if (!queue_.empty()) {
                    wire = std::move(queue_.front());
                    queue_.pop_front();
                } else if (closing_) {
                    return;
                }
            }
            if (wire.empty()) {
                const HANDLE waits[2] = {sendReady_.h, cancel_.h};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return;
                continue;
            }
            const bool ok = WriteAll(wire);
            {
                std::lock_guard lock(queueMutex_);
                backlog_ -= wire.size();
            }
            if (!ok) {
                Cancel();
                return;
            }
        }
    }

    // Reads exactly n bytes; false on disconnect, error, cancellation or (when deadline != 0, a GetTickCount64 value)
    // the deadline passing. Reader thread only.
    bool ReadExact(uint8_t* p, size_t n, HANDLE readEvent, ULONGLONG deadline = 0)
    {
        size_t got = 0;
        while (got < n) {
            if (closed_.load()) return false;
            DWORD timeoutMs = INFINITE;
            if (deadline) {
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) return false;
                timeoutMs = static_cast<DWORD>(deadline - now);
            }
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            const DWORD want = static_cast<DWORD>(n - got);   // <= 16 MiB
            DWORD r = 0;
            if (ReadFile(pipe_.h, p + got, want, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING) {
                const auto res = FinishOverlapped(pipe_.h, ov, cancel_.h, timeoutMs);
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

    // Any thread: true once every queued message has been written, or the connection has closed.
    bool Drained()
    {
        std::lock_guard lock(queueMutex_);
        return backlog_ == 0 || closed_.load();
    }

    // Any thread: stops pending and future I/O and drops the queue. The handle stays open until Finish.
    void Cancel()
    {
        closed_.store(true);
        SetEvent(cancel_.h);
    }

    // Reader thread: no more messages are accepted; the writer drains what is queued, then stops.
    void BeginClosing()
    {
        {
            std::lock_guard lock(queueMutex_);
            closing_ = true;
        }
        SetEvent(sendReady_.h);
    }

    // Reader thread, last (after the writer has been joined): closes the handle. Data already written stays readable
    // by the client (no DisconnectNamedPipe, which would discard it).
    void Finish()
    {
        Cancel();
        {
            std::lock_guard lock(queueMutex_);
            queue_.clear();
            backlog_ = 0;
        }
        pipe_.reset();
    }

    void SetRole(Role r) { role_.store(r); }

private:
    static HANDLE NewAutoEvent()
    {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e) throw std::runtime_error(Win32Message("CreateEventW", GetLastError()));
        return e;
    }

    // Writer thread only.
    bool WriteAll(const std::vector<uint8_t>& wire)
    {
        size_t done = 0;
        while (done < wire.size()) {
            OVERLAPPED ov{};
            ov.hEvent = writeEvent_.h;
            ResetEvent(writeEvent_.h);
            const DWORD chunk = static_cast<DWORD>(wire.size() - done);   // <= 16 MiB + 4
            if (!WriteFile(pipe_.h, wire.data() + done, chunk, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
                return false;
            const auto n = FinishOverlapped(pipe_.h, ov, cancel_.h);
            if (!n || *n == 0) return false;
            done += *n;
        }
        return true;
    }

    Handle pipe_;
    const uint64_t id_;
    std::atomic<Role> role_{Role::Agent};
    std::atomic<bool> closed_{false};
    Handle cancel_;       // manual-reset; set once, never reset
    Handle writeEvent_;   // writer thread only
    Handle sendReady_;    // auto-reset: the queue grew, or closing_ was set
    std::mutex queueMutex_;
    std::deque<std::vector<uint8_t>> queue_;   // under queueMutex_
    size_t backlog_ = 0;                       // queued plus in-flight bytes; under queueMutex_
    bool closing_ = false;                     // under queueMutex_
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

    std::vector<uint8_t> ownerSid;   // the first instance's owner (FILE_FLAG_FIRST_PIPE_INSTANCE: certainly ours)
    mutable std::mutex fatalMutex;
    std::string fatalError;

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
        if (acl) {   // a retry after a failed Start
            LocalFree(acl);
            acl = nullptr;
        }
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

    // Checks the instance's owner and DACL: the owner (copied into ownerOut when non-null, else compared with
    // ownerSid) and a DACL of exactly one allow ACE for the token user. Guards against joining a pipe someone else
    // created under our name (squatting) when an instance is made without FILE_FLAG_FIRST_PIPE_INSTANCE.
    bool CheckInstance(HANDLE h, std::vector<uint8_t>* ownerOut)
    {
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR psd = nullptr;
        if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                            nullptr, &dacl, nullptr, &psd) != ERROR_SUCCESS)
            return false;
        bool ok = owner != nullptr && IsValidSid(owner) && dacl != nullptr && dacl->AceCount == 1;
        if (ok) {
            if (ownerOut) {
                ownerOut->resize(GetLengthSid(owner));
                ok = CopySid(static_cast<DWORD>(ownerOut->size()), ownerOut->data(), owner) != FALSE;
            } else {
                ok = !ownerSid.empty() && EqualSid(owner, ownerSid.data());
            }
        }
        void* ace = nullptr;
        if (ok) ok = GetAce(dacl, 0, &ace) && static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE;
        if (ok) {
            PSID user = reinterpret_cast<TOKEN_USER*>(tokenUser.data())->User.Sid;
            ok = EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, user) != FALSE;
        }
        LocalFree(psd);
        return ok;
    }

    // Stops serving for good: no more connections, every open connection is closed. FatalError() reports it.
    void Fatal(std::string message)
    {
        {
            std::lock_guard lock(fatalMutex);
            if (fatalError.empty()) fatalError = std::move(message);
        }
        SetEvent(stopEvent.h);
        std::lock_guard lock(recordsMutex);
        for (auto& r : records) r.conn->Cancel();
    }

    // A further, verified instance; nullptr on a transient failure. On a security failure, calls Fatal and returns
    // INVALID_HANDLE_VALUE.
    HANDLE NewInstance()
    {
        HANDLE h = CreateInstance(false);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                Fatal("pipe instance: access denied (the pipe name was taken over)");
                return INVALID_HANDLE_VALUE;
            }
            return nullptr;
        }
        if (!CheckInstance(h, nullptr)) {
            CloseHandle(h);
            Fatal("pipe instance: wrong owner or DACL (the pipe name was taken over)");
            return INVALID_HANDLE_VALUE;
        }
        return h;
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
        try {
            Record& rec = records.emplace_back();
            rec.conn = conn;
            try {
                rec.thread = std::thread([this, conn, &rec] { Serve(conn, rec.done); });
            } catch (...) {
                records.pop_back();
                throw;
            }
        } catch (...) {
            conn->Finish();
            active.fetch_sub(1);
        }
    }

    // The pipe name never disappears while serving: a failed connect reuses its instance (DisconnectNamedPipe), and
    // the next listening instance is created before a connected one is handed to its thread. Only at the 8-instance
    // cap is there no listener; every instance made without FILE_FLAG_FIRST_PIPE_INSTANCE is verified (CheckInstance).
    void AcceptLoop(HANDLE firstInstance)
    {
        Handle instance(firstInstance);
        try {
            Handle connectEvent(NewEvent());
            for (;;) {
                if (!instance) {
                    while (active.load() >= kMaxInstances) {   // the listening instance counts as one of the 8
                        const HANDLE waits[2] = {stopEvent.h, slotEvent.h};
                        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) return;
                        Reap();
                    }
                    if (Stopping()) return;
                    HANDLE h = NewInstance();
                    if (h == INVALID_HANDLE_VALUE) return;   // fatal
                    if (!h) {
                        if (WaitForSingleObject(stopEvent.h, 100) == WAIT_OBJECT_0) return;
                        continue;
                    }
                    instance.reset(h);
                }

                OVERLAPPED ov{};
                ov.hEvent = connectEvent.h;
                ResetEvent(connectEvent.h);
                bool connected = false;
                bool clientLeft = false;
                if (ConnectNamedPipe(instance.h, &ov)) {
                    connected = true;
                } else {
                    const DWORD err = GetLastError();
                    if (err == ERROR_PIPE_CONNECTED) {
                        connected = true;
                    } else if (err == ERROR_IO_PENDING) {
                        connected = FinishOverlapped(instance.h, ov, stopEvent.h).has_value();
                        if (!connected && Stopping()) return;   // the instance closes with `instance`
                        clientLeft = true;
                    } else {
                        clientLeft = err == ERROR_NO_DATA;   // the client already left
                    }
                }
                if (!connected) {
                    // Reuse this instance: closing it could remove the pipe name.
                    DisconnectNamedPipe(instance.h);
                    if (!clientLeft && WaitForSingleObject(stopEvent.h, 50) == WAIT_OBJECT_0) return;
                    continue;
                }

                HANDLE next = nullptr;
                if (active.load() + 1 < kMaxInstances) {
                    next = NewInstance();
                    if (next == INVALID_HANDLE_VALUE) return;   // fatal; the connected instance closes unused
                }
                Spawn(instance.release());
                instance.reset(next);
                Reap();
                if (Stopping()) return;
            }
        } catch (const std::exception& e) {
            Fatal(std::string("pipe acceptor: ") + e.what());
        } catch (...) {
            Fatal("pipe acceptor: unknown error");
        }
    }

    // ---- Per connection ----

    // The reader thread. It starts the connection's writer thread and, when the connection ends, lets the writer
    // drain what is queued (for example the error reply before an R10 close) for up to kDrainMs, then closes.
    void Serve(std::shared_ptr<PipeConnection> conn, std::atomic<bool>& done)
    {
        constexpr DWORD kDrainMs = 2000;
        constexpr DWORD kHelloTimeoutMs = 10000;
        std::thread writer;
        bool authedAtEnd = false;
        try {
            writer = std::thread([conn] { conn->WriterLoop(); });
            Handle readEvent(NewEvent());
            bool authed = false;
            std::string body;
            // A connection must complete hello within kHelloTimeoutMs of being accepted, or it is closed.
            const ULONGLONG helloDeadline = GetTickCount64() + kHelloTimeoutMs;
            for (;;) {
                const ULONGLONG deadline = authed ? 0 : helloDeadline;
                std::array<uint8_t, 4> prefix{};
                if (!conn->ReadExact(prefix.data(), prefix.size(), readEvent.h, deadline)) break;
                const auto len = ParseLengthPrefix(std::span<const uint8_t, 4>(prefix), kMaxRequestBytes);
                if (!len) {
                    // Over 1 MiB: close before the body is read. (A zero length also closes, without a log line.)
                    if (prefix != std::array<uint8_t, 4>{})
                        log::Warning(log::Component::Server, "request larger than 1 MiB: connection closed", conn->id());
                    break;
                }
                body.resize(*len);
                if (!conn->ReadExact(reinterpret_cast<uint8_t*>(body.data()), body.size(), readEvent.h, deadline)) break;
                const bool keepOpen = HandleMessage(conn, body, authed);
                authedAtEnd = authed;
                if (!keepOpen) break;
            }
        } catch (...) {
            // Out of memory or similar: drop just this connection.
        }
        if (writer.joinable()) {
            conn->BeginClosing();
            if (WaitForSingleObject(writer.native_handle(), kDrainMs) != WAIT_OBJECT_0) conn->Cancel();
            writer.join();
        }
        if (authedAtEnd)
            log::Info(log::Component::Server, "connection " + std::to_string(conn->id()) + " closed", conn->id());
        conn->Finish();
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
                log::Warning(log::Component::Server, "connection " + std::to_string(conn->id()) + " rejected: bad hello",
                             conn->id());
                return false;
            }
            authed = true;
            log::Info(log::Component::Server,
                      "connection " + std::to_string(conn->id()) + " opened (" + roleName + ")", conn->id());
            conn->Send(MakeResult(id, json{{"protocol_version", kProtocolVersion},
                                           {"role", roleName},
                                           {"server_version", Version()},
                                           {"display_mode", display == DisplayMode::Headless ? "headless" : "windowed"}}));
            conn->SetRole(role);   // after the reply is queued: a `log` notification must never precede it
            return true;
        }

        if (!hasId) return true;   // client notifications are ignored
        if (method == "hello") {
            conn->Send(MakeError(id, kInvalidRequest, "hello already completed"));
            return true;
        }
        if (conn->role() == Role::Agent && isOperatorOnly && isOperatorOnly(method)) {
            conn->Send(MakeError(id, kUnauthorized, "operator role required"));
            // method is a known operator-only name here, never client text
            log::Warning(log::Component::Server,
                         "connection " + std::to_string(conn->id()) + ": agent called operator-only " + method,
                         conn->id());
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
    if (!d.CheckInstance(first, &d.ownerSid)) {   // records the owner later instances must have
        CloseHandle(first);
        throw std::runtime_error("CreateNamedPipeW: the first instance's security descriptor is not ours");
    }
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
    // Let queued replies (close_session's {}) reach their clients first: up to 2 s for every writer to catch up.
    const ULONGLONG drainDeadline = GetTickCount64() + 2000;
    for (auto& r : all)
        while (!r.conn->Drained() && GetTickCount64() < drainDeadline) Sleep(2);
    for (auto& r : all) r.conn->Cancel();
    for (auto& r : all)
        if (r.thread.joinable()) r.thread.join();
}

void PipeServer::BroadcastToOperators(const nlohmann::json& message)
{
    std::vector<std::shared_ptr<PipeConnection>> operators;
    {
        std::lock_guard lock(impl_->recordsMutex);
        for (auto& r : impl_->records)
            if (r.conn->role() == Role::Operator) operators.push_back(r.conn);
    }
    for (auto& c : operators) {
        try {
            c->Send(message);
        } catch (...) {
        }
    }
}

std::string PipeServer::FatalError() const
{
    std::lock_guard lock(impl_->fatalMutex);
    return impl_->fatalError;
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
