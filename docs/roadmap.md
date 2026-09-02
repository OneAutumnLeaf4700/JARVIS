# JARVIS Roadmap

Step-by-step execution detail for the current and next phase. For the flat checklist of everything (done, active, and future), see [`features.md`](features.md). For unsorted raw ideas, see [`vision.md`](vision.md).

**Status legend:** ✅ Done &nbsp;·&nbsp; 🚧 In progress &nbsp;·&nbsp; 📋 To do

---

## Phase 1 — Service Foundation ✅ Complete

Moved JARVIS from a CLI-only prototype to a service-oriented hybrid foundation: the engine is transport-agnostic, a versioned protobuf contract sits between C++ and Python, and both sides talk over gRPC with structured logging and request hardening in place.

What shipped, in build order:

1. **Baseline safety checks** — recorded known-good CLI behaviour before refactoring, to catch regressions.
2. **Engine-facing data model (C++)** — typed command request/response objects, replacing terminal-oriented assumptions.
3. **Transport-agnostic engine** — `executeCommand()` / `getStatus()` style API; CLI became a thin adapter over it instead of owning logic.
4. **First protobuf contract** — `ExecuteCommand` and `GetStatus` RPCs, explicit field types, v1 namespace for compatibility.
5. **C++ gRPC server adapter** — exposes the engine over gRPC without touching engine internals.
6. **Python client/worker** — stubs generated from the same proto; parity-tested against CLI output.
7. **Observability baseline** — structured logs (request id, command, latency, result) via spdlog.
8. **Hardening guardrails** — request validation, per-request timeouts, clear error codes.

Existing command-parsing, dispatch-map, and engine-state-tracking concepts all survived the refactor unchanged — only the I/O boundary moved.

---

## Phase 2 — Intelligence Layer ✅ Complete

Phase 1 built the pipes. Phase 2 fills the Python AI layer with actual intelligence — starting from a deterministic rule-based classifier and growing toward LLM-backed reasoning. Each step below has a concrete task list; do them in order, each one builds on the last.

**Objective:** replace the AI server's placeholder echo (`[AI echo] <text>`) with a real classifier, and teach the C++ side to *act* on structured AI responses instead of just forwarding strings back to the user.

### Step 1 — Understand intent vs. surface form
Pick three phrasings that mean the same thing but look different (e.g. "what's your status" / "how long have you been running" / "are you alive") and confirm each should map to the same existing command (`STATUS`, `ECHO`, `ABOUT`). The point: no amount of keyword tuning at the C++ parser level fixes this — the abstraction belongs one layer up.

### Step 2 — Design the classifier data model
Create `ai/intent_classifier.py`. Define intent → keyword-pattern mapping as data (a dict of `set[str]` patterns), not code:
```python
INTENT_PATTERNS: dict[str, list[set[str]]] = {
    "STATUS": [{"status"}, {"uptime"}, {"how", "long", "running"}, {"alive"}],
    "ECHO":   [{"echo"}, {"repeat", "after"}, {"say"}],
    "ABOUT":  [{"about"}, {"who", "are", "you"}, {"what", "jarvis"}],
}
```
Return shape: `(intent: str, confidence: float)`, with `("UNKNOWN", 0.0)` for no match.

### Step 3 — Preprocessing + token matching
- `preprocess(text) -> set[str]`: lowercase, strip punctuation, split on whitespace.
- `classify(text) -> (intent, confidence)`: score each intent as `len(pattern & tokens) / len(pattern)`, return the top scorer above a threshold (e.g. 0.5), else `UNKNOWN`.
- Add a `__main__` block with a few hard-coded inputs as a manual smoke check.

### Step 4 — Extend `ai.proto` to carry intent
Add to `NaturalLanguageResponse`:
```proto
string intent     = 4;
float  confidence = 5;
```
Field numbers are immutable once shipped — new fields get unused numbers, renaming is fine, renumbering is not. Regenerate C++/Python stubs after.

### Step 5 — Wire the classifier into the AI server
In `ai/jarvis_ai_server.py`, call `classify(request.text)` inside `ProcessNaturalLanguage` and populate the new `intent`/`confidence` fields. Keep `reply` human-readable (e.g. `"[detected intent: STATUS, confidence 0.83]"`).

### Step 6 — Act on the intent in C++
- `core/ai_client.h/.cpp`: `ProcessNaturalLanguage` returns an `AIResult{reply, intent, confidence}` struct instead of a bare string.
- `core/jarvis_service.cpp`: in the `UNKNOWN` branch, call the AI client; if `confidence ≥ threshold` and the intent maps to a known `CommandType`, build a new `ParsedCommand` and re-run `runCMD()`. Otherwise fall through to the AI reply as before.
- Add an intent-string → `CommandType` map.

### Step 7 — Extend the smoke test
Add cases to `tools/grpc_smoke_test.py` covering classified inputs (STATUS/ECHO/ABOUT phrasings) and a genuine fallback ("what is the meaning of life"). Print detected intent + confidence so output is self-documenting.

### Step 8 — Observability for the AI layer
Replace `print()` in `jarvis_ai_server.py` with `logging` (or `structlog`). Log timestamp, request text, intent, confidence, latency. Optionally add a request-id field to `NaturalLanguageRequest` to correlate C++ and Python logs for the same call.

**Phase 2 lands when:** the AI server classifies into the four current intents, C++ re-dispatches on classified intent, the smoke test covers both paths, and Python logs are structured. **✅ Landed** — `core/ai_client.{h,cpp}` returns a structured `AIResult`, `core/jarvis_service.cpp` re-dispatches through `runCMD()` on confidence ≥ 0.5, `tools/grpc_smoke_test.py` covers classified (STATUS/ECHO/ABOUT) and unclassified (UNKNOWN) inputs, and `ai/jarvis_ai_server.py` logs via `logging` instead of `print()`. Verified end-to-end: all three commands round-trip through the classifier and re-dispatch to the correct `command_type`.

### What Phase 2 unlocks next
- **Phase 2.5 — LLM integration:** swap the rule classifier for an Ollama/llama.cpp call behind the same `classify()` signature, so nothing on the C++ side changes. Rules stay the fast/cheap path; the LLM is the fallback.
- **Phase 3 — Voice I/O and Phase 4 — Plugins:** the intent layer becomes the natural place to register new intents against new plugins.

## Phase 3 — Voice I/O ✅ Complete

Adds a voice-driven front end without touching the existing pipeline: a new `voice/` package
plugs in *below* the gRPC seam, exactly where `tools/interactive_client.py` plugs in today.

- **`voice/audio_capture.py`** — yields int16 mono frames from either a live microphone or a
  WAV file (`frames()` / `frames_from_wav()`), so tests can exercise real fixture clips without
  hardware.
- **`voice/wake_word.py`** — `WakeWordDetector` wraps openWakeWord (local ONNX model, no
  account/API key) with a `detect(frame) -> bool` call per frame.
- **`voice/stt.py`** — `SpeechToText` wraps faster-whisper, turning a buffered utterance into a
  `TranscriptResult(text, confidence)`.
- **`voice/config.py`** — loads `voice_config.yaml` (mode, wake-word sensitivity, STT model
  size/confidence threshold, audio device) with a documented example file.
- **`voice/voice_client.py`** — the orchestrator. `capture_utterance()` buffers frames until
  trailing silence or a cap is hit; `build_request()` mirrors
  `tools/interactive_client.py`'s known-command-or-full-text routing; `dispatch_transcript()`
  applies the two-signal low-confidence handling (§6 of the design spec) and calls
  `JarvisService.ProcessCommand` over the same unmodified gRPC boundary every other client
  uses. Push-to-talk (`_run_push_to_talk`) and always-listen (`_run_always_listen`, wake-word
  gated) are both thin loops around the same capture → STT → dispatch path.

Because this is entirely a new thin surface reading the mic and calling the existing
`ProcessCommand` RPC, no `core/`, `ai/`, or `proto/` changes were needed (INV-3) — Understanding
and Orchestrate stay exactly as Phase 2/2.5 left them. `start_jarvis.sh --voice` launches it in
place of the text client.

Voice output (text-to-speech) landed as a same-phase follow-on, Render-side only and equally
additive:

- **`voice/tts.py`** — `TextToSpeech` wraps Piper (local ONNX-based synthesis, no API key);
  `resolve_voice_model_path()` maps a config voice name (`tts.voice`) to its local model file
  under `voice/tts_models/`, fail-fast with a clear error (naming the missing voice, the README
  setup section, and the `tts.enabled: false` escape hatch) if it hasn't been downloaded yet.
- **`voice/audio_playback.py`** — blocking playback via `sounddevice`, reused pattern from the
  input side's audio handling.
- **`voice/config.py`** gained `tts.enabled` / `tts.voice`, and `voice/voice_client.py`'s
  `dispatch_transcript()` speaks the response alongside — never instead of — the existing
  terminal output, for both a normal dispatched reply and a low-confidence clarification
  message. `tts.enabled: false` (the default) skips the one-time Piper model download entirely
  for text-only use.

**Phase 3 lands when:** microphone capture, wake-word detection, and speech-to-text all work
end-to-end through the existing `ProcessCommand` pipeline, both push-to-talk and always-listen
modes are usable, low-confidence input is handled gracefully (clarify, don't guess), voice
output can speak both normal and clarification responses without replacing the terminal output,
and the `voice/` test suite passes. **✅ Landed** — both sub-projects (Voice Input/"Ears" and
Voice Output/"Voice") are done; Phase 3 is complete.

## Phase 4 — Plugins, Desktop Control & Integrations 🚧 In progress

The plugin manager substrate (enable/disable + consent gate) landed first, ahead of any actual
plugin, per the project rule that architectural dependency (registry/gate) comes before
capability sprawl:

1. **`PluginConfig`** (`core/plugin_config.h/.cpp`) — loads `config/capabilities.cfg` (per-name
   `enabled=true/false`) and `config/consent_grants.cfg` (per-name recorded grants); `grant(name)`
   persists a new grant back to disk.
2. **`ConsentGate`** (`core/consent_gate.h/.cpp`) — a small class binding a `const PluginConfig&` via its constructor (`explicit ConsentGate(const PluginConfig&)`), exposing `ConsentResult check(const Capability&) const`:
   T0/T1 always allowed, T2 requires a grant, T3/T4 always denied (enforcement for those tiers is
   future work, not yet implemented).
3. **`CapabilityRegistry::setPluginConfig()`** — wires both checks into `dispatch()` ahead of
   invoking any capability, keeping the registry (not the capability) responsible for the gate
   (INV-9).
4. **`jarvis --grant <capability_name>`** (`core/main.cpp`) — the one interactive consent prompt,
   CLI-only since it's the only surface with a real terminal; unknown/T0-T1/T3-T4 capabilities
   each get a clear message and exit without prompting, T2 capabilities prompt `[y/n]` and persist
   via `PluginConfig::grant()`. Both `core/main.cpp`'s normal startup and
   `core/grpc_server_main.cpp` now load `PluginConfig` and pass it into the registry.

Next, the plugin SDK & dynamic loader landed, making the substrate above actually load
capabilities from independently-compiled `.so` files instead of only compiled-in ones:

1. **String-intent dispatch bridge** — `CapabilityRegistry` gained a `std::string`-keyed
   dispatch path (`namedCapabilities_`, `resolve`/`dispatch(const std::string&)`,
   `allByIntent()`) alongside its original `CommandType`-keyed one, since a dynamically-loaded
   plugin has no compile-time enum value. CLI (`core/engine.cpp`) and gRPC
   (`core/jarvis_service.cpp`, via `normalizeClassifierIntent()`) both fall back to string-intent
   dispatch when a `CommandType` lookup misses.
2. **`plugin_sdk/jarvis_plugin_abi.h`** — a pure-C, versioned ABI (`JARVIS_PLUGIN_ABI_VERSION`)
   every plugin implements: `jarvis_plugin_abi_version()` + `jarvis_plugin_register()`, with a
   `malloc`/`free` ownership convention for capability return strings across the `.so` boundary
   (the one allocator convention safe across an independently-compiled shared library).
3. **`core/minimal_json.h/.cpp`** — a hand-rolled JSON parser scoped to exactly the plugin
   manifest schema (INV-10: no mature library punched through the spine for something this
   narrow).
4. **`core/plugin_loader.h/.cpp`** (`PluginLoader`) — discovers one subdirectory per plugin
   under each directory listed in `config/plugin_dirs.cfg`, validates `manifest.json` in full
   (required fields, ABI version, valid power tiers, no duplicate intents — including within the
   same manifest) *before* ever calling `dlopen`, then stages the plugin's registrations and
   cross-checks them against the manifest as a true 1:1 match (not just matching counts) before
   committing anything into the registry. Supports safe, invocation-counted `disablePlugin`/
   `unloadPlugin`. Must be declared before `CapabilityRegistry` in `main.cpp`/
   `grpc_server_main.cpp` so it outlives it.
5. **`system-info` becomes the bundled reference dynamic plugin** (`plugins/system-info/`) —
   the same T0, read-only local system information capability as before (compile-time OS,
   architecture, compiler, C++ standard, hardware threads), now loaded from a real `.so` at
   startup instead of compiled in, proving the whole plugin path end-to-end.

`CapabilityRegistry`'s maps have no internal synchronization — `disablePlugin`/`unloadPlugin`
must only run before the server begins serving concurrent requests, or from a maintenance path
that first stops dispatch. This is fine today (nothing calls disable/unload from a live request
path yet) but is a hard constraint for any future live-reload capability.

Next, `ConsentGate` grew real T3/T4 enforcement, and `system-control` landed as the second
bundled dynamic plugin to prove it on a real capability:

1. **Per-call confirm-token enforcement** (`core/consent_gate.h/.cpp`) — `ConsentGate::check()`
   gained a `payload` parameter; T3/T4 capabilities are now allowed only when the payload's last
   whitespace-delimited token is exactly `confirm` (anchored to the end, not a substring match
   anywhere — a background security review during this work caught that scanning anywhere would
   let free text like "shutdown don't confirm this yet" accidentally satisfy the gate). Per
   INV-9, this is never satisfiable by a persisted grant. Both `CapabilityRegistry::dispatch()`
   overloads thread the payload through, and `jarvis --grant`'s T3/T4 message now explains the
   confirm-token convention instead of the old "not yet implemented" text.
2. **`system-control` plugin** (`plugins/system-control/`) — the second bundled dynamic plugin
   alongside `system-info`. `volume` (T2) gets/sets output level via `pactl`, gated by the
   existing grant flow; a follow-up fix added `runCommandCapturingOutput` so `volume get` returns
   `pactl`'s actual captured output rather than relying on inherited stdout (an INV-1 fix).
   `shutdown` (T3) powers off via `systemctl poweroff`, gated by the new confirm-token
   enforcement rather than a grant — the plugin itself never re-checks for "confirm"; it trusts
   `ConsentGate` to have already enforced it before `shutdownExecute` is reached. Both commands
   build their argv explicitly and invoke it via `posix_spawnp`, never a shell string.
   Argument-parsing/argv-building logic was factored into `plugin_internal.h/.cpp` so it's
   unit-testable without a live audio backend or without actually powering the machine off —
   `system-control` is the first plugin to need that split, since `system-info` had no branching
   logic to isolate. "Open apps" and "lock screen" — on the same original roadmap line — were
   explicitly scoped out of this plan: arbitrary process launch needs its own allowlist/security
   design, deferred to a future plan rather than folded in here.

The remaining Phase 4 items (open apps / lock screen — system-control follow-up, desktop
interaction, file search, reminders, media control, calendar) are still 📋 To do — see
[`features.md`](features.md).

See [`features.md`](features.md) for the full phase-by-phase checklist beyond this point.
