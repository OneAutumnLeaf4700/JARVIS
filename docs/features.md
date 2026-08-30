# JARVIS Feature Roadmap & Checklist

The single source of truth for "what's built, what's next, what's later." Replaces the old split between a phase checklist and a separate long-horizon tier document — this file now covers both, from shipped features down to stretch goals, in one place.

**Status legend:** ✅ Done &nbsp;·&nbsp; 🚧 In progress &nbsp;·&nbsp; 📋 To do

For raw unstructured ideas that haven't been sorted into a phase yet, see [`vision.md`](vision.md). For the step-by-step execution plan behind the current phase, see [`roadmap.md`](roadmap.md).

---

## Phase 1 — CLI Assistant & Service Foundation ✅ Complete

Core engine, gRPC service boundary, and C++↔Python round-trip are all working end-to-end.

- ✅ CLI entry point and stateful command loop (`echo`, `help`, `help <command>`, `about`, `status`, `exit`)
- ✅ Engine logic decoupled from stdout (`runCMD()` returns strings, not prints)
- ✅ Command router / dispatch map
- ✅ First protobuf contract (`ExecuteCommand`, `GetStatus`)
- ✅ C++ gRPC server (`:50051`) exposing the engine via `JarvisService.ProcessCommand`
- ✅ Python gRPC AI server (`:50052`) exposing `JarvisAIService.ProcessNaturalLanguage`
- ✅ C++ → Python gRPC client (`JarvisAIClient`) forwarding `UNKNOWN` commands, with a 5s deadline so a dead AI process never blocks the core server
- ✅ Structured logging via spdlog (C++ side)
- ✅ Request hardening (validation, timeout handling on the AI round-trip)
- ✅ Python smoke test covering known-command and `UNKNOWN → AI` paths
- 📋 Plugin manager for loading/unloading plugins — deferred to Phase 3, see below
- 📋 Configuration system (paths, models, settings) — not yet started

---

## Phase 2 — Intelligence Layer ✅ Complete

The Python AI server started as a placeholder echo (`[AI echo] <text>`) — this phase replaced that with real rule-based classification, and taught the C++ side to act on it.

Full step-by-step plan: [`roadmap.md`](roadmap.md#phase-2--intelligence-layer).

- ✅ Rule-based intent classifier (`ai/intent_classifier.py`) — keyword/token-set matching over `STATUS` / `ECHO` / `ABOUT` / `UNKNOWN`, with a 24-case pytest suite. Live in the AI server (see next two items).
- ✅ Extend `ai.proto` with `intent` + `confidence` fields on `NaturalLanguageResponse`
- ✅ Wire classifier into `jarvis_ai_server.py` — natural-language input now returns a classified `(intent, confidence)` instead of an echo; verified end-to-end over the full C++→Python gRPC path
- ✅ C++ side re-dispatches on classified intent instead of just forwarding AI reply text — `JarvisAIClient::ProcessNaturalLanguage` now returns an `AIResult{reply, intent, confidence}`; `jarvis_service.cpp` re-runs `runCMD()` under the classified `CommandType` when confidence ≥ 0.5
- ✅ Smoke test coverage for classified vs. unclassified inputs — `tools/grpc_smoke_test.py` covers STATUS/ECHO/ABOUT phrasings plus a genuine UNKNOWN fallback
- ✅ Structured logging on the Python side (replace `print()`) — `jarvis_ai_server.py` uses `logging` with text/intent/confidence/latency per request

---

## Phase 2.5 — LLM-Backed Understanding 🚧 In progress

Grows the Understanding tier beyond the rule classifier without changing its stable interface (INV-8).

- ✅ **Local LLM integration (Ollama)** — `ai/llm_backend.py` (`llm_classify()`, calls a local `llama3.2:latest` via Ollama, 3.0s timeout, degrades to `("UNKNOWN", 0.0)` on any failure) plus `ai/resolver.py` (`resolve()`, tries the rule classifier first, escalates to the LLM only on a miss, single stable entry point). Wired live into `ai/jarvis_ai_server.py`, which now logs which tier (`rule`/`llm`/`none`) answered each request. `tools/eval_understanding.py` measures rule-only/LLM-only/hybrid accuracy and latency (INV-13). On this dev machine, real local Ollama inference (~8–22s, CPU-bound) exceeds the 3.0s timeout, so the LLM tier degrades gracefully to `UNKNOWN` in practice here rather than demonstrating an accuracy uplift — verified end-to-end via `tools/grpc_smoke_test.py`'s live LLM-escalation case; the mechanism (graceful degradation, no hang/crash) is proven even though this machine's hardware doesn't currently showcase a successful LLM resolve.
- ✅ **Rule classifier precision fix (found via live testing)** — `ai/intent_classifier.py` now checks the first word of the sentence first: an exact command keyword (or a likely typo of one, corrected via `difflib`) routes instantly; anything else falls back to phrase-pattern matching, where a bare single-word pattern (other than `ABOUT`'s) is now suppressed when a "meta-question" word (`does`/`command`/`explain`/`mean`/`meaning`) is present — so "what does the echo command do?" correctly falls through instead of triggering `ECHO`. Also tightened `ai/llm_backend.py`'s prompt so the LLM tier makes the same distinction (it was independently making the identical mistake once escalated to). 13 new tests, 62/62 passing, verified live end-to-end.
- 📋 **Small-model fast-path escalation (future idea, not scoped)** — a very small/fast model between the rule layer and the full LLM for near-instant recognition, escalating to the bigger model only for complex requests. Rules keep handling the easiest prompts (power on/shutdown/sleep). See [`vision.md`](vision.md#understanding-tier-future-idea-tiered-model-escalation).
- 📋 **Multi-turn context — deliberately deferred (not a Phase 2.5 blocker).** LLM integration (its stated dependency) is done, but today's three intents (`STATUS`/`ECHO`/`ABOUT`) are stateless one-shot commands with no entities or slots — there's nothing a follow-up like "are you sure?" or "open that" could meaningfully refer back to yet. Revisit once Phase 4 lands capabilities with real state to reference (e.g. "open that file" needing to know what "that" is) — building against a real need instead of a hypothetical one.
- 📋 **Safety guardrails** — confirmation prompts before destructive intents (delete file, shut down, etc.), allowlists of safe operations. Real teeth on this depend on Phase 3 having a plugin that can actually do something destructive.

---

## Phase 3 — Voice I/O 📋 To do

- 📋 Microphone capture + wake word ("Jarvis") — needs a wake-word library (Porcupine / openWakeWord)
- 📋 Speech-to-text (Whisper / Vosk) — transcript feeds into the existing `UNKNOWN`-command pipeline unchanged
- 📋 Text-to-speech (Piper / Coqui) — orthogonal to STT, no dependency
- 📋 Push-to-talk vs. always-listen mode switching (config-driven)
- 📋 Graceful low-confidence handling — ask for clarification instead of guessing

---

## Phase 4 — Plugins, Desktop Control & Integrations 📋 To do

The plugin manager is the prerequisite for everything else in this phase — it's the "intent → handler" registration layer the intent classifier (Phase 2) plugs into.

- 📋 Plugin manager — discover/load/dispatch to plugins at runtime, registered by intent rather than raw command string
- 📋 System control plugin — open apps, volume, lock screen, shutdown (needs plugin manager + safety guardrails)
- 📋 **Desktop interaction** — open a browser and execute a given task in it; take a screenshot on command; explain what's happening on screen (likely vision-model-backed). This is a new plugin category beyond simple OS commands — effectively makes JARVIS an OS-level agent.
- 📋 File search plugin — by name, later by content
- 📋 Reminders / notes / TODO plugin — needs plugin manager + persistence (Phase 5)
- 📋 Media control plugin — play/pause/next/volume, eventually a real service (Spotify)
- 📋 Calendar / scheduling — CalDAV or provider API, needs a credential store

---

## Phase 5 — Persistent Memory 📋 To do

- 📋 Local SQLite store — schema for users, sessions, captured items
- 📋 Embedding generation for stored notes/history
- 📋 Vector store + semantic recall (Chroma / FAISS)
- 📋 Privacy controls — inspect/export/delete stored data, configurable retention

---

## Phase 6 — UI & Dashboard 📋 To do

- 📋 Local dashboard (web or desktop) — interaction history, plugin status, system stats. No new backend work needed — just another client of the existing gRPC contract.
- 📋 Settings panel — models, audio devices, intent thresholds
- 📋 System monitoring — CPU, memory, active tasks

---

## Smart environment (longer horizon) 📋 To do

- 📋 Smart home integration — temperature, lighting, etc. Likely via a Home Assistant bridge rather than reinventing device protocols.
- 📋 Broader ambient device control as more services get bound into the intent → service mapping system

---

## Stretch / experimental

These are explicitly *might never happen* — they mark where curiosity could pull the project later, not committed work.

- 📋 Multi-device support — mobile companion talking to the C++ service over the network (forces real auth + TLS)
- 📋 Custom wake-word training on your own voice
- 📋 Advanced routines — time/event-based action chains ("every weekday at 8am, summarise my calendar")
- 📋 JARVIS drafting its own feature plans when it hits an unsupported command — see [`vision.md`](vision.md) for the raw idea; needs a scoping decision before it's more than a note

---

## How to use this document

When a phase completes and you're deciding what's next:

1. Look at what's freshly unblocked — anything whose stated dependency just turned ✅.
2. Pick whichever item teaches you the most about the concept you want to learn next — not necessarily the most "useful" feature.
3. Flesh it out into a step-by-step plan in [`roadmap.md`](roadmap.md) before writing code.
