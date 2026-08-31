"""Interactive gRPC client for JARVIS — type anything, see the full pipeline respond.

The standalone `build/jarvis` CLI never leaves the C++ process (see core/engine.cpp), so it
can't reach the Python AI layer or the LLM fallback tier. This client talks to the C++ gRPC
server the same way tools/grpc_smoke_test.py does, but interactively: known commands (echo,
status, about, help, system-info, exit) are sent directly; anything else is sent as UNKNOWN and
flows through the real classify-then-escalate pipeline, exactly like a natural-language prompt
would.

Usage: python3 tools/interactive_client.py
(Requires ai/jarvis_ai_server.py and build/jarvis_grpc_server already running — see
guides/testing-guide.md, or just run start_jarvis.sh instead of this directly.)
"""

from pathlib import Path
import sys

import grpc

ROOT = Path(__file__).resolve().parents[1]
GENERATED_PY = ROOT / "generated" / "python"
if str(GENERATED_PY) not in sys.path:
    sys.path.insert(0, str(GENERATED_PY))

import jarvis_pb2
import jarvis_pb2_grpc

KNOWN_COMMANDS = {
    "echo": jarvis_pb2.COMMAND_TYPE_ECHO,
    "status": jarvis_pb2.COMMAND_TYPE_STATUS,
    "about": jarvis_pb2.COMMAND_TYPE_ABOUT,
    "help": jarvis_pb2.COMMAND_TYPE_HELP,
    "system-info": jarvis_pb2.COMMAND_TYPE_SYSTEM_INFO,
}


def parse_line(line: str):
    """Mirror the CLI's own first-word lookup, client-side. Unlike the CLI's C++ parser,
    an unrecognised first word keeps the FULL line as payload — natural-language input needs
    every word to reach the classifier, not just the words after the first unrecognised one."""
    stripped = line.strip()
    first_word = stripped.split(" ", 1)[0].lower() if stripped else ""

    if first_word in KNOWN_COMMANDS:
        payload = stripped[len(first_word):].strip()
        return KNOWN_COMMANDS[first_word], payload

    return jarvis_pb2.COMMAND_TYPE_UNKNOWN, stripped


def main():
    channel = grpc.insecure_channel("localhost:50051")
    stub = jarvis_pb2_grpc.JarvisServiceStub(channel)

    print("JARVIS interactive client — connected to localhost:50051")
    print("Type a known command (echo/status/about/help/system-info/exit) or any natural-language prompt.")
    print("Ctrl+C or 'exit' to quit.\n")

    while True:
        try:
            line = input("> ")
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line.strip():
            continue

        if line.strip().lower() == "exit":
            print("Goodbye.")
            break

        command, payload = parse_line(line)
        request = jarvis_pb2.ExecuteCommandRequest(command=command, payload=payload)

        try:
            response = stub.ProcessCommand(request, timeout=10)
        except grpc.RpcError as exc:
            print(f"  [gRPC error: {exc.code()} — {exc.details()}]\n")
            continue

        print(f"  command_type: {jarvis_pb2.CommandType.Name(response.command_type)}")
        print(f"  {response.message}\n")


if __name__ == "__main__":
    main()
