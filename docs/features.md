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
- ✅ Runtime plugin loading/unloading (`.so` discovery without rebuilding) — landed in Phase 4 (see below) as the Plugin SDK & dynamic loader
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
- ✅ **Capability Registry & C++ test suite** — `Capability` struct, `CapabilityRegistry` class (`core/capability.h`, `core/capability_registry.h/.cpp`), and `ExecutionContext` enable self-registration of capabilities against `CommandType` intents; each capability declares a `PowerTier` (the current built-ins are `T0_READ_ONLY`). Both CLI (`core/engine.cpp`) and gRPC (`core/jarvis_service.cpp`) dispatch through the registry instead of hardcoded function maps — implementing INV-6 (register, don't hardcode). GoogleTest integration (`CMakeLists.txt` builds `jarvis_tests` target). The registry was later extended (Phase 4) with a parallel string-intent-keyed map alongside the original `CommandType`-keyed one, making the capability identifier space extensible at runtime — see the Plugin SDK & dynamic loader entry below.

---

## Phase 2.5 — LLM-Backed Understanding 🚧 In progress

Grows the Understanding tier beyond the rule classifier without changing its stable interface (INV-8).

- ✅ **Local LLM integration (Ollama)** — `ai/llm_backend.py` (`llm_classify()`, calls a local `llama3.2:latest` via Ollama, 3.0s timeout, degrades to `("UNKNOWN", 0.0)` on any failure) plus `ai/resolver.py` (`resolve()`, tries the rule classifier first, escalates to the LLM only on a miss, single stable entry point). Wired live into `ai/jarvis_ai_server.py`, which now logs which tier (`rule`/`llm`/`none`) answered each request. `tools/eval_understanding.py` measures rule-only/LLM-only/hybrid accuracy and latency (INV-13). On this dev machine, real local Ollama inference (~8–22s, CPU-bound) exceeds the 3.0s timeout, so the LLM tier degrades gracefully to `UNKNOWN` in practice here rather than demonstrating an accuracy uplift — verified end-to-end via `tools/grpc_smoke_test.py`'s live LLM-escalation case; the mechanism (graceful degradation, no hang/crash) is proven even though this machine's hardware doesn't currently showcase a successful LLM resolve.
- ✅ **Rule classifier precision fix (found via live testing)** — `ai/intent_classifier.py` now checks the first word of the sentence first: an exact command keyword (or a likely typo of one, corrected via `difflib`) routes instantly; anything else falls back to phrase-pattern matching, where a bare single-word pattern (other than `ABOUT`'s) is now suppressed when a "meta-question" word (`does`/`command`/`explain`/`mean`/`meaning`) is present — so "what does the echo command do?" correctly falls through instead of triggering `ECHO`. Also tightened `ai/llm_backend.py`'s prompt so the LLM tier makes the same distinction (it was independently making the identical mistake once escalated to). 13 new tests, 62/62 passing, verified live end-to-end. **Follow-up (found via voice input live testing):** removed ABOUT's `{"what", "jarvis"}` pattern — bag-of-words scoring meant any sentence containing both words fired ABOUT regardless of order, and voice input naturally addresses the assistant by name ("Jarvis, what's the weather?") in a way typed CLI input never did, so this false-fired constantly. A genuine "what is jarvis" question now correctly falls through to the LLM tier instead.
- 📋 **Small-model fast-path escalation (future idea, not scoped)** — a very small/fast model between the rule layer and the full LLM for near-instant recognition, escalating to the bigger model only for complex requests. Rules keep handling the easiest prompts (power on/shutdown/sleep). See [`vision.md`](vision.md#understanding-tier-future-idea-tiered-model-escalation).
- 📋 **Multi-turn context — deliberately deferred (not a Phase 2.5 blocker).** LLM integration (its stated dependency) is done, but today's three intents (`STATUS`/`ECHO`/`ABOUT`) are stateless one-shot commands with no entities or slots — there's nothing a follow-up like "are you sure?" or "open that" could meaningfully refer back to yet. Revisit once Phase 4 lands capabilities with real state to reference (e.g. "open that file" needing to know what "that" is) — building against a real need instead of a hypothetical one.
- 📋 **Safety guardrails** — confirmation prompts before destructive intents (delete file, shut down, etc.), allowlists of safe operations. Real teeth on this depend on Phase 3 having a plugin that can actually do something destructive.

---

## Phase 3 — Voice I/O ✅ Complete

- ✅ **Voice input** — new `voice/` package: `audio_capture.py` (mic + WAV framing), `wake_word.py` (openWakeWord-based `WakeWordDetector`, fully local/ONNX), `stt.py` (faster-whisper wrapper, transcript + confidence), `config.py` (`voice_config.yaml` loader), `voice_client.py` (thin gRPC client orchestrating capture → wake word → STT → dispatch). Push-to-talk and always-listen modes, config-driven. `voice_client.py` dispatches through the existing, unmodified `JarvisService.ProcessCommand` pipeline exactly like `tools/interactive_client.py` — no `core/`/`ai/` changes were needed (INV-3). Graceful low-confidence handling: low STT confidence skips the round-trip and asks the user to repeat; low AI/intent confidence prints a clarification after the round-trip. Wired into `start_jarvis.sh --voice`. Push-to-talk capture is explicitly bracketed (press Enter to start, press Enter again to stop — `capture_utterance_bounded()`, no silence detection); always-listen still uses `capture_utterance()`'s silence-based auto-stop since there's no button to press there. A `--log` flag (`voice_client.py --log`, forwarded through `start_jarvis.sh --voice --log`) echoes every heard transcript and its confidence before the threshold decision, for calibrating `stt.confidence_threshold` / diagnosing mic pickup. `stt.language` (default `en`) pins Whisper's language rather than letting it auto-detect per utterance, which could otherwise mis-detect short/ambiguous clips as a different language entirely.
- ✅ **Voice output (text-to-speech)** — new `voice/tts.py` (`TextToSpeech` wrapping Piper, local ONNX-based synthesis, no API key; `resolve_voice_model_path()` maps a config voice name to its local model file) and `voice/audio_playback.py` (blocking playback via `sounddevice`). Config additions `tts.enabled` / `tts.voice` in `voice_config.yaml`. Wired additively into `voice/voice_client.py`'s `dispatch_transcript()`: when enabled, JARVIS speaks alongside — never instead of — the existing terminal output, for both a dispatched response and a low-confidence clarification message. `tts.enabled: false` (the default) skips the one-time Piper voice model download entirely for text-only use; the download itself is documented in `README.md`'s Voice (optional) section.

---

## Phase 4 — Plugins, Desktop Control & Integrations 🚧 In progress

The plugin manager is the prerequisite for everything else in this phase — it's the "intent → handler" registration layer the intent classifier (Phase 2) plugs into.

- ✅ **Plugin manager substrate (enable/disable + consent gate)** — `core/plugin_config.h/.cpp` (`PluginConfig::load(capabilitiesPath, grantsPath)` reads `config/capabilities.cfg` / `config/consent_grants.cfg`, `grant(name)` persists a new grant), `core/consent_gate.h/.cpp` (`ConsentGate` binds a `const PluginConfig&` via its constructor and exposes `ConsentResult check(const Capability&, const std::string& payload) const` — T0/T1 always allowed, T2 requires a recorded grant), and `CapabilityRegistry::setPluginConfig()` wiring `dispatch()` through both the enable/disable check and the consent gate before invoking a capability (INV-9: the orchestrator/registry enforces consent, not the capability itself). `core/main.cpp` gained a `jarvis --grant <capability_name>` CLI flow — the one interactive consent surface, since the CLI is the one surface with a real terminal attached — and both `core/main.cpp`'s normal startup path and `core/grpc_server_main.cpp` now load `PluginConfig` and pass it into the registry. T3/T4 requires the word "confirm" as a standalone token in the capability's payload on every call — never satisfied by a persisted grant, per INV-9. Landed alongside the system-control plugin's `shutdown` capability, the first T3 capability to exist.
- ✅ **Plugin SDK & dynamic loader** — capabilities can now be shipped as independently-compiled `.so` files instead of being compiled into `jarvis`/`jarvis_grpc_server`. `plugin_sdk/jarvis_plugin_abi.h` is a pure-C, versioned ABI (`JARVIS_PLUGIN_ABI_VERSION`) every plugin implements (`jarvis_plugin_abi_version()` + `jarvis_plugin_register()`, one `malloc`/`free` ownership convention for return strings across the boundary). `core/plugin_loader.h/.cpp` (`PluginLoader`) discovers one subdirectory per plugin under each configured directory (`config/plugin_dirs.cfg`), validates its `manifest.json` (via a hand-rolled `core/minimal_json.h/.cpp` parser, scoped to exactly this schema — INV-10, no library punched through the spine) — required fields, ABI version, valid power tiers, no duplicate/within-manifest-duplicate intents — entirely *before* ever calling `dlopen`, then stages the plugin's registrations and cross-checks them against the manifest as a true 1:1 match before committing anything into the registry. `disablePlugin`/`unloadPlugin` support safe, invocation-counted unloading. `CapabilityRegistry` gained a parallel string-intent-keyed dispatch path (`namedCapabilities_`, `resolve`/`dispatch(const std::string&)`, `allByIntent()`, `unregisterCapability()`) alongside its original `CommandType`-keyed one, and both the CLI (`core/engine.cpp`) and gRPC service (`core/jarvis_service.cpp`, via `normalizeClassifierIntent()`) fall back to string-intent dispatch when a `CommandType` lookup misses — so a plugin never needs a compile-time enum value to be reachable. `system-info` is now the reference dynamic plugin (`plugins/system-info/`), proving the whole path end-to-end; `PluginLoader` is deliberately declared before `CapabilityRegistry` in both `main.cpp`/`grpc_server_main.cpp` so it outlives it (plugin `.so`s must stay mapped for as long as any `Capability::execute` closure referencing them could still exist). Note: `CapabilityRegistry`'s maps have no internal synchronization — `disablePlugin`/`unloadPlugin` must only run before the server begins serving concurrent requests, or from a maintenance path that first stops dispatch (documented in code; enforcement is future work if a live-reload capability is ever added).
- ✅ **System information capability (first Phase 4 plugin slice)** — `system-info` is a T0 read-only capability. It reports compile-time OS, architecture, compiler, C++ standard, and available hardware-thread information without subprocesses or network access. It is available through CLI, gRPC text/voice clients, and the rule/LLM Understanding tiers; `config/capabilities.cfg` can disable it like every other registered capability. Originally shipped compiled-in; now the bundled reference dynamic plugin (see above).
- ✅ **System control plugin (volume + shutdown slice)** — `system-control` is the second bundled
  dynamic plugin (`plugins/system-control/`), alongside `system-info`. `volume` (T2) gets/sets
  output level via `pactl`, gated by the existing grant flow (`jarvis --grant volume`).
  `shutdown` (T3) powers off via `systemctl poweroff`, gated by `ConsentGate`'s per-call
  confirm-token enforcement (see the "Plugin manager substrate" bullet above, in this same
  Phase 4 section) rather than a grant — proving both the T2 and T3 enforcement paths on a real
  capability. Both commands build their
  OS argv and invoke it via `posix_spawnp`, never a shell string, so no capability payload can
  inject shell syntax. `volume get` uses a second helper, `runCommandCapturingOutput`, which
  redirects the child's stdout into a pipe so `pactl`'s output actually flows back through
  `execute()`'s return value instead of landing on JARVIS's own inherited stdout (an INV-1 fix
  applied after the initial volume landing).
- 📋 System control plugin — open apps, lock screen (deferred: arbitrary process launch needs its
  own allowlist design, scoped as a separate follow-up rather than folded into the volume/shutdown
  slice above)
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
