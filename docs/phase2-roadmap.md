# Phase 2 Roadmap — Intelligence Layer

Phase 1 built the service foundation: typed contracts, a transport-agnostic core, and a working C++↔Python round-trip. Phase 2 fills the AI layer with actual intelligence, starting from the simplest possible classifier and growing toward LLM-backed reasoning.

This document follows the same per-step structure as `roadmap.md`: each step has a goal, concrete tasks, and a stated learning outcome. Do not skip the learning outcomes — they are the point.

---

## Phase 2 objective

- Replace the Python AI server's placeholder echo with a real, deterministic classifier.
- Teach the C++ side to *act* on structured AI responses, not just forward strings back to the user.
- Establish the patterns (preprocessing, classification, structured response) that the eventual LLM integration will plug into.

---

## Step 1 — Understand intent vs. surface form

Goal:

- Build the mental model that separates *what the user typed* from *what they meant*.

Tasks:

1. Pick three pairs of inputs that mean the same thing but look completely different:
   - "what's your status" / "how long have you been running" / "are you alive"
   - "say hello" / "repeat after me hello" / "echo hello"
   - "tell me about yourself" / "who are you" / "what is jarvis"
2. Note for each pair which existing JARVIS command it maps to (`STATUS`, `ECHO`, `ABOUT`).
3. Convince yourself that no amount of keyword tuning at the C++ parser level fixes this — the abstraction has to live one layer up.

Learning outcome:

- Understand why intent classification belongs in a dedicated layer above the command parser, not inside it.

---

## Step 2 — Design the Python classifier data model

Goal:

- Decide what shape "a rule" takes before writing any matching code.

Tasks:

1. Create `ai/intent_classifier.py`.
2. Define a module-level dict mapping intent name → list of keyword patterns. Example shape:
   ```python
   INTENT_PATTERNS: dict[str, list[set[str]]] = {
       "STATUS": [{"status"}, {"uptime"}, {"how", "long", "running"}, {"alive"}],
       "ECHO":   [{"echo"}, {"repeat", "after"}, {"say"}],
       "ABOUT":  [{"about"}, {"who", "are", "you"}, {"what", "jarvis"}],
   }
   ```
3. Define a return type: `(intent: str, confidence: float)` where unmatched input returns `("UNKNOWN", 0.0)`.

Learning outcome:

- Practice "data first, code second" — when rules are data, the matching loop becomes trivial and the rule set becomes maintainable.

---

## Step 3 — Implement preprocessing + token matching

Goal:

- Turn raw natural language into a comparable token set, then score each intent.

Tasks:

1. Add a `preprocess(text: str) -> set[str]` helper:
   - lowercase
   - strip punctuation
   - split on whitespace
   - return as a `set[str]` (so order doesn't matter and duplicates collapse)
2. Add `classify(text: str) -> tuple[str, float]`:
   - preprocess the input
   - for each intent, compute the best score across its patterns: `len(pattern & tokens) / len(pattern)`
   - return the highest-scoring intent if its score ≥ a threshold (e.g. 0.5), else `UNKNOWN`
3. Write a `__main__` block at the bottom of the file that runs a few hard-coded test inputs and prints the classifications. This becomes your manual unit test.

Learning outcome:

- See why set intersection gives you fuzzy matching for free, and why threshold-based decision making is more robust than "any keyword matches → fire".

---

## Step 4 — Extend `ai.proto` to carry intent back

Goal:

- Make the AI layer's structured output visible to the C++ side via the contract, not via string parsing.

Tasks:

1. Add two fields to `NaturalLanguageResponse` in `proto/ai.proto`:
   ```proto
   string intent     = 4;  // e.g. "STATUS", "ECHO", "ABOUT", or "UNKNOWN"
   float  confidence = 5;  // 0.0–1.0
   ```
2. Read up briefly on protobuf field-numbering rules: numbers are immutable once shipped; new fields must use unused numbers; renaming a field is fine, renumbering is not.
3. Regenerate the C++ and Python stubs (see the README).

Learning outcome:

- Internalise schema versioning discipline: contracts are append-only in spirit, even when you control both sides.

---

## Step 5 — Wire the classifier into the Python AI server

Goal:

- Keep the I/O layer thin and the logic layer pure.

Tasks:

1. In `ai/jarvis_ai_server.py`, import `classify` from `intent_classifier`.
2. Inside `ProcessNaturalLanguage`, call `classify(request.text)` and populate the new `intent` and `confidence` fields on the response.
3. Update the `reply` field to be human-readable based on the intent (e.g. `"[detected intent: STATUS, confidence 0.83]"`) — this stays useful even after Step 6 takes over routing.

Learning outcome:

- See the pattern of "service handler = thin shell calling a pure function". This is what makes the logic unit-testable later.

---

## Step 6 — Use the intent on the C++ side

Goal:

- Close the loop: the C++ server reads the intent, and if it matches a known command, re-dispatches internally rather than just forwarding the AI's reply text.

Tasks:

1. Update `core/ai_client.h/.cpp` so `ProcessNaturalLanguage` returns a small struct (`AIResult` with `reply`, `intent`, `confidence`) instead of a bare `std::string`.
2. In `core/jarvis_service.cpp`, when handling the `UNKNOWN` branch:
   - call the AI client
   - if `confidence ≥ threshold` and the intent maps to a known `CommandType`, build a new `ParsedCommand` with that type and re-run `runCMD()`
   - otherwise, fall through to returning the AI reply as before
3. Add a small map (proto-side or in `jarvis_service.cpp`) from intent string → `CommandType`.

Learning outcome:

- Practice acting on structured cross-service data. The pattern (read structured field → branch on it → re-enter your own dispatch) is exactly how real microservice boundaries work.

---

## Step 7 — Extend the smoke test to verify intent paths

Goal:

- Make the new behaviour visible and regression-proof.

Tasks:

1. Add cases to `tools/grpc_smoke_test.py`:
   - `"how long have you been running"` → expect a STATUS-like response (uptime block)
   - `"repeat after me hello"` → expect "hello"
   - `"who are you"` → expect the ABOUT text
   - `"what is the meaning of life"` → expect a generic AI fallback (no high-confidence intent)
2. Print the detected intent + confidence alongside each response so the test output is self-documenting.

Learning outcome:

- Understand that an end-to-end test isn't "did it return 200" — it's "did the right behaviour fire for an indirect input".

---

## Step 8 — Observability for the AI layer

Goal:

- Make Python-side debugging as good as C++-side debugging.

Tasks:

1. Replace `print()` calls in `jarvis_ai_server.py` with the `logging` module (or `structlog` if you want to learn structured logging in Python).
2. Log: timestamp, request text, detected intent, confidence, latency in ms.
3. Optionally add a request-id field to `NaturalLanguageRequest` so you can correlate C++ and Python logs for the same call.

Learning outcome:

- Learn that hybrid-system debugging is impossible without correlated logs across processes — and that adding the correlation primitive (request id) costs almost nothing if you do it early.

---

## What comes after Phase 2

Phase 2 lands when:

- The Python AI server classifies into the four current intents (`STATUS`, `ECHO`, `ABOUT`, plus `UNKNOWN` fallback)
- The C++ server re-dispatches based on classified intent
- The smoke test exercises both classified and unclassified inputs
- Python-side logs are structured

Once that's in place, the door is open to:

1. **Phase 2.5 — LLM integration**: swap the rule-based classifier for an LLM call (Ollama / llama.cpp), keeping the same `classify()` signature so the C++ side doesn't change. The classifier becomes one of two strategies; rules stay as the fast/cheap path, LLM is fallback.
2. **Phase 3 — Plugin expansion**: the intent layer is the natural place to register new intents tied to new plugins.

See [`future-features.md`](future-features.md) for the longer view.
