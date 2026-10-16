"""Standard-library client for AppleWinHarness.exe --serve (spec: docs/specs/m3.md, Part A "Python client").

Harness.launch() starts the exe and reads its announcement line; connect(role) opens the pipe and says hello.

The pipe is opened with open(name, "r+b", buffering=0), which gives a synchronous handle. Windows serialises
synchronous I/O on one handle, so a ReadFile blocked in one thread would also block a WriteFile from another. The
reader thread therefore polls PeekNamedPipe (through ctypes) and only reads bytes that have already arrived; it never
blocks in ReadFile, so call() can always write.
"""

import ctypes
import json
import msvcrt
import os
import queue
import struct
import subprocess
import threading
import time
from ctypes import wintypes

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PROTOCOL_VERSION = "0.3"
MAX_MESSAGE = 16 * 1024 * 1024

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_PeekNamedPipe = _kernel32.PeekNamedPipe
_PeekNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                           ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_PeekNamedPipe.restype = wintypes.BOOL
_GetNamedPipeServerProcessId = _kernel32.GetNamedPipeServerProcessId
_GetNamedPipeServerProcessId.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG)]
_GetNamedPipeServerProcessId.restype = wintypes.BOOL


def exe_path():
    """HARNESS_EXE, or bin/x64/Debug/AppleWinHarness.exe in the repository."""
    return os.environ.get("HARNESS_EXE") or os.path.join(REPO_ROOT, "bin", "x64", "Debug", "AppleWinHarness.exe")


class RpcError(Exception):
    def __init__(self, code, message, data=None):
        super().__init__("{}: {}".format(code, message))
        self.code = code
        self.message = message
        self.data = data


class ConnectionClosed(Exception):
    pass


class Connection:
    """One pipe connection. call() returns the result or raises RpcError; notifications are queued."""

    def __init__(self, pipe_name, open_timeout=5.0, server_pid=None):
        """server_pid: the announced pid. The pipe's server process must be it, or the connection is refused before
        anything (a token) is sent: another process may have taken over the pipe name."""
        deadline = time.monotonic() + open_timeout
        while True:
            try:
                self._file = open(pipe_name, "r+b", buffering=0)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        self._handle = msvcrt.get_osfhandle(self._file.fileno())
        if server_pid is not None:
            pid = wintypes.ULONG(0)
            if not _GetNamedPipeServerProcessId(self._handle, ctypes.byref(pid)) or pid.value != server_pid:
                self._file.close()
                raise ConnectionError("pipe server process is {}, not the announced pid {}".format(pid.value,
                                                                                                 server_pid))
        self._write_lock = threading.Lock()
        self._next_id = 1
        self._replies = {}
        self._replies_cv = threading.Condition()
        self._notifications = queue.Queue()
        self._closed = False
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    # ---- reading ----

    def _available(self):
        avail = wintypes.DWORD(0)
        if not _PeekNamedPipe(self._handle, None, 0, None, ctypes.byref(avail), None):
            raise ConnectionClosed()
        return avail.value

    def _read_exact(self, n):
        data = bytearray()
        while len(data) < n:
            if self._stop:
                raise ConnectionClosed()
            avail = self._available()
            if avail == 0:
                time.sleep(0.002)
                continue
            chunk = self._file.read(min(avail, n - len(data)))
            if not chunk:
                raise ConnectionClosed()
            data += chunk
        return bytes(data)

    def _read_loop(self):
        try:
            while not self._stop:
                (n,) = struct.unpack("<I", self._read_exact(4))
                if n == 0 or n > MAX_MESSAGE:
                    raise ConnectionClosed()
                message = json.loads(self._read_exact(n).decode("utf-8"))
                if isinstance(message, dict) and "id" in message:
                    with self._replies_cv:
                        self._replies[message["id"]] = message
                        self._replies_cv.notify_all()
                else:
                    self._notifications.put(message)
        except (ConnectionClosed, OSError, ValueError):
            pass
        finally:
            with self._replies_cv:
                self._closed = True
                self._replies_cv.notify_all()

    @property
    def closed(self):
        return self._closed

    def wait_closed(self, timeout=5.0):
        """True once the server has closed the connection."""
        with self._replies_cv:
            return self._replies_cv.wait_for(lambda: self._closed, timeout)

    # ---- writing ----

    def send_raw(self, data):
        with self._write_lock:
            self._file.write(data)

    def send_body(self, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_raw(struct.pack("<I", len(body)) + body)

    def send(self, message):
        self.send_body(json.dumps(message))

    def request(self, method, params=None):
        """Sends a request; returns its id (wait_reply collects the reply)."""
        with self._write_lock:
            request_id = self._next_id
            self._next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method,
                   "params": {} if params is None else params})
        return request_id

    def wait_reply(self, request_id, timeout=60.0):
        """The raw reply message for request_id (a parse error's reply has id null)."""
        with self._replies_cv:
            if not self._replies_cv.wait_for(lambda: request_id in self._replies or self._closed, timeout):
                raise TimeoutError("no reply to request {} within {} s".format(request_id, timeout))
            if request_id not in self._replies:
                raise ConnectionClosed("connection closed before the reply to request {}".format(request_id))
            return self._replies.pop(request_id)

    def result_of(self, request_id, timeout=60.0):
        reply = self.wait_reply(request_id, timeout)
        if "error" in reply:
            error = reply["error"]
            raise RpcError(error.get("code"), error.get("message"), error.get("data"))
        return reply.get("result")

    def call(self, method, timeout=60.0, **params):
        return self.result_of(self.request(method, params), timeout)

    def next_notification(self, timeout=5.0):
        """The next notification as {"method", "params"}; raises TimeoutError if none arrives in time."""
        try:
            message = self._notifications.get(timeout=timeout)
        except queue.Empty:
            raise TimeoutError("no notification within {} s".format(timeout))
        return message

    def drain_notifications(self, quiet=0.5, limit=10.0):
        """Every notification until none has arrived for `quiet` seconds (at most `limit` seconds in all)."""
        out = []
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            try:
                out.append(self._notifications.get(timeout=quiet))
            except queue.Empty:
                break
        return out

    def close(self):
        self._stop = True
        try:
            self._file.close()
        except OSError:
            pass
        self._reader.join(timeout=2.0)


class Harness:
    """A running AppleWinHarness.exe --serve."""

    def __init__(self, process, announcement):
        self.process = process
        self.announcement = announcement
        self.pipe = announcement["pipe"]
        self.agent_token = announcement["agent_token"]
        self.operator_token = announcement["operator_token"]
        self.pid = announcement["pid"]
        self._connections = []
        self.stdout_rest = b""
        self.stderr = b""
        self._drainers = [
            threading.Thread(target=self._drain, args=(process.stdout, "stdout_rest"), daemon=True),
            threading.Thread(target=self._drain, args=(process.stderr, "stderr"), daemon=True),
        ]
        for t in self._drainers:
            t.start()

    def _drain(self, stream, attr):
        try:
            for chunk in iter(lambda: stream.read1(4096) if hasattr(stream, "read1") else stream.read(4096), b""):
                setattr(self, attr, getattr(self, attr) + chunk)
        except (OSError, ValueError):
            pass

    @classmethod
    def launch(cls, headless=True, announce_timeout=15.0):
        args = [exe_path(), "--serve"] + (["--headless"] if headless else [])
        process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   cwd=os.path.dirname(exe_path()))
        line_box = []

        def read_line():
            line_box.append(process.stdout.readline())

        reader = threading.Thread(target=read_line, daemon=True)
        reader.start()
        reader.join(announce_timeout)
        line = line_box[0] if line_box else b""
        if not line.endswith(b"\n"):
            process.kill()
            try:
                _, err = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                err = b""
            raise RuntimeError("no announcement line from {} (exit {}): stdout {!r} stderr {!r}".format(
                args, process.poll(), line, err))
        try:
            announcement = json.loads(line.decode("utf-8"))
        except ValueError:
            process.kill()
            raise RuntimeError("announcement is not JSON: {!r}".format(line))
        harness = cls(process, announcement)
        harness.announcement_line = line
        return harness

    def open_raw(self):
        """A connection that has not said hello."""
        conn = Connection(self.pipe, server_pid=self.pid)
        self._connections.append(conn)
        return conn

    def connect(self, role, timeout=30.0):
        """A connection that has said hello with the role's token."""
        conn = self.open_raw()
        token = self.agent_token if role == "agent" else self.operator_token
        conn.hello_reply = conn.call("hello", timeout=timeout, protocol_version=PROTOCOL_VERSION, role=role,
                                     token=token)
        return conn

    def wait_exit(self, timeout):
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def close(self):
        """close_session if possible, then up to 5 s for the process to exit, then kill it."""
        if self.process.poll() is None:
            try:
                conn = self.connect("operator", timeout=5.0)
                rid = conn.request("close_session", {})
                try:
                    conn.wait_reply(rid, timeout=5.0)
                except (TimeoutError, ConnectionClosed):
                    pass
            except Exception:
                pass
            if self.wait_exit(5.0) is None:
                self.process.kill()
                self.process.wait(timeout=10)
        for conn in self._connections:
            conn.close()
        for t in self._drainers:
            t.join(timeout=2.0)
        for stream in (self.process.stdout, self.process.stderr):
            try:
                stream.close()
            except OSError:
                pass

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
