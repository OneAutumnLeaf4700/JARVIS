# Phase 2.5 — LLM Fallback Tier Design

**Status:** Approved by user in brainstorming session, 2026-08-30. Ready for implementation planning.
**Blueprint stage:** S1 — The Understanding Tier (`docs/architecture-blueprint.md` §5).
**Governing invariants:** INV-5 (understanding above execution), INV-7 (graceful degradation),
INV-8 (tiered intelligence, stable interface), INV-11 (local-first/private), INV-12 (growth is
additive), INV-13 (every seam is verifiable).
**Roadmap link:** `docs/roadmap.md` "What Phase 2 unlocks next → Phase 2.5"; `docs/features.md`
Phase 2.5 checklist.

---

## 1. Scope

This design adds exactly **one** new tier to the Understanding pipeline: a local LLM (via Ollama)
consulted only when the existing rule-based classifier fails to recognise the input. It does
**not** build the fuller "route to a bigger model for harder requests" system the project owner
described as a longer-term goal — that idea is deliberately deferred (see §7) and this design's
interface is shaped so it can be added later without rework.

Out of scope for this design: new capabilities/intents beyond the current four (`STATUS`, `ECHO`,
`ABOUT`, `UNKNOWN`), any proto/contract changes, multi-turn conversation context, and safety
guardrails for destructive actions (all separately tracked in `features.md` Phase 2.5/3/4).

## 2. Current state (baseline)

- `ai/intent_classifier.py` exposes `classify(text) -> (intent, confidence)`, a deterministic
  keyword/token-set matcher over the four known intents, threshold 0.5, `("UNKNOWN", 0.0)` on no
  match.
- `ai/jarvis_ai_server.py` calls `classify()` directly inside the gRPC servicer and returns the
  result over `NaturalLanguageResponse{success, reply, intent, confidence}`.
- `core/jarvis_service.cpp` re-dispatches on the returned intent when confidence ≥ 0.5 (landed in
  the previous session — see `qmul/logbook/2026-08-30-classifier-redispatch.md`).
- No LLM is consulted anywhere yet. Ollama is installed and running locally (confirmed reachable
  at `http://localhost:11434`, models already pulled include `llama3.2:latest`).

## 3. Design

### 3.1 New resolver layer

- `ai/intent_classifier.py`'s `classify()` is **untouched** — it stays the deterministic, free,
  first-tried tier (INV-12: don't rewrite a lower tier to add a higher one).
- New module `ai/llm_backend.py` exposes `llm_classify(text) -> (intent, confidence)` — same
  input/output shape as `classify()`, but backed by a call to the local Ollama server.
- New top-level `resolve(text) -> (intent, confidence)` (location: a new `ai/resolver.py`) is the
  single entry point the server calls. Logic: call `classify()`; if it returns `UNKNOWN`, call
  `llm_classify()`; return whichever result is non-`UNKNOWN`, else `("UNKNOWN", 0.0)`.
- `ai/jarvis_ai_server.py` changes its one call from `classify(request.text)` to
  `resolve(request.text)`. No other change to the servicer.

This matches the blueprint's named target interface: `resolve()`/`classify()` fixed at the top,
implementation (rules vs. LLM vs. future tiers) swappable underneath (INV-8).

### 3.2 Talking to Ollama

- HTTP POST to the local Ollama server (`http://localhost:11434/api/generate` or `/api/chat`)
  using Python's built-in `urllib` — no new pip dependency, since this is a single JSON POST and
  avoiding an unneeded library fits the project's build-from-primitives ethos (INV-10 still
  permits mature libraries at leaf capabilities, but there's no library needed here at all).
- Model: `llama3.2:latest` (3.2B, already pulled locally — user-approved choice; smaller variants
  can be benchmarked later if latency becomes a problem).
- Request forces structured output via Ollama's `"format": "json"` option, so the model's reply
  is guaranteed valid JSON rather than free-form prose needing fragile parsing.
- Prompt content: a short system-style instruction listing the three known intents with one-line
  descriptions of what each means, plus the user's raw text, asking for
  `{"intent": "<STATUS|ECHO|ABOUT|UNKNOWN>", "confidence": <0.0-1.0>}`.
- Timeout: the call has its own 3-second deadline (`urllib` socket timeout), comfortably inside
  the existing outer 5-second C++→Python gRPC deadline, so a slow/stuck model still leaves room
  for Python to reply "unknown" before the whole request times out.
- Error handling (all fall through to `("UNKNOWN", 0.0)`, never raise past this function):
  connection refused/timeout (Ollama not running or too slow), non-200 HTTP response, malformed
  JSON, or an `intent` value that isn't one of the three known names.

### 3.3 Confidence semantics (documented limitation)

The rule classifier's confidence is a real computed ratio (matched pattern tokens / pattern
size). An LLM's self-reported confidence is not a calibrated probability — it's the model's own
guess, which can be over- or under-confident in ways that don't track actual correctness. This
design still asks the model to report one (for interface consistency, and because it's still a
useful-if-imperfect signal), but the asymmetry between "measured" and "self-reported" confidence
is called out explicitly here so it's honest input to the evaluation harness (§3.5) rather than a
hidden assumption.

### 3.4 Observability

The Python-side structured log line (added in the previous session) gains one more field: which
tier actually answered — `rule`, `llm`, or `none` (both tiers drew a blank). Example:
`text='...' tier=llm intent=STATUS confidence=0.82 latency_ms=340`. This is log-only; it does not
touch the gRPC contract (INV-2 only binds cross-process wire formats, not internal logging).

### 3.5 Evaluation harness (INV-13)

A small labeled dataset — `(text, expected_intent)` pairs, extending the phrasings already used
in `ai/test_ai_server.py`/`ai/test_intent_classifier.py` plus additional trickier phrasings the
rule classifier is expected to miss but the LLM should catch — run through three configurations:
rule-only, LLM-only (force escalation even when rules would have matched, for a clean
apples-to-apples comparison), and the hybrid `resolve()` path. Reports accuracy and average
latency per configuration. Lives at `tools/eval_understanding.py`, runnable standalone, producing
a plain results table (this becomes real evidence for the project's Evaluation report section, so
deliberately kept simple/readable over clever).

This is a distinct chunk of implementation work from wiring the resolver in, and will be its own
step in the implementation plan rather than bundled into the same change.

### 3.6 Testing

- Unit tests for `llm_classify()` with the Ollama HTTP call mocked (valid JSON reply, malformed
  JSON, timeout, connection error, unrecognised intent name) — no live model call in the regular
  pytest suite, so CI/tests stay fast and don't depend on Ollama being installed.
- Unit tests for `resolve()` covering: rule hits (LLM never called), rule miss + LLM hit, rule
  miss + LLM also miss.
- `tools/grpc_smoke_test.py` keeps its existing classified/unclassified cases; optionally gains
  one case that only an LLM (not the rule classifier) would resolve correctly, run against the
  real live stack, to prove the escalation path works end-to-end (not just against mocks).

## 4. Requirements/setup impact

No runtime `requirements.txt` exists yet (only `ai/requirements-dev.txt` for pytest). Since this
design avoids adding a new pip dependency (§3.2), no new runtime requirements file is needed for
this feature specifically — worth flagging as a small pre-existing gap (there's currently no
recorded list of what `ai/jarvis_ai_server.py` needs to run, e.g. `grpcio`) but fixing that is
outside this design's scope unless the user wants it folded in.

## 5. What doesn't change

- `proto/ai.proto` — no field changes. `NaturalLanguageResponse` already carries `intent` and
  `confidence`; this design doesn't need `args` or any other new field.
- `core/` — nothing. The re-dispatch logic added last session already acts on whatever
  intent/confidence comes back, regardless of which tier produced it.
- Any existing surface (CLI, gRPC client).

## 6. Verification plan

- `pytest ai/` passes, including new mocked-Ollama tests.
- `tools/eval_understanding.py` runs and prints a results table for all three configurations.
- Full 3-process smoke test still passes, plus the new LLM-only-resolvable case.
- Manual check: stop the Ollama server, confirm a request that would have needed the LLM tier
  still returns a graceful "unknown" reply within the 5-second deadline, not an error or a hang.

## 7. Deliberately deferred (not built in this design)

The project owner's longer-term idea: route by estimated complexity through multiple model tiers
(an instant tiny model for the simplest commands, escalating further for harder requests), rather
than today's two-tier rules→one-LLM shape. Not scoped or built here. Because `resolve()` is kept
as the single fixed entry point `jarvis_ai_server.py` calls, a future third tier can be added
inside `resolve()`'s escalation logic later without changing the server, the proto contract, or
anything on the C++ side (INV-8, INV-12). Parking-lot note already recorded in
`docs/vision.md` under "Understanding-tier future idea: tiered model escalation".
