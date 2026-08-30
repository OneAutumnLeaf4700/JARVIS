# JARVIS

A locally-running, hybrid C++/Python personal assistant — built primarily as a learning project for systems-level C++, modern Python, and inter-process service architecture using gRPC and Protobuf.

JARVIS is intentionally not a thin wrapper around an LLM API. Every layer is built up from primitives so each addition teaches something concrete: parsers, dispatch maps, service boundaries, RPC contracts, schema versioning, structured logging, and so on.

**Status:** Phase 2 in progress. The CLI prototype, the C++ gRPC server, and the Python AI server all work end-to-end. The AI layer is currently a placeholder echo, ready to be replaced with rule-based intent classification next.

---

## What works today

- C++ core engine with a stateful CLI loop (`echo`, `help`, `help <command>`, `about`, `status`, `exit`)
- Engine logic decoupled from stdout — `runCMD()` returns strings, so the same logic serves the CLI and any networked client
- C++ gRPC server on `:50051` exposing the engine via `JarvisService.ProcessCommand`
- Python gRPC AI server on `:50052` exposing `JarvisAIService.ProcessNaturalLanguage`
- C++ gRPC client (`JarvisAIClient`) that forwards `UNKNOWN` commands to the Python AI server with a 5-second deadline so a missing AI process never blocks the main server
- Structured logging via spdlog on the C++ side
- Python smoke test (`tools/grpc_smoke_test.py`) that exercises the full known-command and `UNKNOWN → AI` round-trips

---

## Architecture

JARVIS is split into three runtime tiers communicating through versioned protobuf contracts:

```
            ┌──────────────────────────────────┐
            │  Clients (CLI / smoke test /     │
            │   future voice / future UI)      │
            └────────────────┬─────────────────┘
                             │ gRPC :50051
                             ▼
            ┌──────────────────────────────────┐
            │  C++ Core Service                │
            │   • Engine (state, uptime)       │
            │   • Command parser / dispatcher  │
            │   • gRPC service adapter         │
            └────────────────┬─────────────────┘
                             │ gRPC :50052 (UNKNOWN only)
                             ▼
            ┌──────────────────────────────────┐
            │  Python AI Layer                 │
            │   • Natural-language handling    │
            │   • [next]   intent classifier   │
            │   • [later]  LLM integration     │
            └──────────────────────────────────┘
```

### Why this split?

- **C++ owns the hot path** — engine state, command execution, low-latency dispatch, structured logging. Always-on service, designed to never block.
- **Python owns intelligence** — anything fuzzy, fast-evolving, or LLM-driven. Stateless workers behind a typed contract.
- **Protobuf/gRPC is the contract** — language-neutral schemas, explicit versioning, binary-efficient on the wire, and forces every request/response shape to be a deliberate design decision rather than ad-hoc text parsing.

See [`docs/architecture-blueprint.md`](docs/architecture-blueprint.md) (Appendix A) for the full rationale.

---

## Data flow

There are two paths through the system today:

### Path 1 — CLI (in-process)

```
stdin → Engine::run() loop
      → handleCommand(input)
        → parseCommand()         // → ParsedCommand{type, payload}
          → runCMD(parsed)       // → std::string
            → printed to stdout
```

`Engine::run()` also handles two side-effecting commands directly:
- `STATUS` → calls `Engine::printStatus()` (uptime, last command)
- `EXIT`   → calls `Engine::terminate()`

### Path 2 — gRPC (networked)

```
gRPC client (e.g. Python smoke test)
  → ExecuteCommandRequest → C++ JarvisServiceImpl::ProcessCommand
    → validates (rejects COMMAND_TYPE_UNSPECIFIED with ERROR_CODE_INVALID_COMMAND)
    → protoCommandToInternal()             // proto enum → internal CommandType
    → runCMD(parsed)                       // same dispatch logic as CLI
       ├── known command → returned as ExecuteCommandResponse.message
       └── UNKNOWN       → JarvisAIClient::ProcessNaturalLanguage()
                            → NaturalLanguageRequest → Python :50052
                              → JarvisAIServicer
                                → reply text → response bubbles back up
    → fills ExecuteCommandResponse{success, message, command_type, error_code}
```

The key invariant: **`runCMD()` doesn't know which path called it.** That's what "transport-agnostic core" means in practice — the same function serves stdin, gRPC, and any future transport (websocket, IPC, message bus).

---

## Project layout

```
JARVIS/
├── core/                       # C++ engine and gRPC server
│   ├── main.cpp                # CLI entry point
│   ├── engine.cpp/.h           # Engine state, run loop, status
│   ├── command_handler.cpp/.h  # Parsing + dispatch
│   ├── jarvis_service.cpp/.h   # gRPC service adapter (proto ↔ engine)
│   ├── ai_client.cpp/.h        # gRPC client to Python AI layer
│   └── grpc_server_main.cpp    # Server entry point
├── ai/
│   └── jarvis_ai_server.py     # Python gRPC server (placeholder echo)
├── proto/
│   ├── jarvis.proto            # Core service contract
│   └── ai.proto                # AI layer contract
├── generated/                  # Auto-generated proto stubs (cpp + python)
├── tools/
│   └── grpc_smoke_test.py      # End-to-end smoke test
├── docs/
│   ├── architecture-blueprint.md  # Binding architecture: pipeline, invariants, core→scaled
│   ├── roadmap.md                 # Step-by-step execution plan (current + next phase)
│   ├── features.md                # Full phase-by-phase checklist, done → stretch goals
│   └── vision.md                  # Unsorted brainstorm / parking lot for future ideas
├── CLAUDE.md                      # Enforceable project rules (distils the blueprint)
└── CMakeLists.txt
```

---

## Build & run

### Prerequisites
- C++17 toolchain, CMake ≥ 3.20
- Protobuf, gRPC, spdlog (system-installed or via your package manager / vcpkg)
- Python 3.10+ with `grpcio` and `grpcio-tools`

### Build the C++ binaries

```bash
cmake -S . -B build
cmake --build build
```

This produces two binaries:
- `build/jarvis` — interactive CLI
- `build/jarvis_grpc_server` — gRPC server on `:50051`

### Run the CLI alone (no Python required)

```bash
./build/jarvis
```

Try `help`, `echo hi`, `status`, `about`, `exit`.

### Run the full hybrid stack

In three separate terminals:

```bash
# 1. Python AI server — start first so the C++ server can reach it
python3 ai/jarvis_ai_server.py

# 2. C++ gRPC server
./build/jarvis_grpc_server

# 3. Smoke test (one-shot)
python3 tools/grpc_smoke_test.py
```

If the Python AI server is down, the C++ server still serves known commands; `UNKNOWN` commands return `[AI unavailable: ...]` after the 5-second deadline rather than hanging.

### Regenerate proto stubs (only when `proto/*.proto` changes)

```bash
# C++
protoc --proto_path=proto \
       --cpp_out=generated/cpp \
       --grpc_out=generated/cpp \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       proto/jarvis.proto proto/ai.proto

# Python
python3 -m grpc_tools.protoc -Iproto \
       --python_out=generated/python \
       --grpc_python_out=generated/python \
       proto/jarvis.proto proto/ai.proto
```

---

## Voice (optional)

Voice input is an optional add-on client — it talks to the same `JarvisService.ProcessCommand`
gRPC endpoint as `tools/interactive_client.py`, just fed by speech instead of typed text. It
requires its own Python dependencies, separate from core JARVIS's:

```bash
.venv/bin/pip install -r voice/requirements.txt
```

Copy the example config and adjust `mode` and `audio.device` for your machine:

```bash
cp voice/voice_config.example.yaml voice/voice_config.yaml
```

Then launch with:

```bash
./start_jarvis.sh --voice
```

(or run `python3 -m voice.voice_client` directly from the repo root if the two servers are
already running some other way).

On first run, the `openwakeword` ONNX model and the `faster-whisper` model weights are
downloaded — they aren't committed to the repo. This needs network access once; after that,
voice input runs fully offline (INV-11).

---

## Why this project exists

JARVIS is a learning vehicle, not a product. The goals, in order:

1. Get fluent in modern C++ (RAII, `unordered_map` dispatch, `std::function`, gRPC bindings, spdlog).
2. Get fluent in Python service authoring (gRPC server, structured request handling, integration with native code).
3. Understand hybrid system design — when to stay in C++, when to defer to Python, why the contract layer matters.
4. Practice schema-first API design with protobuf and learn versioning discipline.
5. Build something genuinely useful at the end — a working local assistant — so the abstractions stay grounded.

Every step in the roadmaps below has a stated learning outcome, not just a build outcome. If a step doesn't teach a concept, it doesn't earn a place.

---

## Where to go next

- **[docs/architecture-blueprint.md](docs/architecture-blueprint.md)** — the binding architecture: full core→scaled design, the seven-stage pipeline, and the thirteen invariants every change must obey (why-hybrid/why-gRPC rationale is in its Appendix A). Distilled into the enforceable root [`CLAUDE.md`](CLAUDE.md).
- **[docs/roadmap.md](docs/roadmap.md)** — step-by-step plan: Phase 1 (done) + Phase 2 (current focus: rule-based intent classification)
- **[docs/features.md](docs/features.md)** — full checklist, Phase 1 through stretch goals, with status indicators
- **[docs/vision.md](docs/vision.md)** — unsorted brainstorm of future ideas, not yet promoted into a roadmap
