# LLM Fallback Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a local-LLM (Ollama) fallback tier to JARVIS's Understanding layer, consulted only
when the existing rule-based classifier can't recognise the input, plus the observability and
evaluation harness needed to prove it works.

**Architecture:** Two new small Python modules in `ai/` — `llm_backend.py` (talks to Ollama) and
`resolver.py` (tries the rule classifier first, escalates to the LLM only on a miss) — sit behind
a single `resolve()` entry point that `jarvis_ai_server.py` calls instead of `classify()`
directly. Nothing in `proto/`, `core/`, or any client surface changes.

**Tech Stack:** Python 3, `urllib` (stdlib, no new dependency) for the Ollama HTTP call, `pytest`
+ `unittest.mock`/`monkeypatch` for tests, local Ollama server (already running,
`llama3.2:latest` already pulled).

**Spec:** `docs/superpowers/specs/2026-08-30-llm-fallback-tier-design.md`

## Global Constraints

- Ollama endpoint: `http://localhost:11434/api/generate`. Model: `llama3.2:latest`.
- Inner Ollama call timeout: exactly `3.0` seconds — must stay comfortably under the existing
  5-second C++→Python gRPC deadline (`core/ai_client.cpp`).
- No new pip dependency — use `urllib.request`/`urllib.error` from the standard library only.
- The LLM tier (`llm_classify`) is consulted **only** when `classify()` returns
  `("UNKNOWN", 0.0)`. Never called otherwise.
- No changes to `proto/ai.proto`, `proto/jarvis.proto`, `core/`, or any generated stub.
- Any failure talking to Ollama (connection error, timeout, malformed reply, unrecognised intent
  name) must degrade to `("UNKNOWN", 0.0)` — never raise out of `llm_classify()`.
- **Clarification beyond the spec's literal text:** the spec (§3.1) describes `resolve()` as
  returning `(intent, confidence)`, matching `classify()`'s shape. To satisfy the spec's own
  observability requirement (§3.4 — log which tier answered) without a second, duplicate call,
  `resolve()` in this plan returns a 3-tuple: `(intent: str, confidence: float, tier: str)` where
  `tier` is `"rule"`, `"llm"`, or `"none"`. This is a deliberate, minimal refinement, not a
  deviation from the design's intent — flagging it here so it isn't mistaken for an
  inconsistency.
- Every automated test (anything under `pytest`) must run fully offline — no test may make a real
  network call to Ollama. The LLM tier is mocked in every unit test. Only the manual/live smoke
  test (Task 5) and the eval harness (Task 4, run by hand) talk to a real Ollama server.
- Follow existing `ai/` conventions: `from __future__ import annotations`, module-level docstring
  explaining the *why*, `pytest` with class-grouped tests and `@pytest.mark.parametrize` where it
  reduces repetition (see `ai/test_intent_classifier.py`, `ai/test_ai_server.py`).
- pytest here uses rootless import mode (no `__init__.py` in `ai/`) — new modules import as bare
  names (`from llm_backend import llm_classify`), matching how `ai/jarvis_ai_server.py` already
  does `from intent_classifier import classify`.

---

### Task 1: LLM backend — talk to Ollama

**Files:**
- Create: `ai/llm_backend.py`
- Test: `ai/test_llm_backend.py`

**Interfaces:**
- Consumes: nothing from other tasks (only stdlib `urllib`, `json`).
- Produces: `llm_classify(text: str) -> tuple[str, float]` — same shape as
  `intent_classifier.classify()`. Returns `("UNKNOWN", 0.0)` on any failure. Confidence is
  clamped to `[0.0, 1.0]`. Consumed by Task 2's `resolver.py`.

- [ ] **Step 1: Write the failing tests**

Create `ai/test_llm_backend.py`:

```python
"""Unit tests for the LLM fallback classifier. Every test mocks the Ollama HTTP call — this
module must never make a real network call, so the suite stays fast and doesn't depend on a
running Ollama server."""

import json
import urllib.error
from unittest.mock import MagicMock, patch

from llm_backend import llm_classify


def _fake_response(body: dict) -> MagicMock:
    mock_cm = MagicMock()
    mock_cm.__enter__.return_value.read.return_value = json.dumps(body).encode("utf-8")
    return mock_cm


class TestLLMClassifySuccess:
    @patch("llm_backend.urllib.request.urlopen")
    def test_valid_json_reply_is_parsed(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "STATUS", "confidence": 0.82})}
        )
        assert llm_classify("how long you been up") == ("STATUS", 0.82)

    @patch("llm_backend.urllib.request.urlopen")
    def test_model_reports_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "UNKNOWN", "confidence": 0.0})}
        )
        assert llm_classify("the weather today") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_confidence_is_clamped_above_one(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "ECHO", "confidence": 1.5})}
        )
        assert llm_classify("say something") == ("ECHO", 1.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_confidence_is_clamped_below_zero(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "ECHO", "confidence": -0.3})}
        )
        assert llm_classify("say something") == ("ECHO", 0.0)


class TestLLMClassifyDegradesGracefully:
    @patch("llm_backend.urllib.request.urlopen")
    def test_connection_error_returns_unknown(self, mock_urlopen):
        mock_urlopen.side_effect = urllib.error.URLError("connection refused")
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_timeout_returns_unknown(self, mock_urlopen):
        mock_urlopen.side_effect = TimeoutError()
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_malformed_outer_json_returns_unknown(self, mock_urlopen):
        mock_cm = MagicMock()
        mock_cm.__enter__.return_value.read.return_value = b"not json"
        mock_urlopen.return_value = mock_cm
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_malformed_inner_json_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response({"response": "not valid json"})
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_missing_intent_key_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"confidence": 0.9})}
        )
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_unrecognised_intent_name_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "WEATHER", "confidence": 0.9})}
        )
        assert llm_classify("anything") == ("UNKNOWN", 0.0)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_llm_backend.py -v`
Expected: FAIL/ERROR — `ModuleNotFoundError: No module named 'llm_backend'`

- [ ] **Step 3: Write the implementation**

Create `ai/llm_backend.py`:

```python
"""LLM-backed intent classification — the escalation tier of the Understanding layer.

Consulted only when the rule-based classifier (intent_classifier.classify) can't match the
input. Calls a local Ollama server; any failure (unreachable, slow, malformed reply) degrades to
("UNKNOWN", 0.0) rather than raising — a dead/slow LLM must never block the caller.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request

OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "llama3.2:latest"
TIMEOUT_SECONDS = 3.0

KNOWN_INTENTS = {"STATUS", "ECHO", "ABOUT"}

PROMPT_TEMPLATE = """You are an intent classifier for a personal assistant called JARVIS. \
JARVIS currently understands exactly these commands:

- STATUS: the user is asking about JARVIS's uptime, whether it is running, or its current state.
- ECHO: the user wants JARVIS to repeat/say something back.
- ABOUT: the user is asking who or what JARVIS is.

Given the user's message below, decide which single intent it matches, or UNKNOWN if it \
matches none of them. Respond with ONLY a JSON object of the exact form:
{{"intent": "STATUS" | "ECHO" | "ABOUT" | "UNKNOWN", "confidence": <number between 0.0 and 1.0>}}

User message: {text}
"""


def llm_classify(text: str) -> tuple[str, float]:
    """Ask the local Ollama model to classify `text`. Returns ("UNKNOWN", 0.0) on any failure."""
    payload = json.dumps({
        "model": MODEL,
        "prompt": PROMPT_TEMPLATE.format(text=text),
        "format": "json",
        "stream": False,
    }).encode("utf-8")

    request = urllib.request.Request(
        OLLAMA_URL,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            outer = json.loads(response.read().decode("utf-8"))
        parsed = json.loads(outer["response"])
        intent = parsed["intent"]
        confidence = float(parsed["confidence"])
    except (urllib.error.URLError, TimeoutError, OSError, KeyError, ValueError, TypeError,
            json.JSONDecodeError):
        return "UNKNOWN", 0.0

    if intent not in KNOWN_INTENTS:
        return "UNKNOWN", 0.0

    return intent, max(0.0, min(1.0, confidence))
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_llm_backend.py -v`
Expected: PASS — all 11 tests green.

- [ ] **Step 5: Commit**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
git add ai/llm_backend.py ai/test_llm_backend.py
git commit -m "feat(ai): add Ollama-backed llm_classify() escalation tier"
```

---

### Task 2: Resolver — tiered dispatch between rules and LLM

**Files:**
- Create: `ai/resolver.py`
- Test: `ai/test_resolver.py`

**Interfaces:**
- Consumes: `intent_classifier.classify(text: str) -> tuple[str, float]` (existing, unchanged);
  `llm_backend.llm_classify(text: str) -> tuple[str, float]` (Task 1).
- Produces: `resolve(text: str) -> tuple[str, float, str]` — `(intent, confidence, tier)`,
  `tier` ∈ `{"rule", "llm", "none"}`. Consumed by Task 3's `jarvis_ai_server.py` and Task 4's eval
  harness.

- [ ] **Step 1: Write the failing tests**

Create `ai/test_resolver.py`:

```python
"""Unit tests for the tiered resolver. The LLM tier is always mocked here — these tests prove
the *escalation logic* (when each tier is/isn't consulted), not the LLM's own behaviour (that's
test_llm_backend.py's job)."""

from unittest.mock import patch

from resolver import resolve


class TestResolvePrefersRules:
    @patch("resolver.llm_classify")
    def test_rule_hit_never_calls_llm(self, mock_llm):
        assert resolve("status") == ("STATUS", 1.0, "rule")
        mock_llm.assert_not_called()

    @patch("resolver.llm_classify")
    def test_rule_hit_echo_never_calls_llm(self, mock_llm):
        assert resolve("say hello") == ("ECHO", 1.0, "rule")
        mock_llm.assert_not_called()


class TestResolveEscalatesToLLM:
    @patch("resolver.llm_classify")
    def test_rule_miss_llm_hit(self, mock_llm):
        mock_llm.return_value = ("STATUS", 0.75)
        assert resolve("hows it going") == ("STATUS", 0.75, "llm")
        mock_llm.assert_called_once_with("hows it going")

    @patch("resolver.llm_classify")
    def test_rule_miss_llm_also_miss(self, mock_llm):
        mock_llm.return_value = ("UNKNOWN", 0.0)
        assert resolve("the weather today") == ("UNKNOWN", 0.0, "none")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_resolver.py -v`
Expected: FAIL/ERROR — `ModuleNotFoundError: No module named 'resolver'`

- [ ] **Step 3: Write the implementation**

Create `ai/resolver.py`:

```python
"""The tiered Understanding-layer resolver — the single entry point jarvis_ai_server.py calls.

Tries the deterministic rule classifier first (fast, free, high-precision); only escalates to
the local LLM when the rules draw a blank. Keeping this as the one fixed entry point is what lets
a future third tier (e.g. escalating further for genuinely complex requests) slot in later
without touching the gRPC server or anything on the C++ side.
"""

from __future__ import annotations

from intent_classifier import classify
from llm_backend import llm_classify


def resolve(text: str) -> tuple[str, float, str]:
    """Classify `text`. Returns (intent, confidence, tier) — tier is "rule", "llm", or "none"."""
    intent, confidence = classify(text)
    if intent != "UNKNOWN":
        return intent, confidence, "rule"

    intent, confidence = llm_classify(text)
    if intent != "UNKNOWN":
        return intent, confidence, "llm"

    return "UNKNOWN", 0.0, "none"
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_resolver.py -v`
Expected: PASS — all 4 tests green.

- [ ] **Step 5: Commit**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
git add ai/resolver.py ai/test_resolver.py
git commit -m "feat(ai): add resolve() tiered dispatch between rule classifier and LLM"
```

---

### Task 3: Wire `resolve()` into the AI server, with tier logging

**Files:**
- Modify: `ai/jarvis_ai_server.py`
- Modify: `ai/test_ai_server.py`

**Interfaces:**
- Consumes: `resolver.resolve(text: str) -> tuple[str, float, str]` (Task 2).
- Produces: nothing new consumed by later tasks — this is where the tiered resolver becomes
  live in the actual gRPC response path.

- [ ] **Step 1: Write the failing tests**

Replace the full contents of `ai/test_ai_server.py` with:

```python
"""Tests that the AI server's servicer resolves input (via the tiered resolver) and populates
the response fields. The LLM tier is mocked by default (autouse fixture) so this suite stays
offline; individual tests override it to prove the LLM-escalation wiring works end-to-end
through the servicer."""

import pytest

# Importing the server module sets up sys.path for the generated stubs and pulls in ai_pb2.
from jarvis_ai_server import JarvisAIServicer, ai_pb2
import resolver


@pytest.fixture(autouse=True)
def no_real_llm_calls(monkeypatch):
    """Default the LLM tier to UNKNOWN so a rule-classifier miss never makes a real Ollama call
    in this suite. Individual tests override this via the monkeypatch parameter."""
    monkeypatch.setattr(resolver, "llm_classify", lambda text: ("UNKNOWN", 0.0))


def call(text):
    servicer = JarvisAIServicer()
    request = ai_pb2.NaturalLanguageRequest(text=text)
    return servicer.ProcessNaturalLanguage(request, context=None)


class TestServicerClassifiesViaRules:
    def test_known_intent_is_populated(self):
        resp = call("how long have you been running")
        assert resp.success
        assert resp.intent == "STATUS"
        assert resp.confidence == pytest.approx(1.0)
        assert "STATUS" in resp.reply

    def test_unknown_intent(self):
        resp = call("the weather today")
        assert resp.success
        assert resp.intent == "UNKNOWN"
        assert resp.confidence == 0.0

    @pytest.mark.parametrize(
        "text,expected",
        [("status", "STATUS"), ("say hello", "ECHO"), ("who are you", "ABOUT")],
    )
    def test_maps_intents(self, text, expected):
        assert call(text).intent == expected


class TestServicerEscalatesToLLM:
    def test_llm_resolved_intent_is_populated(self, monkeypatch):
        monkeypatch.setattr(resolver, "llm_classify", lambda text: ("STATUS", 0.7))
        resp = call("hows it going")
        assert resp.success
        assert resp.intent == "STATUS"
        assert resp.confidence == pytest.approx(0.7)
        assert "STATUS" in resp.reply

    def test_llm_also_misses_stays_unknown(self, monkeypatch):
        monkeypatch.setattr(resolver, "llm_classify", lambda text: ("UNKNOWN", 0.0))
        resp = call("the weather today")
        assert resp.success
        assert resp.intent == "UNKNOWN"
        assert resp.confidence == 0.0
```

- [ ] **Step 2: Run tests to verify the new ones fail**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_ai_server.py -v`
Expected: `TestServicerClassifiesViaRules` tests still pass (server still calls `classify()`
today); `TestServicerEscalatesToLLM::test_llm_resolved_intent_is_populated` FAILS because the
server hasn't been wired to `resolve()` yet, so mocking `resolver.llm_classify` has no effect on
its output (it'll still report `UNKNOWN`, not `STATUS`).

- [ ] **Step 3: Wire `resolve()` into the server**

In `ai/jarvis_ai_server.py`, change the import and the servicer body:

```python
from resolver import resolve
```

(replaces `from intent_classifier import classify`)

```python
class JarvisAIServicer(ai_pb2_grpc.JarvisAIServiceServicer):
    def ProcessNaturalLanguage(self, request, context):
        start = time.monotonic()
        intent, confidence, tier = resolve(request.text)
        latency_ms = (time.monotonic() - start) * 1000

        logger.info(
            "text=%r tier=%s intent=%s confidence=%.2f latency_ms=%.2f",
            request.text, tier, intent, confidence, latency_ms,
        )

        reply = f"[detected intent: {intent}, confidence {confidence:.2f}]"
        return ai_pb2.NaturalLanguageResponse(
            success=True,
            reply=reply,
            intent=intent,
            confidence=confidence,
        )
```

(replaces the existing `intent, confidence = classify(request.text)` line and the `logger.info`
call below it — the rest of the file, including `serve()`, is unchanged.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/test_ai_server.py -v`
Expected: PASS — all 7 tests green.

Then run the full `ai/` suite to confirm nothing else broke:

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest ai/ -v`
Expected: PASS — 29 (existing) + 11 (Task 1) + 4 (Task 2) + 2 new in this task = 46 passed.

- [ ] **Step 5: Commit**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
git add ai/jarvis_ai_server.py ai/test_ai_server.py
git commit -m "feat(ai): wire tiered resolve() into the AI server, log which tier answered"
```

---

### Task 4: Evaluation harness (INV-13)

**Files:**
- Create: `tools/eval_understanding.py`
- Test: `tools/test_eval_understanding.py`

**Interfaces:**
- Consumes: `intent_classifier.classify`, `llm_backend.llm_classify`, `resolver.resolve` (all
  existing by this point).
- Produces: a standalone script printing an accuracy/latency table. Nothing later depends on
  its internals except the `_run()` helper, which Step 1's tests exercise directly.

- [ ] **Step 1: Write the failing tests**

Create `tools/test_eval_understanding.py`:

```python
"""Unit tests for the eval harness's scoring logic. Uses fake classify functions — never
imports the real classifier/LLM/resolver, so this stays fast and Ollama-independent."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from eval_understanding import _run


def test_perfect_classifier_scores_full_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ({"a": "X", "b": "Y"}[text], 1.0)

    accuracy, avg_latency_ms = _run("perfect", fake_classify, dataset)
    assert accuracy == 1.0
    assert avg_latency_ms >= 0.0


def test_wrong_classifier_scores_zero_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ("UNKNOWN", 0.0)

    accuracy, _ = _run("wrong", fake_classify, dataset)
    assert accuracy == 0.0


def test_partial_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ("X", 1.0) if text == "a" else ("UNKNOWN", 0.0)

    accuracy, _ = _run("partial", fake_classify, dataset)
    assert accuracy == 0.5
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest tools/test_eval_understanding.py -v`
Expected: FAIL/ERROR — `ModuleNotFoundError: No module named 'eval_understanding'`

- [ ] **Step 3: Write the implementation**

Create `tools/eval_understanding.py`:

```python
"""Evaluation harness for the Understanding tier (INV-13) — measures accuracy and latency of
each classification approach against a small labeled dataset.

Run standalone: `python3 tools/eval_understanding.py`. The rule-only row needs nothing extra;
the llm-only and hybrid rows need a running local Ollama server (see ai/llm_backend.py).
"""

from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
AI_DIR = ROOT / "ai"
if str(AI_DIR) not in sys.path:
    sys.path.insert(0, str(AI_DIR))

from intent_classifier import classify
from llm_backend import llm_classify
from resolver import resolve

# (text, expected_intent) — includes phrasings the rule classifier is expected to miss but a
# reasonable LLM should catch, so the three configurations below are meaningfully different.
DATASET = [
    ("status", "STATUS"),
    ("what's your uptime", "STATUS"),
    ("how long have you been running", "STATUS"),
    ("are you alive", "STATUS"),
    ("hows it going", "STATUS"),
    ("you doing okay up there", "STATUS"),
    ("echo this back", "ECHO"),
    ("repeat after me", "ECHO"),
    ("say hello", "ECHO"),
    ("can you repeat what I just said", "ECHO"),
    ("about", "ABOUT"),
    ("who are you", "ABOUT"),
    ("what is jarvis", "ABOUT"),
    ("tell me what you are", "ABOUT"),
    ("what is the meaning of life", "UNKNOWN"),
    ("the weather today", "UNKNOWN"),
]


def _run(label, classify_fn, dataset):
    """Run `classify_fn` over `dataset`, print and return (accuracy, avg_latency_ms)."""
    correct = 0
    total_latency_ms = 0.0
    for text, expected in dataset:
        start = time.monotonic()
        intent, _confidence = classify_fn(text)
        total_latency_ms += (time.monotonic() - start) * 1000
        if intent == expected:
            correct += 1
    accuracy = correct / len(dataset)
    avg_latency_ms = total_latency_ms / len(dataset)
    print(f"{label:10s} accuracy={accuracy:6.1%}  avg_latency_ms={avg_latency_ms:8.2f}")
    return accuracy, avg_latency_ms


def _resolve_intent_only(text):
    intent, confidence, _tier = resolve(text)
    return intent, confidence


def main():
    print(f"Evaluating {len(DATASET)} labeled examples across three configurations:\n")
    _run("rule-only", classify, DATASET)
    _run("llm-only", llm_classify, DATASET)
    _run("hybrid", _resolve_intent_only, DATASET)


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python -m pytest tools/test_eval_understanding.py -v`
Expected: PASS — all 3 tests green.

Then, separately (not part of the pytest suite — a manual live check against real Ollama):

Run: `cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS" && source .venv/bin/activate && python3 tools/eval_understanding.py`
Expected: prints three lines (`rule-only`, `llm-only`, `hybrid`) each with an accuracy percentage
and average latency in ms. `rule-only` accuracy should be noticeably lower than `hybrid` (it
can't get the "hows it going" style phrasings right); `hybrid` accuracy should be ≥ both
individual rows, since it takes whichever tier succeeds.

- [ ] **Step 5: Commit**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
git add tools/eval_understanding.py tools/test_eval_understanding.py
git commit -m "feat(ai): add Understanding-tier evaluation harness (rule/LLM/hybrid comparison)"
```

---

### Task 5: Smoke test — prove the live escalation path end-to-end

**Files:**
- Modify: `tools/grpc_smoke_test.py`

**Interfaces:**
- Consumes: the full live stack (Ollama + `build/jarvis_grpc_server` + `ai/jarvis_ai_server.py`)
  — no new Python interfaces produced or consumed; this task is a manual verification pass, not
  unit-testable in isolation.

- [ ] **Step 1: Add an LLM-only case to the smoke test**

In `tools/grpc_smoke_test.py`, inside `main()`, immediately after the existing
`"unclassified -> UNKNOWN"` call, add:

```python
    # Only the LLM tier should resolve this — "hows it going" doesn't match any rule pattern
    # (verified: intent_classifier.classify("hows it going") == ("UNKNOWN", 0.0)) but a
    # reasonable local model should recognise it as a STATUS-style check-in.
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "hows it going",
        label="LLM-only -> STATUS (requires live Ollama)",
    )
```

- [ ] **Step 2: Run the full stack and verify manually**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
source .venv/bin/activate
python3 ai/jarvis_ai_server.py &
sleep 1
./build/jarvis_grpc_server &
sleep 1
python3 tools/grpc_smoke_test.py
```

Expected: every case from the previous session still passes as before, and the new
`[LLM-only -> STATUS (requires live Ollama)]` case shows `command_type: COMMAND_TYPE_STATUS`
with real uptime text in `message` (proving the LLM tier resolved it and C++ re-dispatched on
it) — not `COMMAND_TYPE_UNKNOWN`. If Ollama isn't reachable, this one case degrades to
`COMMAND_TYPE_UNKNOWN` gracefully (per Task 1's error handling) rather than erroring — that's
also an acceptable, expected outcome if Ollama happens to be down when this is run, just not the
one that proves the escalation path.

Then stop the background processes:

```bash
pkill -f jarvis_ai_server.py
pkill -f jarvis_grpc_server
```

- [ ] **Step 3: Commit**

```bash
cd "/home/rayyan/Programming/Remote Repos/Public/JARVIS"
git add tools/grpc_smoke_test.py
git commit -m "test: add live LLM-escalation case to the gRPC smoke test"
```

---

## Post-plan documentation (required by project rules, not a numbered task)

After Task 5 lands, per `CLAUDE.md` §4 and §6.2:
- Update `docs/features.md` Phase 2.5 checklist — tick off the LLM integration item, leave
  multi-turn context and safety guardrails as-is (out of scope here).
- Append a same-day Gen AI usage log entry (`qmul/notes/genai-usage-log.md`) and an engineering
  logbook entry (`qmul/logbook/`) covering this implementation work, per the standing
  documentation duty — same format as the two existing Phase 2 entries.

## Self-Review Notes

- **Spec coverage:** §3.1 (interface) → Tasks 1–3; §3.2 (Ollama call) → Task 1; §3.3 (confidence
  semantics) → Task 1 (clamping) + noted in module docstring; §3.4 (observability/tier logging)
  → Task 3; §3.5 (evaluation harness) → Task 4; §3.6 (testing) → Tasks 1, 2, 3 (mocked units) and
  Task 5 (live smoke case); §4 (no new requirements file) → confirmed, no task adds one; §5
  (nothing else changes) → confirmed, no task touches `proto/` or `core/`; §7 (deferred
  multi-tier future) → not built, `resolve()`'s single-entry-point shape in Task 2 is what keeps
  the door open, matching the spec's stated intent.
- **Placeholder scan:** none found — every step has real code, real commands, real expected
  output.
- **Type consistency:** `classify()` and `llm_classify()` both `(str, float)` throughout;
  `resolve()` is `(str, float, str)` consistently across Tasks 2, 3, and 4 (`_resolve_intent_only`
  unpacks and discards the tier for the eval harness's 2-tuple `classify_fn` contract). Function
  names match everywhere they're referenced across tasks.
