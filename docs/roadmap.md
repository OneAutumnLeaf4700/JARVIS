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

## Phase 2 — Intelligence Layer 🚧 In progress

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

**Phase 2 lands when:** the AI server classifies into the four current intents, C++ re-dispatches on classified intent, the smoke test covers both paths, and Python logs are structured.

### What Phase 2 unlocks next
- **Phase 2.5 — LLM integration:** swap the rule classifier for an Ollama/llama.cpp call behind the same `classify()` signature, so nothing on the C++ side changes. Rules stay the fast/cheap path; the LLM is the fallback.
- **Phase 3 — Voice I/O and Phase 4 — Plugins:** the intent layer becomes the natural place to register new intents against new plugins.

See [`features.md`](features.md) for the full phase-by-phase checklist beyond this point.
