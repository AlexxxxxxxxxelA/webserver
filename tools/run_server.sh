#!/usr/bin/env bash
set -euo pipefail

project_root=${1:?project root is required}
cd "$project_root"

echo "[INFO] Configuring and building C++ Agent Server..."
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel

echo "[INFO] Starting $project_root/bin/main"
echo "[INFO] Press Ctrl+C here or run stop_agent.bat to stop the server."
exec "$project_root/bin/main"
