# Understanding Tier Robustness — Design Spec

**Status:** approved (user approved this design in-chat on 2026-09-03; pre-authorized proceeding
straight to implementation plan + build without a separate spec-review pause).
**Phase:** Phase 4-adjacent — this is a depth pass on the Understanding tier (`ai/`), not a new
phase. It's motivated directly by Phase 4 landing `volume`/`shutdown` as dynamically-loaded
plugins that are currently *unreachable by natural language at all*.

## 1. Scope

**In scope:**
- **Data-driven intent registration.** Replace the two hardcoded intent lists (`INTENT_PATTERNS`
  in `ai/intent_classifier.py`, `PROMPT_TEMPLATE`/`KNOWN_INTENTS` in `ai/llm_backend.py`) with a
  single Intent Registry loaded from JSON at AI-server startup, sourced from `config/intents.json`
  (built-ins) and each plugin's existing `manifest.json` (extended with new optional fields). A
  new capability becomes understandable in natural language without editing any Python file.
- **Deterministic-first slot/argument extraction**, with LLM fallback, so a capability that takes
  an argument (currently only `volume`) can be driven by natural phrasing ("turn it up to 50"),
  not just its literal CLI syntax ("volume set 50").
- **A new proto field** carrying the extracted payload from Python back to C++ (append-only,
  INV-2), since today the AI server has no channel to hand back anything but `intent`/`confidence`
  — C++ always re-dispatches with the original raw sentence as payload, unconditionally.
- **Negation handling** in the rule classifier — a data-driven veto (same mechanism as the
  existing meta-question veto) so "don't shut down", "cancel that", "never mind the volume"
  suppress a match instead of firing it.
- **Case-insensitive confirm-token matching** in `ConsentGate::payloadConfirms()` — a real,
  narrow bug (exact-case-only comparison) directly in the reliability chain this work is meant to
  harden.
- **A structural (code-level, not just documented) guarantee** that T3/T4 capabilities never get
  slot-extracted payloads — the full original sentence always reaches `ConsentGate`, preserving
  the existing confirm-token contract regardless of what any plugin manifest declares.
- **Expanded evaluation harness** (`tools/eval_understanding.py`) — categorized scenario coverage:
  typos, synonyms, negation, off-topic/no-match, voice/STT-specific noise (filler words,
  "Jarvis, ..." address-term prefixes), adversarial/injection-like phrasing, genuinely ambiguous
  input, and very short/very long input — plus new assertions for slot-extraction accuracy and
  T3/T4 payload preservation.
- Migrating the existing 4 hardcoded intents (STATUS, ECHO, ABOUT, SYSTEM_INFO) onto the new
  registry mechanism, and adding a 5th (HELP) that the classifier doesn't currently recognize at
  all despite the capability existing.

**Out of scope (deliberately, per explicit decisions made during brainstorming):**
- **Multi-intent utterances** ("what's your status and say hello"). Stays single-intent-per-
  utterance; the C++ dispatch loop, proto contract, and CLI/voice loop all assume exactly one
  intent per request today, and extending that is a materially larger, separate feature.
- **Multi-turn conversational confirmation** ("Are you sure? ... Yes."). The project has
  deliberately deferred conversation state before (see `docs/features.md`'s Phase 2.5 notes); this
  spec keeps T3/T4 confirmation single-utterance, just makes the existing single-utterance
  contract actually reliable (full-text preservation + case-insensitivity) rather than introducing
  new state.
- **Non-English input.** Not raised as a requirement; `voice/stt.py`'s `stt.language` config
  already pins English, and nothing here changes that.
- **A new LLM backend or model.** Stays local Ollama `llama3.2:latest`, same 3.0s timeout
  (INV-11). This work only changes what's *asked* of the LLM (a dynamically-built prompt, plus an
  optional second call for slot extraction), never how it's reached.

## 2. Why now (the concrete gap)

`ai/intent_classifier.py`'s `INTENT_PATTERNS` and `ai/llm_backend.py`'s `PROMPT_TEMPLATE`/
`KNOWN_INTENTS` are both hardcoded to exactly `{STATUS, ECHO, ABOUT, SYSTEM_INFO}`. The
`system-control` plugin shipped `volume` and `shutdown` as real, dispatchable capabilities
(2026-09-02) that are structurally invisible to both tiers of the Understanding layer — saying
"turn the volume up" or "shut down the computer" returns UNKNOWN today, unconditionally, no
matter how the sentence is phrased. Every future plugin has the identical problem. This is the
same "adding a capability must not require editing the dispatcher" property INV-6 already
guarantees for the C++ registry, missing one layer up in the Understanding tier that's supposed to
sit *above* execution (INV-5) — right now it's the opposite: execution capabilities exist that
Understanding has no way to know about without a manual Python edit per capability.

Separately, tracing the actual code path (`core/jarvis_service.cpp`'s `ProcessCommand`) confirms
today's re-dispatch always uses `request->payload()` — the original raw sentence — never anything
from the AI response. `NaturalLanguageResponse` (`proto/ai.proto`) has no payload field at all.
So even once `volume` is *recognized*, nothing today can turn "turn it up to fifty" into the
`"set 50"` string `parseVolumeArgument` actually understands — INV-5 says that translation belongs
in the Understanding tier, not the C++ parser, which means it structurally requires a new channel
back from Python, not a smarter regex in `plugin_internal.cpp`.

## 3. Architecture

A new **Intent Registry** becomes the single source of truth for what the Understanding tier
knows how to recognize, replacing the two independently-hardcoded intent lists.

```
config/intents.json  ─┐
                       ├─► ai/intent_registry.py ─► list[IntentSpec] ─┬─► ai/intent_classifier.py
plugins/*/manifest.json┘                                              └─► ai/llm_backend.py
                                                                        (prompt + KNOWN_INTENTS)
```

`config/intents.json` and each plugin's `manifest.json` use the **same schema** for the new
fields, so the registry loader has exactly one parsing path regardless of source. This was
verified safe for the existing C++ `PluginLoader`/manifest parser (`core/plugin_loader.cpp`,
`core/minimal_json.h/.cpp`): it only reads specific known keys by name and never rejects a
manifest for containing extra, unrecognized ones — so adding new optional fields to
`manifest.json` is non-breaking for the C++ side, which continues to ignore them entirely. The
Intent Registry is Python-only; C++ never reads the new fields.

### 3.1 `IntentSpec` (conceptual — one per intent)

| Field | Source | Notes |
|---|---|---|
| `name` | derived | Canonical `UPPER_SNAKE` form, e.g. `volume` → `VOLUME`, `system-info` → `SYSTEM_INFO`. Mirrors the existing `normalizeClassifierIntent()` mapping in `core/jarvis_service.cpp`, just applied in the opposite direction. |
| `description` | manifest's existing `description` field | Reused, not duplicated — every manifest already has a human-readable description written for `help`/registration purposes; it becomes the LLM prompt's per-intent description too. |
| `power_tier` | manifest's existing `power_tier` field | Used only to decide whether slot extraction is structurally disallowed (§3.3) — Understanding never enforces consent itself (INV-9 stays entirely in `ConsentGate`). |
| `trigger_keywords` | new, optional | First-word fast-path keywords (e.g. `["volume"]`). |
| `trigger_patterns` | new, optional | List of word-bags, same semantics as today's `INTENT_PATTERNS` values. |
| `meta_question_immune` | new, optional, default `false` | Replaces the hardcoded "except ABOUT" special-case in the meta-question veto — a data flag instead of an intent-name check. |
| `slot_rules` | new, optional | Ordered list of `{regex, payload_template}` for deterministic extraction. Absent for intents that take no argument. |

## 4. Components

### 4.1 `ai/intent_registry.py` (new)

`load_intents(config_path, plugin_dirs) -> list[IntentSpec]`. Parses `config/intents.json`
(required — if missing, built-ins have zero trigger phrases, which is a real regression, so this
file ships in the repo like `config/capabilities.cfg` does) plus every `manifest.json` under the
configured plugin directories (reusing the same `config/plugin_dirs.cfg` the C++ `PluginLoader`
already reads, so there's one list of plugin locations, not two). Malformed entries (bad regex,
missing required manifest fields the loader itself needs, unreadable file) are skipped with a
logged warning — never fatal, matching `PluginLoader`'s and `PluginConfig`'s existing "a bad
config entry degrades, it doesn't crash the process" discipline (INV-7). A duplicate intent name
across two sources is a logged warning; first-seen wins, deterministic tie-break, not a crash.

### 4.2 `ai/intent_classifier.py` (refactored)

Same two-pass algorithm as today (first-word fast path with `difflib` typo correction, then
phrase-pattern token-set scoring with an exclusive `>0.5` threshold), now parameterized by
`list[IntentSpec]` instead of module-level hardcoded dicts. `classify(text, intents)` — the
registry is loaded once at server startup and passed in, not re-read per call.

**New: negation veto.** A fixed, small set of negation cue words (`don't`, `dont`, `do not`,
`never`, `cancel`, `stop`, `nevermind`, `never mind`, `n't`-suffixed tokens) — when present in the
sentence, phrase-pattern matching is suppressed for that sentence entirely (falls through to
UNKNOWN → LLM escalation, exactly like the meta-question veto already does). This is deliberately
conservative and intent-agnostic (not just T3/T4-scoped): "don't echo that" should not fire ECHO
any more than "don't shut down" should fire SHUTDOWN. Implemented as the same
`suppress-if-cue-word-present` shape as the existing meta-question veto — one more veto function
alongside it, not a new mechanism.

The `meta_question_immune` flag replaces the current `pattern != {"about"}` hardcoded exception.

### 4.3 `ai/slot_extractor.py` (new)

`extract(intent_name: str, text: str, intents: list[IntentSpec]) -> Optional[str]`. Looks up the
matching `IntentSpec`; if its `power_tier` is `T3_DESTRUCTIVE` or `T4_EXTERNAL`, **returns `None`
unconditionally** — this is a code-level short-circuit checked before anything else, not merely
"plugin authors shouldn't declare slot_rules for T3/T4" advice. If the tier check passes, tries
each `slot_rules` entry's regex against `text` in declaration order; first match substitutes
capture groups into `payload_template` and returns it. Returns `None` if the intent has no
`slot_rules` or none matched (the caller's fallback is always "use the original raw text" —
unchanged current behavior).

### 4.4 `ai/llm_backend.py` (refactored)

`PROMPT_TEMPLATE` is no longer a literal string constant — `build_prompt(text, intents)` formats
one bullet per `IntentSpec` from its `description`, same format as today. `KNOWN_INTENTS` becomes
`{spec.name for spec in intents}`, computed from the registry instead of hardcoded.

**New: `llm_extract_slot(text, intent_spec) -> Optional[str]`.** A second, narrow Ollama call —
only invoked by the resolver when an intent has `slot_rules` declared (so it legitimately expects
an argument) but the deterministic pass in `slot_extractor.py` found nothing. Same failure
discipline as `llm_classify`: any error, timeout, or malformed response degrades to `None`, never
raises, never blocks (INV-7). Small structured prompt, JSON response format matching the existing
`llm_classify` pattern (`{"value": ...}` or `{"value": null}`).

### 4.5 `ai/resolver.py` (extended)

```python
def resolve(text: str) -> tuple[str, float, str, str]:
    """Returns (intent, confidence, tier, payload). payload is "" when nothing was
    extracted — the caller (jarvis_ai_server.py) leaves the proto payload field unset,
    and C++ falls back to the original request payload exactly as it does today."""
```

Sequence: classify (rule, then LLM on a miss, unchanged escalation order) → if intent resolved,
call `slot_extractor.extract()` → if that's `None` and the intent declares `slot_rules`, call
`llm_extract_slot()` → payload is whatever was found, or `""`.

### 4.6 `proto/ai.proto`

```proto
message NaturalLanguageResponse {
    bool success = 1;
    string reply = 2;
    string error  = 3;
    string intent = 4;
    float confidence = 5;
    string payload = 6;  // Extracted capability argument, "" if none extracted (INV-2: append-only)
}
```

### 4.7 `core/ai_client.h/.cpp`

`AIResult` gains `std::string payload;`, populated from the new proto field in
`ProcessNaturalLanguage`.

### 4.8 `core/jarvis_service.cpp`

In the AI re-dispatch branch, `dispatchResult = registry_.dispatch(classifiedCmd,
aiResult.payload.empty() ? payload : aiResult.payload, execContext);` (both the `CommandType` and
string-intent branches) — the one behavior change in C++, commented to explain *why* (INV-5: the
Understanding tier decides the argument, C++ just carries it).

### 4.9 `core/consent_gate.cpp`

`payloadConfirms()`'s token comparison becomes case-insensitive (lowercase each extracted token
before comparing to `"confirm"`). Purely additive — a payload already using lowercase `"confirm"`
is unaffected; this only widens what's accepted, never narrows it, so no existing passing test can
regress.

### 4.10 `config/intents.json` (new)

Built-in capabilities' trigger data, migrated 1:1 from today's hardcoded values for STATUS/ECHO/
ABOUT, plus a new HELP entry (the `help` capability exists and is dispatchable but the classifier
has never recognized any natural-language phrasing for it — e.g. "what can you do", "help",
"what commands do you have"). `system-info`'s current classifier patterns move out of
`intent_classifier.py` into `plugins/system-info/manifest.json` instead (it's a plugin, its
trigger data belongs with it, not in the built-ins file).

### 4.11 Plugin manifests

`plugins/system-info/manifest.json` and `plugins/system-control/manifest.json` gain
`trigger_keywords`/`trigger_patterns` per capability; `system-control`'s `volume` entry
additionally gains `slot_rules` (e.g. a `set\s+(\d{1,3})` numeric regex → `"set {1}"`, and a
`\bget\b|what.*volume` pattern → literal `"get"`). `shutdown` gets trigger data but explicitly no
`slot_rules` (and would be ignored by the extractor's T3 short-circuit even if it did).

## 5. Data flow (end to end)

**"turn the volume up to fifty please"**
1. CLI/voice → gRPC `ProcessCommand` → C++ sees `UNKNOWN` → forwards to Python AI server
   (unchanged).
2. `resolve()`: `classify()` matches VOLUME via `trigger_patterns` (rule tier, no LLM needed for
   intent itself). `slot_extractor.extract("VOLUME", text, intents)` — VOLUME's numeric-only regex
   doesn't match the word "fifty" → `None`. VOLUME declares `slot_rules`, so
   `llm_extract_slot(text, volume_spec)` is called → Ollama returns `"50"` → payload =
   `"set 50"`.
3. `jarvis_ai_server.py` returns `intent=VOLUME confidence=<rule score> payload="set 50"`.
4. C++ re-dispatches `registry_.dispatch("volume", "set 50", ctx)` instead of the raw sentence —
   `parseVolumeArgument("set 50")` succeeds as it already does today for literal CLI input.

**"don't shut down the computer"**
1. `classify()`: SHUTDOWN's patterns would otherwise match on `{"shut","down"}`, but the negation
   veto sees `"don't"` and suppresses the match → UNKNOWN from the rule tier.
2. Escalates to `llm_classify()` — the LLM sees the full sentence including the negation and (per
   its existing "if this is a question/discussion rather than actual use, respond UNKNOWN"
   instruction, extended to also cover explicit refusal) is expected to return UNKNOWN too.
3. No capability dispatched, no consent check ever reached — correct outcome without JARVIS ever
   asking for confirmation on a request that was actually a refusal.

**"shutdown please, i confirm"** (voice, capitalized by STT as "I Confirm")
1. `classify()` matches SHUTDOWN (no negation cue present).
2. `slot_extractor.extract()` short-circuits to `None` unconditionally (T3 tier) — payload stays
   unset, C++ falls back to the *original full sentence* as payload, exactly as today.
3. `ConsentGate::payloadConfirms()` tokenizes the full sentence, lowercases each token, finds the
   last token is `"confirm"` (case-insensitively) → allowed. Without the case-insensitivity fix,
   `"Confirm"` (capitalized by STT) would have been wrongly denied.

## 6. Error handling

- Missing/malformed `config/intents.json`, a plugin manifest missing new optional fields, or a
  bad regex in `slot_rules`: skip the offending field/entry, log a warning, continue — never
  crash the AI server (INV-7). A plugin manifest that's *entirely* unparseable already fails
  C++'s stricter validation before `dlopen`; this only concerns the new, Python-only, optional
  fields layered on top of an otherwise-valid manifest.
- `llm_extract_slot()` failure/timeout: same as `llm_classify()` today — returns `None`, resolver
  falls back to `""` payload, C++ falls back to the original sentence. Never blocks, never raises.
- T3/T4 short-circuit in `slot_extractor.py` is unconditional code, not a manifest convention —
  even a plugin author's mistake (declaring `slot_rules` on a `T3_DESTRUCTIVE` capability) cannot
  bypass it.
- The negation and meta-question vetoes both fail open toward UNKNOWN (never toward a false
  match) — consistent with the classifier's existing "stay high-precision, defer uncertainty to
  the LLM tier" design (INV-8).

## 7. Testing

- `ai/test_intent_registry.py` (new): loads a small in-test fixture directory tree (not
  production `config/`/`plugins/`) — valid entries parse correctly; a malformed manifest field is
  skipped with the rest of that manifest still loading; a duplicate intent name across two
  sources is deterministic (first-seen wins) and logs a warning, not a crash.
- `ai/test_slot_extractor.py` (new): deterministic regex → template substitution correctness; no
  match → `None`; **a T3/T4 `IntentSpec` with `slot_rules` deliberately attached still returns
  `None`** — this is the test that actually proves §4.3's short-circuit, not just documents it.
- `ai/test_intent_classifier.py` (extended): parametrized over an in-test fixture `IntentSpec`
  list (not real production JSON, so classifier logic tests aren't coupled to production content
  drifting). New cases for the negation veto and the data-driven `meta_question_immune` flag.
- `tools/eval_understanding.py`: dataset grows from 16 examples to a categorized set (rough shape:
  10-15 per category — typo, synonym, negation, off-topic, voice/STT noise, adversarial,
  ambiguous, short/long — plus dedicated slot-extraction-accuracy and T3/T4-payload-preservation
  checks), each case tagged with its category so a failing run says *which kind* of robustness
  regressed, not just an aggregate percentage.
- `tests/consent_gate_test.cpp` (extended): a case proving `"CONFIRM"`/`"Confirm"`/mixed-case
  confirms are accepted identically to lowercase.
- A C++/gRPC-level test (either `tests/capability_registry_test.cpp`-style unit test around the
  new payload-selection logic in `jarvis_service.cpp`, or a `tools/grpc_smoke_test.py` case if
  Ollama is available in the test environment) proving: AI-supplied non-empty payload is used when
  present, and the original request payload is used when the AI payload is empty — the exact
  behavior change in §4.8.

## 8. Migration / compatibility

- `NaturalLanguageResponse.payload` is a new append-only field (INV-2) — no existing consumer
  breaks; an old client simply never reads it.
- Every existing intent (STATUS, ECHO, ABOUT, SYSTEM_INFO) keeps behaving identically: they have
  no `slot_rules`, so `payload` stays `""` for them and C++'s fallback-to-original-text path is
  exactly today's only path. Only `volume` gains new behavior (a populated payload). `shutdown`
  is explicitly unaffected by design (§4.3's short-circuit).
- `ConsentGate`'s case-insensitivity change is strictly additive — accepts a superset of what it
  accepted before.
- No changes to `core/capability_registry.*`, `core/consent_gate.h`'s public signature (only the
  `.cpp` body), `PluginLoader`, or anything in the C++ dispatch/consent path from the previous
  plan (2026-09-02) — this work sits entirely in the Understanding tier plus the one payload-relay
  change in `jarvis_service.cpp`.

## 9. Invariants touched

INV-1 (capabilities stay transport-agnostic — extraction happens in Understanding, not the C++
parser), INV-2 (append-only proto field), INV-5 (Understanding, not C++, decides what a fuzzy
request means, including its argument), INV-6 (adding a capability's natural-language reachability
now requires zero Python edits, matching the existing zero-C++-edits guarantee), INV-7 (every new
failure mode degrades gracefully, never crashes/hangs), INV-8 (rules first, LLM only on a genuine
miss — preserved and extended to slot extraction), INV-9 (ConsentGate remains the only enforcement
point; Understanding-tier changes never invent or bypass a consent check), INV-11 (still local-only
Ollama, no new network dependency), INV-13 (the expanded eval harness is the measurable evidence
this actually got more robust, not just a claim).

## 10. Self-review

**Placeholder scan:** no TBD/TODO; every component has concrete signatures and behavior.
**Internal consistency:** §4.3's T3/T4 short-circuit is referenced consistently in §5's shutdown
example, §6's error handling, and §8's compatibility section — no contradiction. §4.6's proto
field number (6) is the next free number after the existing 1-5, consistent with `jarvis.proto`'s
own append-only history.
**Scope check:** focused on the Understanding tier plus the minimal necessary C++ payload-relay
change; explicitly excludes multi-intent and multi-turn state, both flagged as separate, larger
features rather than silently folded in.
**Ambiguity check:** "many scenarios" was resolved into 8 concrete, named eval categories (§1,
§7) rather than left as an unbounded goal.
