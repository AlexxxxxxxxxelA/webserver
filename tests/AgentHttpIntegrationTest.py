import glob
import http.client
import json
import os
import signal
import sqlite3
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/weather/FailureCity"):
            body = b"SENSITIVE_UPSTREAM_BODY_SENTINEL"
            self.send_response(500)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/weather/"):
            body = b"Beijing: Sunny +25C"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        assert request["thinking"] == {"type": "enabled"}
        user_message = next(
            message["content"]
            for message in reversed(request["messages"])
            if message["role"] == "user"
        )

        if user_message == "RATE_LIMIT_TEST":
            body = b'{"error":"SENSITIVE_UPSTREAM_BODY_SENTINEL"}'
            self.send_response(429)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        tool_messages = [m for m in request["messages"] if m["role"] == "tool"]
        if user_message == "MULTI_THINKING_TEST":
            assistant_calls = [m for m in request["messages"] if m.get("tool_calls")]
            if len(tool_messages) == 0:
                message = {
                    "role": "assistant",
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_MULTI_R1",
                    "content": None,
                    "tool_calls": [{
                        "id": "multi-time-1",
                        "type": "function",
                        "function": {"name": "time", "arguments": "{}"},
                    }],
                }
                finish_reason = "tool_calls"
            elif len(tool_messages) == 1:
                assert [m["reasoning_content"] for m in assistant_calls] == [
                    "PRIVATE_REASONING_SENTINEL_MULTI_R1"
                ]
                message = {
                    "role": "assistant",
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_MULTI_R2",
                    "content": None,
                    "tool_calls": [{
                        "id": "multi-weather-2",
                        "type": "function",
                        "function": {
                            "name": "weather",
                            "arguments": json.dumps({"location": "Beijing"}),
                        },
                    }],
                }
                finish_reason = "tool_calls"
            else:
                assert [m["reasoning_content"] for m in assistant_calls] == [
                    "PRIVATE_REASONING_SENTINEL_MULTI_R1",
                    "PRIVATE_REASONING_SENTINEL_MULTI_R2",
                ]
                message = {
                    "role": "assistant",
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_MULTI_R3",
                    "content": "MULTI_THINKING_OK",
                }
                finish_reason = "stop"
            usage = {
                "prompt_tokens": 7,
                "completion_tokens": 3,
                "total_tokens": 10,
                "completion_tokens_details": {"reasoning_tokens": 2},
            }
        elif not tool_messages:
            location = "FailureCity" if user_message == "WEATHER_FAILURE_TEST" else "Beijing"
            message = {
                "role": "assistant",
                "reasoning_content": "PRIVATE_REASONING_SENTINEL_HTTP_R1",
                "content": None,
                "tool_calls": [{
                    "id": "trace-weather-1",
                    "type": "function",
                    "function": {
                        "name": "weather",
                        "arguments": json.dumps({"location": location}),
                    },
                }],
            }
            finish_reason = "tool_calls"
            usage = {
                "prompt_tokens": 10,
                "completion_tokens": 2,
                "total_tokens": 12,
                "prompt_cache_hit_tokens": 4,
                "prompt_cache_miss_tokens": 6,
            }
        else:
            assistant_calls = [m for m in request["messages"] if m.get("tool_calls")]
            assert assistant_calls[-1]["reasoning_content"] == \
                "PRIVATE_REASONING_SENTINEL_HTTP_R1"
            tool_result = json.loads(tool_messages[-1]["content"])
            if user_message == "WEATHER_FAILURE_TEST":
                assert tool_result == {"ok": False, "error": "weather service request failed"}
                message = {
                    "role": "assistant",
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_HTTP_R2",
                    "content": "天气服务暂时不可用。",
                }
            else:
                assert tool_result == {"ok": True, "result": "Beijing: Sunny +25C"}
                message = {
                    "role": "assistant",
                    "reasoning_content": "PRIVATE_REASONING_SENTINEL_HTTP_R2",
                    "content": "北京天气晴朗。",
                }
            finish_reason = "stop"
            usage = {
                "prompt_tokens": 20,
                "completion_tokens": 3,
                "total_tokens": 23,
                "prompt_cache_hit_tokens": 8,
                "prompt_cache_miss_tokens": 12,
            }

        body = json.dumps({
            "choices": [{"finish_reason": finish_reason, "message": message}],
            "usage": usage,
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def post_agent(message, session_id):
    connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=5)
    body = json.dumps({"session_id": session_id, "message": message})
    connection.request("POST", "/agent/run", body, {"Content-Type": "application/json"})
    response = connection.getresponse()
    response_body = response.read().decode("utf-8")
    headers = dict(response.getheaders())
    connection.close()
    return response.status, headers, json.loads(response_body)


def wait_for_server():
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            connection = http.client.HTTPConnection("127.0.0.1", 18081, timeout=0.2)
            connection.request("GET", "/health")
            connection.getresponse().read()
            connection.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("agent server did not start")


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


def main():
    binary, source_root = sys.argv[1], sys.argv[2]
    database_path = f"/tmp/webserver-agent-http-{os.getpid()}.db"
    mock = ThreadingHTTPServer(("127.0.0.1", 19090), MockHandler)
    mock_thread = threading.Thread(target=mock.serve_forever)
    mock_thread.daemon = True
    mock_thread.start()

    environment = os.environ.copy()
    environment.update({
        "DEEPSEEK_API_KEY": "local-test-token",
        "DEEPSEEK_API_URL": "http://127.0.0.1:19090/deepseek",
        "DEEPSEEK_MODEL": "deepseek-v4-flash",
        "WEATHER_API_BASE_URL": "http://127.0.0.1:19090/weather",
        "CONVERSATION_DATABASE_PATH": database_path,
    })
    process = subprocess.Popen(
        [binary], cwd=source_root, env=environment,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        wait_for_server()
        status, _, success = post_agent("北京天气怎么样？", "trace-success")
        assert status == 200
        assert success["run_id"].startswith("run-")
        assert success["metrics"]["model_calls"] == 2
        assert success["metrics"]["tool_calls"] == 1
        assert success["metrics"]["usage"]["total_tokens"] == 35
        assert success["metrics"]["usage"]["prompt_cache_hit_tokens"] == 12
        assert success["metrics"]["queue_wait_ms"] >= 0
        assert success["metrics"]["total_latency_ms"] >= 0
        assert success["metrics"]["model_latency_ms"] >= 0
        assert success["metrics"]["tool_latency_ms"] >= 0
        assert success["tool_calls"][0]["latency_ms"] >= 0
        assert "PRIVATE_REASONING_SENTINEL" not in json.dumps(success)

        status, _, multi = post_agent("MULTI_THINKING_TEST", "trace-multi-thinking")
        assert status == 200
        assert multi["answer"] == "MULTI_THINKING_OK"
        assert multi["metrics"]["model_calls"] == 3
        assert multi["metrics"]["tool_calls"] == 2
        assert [tool["name"] for tool in multi["tool_calls"]] == ["time", "weather"]
        assert "PRIVATE_REASONING_SENTINEL" not in json.dumps(multi)

        status, _, tool_failure = post_agent("WEATHER_FAILURE_TEST", "trace-tool-failure")
        assert status == 200
        assert tool_failure["tool_calls"][0]["ok"] is False
        assert tool_failure["tool_calls"][0]["result"] == "weather service request failed"
        assert "SENSITIVE_UPSTREAM_BODY_SENTINEL" not in json.dumps(tool_failure)

        status, headers, failure = post_agent("RATE_LIMIT_TEST", "trace-rate")
        assert status == 429
        assert headers.get("Retry-After") == "1"
        assert failure["code"] == "UPSTREAM_RATE_LIMITED"
        assert failure["run_id"].startswith("run-")
        assert "SENSITIVE_UPSTREAM_BODY_SENTINEL" not in json.dumps(failure)

        success_log = wait_for_trace(source_root, success["run_id"])
        rate_log = wait_for_trace(source_root, failure["run_id"])
        tool_failure_log = wait_for_trace(source_root, tool_failure["run_id"])
        multi_log = wait_for_trace(source_root, multi["run_id"])
        assert '"total_tokens":35' in success_log
        assert '"provider_status":429' in rate_log
        assert "SENSITIVE_UPSTREAM_BODY_SENTINEL" not in rate_log
        assert "SENSITIVE_UPSTREAM_BODY_SENTINEL" not in tool_failure_log
        assert "PRIVATE_REASONING_SENTINEL" not in success_log
        assert "PRIVATE_REASONING_SENTINEL" not in rate_log
        assert "PRIVATE_REASONING_SENTINEL" not in tool_failure_log
        assert "PRIVATE_REASONING_SENTINEL" not in multi_log

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
