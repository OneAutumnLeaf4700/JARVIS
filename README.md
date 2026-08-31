# JARVIS

A locally-running, hybrid C++/Python personal assistant — built primarily as a learning project for systems-level C++, modern Python, and inter-process service architecture using gRPC and Protobuf.

JARVIS is intentionally not a thin wrapper around an LLM API. Every layer is built up from primitives so each addition teaches something concrete: parsers, dispatch maps, service boundaries, RPC contracts, schema versioning, structured logging, and so on.

**Status:** Phases 1–3 are complete. JARVIS has a C++ capability registry, a tiered Python
understanding layer (rules with local Ollama fallback), and optional local voice input/output.
Phase 4 — runtime plugins, desktop control, and integrations — is next.

---

## What works today

- C++ core engine with a stateful CLI loop (`echo`, `help`, `help <command>`, `about`, `status`, `system-info`, `exit`)
- Registry-based capability dispatch for `echo`, `help`, `about`, `status`, and `system-info`; each capability declares a power tier (all current built-ins are read-only T0)
- Transport-agnostic capability execution — capabilities return strings, so the same dispatch serves the CLI and gRPC service
- C++ gRPC server on `:50051` exposing the engine via `JarvisService.ProcessCommand`
- Python gRPC AI server on `:50052` exposing `JarvisAIService.ProcessNaturalLanguage`, with rule-based intent classification and local Ollama escalation on a rule miss
- C++ gRPC client (`JarvisAIClient`) that forwards `UNKNOWN` commands to the Python AI server with a 5-second deadline; confident classified intents are re-dispatched through the registry
- Graceful local LLM degradation: Ollama calls time out after 3 seconds and resolve to `UNKNOWN` rather than blocking
- Structured logging on both C++ and Python service sides
- Optional local voice client: openWakeWord, faster-whisper STT, and Piper TTS through the existing gRPC API
- C++ GoogleTest suite, Python/voice pytest suites, a gRPC smoke test, and an understanding accuracy/latency evaluation harness

---

## Architecture

JARVIS is split into three runtime tiers communicating through versioned protobuf contracts:

```
            ┌──────────────────────────────────┐
            │  Clients (CLI / text / voice /   │
            │      smoke test / future UI)     │
            └────────────────┬─────────────────┘
                             │ gRPC :50051
                             ▼
            ┌──────────────────────────────────┐
            │  C++ Core Service                │
            │   • Engine (state, uptime)       │
            │   • Parser + capability registry │
            │   • gRPC service adapter         │
            └────────────────┬─────────────────┘
                             │ gRPC :50052 (unresolved input)
                             ▼
            ┌──────────────────────────────────┐
            │  Python AI Layer                 │
            │   • Rule-based intent classifier │
            │   • Local Ollama fallback tier   │
            │   • Structured intent/confidence │
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
      → parseCommand()                    // → ParsedCommand{type, payload}
        → CapabilityRegistry::dispatch()  // → std::string
          → printed to stdout
```

`status` is a registered read-only capability that reads engine state. `exit` remains engine
lifecycle control and calls `Engine::terminate()` directly.

### Path 2 — gRPC (networked)

```
gRPC client (e.g. Python smoke test)
  → ExecuteCommandRequest → C++ JarvisServiceImpl::ProcessCommand
    → validates (rejects COMMAND_TYPE_UNSPECIFIED with ERROR_CODE_INVALID_COMMAND)
    → protoCommandToInternal()             // proto enum → internal CommandType
    → CapabilityRegistry::dispatch()       // same dispatch logic as CLI
       ├── known command → returned as ExecuteCommandResponse.message
       └── UNKNOWN       → JarvisAIClient::ProcessNaturalLanguage()
                            → NaturalLanguageRequest → Python :50052
                              → JarvisAIServicer → rules first → local Ollama on a miss
                                ├── confident known intent → re-dispatch through registry
                                └── unresolved/error → descriptive AI reply
    → fills ExecuteCommandResponse{success, message, command_type, error_code}
```

The key invariant: **capabilities do not know which path called them.** That's what
"transport-agnostic core" means in practice — the same registry dispatch serves stdin, gRPC,
and any future transport (websocket, IPC, message bus).

---

## Project layout

```
JARVIS/
├── core/                       # C++ engine and gRPC server
│   ├── main.cpp                # CLI entry point
│   ├── engine.cpp/.h           # Engine state, run loop, status
│   ├── command_handler.cpp/.h  # Deterministic command parsing
│   ├── capability*.{h,cpp}     # Capability model, registry, and built-ins
│   ├── jarvis_service.cpp/.h   # gRPC service adapter (proto ↔ engine)
│   ├── ai_client.cpp/.h        # gRPC client to Python AI layer
│   └── grpc_server_main.cpp    # Server entry point
├── ai/                         # Python understanding tier
│   ├── intent_classifier.py    # Deterministic rule-based classifier
│   ├── resolver.py             # Rules → local LLM fallback
│   ├── llm_backend.py          # Ollama client with a deadline/fallback
│   └── jarvis_ai_server.py     # Python gRPC service adapter
├── proto/
│   ├── jarvis.proto            # Core service contract
│   └── ai.proto                # AI layer contract
├── generated/                  # Auto-generated proto stubs (cpp + python)
├── tools/                      # Interactive, smoke-test, and evaluation clients
├── voice/                      # Optional local voice surface (wake word, STT, TTS)
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

Try `help`, `echo hi`, `status`, `system-info`, `about`, `exit`.

### Plugin manager: enable/disable and T2 consent

`config/capabilities.cfg` and `config/consent_grants.cfg` (both optional — a missing file just
means every capability defaults to enabled / not-granted) hold `<name>.enabled=true` and
`<name>.granted=true` lines respectively. A capability declared power tier T2 (system-affecting)
needs an explicit grant before it will run:

```bash
./build/jarvis --grant <capability_name>
```

This is the one interactive consent surface — it prompts `[y/n]` and persists the grant to
`config/consent_grants.cfg`. T0/T1 capabilities don't need a grant (exits 0 immediately); T3/T4
enforcement isn't implemented yet (exits 1).

### Run the full hybrid stack

In three separate terminals:

```bash
# 1. Python AI server — start first so the C++ server can reach it
.venv/bin/python ai/jarvis_ai_server.py

# 2. C++ gRPC server
./build/jarvis_grpc_server

# 3. Smoke test (one-shot)
.venv/bin/python tools/grpc_smoke_test.py
```

If the Python AI server is down, the C++ server still serves known commands; `UNKNOWN` commands return `[AI unavailable: ...]` after the 5-second deadline rather than hanging.

### Run the full stack interactively

With the project virtual environment and generated protobuf stubs available, the launcher starts
both services and opens the text client:

```bash
./start_jarvis.sh
```

Ollama is optional: rule matches work without it, while a rule miss degrades to `UNKNOWN` after
the local LLM timeout.

### Run tests

```bash
cmake --build build --target jarvis_tests
ctest --test-dir build --output-on-failure
PYTHONPATH=ai:generated/python .venv/bin/python -m pytest ai tools/test_eval_understanding.py voice/tests -q
```

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

Voice *output* (text-to-speech) is controlled by `tts.enabled` in `voice_config.yaml`. When
enabled, download the Piper voice model named by `tts.voice` (for example, `en_US-lessac-medium`)
into `voice/tts_models/`:

```bash
.venv/bin/python -m piper.download_voices --download-dir voice/tts_models en_US-lessac-medium
```

`voice/tts_models/` is gitignored — the model weights are per-machine, not repo content, same
treatment as `voice/voice_config.yaml`. Setting `tts.enabled: false` in `voice_config.yaml`
skips this download entirely — voice input still works, text/print-only.

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
- **[docs/roadmap.md](docs/roadmap.md)** — implementation record for the completed foundation, intelligence, and voice phases
- **[docs/features.md](docs/features.md)** — full checklist, Phase 1 through stretch goals, with status indicators
- **[docs/vision.md](docs/vision.md)** — unsorted brainstorm of future ideas, not yet promoted into a roadmap
