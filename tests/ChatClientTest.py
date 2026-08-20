import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SOURCE_ROOT = Path(sys.argv[1]).resolve()
MODULE_PATH = SOURCE_ROOT / "tools" / "chat_client.py"
SPEC = importlib.util.spec_from_file_location("chat_client", MODULE_PATH)
chat_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(chat_client)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def event_payload(event_type, data):
    return (
        f"event: {event_type}\n"
        f"data: {json.dumps(data, ensure_ascii=False, separators=(',', ':'))}\n\n"
    ).encode("utf-8")


def http_chunk(payload):
    return f"{len(payload):X}\r\n".encode("ascii") + payload + b"\r\n"


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    cleared_sessions = []
    received_messages = []
    sessions = [{
        "session_id": "existing-chat",
        "title": "已有聊天",
        "created_at_ms": 1000,
        "updated_at_ms": 2000,
        "turn_count": 2,
    }]

    def do_GET(self):
        if self.path == "/agent/sessions":
            body = json.dumps({"sessions": self.sessions}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path != "/health":
            self.send_error(404)
            return
        body = b"healthy\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if self.path == "/agent/sessions":
            created = {
                "session_id": f"created-chat-{len(self.sessions) + 1}",
                "title": request.get("title") or "新聊天",
                "created_at_ms": 3000,
                "updated_at_ms": 3000,
                "turn_count": 0,
            }
            self.sessions.insert(0, created)
            body = json.dumps({
                "session_id": created["session_id"], "title": created["title"]
            }).encode("utf-8")
            self.send_response(201)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/agent/clear":
            self.cleared_sessions.append(request["session_id"])
            body = json.dumps({
                "session_id": request["session_id"], "cleared": True
            }).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path != "/agent/run/stream":
            self.send_error(404)
            return
        self.received_messages.append(request)
        if request["message"] == "ERROR_TEST":
            events = [
                ("run.started", {"run_id": "run-error"}),
                ("error", {
                    "run_id": "run-error",
                    "code": "UPSTREAM_ERROR",
                    "message": "mock failure",
                }),
            ]
        elif request["message"] == "LATE_EVENT_TEST":
            events = [
                ("error", {"code": "FIRST_TERMINAL", "message": "first"}),
                ("run.completed", {"answer_sequence": 1, "metrics": {}}),
            ]
        else:
            events = [
                ("run.started", {"run_id": "run-cli", "queue_wait_ms": 0}),
                ("model.started", {"run_id": "run-cli", "sequence": 1}),
                ("assistant.thinking.started", {"run_id": "run-cli", "sequence": 1}),
                ("assistant.thinking.completed", {"run_id": "run-cli", "sequence": 1}),
                ("tool.started", {"run_id": "run-cli", "name": "weather"}),
                ("tool.completed", {
                    "run_id": "run-cli", "name": "weather", "ok": True,
                    "latency_ms": 3,
                }),
                ("assistant.delta", {
                    "run_id": "run-cli", "sequence": 2, "text": "北",
                }),
                ("assistant.delta", {
                    "run_id": "run-cli", "sequence": 2, "text": "京天气晴朗。",
                }),
                ("run.completed", {
                    "run_id": "run-cli",
                    "answer_sequence": 2,
                    "answer_bytes": len("北京天气晴朗。".encode("utf-8")),
                    "metrics": {
                        "total_latency_ms": 12,
                        "model_calls": 2,
                        "tool_calls": 1,
                        "usage": {"total_tokens": 35, "reasoning_tokens": 12},
                    },
                }),
            ]

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        for event_type, data in events:
            encoded = http_chunk(event_payload(event_type, data))
            # Split every frame inside arbitrary UTF-8/HTTP boundaries.
            for index in range(0, len(encoded), 3):
                self.wfile.write(encoded[index:index + 3])
                self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def log_message(self, format, *args):
        pass


def test_sse_parser():
    parser = chat_client.SseParser()
    raw = (
        ": heartbeat\r\n"
        "event: assistant.delta\r\n"
        "data: {\"sequence\":1,\"text\":\"中\"}\r\n\r\n"
    ).encode("utf-8")
    events = []
    for byte in raw:
        events.extend(parser.feed(bytes([byte])))
    events.extend(parser.finish())
    check(events == [("assistant.delta", {"sequence": 1, "text": "中"})],
          "incremental SSE parsing failed")

    multiline = chat_client.SseParser()
    multiline_raw = b"event: message\ndata: {\"value\":\ndata: 1}\n\n"
    check(multiline.feed(multiline_raw) == [("message", {"value": 1})],
          "multiple data lines were not joined")
    try:
        chat_client.SseParser().feed(b"\xff")
    except chat_client.AgentClientError:
        pass
    else:
        raise AssertionError("invalid UTF-8 was accepted")


def test_session_file():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "session.txt"
        first = chat_client.load_or_create_session(path)
        second = chat_client.load_or_create_session(path)
        check(first == second, "session ID was not restored")
        third = chat_client.load_or_create_session(path, force_new=True)
        check(third != first, "--new-session did not rotate session ID")
        chat_client.validate_session_id("cli-valid_1.test")
        try:
            chat_client.validate_session_id("invalid session")
        except ValueError:
            pass
        else:
            raise AssertionError("invalid session ID was accepted")
        if os.name != "nt":
            check(stat.S_IMODE(path.stat().st_mode) == 0o600,
                  "session file permissions are not 0600")
        check("\u202e" not in chat_client.safe_terminal_text("safe\u202ehidden"),
              "bidirectional control was not removed from terminal text")


def test_http_client(base_url):
    client = chat_client.AgentHttpClient(base_url, timeout=5)
    check(client.health() == "healthy", "health response mismatch")
    sessions = client.list_sessions()
    check(sessions[0]["session_id"] == "existing-chat", "session list mismatch")
    created = client.create_session("测试聊天")
    check(created["title"] == "测试聊天", "session creation mismatch")

    with tempfile.TemporaryDirectory() as directory:
        session_file = Path(directory) / "last-session"
        selected = chat_client.choose_session(
            client, session_file, input_fn=lambda _: "2", output=io.StringIO()
        )
        check(selected == "existing-chat", "historical session selection failed")
        check(chat_client.read_saved_session(session_file) == "existing-chat",
              "selected session was not saved")
        created_from_menu = chat_client.choose_session(
            client, session_file, input_fn=lambda _: "0", output=io.StringIO()
        )
        check(created_from_menu.startswith("created-chat-"), "menu new chat failed")
        reused = chat_client.choose_session(
            client, session_file, input_fn=lambda _: "", output=io.StringIO()
        )
        check(reused == created_from_menu, "Enter did not reuse last session")

    events = []
    terminal = client.stream_run(
        "cli-test", "北京天气怎么样？",
        lambda event_type, data: events.append((event_type, data)),
    )
    check(terminal["answer_sequence"] == 2, "terminal sequence mismatch")
    deltas = [data["text"] for event_type, data in events
              if event_type == "assistant.delta" and data["sequence"] == 2]
    check("".join(deltas) == "北京天气晴朗。", "delta answer mismatch")
    check(any(event_type == "assistant.thinking.started" for event_type, _ in events),
          "Thinking status was not parsed")

    output = io.StringIO()
    renderer = chat_client.TerminalRenderer(output=output, show_metrics=True)
    for event in events:
        renderer.handle(*event)
    rendered = output.getvalue()
    check("[思考中 #1]" in rendered, "Thinking UI missing")
    check("[调用工具] weather" in rendered, "Tool UI missing")
    check("北京天气晴朗。" in rendered, "Answer UI missing")
    check("思考Token=12" in rendered, "Metrics UI missing")

    cleared = client.clear_session("cli-test")
    check(cleared == {"session_id": "cli-test", "cleared": True},
          "clear response mismatch")
    check(MockHandler.cleared_sessions[-1] == "cli-test", "clear request mismatch")

    error_events = []
    try:
        client.stream_run(
            "cli-test", "ERROR_TEST",
            lambda event_type, data: error_events.append((event_type, data)),
        )
    except chat_client.AgentClientError as exc:
        check(exc.code == "UPSTREAM_ERROR", "stream error code mismatch")
        check(exc.displayed, "stream error should be marked as displayed")
    else:
        raise AssertionError("SSE error terminal was treated as success")

    try:
        client.stream_run("cli-test", "LATE_EVENT_TEST", lambda *_: None)
    except chat_client.AgentClientError as exc:
        check("after terminal" in str(exc), "late terminal error was not detected")
    else:
        raise AssertionError("event after terminal was accepted")


def main():
    test_sse_parser()
    test_session_file()
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        host, port = server.server_address
        test_http_client(f"http://{host}:{port}")
    finally:
        server.shutdown()
        server.server_close()
    print("ChatClientTest passed")


if __name__ == "__main__":
    main()
