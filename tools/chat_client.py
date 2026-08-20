#!/usr/bin/env python3
"""Interactive HTTP/SSE client for the C++ Agent server.

The CLI is intentionally a thin terminal UI. Tool Calling, Thinking, context,
SQLite, cancellation, and model calls remain in the C++ server.
"""

import argparse
import codecs
from datetime import datetime
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import unicodedata
import uuid
from urllib.parse import urlsplit


MAX_ERROR_BODY_BYTES = 64 * 1024
MAX_SSE_BUFFER_BYTES = 1024 * 1024


class AgentClientError(RuntimeError):
    def __init__(self, message, status=None, code=None, displayed=False):
        super().__init__(message)
        self.status = status
        self.code = code
        self.displayed = displayed


class SseParser:
    """Incrementally converts arbitrary UTF-8 bytes into SSE events.

    HTTP chunk boundaries, UTF-8 character boundaries, and SSE event boundaries
    are independent. http.client removes HTTP chunk framing; this parser still
    needs an incremental UTF-8 decoder and an unfinished-event buffer.
    """

    def __init__(self):
        self._decoder = codecs.getincrementaldecoder("utf-8")("strict")
        self._pending = ""

    def feed(self, data):
        try:
            self._pending += self._decoder.decode(data)
        except UnicodeDecodeError as exc:
            raise AgentClientError(f"invalid UTF-8 in SSE stream: {exc}") from exc
        if len(self._pending.encode("utf-8")) > MAX_SSE_BUFFER_BYTES:
            raise AgentClientError("SSE event buffer exceeded 1 MiB")
        return self._extract_events()

    def finish(self):
        try:
            self._pending += self._decoder.decode(b"", final=True)
        except UnicodeDecodeError as exc:
            raise AgentClientError(f"incomplete UTF-8 in SSE stream: {exc}") from exc
        events = self._extract_events()
        if self._pending.strip():
            raise AgentClientError("SSE stream ended with an incomplete event")
        return events

    def _extract_events(self):
        events = []
        # The C++ server emits LF. Normalizing CRLF also makes the parser usable
        # with proxies or other SSE providers.
        self._pending = self._pending.replace("\r\n", "\n")
        while "\n\n" in self._pending:
            block, self._pending = self._pending.split("\n\n", 1)
            event = self._parse_block(block)
            if event is not None:
                events.append(event)
        return events

    @staticmethod
    def _parse_block(block):
        event_type = "message"
        data_lines = []
        for line in block.split("\n"):
            if not line or line.startswith(":"):
                continue
            field, separator, value = line.partition(":")
            if not separator:
                value = ""
            elif value.startswith(" "):
                value = value[1:]
            if field == "event":
                event_type = value
            elif field == "data":
                data_lines.append(value)
            # id/retry and unknown extension fields are not needed by this CLI.
        if not data_lines:
            return None
        raw_data = "\n".join(data_lines)
        try:
            payload = json.loads(raw_data)
        except json.JSONDecodeError as exc:
            raise AgentClientError(f"invalid JSON in SSE data: {exc}") from exc
        if not isinstance(payload, dict):
            raise AgentClientError("SSE data JSON must be an object")
        return event_type, payload


class AgentHttpClient:
    def __init__(self, base_url, timeout=75.0):
        parsed = urlsplit(base_url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname:
            raise ValueError("URL must start with http:// or https:// and contain a host")
        if parsed.query or parsed.fragment:
            raise ValueError("base URL must not contain query or fragment")
        self._scheme = parsed.scheme
        self._host = parsed.hostname
        self._port = parsed.port or (443 if parsed.scheme == "https" else 80)
        self._prefix = parsed.path.rstrip("/")
        self._timeout = timeout

    def _connect(self):
        connection_type = (
            http.client.HTTPSConnection if self._scheme == "https"
            else http.client.HTTPConnection
        )
        return connection_type(self._host, self._port, timeout=self._timeout)

    def _path(self, suffix):
        return self._prefix + suffix

    @staticmethod
    def _request_body(session_id, message=None):
        validate_session_id(session_id)
        body = {"session_id": session_id}
        if message is not None:
            if not isinstance(message, str) or not message.strip():
                raise ValueError("message must not be empty")
            body["message"] = message
        return json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    def health(self):
        connection = self._connect()
        try:
            connection.request("GET", self._path("/health"))
            response = connection.getresponse()
            body = response.read(MAX_ERROR_BODY_BYTES).decode("utf-8", errors="replace")
            if response.status != 200:
                raise AgentClientError(
                    f"health check failed: HTTP {response.status}: {body.strip()}",
                    status=response.status,
                )
            return body.strip()
        finally:
            connection.close()

    def list_sessions(self):
        payload = self._get_json("/agent/sessions")
        sessions = payload.get("sessions")
        if not isinstance(sessions, list) or any(not isinstance(item, dict) for item in sessions):
            raise AgentClientError("session list response is invalid")
        return sessions

    def create_session(self, title=""):
        if not isinstance(title, str) or len(title.encode("utf-8")) > 256:
            raise ValueError("session title must not exceed 256 UTF-8 bytes")
        body = json.dumps(
            {"title": title}, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        return self._post_json("/agent/sessions", body)

    def clear_session(self, session_id):
        return self._post_json("/agent/clear", self._request_body(session_id))

    def _post_json(self, path, body):
        connection = self._connect()
        try:
            connection.request(
                "POST", self._path(path), body,
                {"Content-Type": "application/json; charset=utf-8"},
            )
            response = connection.getresponse()
            raw = response.read(MAX_ERROR_BODY_BYTES)
            payload = decode_json_body(raw, response.status)
            if not 200 <= response.status < 300:
                raise error_from_payload(response.status, payload)
            return payload
        finally:
            connection.close()

    def _get_json(self, path):
        connection = self._connect()
        try:
            connection.request("GET", self._path(path), headers={"Accept": "application/json"})
            response = connection.getresponse()
            raw = response.read(MAX_ERROR_BODY_BYTES)
            payload = decode_json_body(raw, response.status)
            if not 200 <= response.status < 300:
                raise error_from_payload(response.status, payload)
            return payload
        finally:
            connection.close()

    def stream_run(self, session_id, message, on_event):
        body = self._request_body(session_id, message)
        connection = self._connect()
        response = None
        raw_socket = None
        parser = SseParser()
        terminal = None
        try:
            connection.request(
                "POST", self._path("/agent/run/stream"), body,
                {
                    "Content-Type": "application/json; charset=utf-8",
                    "Accept": "text/event-stream",
                },
            )
            # HTTPResponse may take ownership and clear connection.sock after seeing
            # Connection: close, so retain the Socket reference for Ctrl+C RST abort.
            raw_socket = connection.sock
            response = connection.getresponse()
            if response.status != 200:
                raw = response.read(MAX_ERROR_BODY_BYTES)
                raise error_from_payload(response.status, decode_json_body(raw, response.status))
            content_type = response.getheader("Content-Type", "").lower()
            if "text/event-stream" not in content_type:
                raise AgentClientError(
                    f"server returned unexpected Content-Type: {content_type or '<missing>'}"
                )

            while True:
                # read1 returns available decoded HTTP body data without waiting to fill the
                # entire requested size, which preserves SSE interactivity.
                chunk = response.read1(4096)
                if not chunk:
                    break
                for event in parser.feed(chunk):
                    if terminal is not None:
                        raise AgentClientError(
                            f"SSE event {event[0]} arrived after terminal {terminal[0]}"
                        )
                    on_event(*event)
                    if event[0] in ("run.completed", "error"):
                        terminal = event
            for event in parser.finish():
                if terminal is not None:
                    raise AgentClientError(
                        f"SSE event {event[0]} arrived after terminal {terminal[0]}"
                    )
                on_event(*event)
                if event[0] in ("run.completed", "error"):
                    terminal = event

            if terminal is None:
                raise AgentClientError("SSE stream ended without run.completed or error")
            if terminal[0] == "error":
                payload = terminal[1]
                raise AgentClientError(
                    payload.get("message", "agent stream failed"),
                    code=payload.get("code"),
                    displayed=True,
                )
            return terminal[1]
        except KeyboardInterrupt:
            force_reset_socket(raw_socket)
            raise
        except AgentClientError:
            raise
        except (UnicodeDecodeError, http.client.HTTPException, AttributeError, TypeError) as exc:
            raise AgentClientError(f"invalid HTTP/SSE response: {exc}") from exc
        finally:
            # Closing an unfinished response also closes the underlying socket, allowing the
            # C++ server to propagate cancellation to DeepSeek/Weather.
            if response is not None:
                response.close()
            connection.close()


class TerminalRenderer:
    def __init__(self, output=None, show_metrics=False):
        self.output = output or sys.stdout
        self.show_metrics = show_metrics
        self._open_sequence = None
        self._seen_sequences = set()

    def write_line(self, text=""):
        if self._open_sequence is not None:
            self.output.write("\n")
            self._open_sequence = None
        self.output.write(text + "\n")
        self.output.flush()

    def handle(self, event_type, data):
        sequence = data.get("sequence")
        if event_type == "run.started":
            self.write_line(f"[Run] {data.get('run_id', '<unknown>')}")
        elif event_type == "assistant.thinking.started":
            self.write_line(f"[思考中 #{sequence}]")
        elif event_type == "assistant.thinking.completed":
            self.write_line(f"[思考完成 #{sequence}]")
        elif event_type == "tool.started":
            self.write_line(f"[调用工具] {data.get('name', '<unknown>')}")
        elif event_type == "tool.completed":
            status = "成功" if data.get("ok") else "失败"
            latency = data.get("latency_ms", 0)
            self.write_line(f"[工具{status}] {data.get('name', '<unknown>')} ({latency} ms)")
        elif event_type == "assistant.delta":
            if sequence not in self._seen_sequences:
                self.write_line(f"[模型输出 #{sequence}] ")
                self._seen_sequences.add(sequence)
                self._open_sequence = sequence
            elif self._open_sequence != sequence:
                self.write_line(f"[模型输出 #{sequence}] ")
                self._open_sequence = sequence
            self.output.write(str(data.get("text", "")))
            self.output.flush()
        elif event_type == "run.completed":
            final_sequence = data.get("answer_sequence")
            self.write_line(f"[完成] 最终答案序号 #{final_sequence}")
            if self.show_metrics:
                self._render_metrics(data.get("metrics", {}))
        elif event_type == "error":
            code = data.get("code", "UNKNOWN_ERROR")
            self.write_line(f"[错误 {code}] {data.get('message', 'agent stream failed')}")

    def _render_metrics(self, metrics):
        usage = metrics.get("usage", {})
        self.write_line(
            "[指标] "
            f"总耗时={metrics.get('total_latency_ms', 0)}ms, "
            f"模型调用={metrics.get('model_calls', 0)}, "
            f"工具调用={metrics.get('tool_calls', 0)}, "
            f"总Token={usage.get('total_tokens', 0)}, "
            f"思考Token={usage.get('reasoning_tokens', 0)}"
        )


def decode_json_body(raw, status):
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise AgentClientError(
            f"HTTP {status} returned invalid JSON", status=status
        ) from exc
    if not isinstance(payload, dict):
        raise AgentClientError(f"HTTP {status} JSON body must be an object", status=status)
    return payload


def error_from_payload(status, payload):
    return AgentClientError(
        payload.get("error", payload.get("message", f"HTTP {status}")),
        status=status,
        code=payload.get("code"),
    )


def validate_session_id(session_id):
    if not isinstance(session_id, str) or not 1 <= len(session_id) <= 120:
        raise ValueError("session ID length must be in [1, 120]")
    if any(not (character.isascii() and (character.isalnum() or character in "-_.:"))
           for character in session_id):
        raise ValueError("session ID may contain only ASCII letters, digits, '-', '_', '.', ':'")


def new_session_id():
    return "cli-" + uuid.uuid4().hex


def read_saved_session(path):
    path = path.expanduser()
    if not path.exists():
        return None
    session_id = path.read_text(encoding="utf-8").strip()
    validate_session_id(session_id)
    return session_id


def load_or_create_session(path, explicit_session=None, force_new=False, save=True):
    if explicit_session:
        validate_session_id(explicit_session)
        session_id = explicit_session
    elif not force_new and path.exists():
        session_id = path.read_text(encoding="utf-8").strip()
        validate_session_id(session_id)
    else:
        session_id = new_session_id()
    if save:
        save_session(path, session_id)
    return session_id


def safe_terminal_text(value):
    text = str(value)
    return "".join(character if unicodedata.category(character) not in ("Cc", "Cf") else "?"
                   for character in text)


def format_timestamp(milliseconds):
    try:
        return datetime.fromtimestamp(int(milliseconds) / 1000).strftime("%Y-%m-%d %H:%M")
    except (TypeError, ValueError, OSError, OverflowError):
        return "未知时间"


def show_sessions(sessions, last_session=None, output=sys.stdout):
    if not sessions:
        output.write("当前没有历史聊天。\n")
        output.flush()
        return
    output.write("历史聊天（最近更新优先）：\n")
    for index, session in enumerate(sessions, 1):
        session_id = safe_terminal_text(session.get("session_id", ""))
        title = safe_terminal_text(session.get("title") or "新聊天")
        marker = " [上次使用]" if session_id == last_session else ""
        output.write(
            f"  [{index}] {title}{marker}\n"
            f"      ID={session_id}  Turn={session.get('turn_count', 0)}  "
            f"更新={format_timestamp(session.get('updated_at_ms'))}\n"
        )
    output.flush()


def choose_session(client, session_file, save=True, input_fn=input, output=sys.stdout):
    """Interactively choose a server-side chat session or create a new one."""
    last_session = None
    try:
        last_session = read_saved_session(session_file)
    except (ValueError, OSError):
        # A broken local pointer must not prevent listing the authoritative server catalog.
        last_session = None
    sessions = client.list_sessions()
    output.write("\n请选择聊天框：\n  [0] 新建聊天\n")
    show_sessions(sessions, last_session=last_session, output=output)

    default_index = None
    if last_session:
        for index, session in enumerate(sessions, 1):
            if session.get("session_id") == last_session:
                default_index = index
                break
    prompt = "选择编号"
    if default_index is not None:
        prompt += f"（直接回车继续上次 [{default_index}]）"
    prompt += ": "

    while True:
        choice = input_fn(prompt).strip()
        if not choice and default_index is not None:
            choice = str(default_index)
        if choice == "0":
            created = client.create_session()
            session_id = created.get("session_id")
            validate_session_id(session_id)
            title = created.get("title", "新聊天")
            output.write(f"[已新建] {safe_terminal_text(title)} ({session_id})\n")
            break
        try:
            index = int(choice)
        except ValueError:
            output.write("请输入列表中的数字。\n")
            output.flush()
            continue
        if 1 <= index <= len(sessions):
            session_id = sessions[index - 1].get("session_id")
            validate_session_id(session_id)
            output.write(
                f"[已进入] {safe_terminal_text(sessions[index - 1].get('title') or '新聊天')}\n"
            )
            break
        output.write("编号不在列表中。\n")
        output.flush()

    if save:
        save_session(session_file, session_id)
    output.flush()
    return session_id


def save_session(path, session_id):
    path = path.expanduser()
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=str(path.parent)
    )
    try:
        if os.name != "nt":
            os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            descriptor = -1
            stream.write(session_id + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_name, path)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.remove(temporary_name)
        except FileNotFoundError:
            pass


def force_reset_socket(sock):
    """Close a cancelled stream with RST so the C++ server observes cancellation promptly."""
    if sock is None:
        return
    try:
        linger = struct.pack("HH", 1, 0) if os.name == "nt" else struct.pack("ii", 1, 0)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass


def print_help(output=sys.stdout):
    output.write(
        "命令：\n"
        "  /help              显示帮助\n"
        "  /health            检查 HTTP Server\n"
        "  /session           显示当前 Session ID\n"
        "  /list              列出并选择历史聊天\n"
        "  /use <session_id>  切换到指定 Session\n"
        "  /new               在服务器创建新的聊天框\n"
        "  /clear             清除当前 Session 的 SQLite 历史\n"
        "  /metrics           切换完成后的指标显示\n"
        "  /quit              退出\n"
        "其他输入会作为自然语言通过 POST /agent/run/stream 发送。\n"
    )
    output.flush()


def configure_console():
    # Windows legacy consoles may not default to UTF-8. reconfigure is harmless on
    # modern PowerShell/Windows Terminal and unavailable on some redirected streams.
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass


def run_message(client, renderer, session_id, message):
    try:
        client.stream_run(session_id, message, renderer.handle)
        return True
    except KeyboardInterrupt:
        renderer.write_line("[已取消] 当前流连接已关闭")
        return False
    except (AgentClientError, OSError) as exc:
        if not getattr(exc, "displayed", False):
            renderer.write_line(f"[请求失败] {exc}")
        return False


def noninteractive_session(client, session_file, explicit_session, force_new, save):
    if explicit_session:
        validate_session_id(explicit_session)
        session_id = explicit_session
    elif force_new:
        session_id = client.create_session().get("session_id")
        validate_session_id(session_id)
    else:
        try:
            session_id = read_saved_session(session_file)
        except (ValueError, OSError):
            session_id = None
        if not session_id:
            session_id = client.create_session().get("session_id")
            validate_session_id(session_id)
    if save:
        save_session(session_file, session_id)
    return session_id


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Interactive natural-language client for the C++ HTTP/SSE Agent"
    )
    parser.add_argument("--url", default="http://127.0.0.1:18081")
    parser.add_argument("--session", help="use an explicit HTTP session ID")
    parser.add_argument(
        "--session-file", type=Path,
        default=Path.home() / ".webserver_agent_cli_session",
    )
    parser.add_argument("--new-session", action="store_true")
    parser.add_argument("--no-save-session", action="store_true")
    parser.add_argument("--timeout", type=float, default=75.0)
    parser.add_argument("--show-metrics", action="store_true")
    parser.add_argument("--once", help="send one message and exit")
    args = parser.parse_args(argv)

    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    configure_console()
    session_file = args.session_file.expanduser()
    if args.once is None:
        print("C++ Agent HTTP/SSE CLI")
        print(f"Server:  {args.url}")
    try:
        client = AgentHttpClient(args.url, timeout=args.timeout)
        if args.once is not None or args.session or args.new_session:
            session_id = noninteractive_session(
                client, session_file, args.session, args.new_session,
                save=not args.no_save_session,
            )
        else:
            session_id = choose_session(
                client, session_file, save=not args.no_save_session
            )
    except (EOFError, KeyboardInterrupt):
        print("\n已退出。")
        return 0
    except (ValueError, OSError, AgentClientError) as exc:
        raise SystemExit(f"CLI configuration error: {exc}")

    renderer = TerminalRenderer(show_metrics=args.show_metrics)
    if args.once is not None:
        if not run_message(client, renderer, session_id, args.once):
            raise SystemExit(1)
        return 0

    print(f"Session: {session_id}")
    print("输入 /help 查看命令，直接输入自然语言开始聊天。")

    while True:
        try:
            message = input("\n> ").strip()
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print("\n再次输入 /quit 退出，或继续输入消息。")
            continue
        if not message:
            continue
        if message == "/quit":
            break
        if message == "/help":
            print_help()
            continue
        if message == "/health":
            try:
                print(f"[健康] {client.health()}")
            except (AgentClientError, OSError) as exc:
                print(f"[健康检查失败] {exc}")
            continue
        if message == "/session":
            print(f"Session: {session_id}")
            continue
        if message == "/list":
            try:
                session_id = choose_session(
                    client, session_file, save=not args.no_save_session
                )
            except (EOFError, KeyboardInterrupt):
                print("\n[已取消选择]")
            except (ValueError, OSError, AgentClientError) as exc:
                print(f"[聊天列表失败] {exc}")
            continue
        if message.startswith("/use "):
            candidate = message[5:].strip()
            try:
                validate_session_id(candidate)
                if not args.no_save_session:
                    save_session(session_file, candidate)
                session_id = candidate
                print(f"[已切换 Session] {session_id}")
            except (ValueError, OSError) as exc:
                print(f"[切换失败] {exc}")
            continue
        if message == "/new":
            try:
                created = client.create_session()
                candidate = created.get("session_id")
                validate_session_id(candidate)
                if not args.no_save_session:
                    save_session(session_file, candidate)
                session_id = candidate
                print(
                    f"[新聊天] {safe_terminal_text(created.get('title') or '新聊天')} "
                    f"({session_id})"
                )
            except (ValueError, OSError, AgentClientError) as exc:
                print(f"[创建 Session 失败] {exc}")
            continue
        if message == "/clear":
            try:
                client.clear_session(session_id)
                print(f"[已清除] {session_id}")
            except (AgentClientError, OSError) as exc:
                print(f"[清除失败] {exc}")
            continue
        if message == "/metrics":
            renderer.show_metrics = not renderer.show_metrics
            print(f"[指标显示] {'开启' if renderer.show_metrics else '关闭'}")
            continue
        if message.startswith("/"):
            print("[未知命令] 输入 /help 查看可用命令")
            continue
        run_message(client, renderer, session_id, message)


if __name__ == "__main__":
    main()
