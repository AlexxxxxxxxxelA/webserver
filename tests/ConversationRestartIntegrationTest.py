import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ContextMockHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        assert request["thinking"] == {"type": "disabled"}
        messages = request["messages"]
        current = messages[-1]["content"]

        if current == "REMEMBER_TEST":
            answer = "持久化记忆答案"
        elif current == "CHECK_RESTORED_TEST":
            restored = any(
                message.get("role") == "assistant"
                and message.get("content") == "持久化记忆答案"
                for message in messages[:-1]
            )
            answer = "RESTORED" if restored else "MISSING"
        elif current == "CHECK_CLEARED_TEST":
            restored = any(
                message.get("role") == "assistant"
                and message.get("content") == "持久化记忆答案"
                for message in messages[:-1]
            )
            answer = "NOT_CLEARED" if restored else "CLEARED"
        else:
            answer = "UNKNOWN"

        body = json.dumps({
            "choices": [{
                "finish_reason": "stop",
                "message": {"role": "assistant", "content": answer},
            }],
            "usage": {
                "prompt_tokens": len(messages) * 4,
                "completion_tokens": 1,
                "total_tokens": len(messages) * 4 + 1,
            },
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def wait_for_server(process):
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            raise RuntimeError("agent server exited before becoming ready")
        try:
            connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=0.2)
            connection.request("GET", "/health")
            response = connection.getresponse()
            response.read()
            connection.close()
            if response.status == 200:
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("agent server did not become ready")


def start_server(binary, source_root, database_path):
    environment = os.environ.copy()
    environment.update({
        "DEEPSEEK_API_KEY": "local-context-token",
        "DEEPSEEK_API_URL": "http://127.0.0.1:19090/deepseek",
        "DEEPSEEK_MODEL": "deepseek-v4-flash",
        "CONVERSATION_DATABASE_PATH": database_path,
        # 该测试只关注跨进程 Context；同时覆盖 Thinking 配置可以安全关闭。
        "DEEPSEEK_THINKING_ENABLED": "false",
    })
    process = subprocess.Popen(
        [binary], cwd=source_root, env=environment,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    wait_for_server(process)
    return process


def assert_invalid_thinking_config_fails(binary, source_root, database_path):
    environment = os.environ.copy()
    environment.update({
        "DEEPSEEK_API_KEY": "local-context-token",
        "DEEPSEEK_API_URL": "http://127.0.0.1:19090/deepseek",
        "CONVERSATION_DATABASE_PATH": database_path,
        "DEEPSEEK_THINKING_ENABLED": "fales",
    })
    process = subprocess.Popen(
        [binary], cwd=source_root, env=environment,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        assert process.wait(timeout=2) != 0
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise AssertionError("invalid Thinking configuration was silently accepted")


def stop_server(process):
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def post(path, payload):
    connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
    connection.request("POST", path, json.dumps(payload),
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    body = json.loads(response.read().decode("utf-8"))
    status = response.status
    connection.close()
    return status, body


def get_json(path):
    connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
    connection.request("GET", path)
    response = connection.getresponse()
    body = json.loads(response.read().decode("utf-8"))
    status = response.status
    connection.close()
    return status, body


def receive_until(sock, marker):
    data = b""
    deadline = time.time() + 5
    while marker not in data and time.time() < deadline:
        data += sock.recv(4096)
    return data.decode("utf-8")


def main():
    binary, source_root = sys.argv[1], sys.argv[2]
    database_path = f"/tmp/webserver-context-restart-{os.getpid()}.db"
    mock = ThreadingHTTPServer(("127.0.0.1", 19090), ContextMockHandler)
    mock_thread = threading.Thread(target=mock.serve_forever, daemon=True)
    mock_thread.start()
    process = None
    try:
        process = start_server(binary, source_root, database_path)
        status, created_session = post("/agent/sessions", {"title": "重启测试聊天"})
        assert status == 201
        managed_session = created_session["session_id"]
        status, _ = post("/agent/sessions", {"title": "bad\u202etitle"})
        assert status == 400
        status, session_list = get_json("/agent/sessions")
        assert status == 200
        assert any(
            session["session_id"] == managed_session
            and session["title"] == "重启测试聊天"
            and session["turn_count"] == 0
            for session in session_list["sessions"]
        )
        tcp = socket.create_connection(("127.0.0.1", 18080), timeout=5)
        receive_until(tcp, b"> ")
        tcp.sendall(b"/clear\n")
        tcp_reply = receive_until(tcp, b"> ")
        assert "Conversation cleared." in tcp_reply
        tcp.close()

        status, first = post("/agent/run", {
            "session_id": managed_session, "message": "REMEMBER_TEST"
        })
        assert status == 200
        assert first["answer"] == "持久化记忆答案"
        assert first["metrics"]["context_recent_turns"] == 0
        stop_server(process)
        process = None

        process = start_server(binary, source_root, database_path)
        status, restored = post("/agent/run", {
            "session_id": managed_session, "message": "CHECK_RESTORED_TEST"
        })
        assert status == 200
        assert restored["answer"] == "RESTORED"
        assert restored["metrics"]["context_recent_turns"] >= 1

        status, cleared = post("/agent/clear", {
            "session_id": managed_session
        })
        assert status == 200
        assert cleared == {"session_id": managed_session, "cleared": True}
        stop_server(process)
        process = None

        process = start_server(binary, source_root, database_path)
        status, after_clear = post("/agent/run", {
            "session_id": managed_session, "message": "CHECK_CLEARED_TEST"
        })
        assert status == 200
        assert after_clear["answer"] == "CLEARED"
        assert after_clear["metrics"]["context_recent_turns"] == 0
        stop_server(process)
        process = None

        assert_invalid_thinking_config_fails(binary, source_root, database_path)
    finally:
        if process is not None:
            stop_server(process)
        mock.shutdown()
        mock.server_close()
        for suffix in ("", "-wal", "-shm"):
            try:
                os.remove(database_path + suffix)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
