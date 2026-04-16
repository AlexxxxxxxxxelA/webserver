#!/usr/bin/env python3
import argparse
import socket
import threading
import time


def recv_line(sock, buffer):
    while b"\n" not in buffer:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed")
        buffer += chunk
    line, buffer = buffer.split(b"\n", 1)
    return line, buffer


def recv_pongs(sock, expected, buffer):
    completed = 0
    errors = 0
    while completed + errors < expected:
        while b"\n" in buffer and completed + errors < expected:
            line, buffer = buffer.split(b"\n", 1)
            if line == b"pong":
                completed += 1
            else:
                errors += 1
        if completed + errors >= expected:
            break

        chunk = sock.recv(65536)
        if not chunk:
            raise RuntimeError("connection closed")
        buffer += chunk
    return completed, errors, buffer


def worker(host, port, requests, pipeline, result, index):
    completed = 0
    errors = 0
    buffer = b""
    try:
        with socket.create_connection((host, port), timeout=5.0) as sock:
            sock.settimeout(5.0)
            remaining = requests
            payload = b"/ping\n" * pipeline
            while remaining > 0:
                batch = min(pipeline, remaining)
                if batch == pipeline:
                    sock.sendall(payload)
                else:
                    sock.sendall(b"/ping\n" * batch)
                batch_completed, batch_errors, buffer = recv_pongs(sock, batch, buffer)
                completed += batch_completed
                errors += batch_errors
                remaining -= batch
    except Exception:
        errors += requests - completed
    result[index] = (completed, errors)


def main():
    parser = argparse.ArgumentParser(description="TCP ping/pong QPS benchmark")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--pipeline", type=int, default=1)
    args = parser.parse_args()

    if args.connections <= 0 or args.requests <= 0 or args.pipeline <= 0:
        raise SystemExit("connections, requests, and pipeline must be positive")

    base = args.requests // args.connections
    extra = args.requests % args.connections
    results = [(0, 0)] * args.connections
    threads = []

    started = time.perf_counter()
    for i in range(args.connections):
        count = base + (1 if i < extra else 0)
        thread = threading.Thread(
            target=worker,
            args=(args.host, args.port, count, args.pipeline, results, i),
        )
        thread.start()
        threads.append(thread)

    for thread in threads:
        thread.join()

    elapsed = time.perf_counter() - started
    completed = sum(item[0] for item in results)
    errors = sum(item[1] for item in results)
    qps = completed / elapsed if elapsed > 0 else 0.0

    print(f"requests={args.requests}")
    print(f"completed={completed}")
    print(f"errors={errors}")
    print(f"connections={args.connections}")
    print(f"pipeline={args.pipeline}")
    print(f"duration={elapsed:.3f}s")
    print(f"qps={qps:.2f}")

    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
