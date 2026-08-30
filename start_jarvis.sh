#!/usr/bin/env bash
# One-shot launcher for the full JARVIS stack: Ollama check, C++ build (if missing), the
# Python AI server, the C++ gRPC server, then drops you into an interactive client that talks
# to the real pipeline (known commands + natural language + LLM escalation).
#
# Usage: ./start_jarvis.sh
# Ctrl+C or 'exit' at the prompt stops the interactive client and shuts down both servers.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

AI_SERVER_LOG="$ROOT/.jarvis-ai-server.log"
GRPC_SERVER_LOG="$ROOT/.jarvis-grpc-server.log"

AI_SERVER_PID=""
GRPC_SERVER_PID=""

cleanup() {
    echo
    echo "Shutting down..."
    [[ -n "$GRPC_SERVER_PID" ]] && kill "$GRPC_SERVER_PID" 2>/dev/null || true
    [[ -n "$AI_SERVER_PID" ]] && kill "$AI_SERVER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    echo "Stopped. Logs kept at:"
    echo "  $AI_SERVER_LOG"
    echo "  $GRPC_SERVER_LOG"
}
trap cleanup EXIT

echo "== JARVIS startup =="

# 1. Ollama check — warn, don't block. The LLM tier degrades gracefully without it.
echo "-- Checking Ollama (LLM fallback tier)..."
if curl -s -m 2 http://localhost:11434/api/version > /dev/null 2>&1; then
    echo "   Ollama is reachable."
else
    echo "   WARNING: Ollama not reachable at localhost:11434."
    echo "   JARVIS will still run — natural-language input the rule classifier can't"
    echo "   match will just fall back to 'unknown' instead of escalating to the LLM."
fi

# 2. Python virtualenv check
if [[ ! -x "$ROOT/.venv/bin/python" ]]; then
    echo "ERROR: .venv/ not found or has no python. Set up the venv first — see README.md."
    exit 1
fi
PYTHON="$ROOT/.venv/bin/python"

# 3. Generated protobuf stubs — regenerate if missing (gitignored, not shipped)
if [[ ! -f "$ROOT/generated/python/jarvis_pb2.py" ]] || [[ ! -f "$ROOT/generated/cpp/jarvis.pb.cc" ]]; then
    echo "-- Generated protobuf stubs missing, regenerating..."
    mkdir -p generated/cpp generated/python
    protoc --proto_path=proto \
           --cpp_out=generated/cpp \
           --grpc_out=generated/cpp \
           --plugin=protoc-gen-grpc="$(which grpc_cpp_plugin)" \
           proto/jarvis.proto proto/ai.proto
    "$PYTHON" -m grpc_tools.protoc -Iproto \
           --python_out=generated/python \
           --grpc_python_out=generated/python \
           proto/jarvis.proto proto/ai.proto
fi

# 4. C++ build — build if the server binary is missing
if [[ ! -x "$ROOT/build/jarvis_grpc_server" ]]; then
    echo "-- build/jarvis_grpc_server not found, building..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j"$(nproc)"
fi

# 5. Start the Python AI server
echo "-- Starting AI server (ai/jarvis_ai_server.py) on :50052..."
"$PYTHON" ai/jarvis_ai_server.py > "$AI_SERVER_LOG" 2>&1 &
AI_SERVER_PID=$!
sleep 1
if ! kill -0 "$AI_SERVER_PID" 2>/dev/null; then
    echo "ERROR: AI server failed to start. Log:"
    cat "$AI_SERVER_LOG"
    exit 1
fi

# 6. Start the C++ gRPC server
echo "-- Starting gRPC server (build/jarvis_grpc_server) on :50051..."
"$ROOT/build/jarvis_grpc_server" > "$GRPC_SERVER_LOG" 2>&1 &
GRPC_SERVER_PID=$!
sleep 1
if ! kill -0 "$GRPC_SERVER_PID" 2>/dev/null; then
    echo "ERROR: gRPC server failed to start. Log:"
    cat "$GRPC_SERVER_LOG"
    exit 1
fi

echo "== Both servers running. Launching interactive client. =="
echo

"$PYTHON" tools/interactive_client.py
