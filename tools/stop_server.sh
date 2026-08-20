#!/usr/bin/env bash
set -euo pipefail

project_root=${1:?project root is required}
if [[ ! -x "$project_root/bin/main" ]]; then
    echo "[INFO] No built project server exists at $project_root/bin/main"
    exit 0
fi
target=$(readlink -f "$project_root/bin/main")
pids=()

is_agent_server() {
    local pid=$1
    local resolved
    resolved=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    [[ "$resolved" == "$target" ]] || return 1
    local command_line
    command_line=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
    [[ "$command_line" != *"--qps"* ]]
}

for executable in /proc/[0-9]*/exe; do
    pid=${executable#/proc/}
    pid=${pid%/exe}
    if is_agent_server "$pid"; then
        pids+=("$pid")
    fi
done

if (( ${#pids[@]} == 0 )); then
    echo "[INFO] No running server was found for $target"
    exit 0
fi

echo "[INFO] Stopping project server PID(s): ${pids[*]}"
for pid in "${pids[@]}"; do
    if is_agent_server "$pid"; then kill -INT "$pid" 2>/dev/null || true; fi
done

for _ in {1..30}; do
    alive=()
    for pid in "${pids[@]}"; do
        if is_agent_server "$pid"; then
            alive+=("$pid")
        fi
    done
    if (( ${#alive[@]} == 0 )); then
        echo "[INFO] Server stopped."
        exit 0
    fi
    pids=("${alive[@]}")
    sleep 0.1
done

echo "[WARN] Server did not stop after SIGINT; sending SIGTERM to: ${pids[*]}"
for pid in "${pids[@]}"; do
    if is_agent_server "$pid"; then kill -TERM "$pid" 2>/dev/null || true; fi
done

for _ in {1..30}; do
    alive=()
    for pid in "${pids[@]}"; do
        if is_agent_server "$pid"; then alive+=("$pid"); fi
    done
    if (( ${#alive[@]} == 0 )); then
        echo "[INFO] Server stopped after SIGTERM."
        exit 0
    fi
    pids=("${alive[@]}")
    sleep 0.1
done

echo "[ERROR] Server is still running after SIGTERM: ${pids[*]}" >&2
exit 1
