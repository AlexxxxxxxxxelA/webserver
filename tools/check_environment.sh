#!/usr/bin/env bash
set -euo pipefail

project_root=${1:?project root is required}

[[ -d "$project_root" ]]
[[ -f "$project_root/CMakeLists.txt" ]]
[[ -f "$project_root/tools/chat_client.py" ]]

command -v bash >/dev/null
command -v cmake >/dev/null
command -v c++ >/dev/null
command -v pkg-config >/dev/null
pkg-config --exists libcurl sqlite3
[[ -f /usr/include/nlohmann/json.hpp ]]

echo "[OK] WSL project and C++ build dependencies are available."
