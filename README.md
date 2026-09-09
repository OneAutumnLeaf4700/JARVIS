# JARVIS

A locally-running, hybrid C++/Python personal assistant — built as a learning project for
systems-level C++, modern Python, and inter-process service architecture using gRPC and
Protobuf. It's intentionally **not** a thin wrapper around an LLM API: every layer is built up
from primitives (parsers, dispatch tables, service boundaries, versioned RPC contracts) so each
piece teaches something concrete.

**Status:** Phases 1–3 are complete. JARVIS has a C++ capability registry, a tiered Python
understanding layer (rules with local Ollama fallback), optional local voice input/output, and
a dynamic plugin SDK/loader (Phase 4, in progress).

**Platform:** Linux only right now — see [Platform support](docs/project-guide.md#platform-support) for why and what a port would take.

---

## What it can do today

- A stateful CLI (`echo`, `help`, `about`, `status`, `system-info`, `volume`, `shutdown`, `exit`) served by a registry-based capability dispatcher — adding a capability never touches the dispatcher
- The same dispatcher served over gRPC (`:50051`), so CLI and network clients share identical logic
- A Python AI layer (`:50052`) that classifies natural language with fast rules first, escalating to a local Ollama model only on a miss — with a 3s LLM deadline and a 5s end-to-end deadline, so a slow/dead model never hangs the assistant
- Power-tiered consent: read-only capabilities run freely, system-affecting ones need an explicit grant, and destructive ones (`shutdown`) require confirmation on every single call
- A dynamic plugin loader — capabilities can ship as separately-compiled `.so` files with no core code changes
- Optional fully-local voice input/output (wake word, speech-to-text, text-to-speech)
- C++ and Python test suites, a gRPC contract smoke test, and an understanding accuracy/latency evaluation harness

## Quick start (CLI only, no Python needed)

```bash
cmake -S . -B build
cmake --build build
./build/jarvis
```

Try `help`, `echo hi`, `status`, `system-info`, `about`, `exit`.

## Run the full hybrid stack

```bash
.venv/bin/python ai/jarvis_ai_server.py &   # Python AI layer, :50052
./build/jarvis_grpc_server &                # C++ core service, :50051
.venv/bin/python tools/grpc_smoke_test.py   # one-shot end-to-end check
```

Or just `./start_jarvis.sh` to launch both services and an interactive text client together
(add `--voice` for speech input/output).

---

## For everything else

This README is deliberately short. **For architecture, the full build/run reference, the
plugin SDK, voice setup, and the reasoning behind each design choice, see the
[Full Project Guide](docs/project-guide.md).**

Other docs:

- **[docs/architecture-blueprint.md](docs/architecture-blueprint.md)** — the binding architecture: pipeline stages and the invariants every change must obey
- **[docs/roadmap.md](docs/roadmap.md)** — implementation record for completed phases
- **[docs/features.md](docs/features.md)** — full checklist, Phase 1 through stretch goals
- **[docs/vision.md](docs/vision.md)** — unsorted ideas not yet promoted into a roadmap
- **[CLAUDE.md](CLAUDE.md)** — enforceable project rules distilled from the blueprint
