#!/usr/bin/env python3
"""Simple long-connection benchmark for the /ping -> pong QPS mode."""

import argparse
import socket
import select
import threading
import time


def receive_lines(sock, expected, buffered):
    completed = 0
    errors = 0
    while completed + errors < expected:
        while b"\n" in buffered and completed + errors < expected:
            line, buffered = buffered.split(b"\n", 1)
            if line == b"pong":
                completed += 1
            else:
                errors += 1
        if completed + errors < expected:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            buffered += chunk
    return completed, errors, buffered


def worker(host, port, requests, pipeline, results, index):
    completed = 0
    errors = 0
    buffered = b""
    try:
        with socket.create_connection((host, port), timeout=5.0) as sock:
            sock.settimeout(10.0)
            remaining = requests
            full_payload = b"/ping\n" * pipeline
            while remaining:
                batch = min(pipeline, remaining)
                sock.sendall(full_payload if batch == pipeline else b"/ping\n" * batch)
                done, failed, buffered = receive_lines(sock, batch, buffered)
                completed += done
                errors += failed
                remaining -= batch
            if buffered:
                errors += len([line for line in buffered.split(b"\n") if line])
            readable, _, _ = select.select([sock], [], [], 0.05)
            if readable:
                extra = sock.recv(65536)
                if extra:
                    errors += len([line for line in extra.split(b"\n") if line])
    except Exception as exc:
        # Only requests not already classified as completed/error are outstanding.
        errors += requests - completed - errors
        results[index] = (completed, errors, str(exc))
        return
    results[index] = (completed, errors, "")


def main():
    parser = argparse.ArgumentParser(description="Benchmark webserver TCP QPS mode")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18082)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--requests", type=int, default=100000)
    parser.add_argument("--pipeline", type=int, default=100)
    args = parser.parse_args()

    if not 1 <= args.connections <= 2000:
        raise SystemExit("connections must be in [1, 2000]")
    if args.requests <= 0:
        raise SystemExit("requests must be positive")
    if args.connections > args.requests:
        raise SystemExit("connections must not exceed requests")
    if not 1 <= args.pipeline <= 10000:
        raise SystemExit("pipeline must be in [1, 10000]")

    base, extra = divmod(args.requests, args.connections)
    results = [(0, 0, "")] * args.connections
    threads = []
    started = time.perf_counter()
    for index in range(args.connections):
        count = base + (1 if index < extra else 0)
        thread = threading.Thread(
            target=worker,
            args=(args.host, args.port, count, args.pipeline, results, index),
        )
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()

    elapsed = time.perf_counter() - started
    completed = sum(item[0] for item in results)
    errors = sum(item[1] for item in results)
    messages = [item[2] for item in results if item[2]]
    print(f"requests={args.requests}")
    print(f"completed={completed}")
    print(f"errors={errors}")
    print(f"connections={args.connections}")
    print(f"pipeline={args.pipeline}")
    print(f"duration={elapsed:.3f}s")
    print(f"qps={completed / elapsed if elapsed > 0 else 0:.2f}")
    if messages:
        print(f"first_error={messages[0]}")
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
