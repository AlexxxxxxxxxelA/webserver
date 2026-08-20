import http.client
import codecs
import glob
import json
import os
import select
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def sse_chunk(data):
    payload = ("data: " + json.dumps(data, ensure_ascii=False) + "\n\n").encode("utf-8")
    return f"{len(payload):X}\r\n".encode("ascii") + payload + b"\r\n"


class StreamingMockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    cancelled_upstream = threading.Event()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        assert request["stream"] is True
        assert request["stream_options"] == {"include_usage": True}
        assert request["thinking"] == {"type": "enabled"}
        tool_messages = [m for m in request["messages"] if m["role"] == "tool"]
        user_message = next(
            message["content"] for message in reversed(request["messages"])
            if message["role"] == "user"
        )

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        if user_message == "LARGE_ANSWER_TEST":
            events = [
                {"choices": [{"delta": {"content": "x" * 1024},
                              "finish_reason": None}]}
                for _ in range(70)
            ]
            events[-1]["choices"][0]["finish_reason"] = "stop"
            events.append({"choices": [], "usage": {
                "prompt_tokens": 5, "completion_tokens": 70, "total_tokens": 75,
            }})
        elif user_message == "CANCEL_TEST":
            events = [
                {"choices": [{"delta": {
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_CANCEL"
                }, "finish_reason": None}]},
                {"choices": [{"delta": {"content": "不应继续"},
                              "finish_reason": "stop"}]},
            ]
        elif not tool_messages:
            events = [
                {"choices": [{"delta": {
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_SSE_R1_A"
                }, "finish_reason": None}]},
                {"choices": [{"delta": {
                    "reasoning_content": "_R1_B"
                }, "finish_reason": None}]},
                {"choices": [{"delta": {"tool_calls": [{
                    "index": 0,
                    "id": "stream-weather-",
                    "type": "function",
                    "function": {"name": "wea", "arguments": "{\"loc"},
                }], "content": "正在查询天气。"}, "finish_reason": None}]},
                {"choices": [{"delta": {"tool_calls": [{
                    "index": 0,
                    "id": "1",
                    "function": {"name": "ther", "arguments": "ation\":\"Beijing\"}"},
                }]}, "finish_reason": "tool_calls"}]},
                {"choices": [], "usage": {
                    "prompt_tokens": 10, "completion_tokens": 2, "total_tokens": 12,
                    "prompt_cache_hit_tokens": 4, "prompt_cache_miss_tokens": 6,
                    "completion_tokens_details": {"reasoning_tokens": 5},
                }},
            ]
        else:
            assistant_calls = [m for m in request["messages"] if m.get("tool_calls")]
            assert assistant_calls[-1]["reasoning_content"] == \
                "PRIVATE_REASONING_SENTINEL_SSE_R1_A_R1_B"
            events = [
                {"choices": [{"delta": {
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_SSE_R2"
                }, "finish_reason": None}]},
                {"choices": [{"delta": {"role": "assistant", "content": "北"},
                              "finish_reason": None}]},
                {"choices": [{"delta": {"content": "京天气晴朗。"},
                              "finish_reason": "stop"}]},
                {"choices": [], "usage": {
                    "prompt_tokens": 20, "completion_tokens": 3, "total_tokens": 23,
                    "prompt_cache_hit_tokens": 8, "prompt_cache_miss_tokens": 12,
                    "completion_tokens_details": {"reasoning_tokens": 7},
                }},
            ]

        try:
            for index, event in enumerate(events):
                if user_message == "CANCEL_TEST" and index > 0:
                    time.sleep(0.4)
                encoded = sse_chunk(event)
                # 故意把 HTTP chunk 分成多次 socket write，证明 Parser 不依赖 callback 边界。
                midpoint = max(1, len(encoded) // 2)
                self.wfile.write(encoded[:midpoint])
                self.wfile.flush()
                time.sleep(0.03)
                self.wfile.write(encoded[midpoint:])
                self.wfile.flush()
                if user_message == "CANCEL_TEST" and index == 0:
                    # 下游收到 thinking.started 后会 RST。这里直接观察上游 curl Socket
                    # 的 EOF/RST，而不是等待不确定的发送缓冲何时产生 BrokenPipe。
                    deadline = time.time() + 3
                    while time.time() < deadline:
                        readable, _, exceptional = select.select(
                            [self.connection], [], [self.connection], 0.05
                        )
                        if exceptional:
                            self.cancelled_upstream.set()
                            return
                        if readable:
                            try:
                                pending = self.connection.recv(1, socket.MSG_PEEK)
                            except (BrokenPipeError, ConnectionResetError, OSError):
                                self.cancelled_upstream.set()
                                return
                            if not pending:
                                self.cancelled_upstream.set()
                                return
                if tool_messages and index == 0:
                    time.sleep(0.25)  # 客户端应在最终响应前看到第一个 assistant.delta。

            done = b"data: [DONE]\n\n"
            self.wfile.write(f"{len(done):X}\r\n".encode("ascii") + done + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            if user_message == "CANCEL_TEST":
                self.cancelled_upstream.set()
            pass  # 断连取消测试的预期行为。

    def do_GET(self):
        if self.path.startswith("/weather/"):
            body = b"Beijing: Sunny +25C"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def log_message(self, format, *args):
        pass


def wait_for_server(process):
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError("new agent server exited before becoming ready")
        try:
            connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=0.2)
            connection.request("GET", "/health")
            connection.getresponse().read()
            connection.close()
            if process.poll() is not None:
                raise RuntimeError("health response came from a stale server")
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("agent server did not start")


def parse_event(block):
    event_type = None
    data = None
    for line in block.splitlines():
        if line.startswith("event: "):
            event_type = line[7:]
        elif line.startswith("data: "):
            data = json.loads(line[6:])
    return event_type, data


def wait_for_trace(source_root, run_id):
    deadline = time.time() + 5
    while time.time() < deadline:
        for path in glob.glob(os.path.join(source_root, "logs", "*.log")):
            with open(path, "r", encoding="utf-8", errors="replace") as log_file:
                text = log_file.read()
            if run_id in text:
                return text
        time.sleep(0.1)
    raise RuntimeError("agent trace was not flushed")


def read_events(response):
    pending = ""
    decoder = codecs.getincrementaldecoder("utf-8")()
    events = []
    while True:
        chunk = response.read(1)
        if not chunk:
            break
        pending += decoder.decode(chunk)
        while "\n\n" in pending:
            block, pending = pending.split("\n\n", 1)
            events.append(parse_event(block))
    pending += decoder.decode(b"", final=True)
    return events


def main():
    binary, source_root = sys.argv[1], sys.argv[2]
    database_path = f"/tmp/webserver-agent-sse-{os.getpid()}.db"
    mock = ThreadingHTTPServer(("127.0.0.1", 19090), StreamingMockHandler)
    mock_thread = threading.Thread(target=mock.serve_forever, daemon=True)
    mock_thread.start()
    environment = os.environ.copy()
    environment.update({
        "DEEPSEEK_API_KEY": "local-stream-token",
        "DEEPSEEK_API_URL": "http://127.0.0.1:19090/deepseek",
        "DEEPSEEK_MODEL": "deepseek-v4-flash",
        "WEATHER_API_BASE_URL": "http://127.0.0.1:19090/weather",
        "CONVERSATION_DATABASE_PATH": database_path,
    })
    process = subprocess.Popen([binary], cwd=source_root, env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(process)
        connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
        body = json.dumps({"session_id": "sse-test", "message": "北京天气怎么样？"})
        request_started = time.monotonic()
        connection.request("POST", "/agent/run/stream", body,
                           {"Content-Type": "application/json"})
        response = connection.getresponse()
        assert response.status == 200
        assert response.getheader("Content-Type") == "text/event-stream; charset=utf-8"
        assert response.getheader("Transfer-Encoding") == "chunked"
        assert response.getheader("Content-Length") is None

        pending = ""
        decoder = codecs.getincrementaldecoder("utf-8")()
        events = []
        first_delta_at = None
        answer_parts = []
        while True:
            chunk = response.read(1)
            if not chunk:
                break
            # TCP/HTTP chunk 可以切在中文 UTF-8 三字节编码中间，必须增量解码。
            pending += decoder.decode(chunk)
            while "\n\n" in pending:
                block, pending = pending.split("\n\n", 1)
                event = parse_event(block)
                events.append(event)
                if event[0] == "assistant.delta" and first_delta_at is None:
                    first_delta_at = time.monotonic()
                if event[0] == "assistant.delta":
                    answer_parts.append((event[1]["sequence"], event[1]["text"]))

        pending += decoder.decode(b"", final=True)

        types = [event[0] for event in events]
        serialized_events = json.dumps(events, ensure_ascii=False)
        assert "PRIVATE_REASONING_SENTINEL" not in serialized_events
        assert types[0] == "run.started"
        assert "tool.started" in types
        assert "tool.completed" in types
        assert types.count("assistant.delta") == 3
        assert types.count("assistant.thinking.started") == 2
        assert types.count("assistant.thinking.completed") == 2
        assert types.index("assistant.thinking.completed") < types.index("tool.started")
        assert types[-1] == "run.completed"
        completed = events[-1][1]
        final_sequence = completed["answer_sequence"]
        final_delta_index = next(
            index for index, event in enumerate(events)
            if event[0] == "assistant.delta" and event[1]["sequence"] == final_sequence
        )
        assert types.index("tool.completed") < final_delta_index
        final_answer = "".join(text for sequence, text in answer_parts
                               if sequence == final_sequence)
        assert final_answer == "北京天气晴朗。"
        assert any(sequence == 1 and text == "正在查询天气。"
                   for sequence, text in answer_parts)
        assert completed["answer_bytes"] == len("北京天气晴朗。".encode("utf-8"))
        assert completed["metrics"]["usage"]["total_tokens"] == 35
        assert completed["metrics"]["usage"]["reasoning_tokens"] == 12
        assert first_delta_at is not None
        assert first_delta_at - request_started < 0.8
        trace = wait_for_trace(source_root, completed["run_id"])
        assert "PRIVATE_REASONING_SENTINEL" not in trace
        connection.close()

        # 真正端到端：Python CLI 子进程 -> C++ HTTP/SSE Server -> Mock DeepSeek/Weather。
        cli = subprocess.run(
            [
                sys.executable,
                os.path.join(source_root, "tools", "chat_client.py"),
                "--url", "http://127.0.0.1:18081",
                "--session", "cli-e2e-session",
                "--no-save-session",
                "--show-metrics",
                "--once", "北京天气怎么样？",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=12,
        )
        assert cli.returncode == 0, cli.stderr
        assert "[思考中 #1]" in cli.stdout
        assert "[调用工具] weather" in cli.stdout
        assert "北京天气晴朗。" in cli.stdout
        assert "思考Token=12" in cli.stdout
        assert "PRIVATE_REASONING_SENTINEL" not in cli.stdout

        # Header 提交前的校验失败必须仍是普通 HTTP JSON，而不是 SSE 200。
        bad = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
        bad.request("POST", "/agent/run/stream", "{}", {"Content-Type": "application/json"})
        bad_response = bad.getresponse()
        assert bad_response.status == 400
        assert bad_response.getheader("Content-Length") is not None
        assert bad_response.getheader("Transfer-Encoding") is None
        bad_response.read()
        bad.close()

        # 完整答案超过单事件 64 KiB 时，正文仍由小 delta 组成，终态必须存在。
        large = http.client.HTTPConnection("127.0.0.1", 18081, timeout=8)
        large_body = json.dumps({
            "session_id": "large-answer",
            "message": "LARGE_ANSWER_TEST",
        })
        large.request("POST", "/agent/run/stream", large_body,
                      {"Content-Type": "application/json"})
        large_response = large.getresponse()
        assert large_response.status == 200
        large_events = read_events(large_response)
        large_deltas = [event[1]["text"] for event in large_events
                        if event[0] == "assistant.delta"]
        assert len("".join(large_deltas)) == 70 * 1024
        assert large_events[-1][0] == "run.completed"
        assert large_events[-1][1]["answer_bytes"] == 70 * 1024
        large.close()

        # 客户端在 run.started 后断开；同一 Session 应很快解除 inFlight，而不是忙 60 秒。
        cancelled = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
        cancel_body = json.dumps({"session_id": "cancel-session", "message": "CANCEL_TEST"})
        cancelled.request("POST", "/agent/run/stream", cancel_body,
                          {"Content-Type": "application/json"})
        cancel_socket = cancelled.sock
        cancel_response = cancelled.getresponse()
        assert cancel_response.status == 200
        cancel_prefix = b""
        cancel_deadline = time.time() + 3
        while b"assistant.thinking.started" not in cancel_prefix and \
                time.time() < cancel_deadline:
            cancel_prefix += cancel_response.read(1)
        assert b"assistant.thinking.started" in cancel_prefix
        # SO_LINGER(1, 0) 让 close 发送 RST，覆盖 EPOLLERR/read error 取消路径。
        cancel_socket.setsockopt(
            socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
        )
        # Connection: close 响应后，http.client 可能把 Socket 所有权交给 HTTPResponse；
        # 必须同时关闭 Response 的文件对象和原 Socket，才能真正发出测试所需的 RST。
        cancel_response.close()
        cancel_socket.close()
        cancelled.close()

        released = False
        deadline = time.time() + 3
        while time.time() < deadline and not released:
            retry = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
            retry_body = json.dumps({
                "session_id": "cancel-session",
                "message": "北京天气怎么样？",
            })
            retry.request("POST", "/agent/run/stream", retry_body,
                          {"Content-Type": "application/json"})
            retry_response = retry.getresponse()
            if retry_response.status == 200:
                retry_response.read()
                released = True
            else:
                retry_response.read()
                time.sleep(0.05)
            retry.close()
        assert released
        assert StreamingMockHandler.cancelled_upstream.wait(timeout=2)

        with sqlite3.connect(database_path) as database:
            rows = database.execute(
                "SELECT user_message, assistant_message, tool_executions_json FROM turns"
            ).fetchall()
        assert rows
        assert "PRIVATE_REASONING_SENTINEL" not in json.dumps(rows)
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        mock.shutdown()
        mock.server_close()
        for suffix in ("", "-wal", "-shm"):
            try:
                os.remove(database_path + suffix)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
