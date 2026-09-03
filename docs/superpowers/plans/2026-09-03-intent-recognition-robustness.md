# Understanding Tier Robustness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Understanding tier's two hardcoded intent lists with a data-driven Intent
Registry (so any capability's manifest makes it recognizable in natural language with zero Python
edits), add deterministic-first/LLM-fallback slot extraction, a negation veto, and case-insensitive
T3/T4 confirmation — closing the concrete gap where `volume`/`shutdown` are currently unreachable
by natural language at all.

**Architecture:** A new `ai/intent_registry.py` loads `IntentSpec` objects from `config/intents.json`
(built-ins) and every `plugins/*/manifest.json` (extended with optional trigger/slot fields) at
process start. `ai/intent_classifier.py` and `ai/llm_backend.py` become data-agnostic — driven by
the registry instead of hardcoded dicts/prompts. A new `ai/slot_extractor.py` turns natural phrasing
into a clean capability payload, with a code-level (not just documented) refusal to ever do this
for T3/T4 capabilities. A new proto field carries the extracted payload back to C++, which prefers
it over the raw sentence only when non-empty — every existing intent's behavior is unchanged unless
its manifest opts into `slot_rules`.

**Tech Stack:** Python 3 (stdlib `json`/`re`/`dataclasses`, `pytest`), C++17/protobuf/gRPC
(unchanged toolchain), local Ollama `llama3.2:latest` (unchanged).

**Spec:** [`docs/superpowers/specs/2026-09-03-intent-recognition-robustness-design.md`](../specs/2026-09-03-intent-recognition-robustness-design.md)

## Global Constraints

- INV-1: capability argument extraction happens in the Understanding tier, never in a C++ parser.
- INV-2: `proto/ai.proto` changes are append-only — the new field is `string payload = 6;`.
- INV-5: Understanding (Python), not C++, decides what a fuzzy request means, including its
  argument.
- INV-6: a new capability's natural-language reachability requires zero Python edits — only its
  own manifest.
- INV-7: every new failure mode (malformed config, LLM timeout, bad regex) degrades gracefully —
  logs a warning, never crashes the AI server, never blocks.
- INV-8: rules tried first, LLM only on a genuine miss — preserved for intent classification and
  extended identically to slot extraction.
- INV-9: `ConsentGate` remains the only enforcement point. T3/T4 capabilities structurally never
  receive an extracted payload — the full original sentence always reaches the gate. This is a
  code-level short-circuit in `ai/slot_extractor.py`, checked unconditionally, not merely
  documented as a manifest-authoring convention.
- INV-11: still local-only Ollama `llama3.2:latest`, 3.0s timeout — no new network dependency.
- INV-13: `tools/eval_understanding.py`'s expanded, categorized dataset is the measurable evidence
  for this work, not just a claim.
- Backward compatibility (§8 of the spec): every existing intent (STATUS, ECHO, ABOUT,
  SYSTEM_INFO) must keep behaving identically — `pytest ai` must pass with zero changes to
  `ai/test_intent_classifier.py`'s and `ai/test_llm_backend.py`'s *existing* test bodies (only
  new test classes/functions are added to those files, in Tasks 2 and 4).
- Out of scope, explicitly: multi-intent utterances, multi-turn conversational confirmation,
  non-English input, any change to the LLM backend/model.

---

### Task 1: Intent Registry — `ai/intent_registry.py`, `config/intents.json`, plugin manifest data

**Files:**
- Create: `ai/intent_registry.py`
- Create: `config/intents.json`
- Modify: `plugins/system-info/manifest.json`
- Modify: `plugins/system-control/manifest.json`
- Create: `ai/test_intent_registry.py`

**Interfaces:**
- Produces: `@dataclass(frozen=True) class SlotRule: regex: str; payload_template: str` and
  `@dataclass(frozen=True) class IntentSpec: name: str; description: str; power_tier: str;
  trigger_keywords: frozenset[str]; trigger_patterns: tuple[frozenset[str], ...];
  meta_question_immune: bool; slot_rules: tuple[SlotRule, ...]` with property
  `is_destructive_or_external: bool`. `def load_intents(intents_json: Path = DEFAULT_INTENTS_JSON,
  plugins_dir: Path = DEFAULT_PLUGINS_DIR) -> list[IntentSpec]`. `def get_default_intents() ->
  list[IntentSpec]` (memoized). `def reset_default_intents_cache() -> None` (test helper).
  Consumed by Tasks 2, 3, 4, 5.

- [ ] **Step 1: Write `ai/intent_registry.py`**

```python
"""Intent Registry — the single source of truth for what the Understanding tier can recognize.

Loads intent metadata (trigger phrases, slot-extraction rules, descriptions) from two JSON
sources that share the identical schema: config/intents.json (capabilities compiled into the
C++ core — STATUS/ECHO/ABOUT/HELP) and each dynamically-loaded plugin's manifest.json
(plugins/*/manifest.json, read directly from the source tree so `pytest ai` never needs a prior
C++ build to have staged them). A new capability becomes reachable by natural language without
editing any Python file — only its own manifest.

Malformed entries are skipped with a logged warning, never fatal (INV-7) — this module must
never prevent the AI server from starting.
"""

from __future__ import annotations

import json
import logging
import re
from dataclasses import dataclass, field
from pathlib import Path

logger = logging.getLogger("jarvis_ai_server.intent_registry")

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INTENTS_JSON = ROOT / "config" / "intents.json"
DEFAULT_PLUGINS_DIR = ROOT / "plugins"

DESTRUCTIVE_TIERS = {"T3_DESTRUCTIVE", "T4_EXTERNAL"}


@dataclass(frozen=True)
class SlotRule:
    """One deterministic extraction rule: if `regex` matches the input, `payload_template` is
    filled in via str.format with the match's non-None capture groups, in order."""
    regex: str
    payload_template: str


@dataclass(frozen=True)
class IntentSpec:
    """Everything the Understanding tier knows about one intent."""
    name: str  # canonical UPPER_SNAKE form, e.g. "VOLUME", "SYSTEM_INFO"
    description: str
    power_tier: str
    trigger_keywords: frozenset[str] = field(default_factory=frozenset)
    trigger_patterns: tuple[frozenset[str], ...] = field(default_factory=tuple)
    meta_question_immune: bool = False
    slot_rules: tuple[SlotRule, ...] = field(default_factory=tuple)

    @property
    def is_destructive_or_external(self) -> bool:
        return self.power_tier in DESTRUCTIVE_TIERS


def _to_canonical_name(intent: str) -> str:
    """Kebab-case or already-canonical intent name -> UPPER_SNAKE. "system-info" -> "SYSTEM_INFO",
    "volume" -> "VOLUME", "STATUS" -> "STATUS". Mirrors normalizeClassifierIntent() in
    core/jarvis_service.cpp, applied in the opposite direction."""
    return re.sub(r"[-\s]+", "_", intent.strip()).upper()


def _parse_capability_entry(entry: dict, source: str) -> IntentSpec | None:
    """Parses one capability object into an IntentSpec. Returns None (logged) if a required
    field is missing — "intent", "description", "power_tier" are required (an entry cannot
    function without them); everything else is optional."""
    try:
        name = _to_canonical_name(entry["intent"])
        description = entry["description"]
        power_tier = entry["power_tier"]
    except KeyError as exc:
        logger.warning("intent_registry: skipping malformed capability entry in %s: missing %s", source, exc)
        return None

    trigger_keywords = frozenset(str(kw).lower() for kw in entry.get("trigger_keywords", []))

    trigger_patterns = []
    for pattern in entry.get("trigger_patterns", []):
        if not isinstance(pattern, list) or not pattern:
            logger.warning("intent_registry: skipping malformed trigger_pattern in %s for %s", source, name)
            continue
        trigger_patterns.append(frozenset(str(w).lower() for w in pattern))

    slot_rules = []
    for rule in entry.get("slot_rules", []):
        try:
            regex = rule["regex"]
            payload_template = rule["payload_template"]
            re.compile(regex)  # validate now, not at match time
        except (KeyError, re.error) as exc:
            logger.warning("intent_registry: skipping malformed slot_rule in %s for %s: %s", source, name, exc)
            continue
        slot_rules.append(SlotRule(regex=regex, payload_template=payload_template))

    return IntentSpec(
        name=name,
        description=description,
        power_tier=power_tier,
        trigger_keywords=trigger_keywords,
        trigger_patterns=tuple(trigger_patterns),
        meta_question_immune=bool(entry.get("meta_question_immune", False)),
        slot_rules=tuple(slot_rules),
    )


def _load_capabilities_file(path: Path, specs: dict[str, IntentSpec]) -> None:
    """Parses one capabilities-bearing JSON file (config/intents.json or a plugin manifest) into
    `specs`, in place. Missing file / bad JSON / a duplicate intent name are all logged warnings,
    never exceptions."""
    if not path.is_file():
        logger.warning("intent_registry: no file at %s, its intents won't be recognised", path)
        return
    try:
        data = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        logger.warning("intent_registry: %s is not valid JSON: %s", path, exc)
        return
    for entry in data.get("capabilities", []):
        spec = _parse_capability_entry(entry, str(path))
        if spec is None:
            continue
        if spec.name in specs:
            logger.warning("intent_registry: duplicate intent %s in %s, keeping first", spec.name, path)
            continue
        specs[spec.name] = spec


def load_intents(
    intents_json: Path = DEFAULT_INTENTS_JSON,
    plugins_dir: Path = DEFAULT_PLUGINS_DIR,
) -> list[IntentSpec]:
    """Loads every IntentSpec from `intents_json` plus every `<plugins_dir>/*/manifest.json`.
    Duplicate intent names: first-seen wins (intents_json's entries win over plugin manifests,
    and plugins are scanned in sorted directory-name order for determinism)."""
    specs: dict[str, IntentSpec] = {}
    _load_capabilities_file(intents_json, specs)

    if plugins_dir.is_dir():
        for manifest_path in sorted(plugins_dir.glob("*/manifest.json")):
            _load_capabilities_file(manifest_path, specs)
    else:
        logger.warning("intent_registry: no plugins directory at %s, plugin intents won't be recognised", plugins_dir)

    return list(specs.values())


_default_intents_cache: list[IntentSpec] | None = None


def get_default_intents() -> list[IntentSpec]:
    """Lazily loads and caches the real production intent set once per process."""
    global _default_intents_cache
    if _default_intents_cache is None:
        _default_intents_cache = load_intents()
    return _default_intents_cache


def reset_default_intents_cache() -> None:
    """Test helper — clears the memoized default so the next get_default_intents() call
    re-reads from disk."""
    global _default_intents_cache
    _default_intents_cache = None
```

- [ ] **Step 2: Write `config/intents.json`**

```json
{
  "capabilities": [
    {
      "intent": "STATUS",
      "description": "Reports JARVIS's uptime and running state.",
      "power_tier": "T0_READ_ONLY",
      "trigger_keywords": ["status"],
      "trigger_patterns": [["status"], ["uptime"], ["how", "long", "running"], ["alive"]]
    },
    {
      "intent": "ECHO",
      "description": "Repeats the given text back.",
      "power_tier": "T0_READ_ONLY",
      "trigger_keywords": ["echo"],
      "trigger_patterns": [["echo"], ["repeat", "after"], ["say"]]
    },
    {
      "intent": "ABOUT",
      "description": "Describes who or what JARVIS is.",
      "power_tier": "T0_READ_ONLY",
      "trigger_keywords": ["about"],
      "trigger_patterns": [["about"], ["who", "are", "you"]],
      "meta_question_immune": true
    },
    {
      "intent": "HELP",
      "description": "Lists JARVIS's available commands.",
      "power_tier": "T0_READ_ONLY",
      "trigger_keywords": ["help"],
      "trigger_patterns": [["help"], ["what", "can", "you", "do"], ["what", "commands"]]
    }
  ]
}
```

- [ ] **Step 3: Update `plugins/system-info/manifest.json`**

Replace its full contents with:

```json
{
  "id": "system-info",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libsystem_info_plugin.so",
  "capabilities": [
    {
      "intent": "system-info",
      "description": "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
      "power_tier": "T0_READ_ONLY",
      "trigger_patterns": [["system", "info"], ["system", "information"], ["machine", "info"]]
    }
  ]
}
```

- [ ] **Step 4: Update `plugins/system-control/manifest.json`**

Replace its full contents with:

```json
{
  "id": "system-control",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libsystem_control_plugin.so",
  "capabilities": [
    {
      "intent": "volume",
      "description": "Gets or sets system output volume (0-100). Usage: volume get | volume set <0-100>",
      "power_tier": "T2_SYSTEM_AFFECTING",
      "trigger_keywords": ["volume"],
      "trigger_patterns": [["volume"], ["sound"]],
      "slot_rules": [
        {"regex": "\\bget\\b|what.*volume", "payload_template": "get"},
        {"regex": "(\\d{1,3})", "payload_template": "set {0}"}
      ]
    },
    {
      "intent": "shutdown",
      "description": "Powers off the machine. Requires the word 'confirm' in the command every time (T3 — never covered by a grant). Usage: shutdown confirm",
      "power_tier": "T3_DESTRUCTIVE",
      "trigger_keywords": ["shutdown"],
      "trigger_patterns": [["shutdown"], ["shut", "down"], ["power", "off"], ["turn", "off"]]
    }
  ]
}
```

(`shutdown` deliberately gets no `slot_rules` — Task 3's extractor refuses to run for T3/T4
intents unconditionally regardless, but the manifest stays honest about what it does.)

- [ ] **Step 5: Write the failing tests — `ai/test_intent_registry.py`**

```python
"""Unit tests for the Intent Registry loader. Uses isolated tmp_path fixtures, never the real
production config/plugins — so these tests prove the loader's own parsing/merging logic, not
today's production content."""

import json

import pytest

from intent_registry import DESTRUCTIVE_TIERS, IntentSpec, load_intents


def _write_capabilities_file(path, capabilities):
    path.write_text(json.dumps({"capabilities": capabilities}))


class TestLoadIntentsFromIntentsJson:
    def test_valid_entry_parses_correctly(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [
            {
                "intent": "STATUS",
                "description": "Reports uptime.",
                "power_tier": "T0_READ_ONLY",
                "trigger_keywords": ["status"],
                "trigger_patterns": [["status"], ["uptime"]],
            }
        ])
        plugins_dir = tmp_path / "plugins"
        plugins_dir.mkdir()

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert len(specs) == 1
        spec = specs[0]
        assert spec.name == "STATUS"
        assert spec.description == "Reports uptime."
        assert spec.power_tier == "T0_READ_ONLY"
        assert spec.trigger_keywords == frozenset({"status"})
        assert frozenset({"status"}) in spec.trigger_patterns
        assert frozenset({"uptime"}) in spec.trigger_patterns
        assert spec.meta_question_immune is False
        assert spec.slot_rules == ()

    def test_missing_intents_json_yields_no_builtin_intents(self, tmp_path):
        plugins_dir = tmp_path / "plugins"
        plugins_dir.mkdir()

        specs = load_intents(intents_json=tmp_path / "does_not_exist.json", plugins_dir=plugins_dir)

        assert specs == []

    def test_malformed_json_is_skipped_not_fatal(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        intents_json.write_text("{not valid json")
        plugins_dir = tmp_path / "plugins"
        plugins_dir.mkdir()

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert specs == []

    def test_entry_missing_required_field_is_skipped(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [
            {"intent": "STATUS", "power_tier": "T0_READ_ONLY"},  # missing "description"
        ])
        plugins_dir = tmp_path / "plugins"
        plugins_dir.mkdir()

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert specs == []


class TestLoadIntentsFromPlugins:
    def test_kebab_case_intent_name_becomes_upper_snake(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [])
        plugins_dir = tmp_path / "plugins"
        (plugins_dir / "system-info").mkdir(parents=True)
        _write_capabilities_file(
            plugins_dir / "system-info" / "manifest.json",
            [{"intent": "system-info", "description": "d", "power_tier": "T0_READ_ONLY"}],
        )

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert specs[0].name == "SYSTEM_INFO"

    def test_slot_rules_parse_correctly(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [])
        plugins_dir = tmp_path / "plugins"
        (plugins_dir / "system-control").mkdir(parents=True)
        _write_capabilities_file(
            plugins_dir / "system-control" / "manifest.json",
            [{
                "intent": "volume",
                "description": "d",
                "power_tier": "T2_SYSTEM_AFFECTING",
                "slot_rules": [{"regex": r"(\d+)", "payload_template": "set {0}"}],
            }],
        )

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert len(specs[0].slot_rules) == 1
        assert specs[0].slot_rules[0].regex == r"(\d+)"
        assert specs[0].slot_rules[0].payload_template == "set {0}"

    def test_bad_regex_in_slot_rule_is_skipped_not_fatal(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [])
        plugins_dir = tmp_path / "plugins"
        (plugins_dir / "broken") .mkdir(parents=True)
        _write_capabilities_file(
            plugins_dir / "broken" / "manifest.json",
            [{
                "intent": "broken",
                "description": "d",
                "power_tier": "T2_SYSTEM_AFFECTING",
                "slot_rules": [{"regex": "(unclosed", "payload_template": "x"}],
            }],
        )

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert specs[0].slot_rules == ()

    def test_missing_plugins_dir_yields_only_builtins(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [
            {"intent": "STATUS", "description": "d", "power_tier": "T0_READ_ONLY"},
        ])

        specs = load_intents(intents_json=intents_json, plugins_dir=tmp_path / "does_not_exist")

        assert len(specs) == 1
        assert specs[0].name == "STATUS"

    def test_duplicate_intent_name_first_seen_wins(self, tmp_path):
        intents_json = tmp_path / "intents.json"
        _write_capabilities_file(intents_json, [
            {"intent": "STATUS", "description": "builtin version", "power_tier": "T0_READ_ONLY"},
        ])
        plugins_dir = tmp_path / "plugins"
        (plugins_dir / "impostor").mkdir(parents=True)
        _write_capabilities_file(
            plugins_dir / "impostor" / "manifest.json",
            [{"intent": "status", "description": "plugin version", "power_tier": "T0_READ_ONLY"}],
        )

        specs = load_intents(intents_json=intents_json, plugins_dir=plugins_dir)

        assert len(specs) == 1
        assert specs[0].description == "builtin version"


class TestIsDestructiveOrExternal:
    @pytest.mark.parametrize("tier,expected", [
        ("T0_READ_ONLY", False),
        ("T1_STATEFUL_LOCAL", False),
        ("T2_SYSTEM_AFFECTING", False),
        ("T3_DESTRUCTIVE", True),
        ("T4_EXTERNAL", True),
    ])
    def test_tier_classification(self, tier, expected):
        spec = IntentSpec(name="X", description="d", power_tier=tier)
        assert spec.is_destructive_or_external is expected

    def test_destructive_tiers_constant_matches_property(self):
        assert DESTRUCTIVE_TIERS == {"T3_DESTRUCTIVE", "T4_EXTERNAL"}
```

- [ ] **Step 6: Run the tests**

Run: `cd "$(git rev-parse --show-toplevel)" && PYTHONPATH=ai .venv/bin/python -m pytest ai/test_intent_registry.py -v`
Expected: PASS, all 12 cases.

- [ ] **Step 7: Verify against the real production data**

Run: `PYTHONPATH=ai .venv/bin/python -c "from intent_registry import load_intents; specs = load_intents(); print(sorted(s.name for s in specs))"`
Expected output: `['ABOUT', 'ECHO', 'HELP', 'SHUTDOWN', 'STATUS', 'SYSTEM_INFO', 'VOLUME']` — all 4
built-ins plus all 3 plugin capabilities, proving Steps 2-4's real files parse correctly together.

- [ ] **Step 8: Commit**

```bash
git add ai/intent_registry.py ai/test_intent_registry.py config/intents.json plugins/system-info/manifest.json plugins/system-control/manifest.json
git commit -m "feat(ai): add data-driven Intent Registry (config/intents.json + plugin manifests)"
```

---

### Task 2: `ai/intent_classifier.py` — registry-driven, negation veto

**Files:**
- Modify: `ai/intent_classifier.py` (full rewrite)
- Modify: `ai/test_intent_classifier.py` (append new test classes only — do not change any
  existing test)

**Interfaces:**
- Consumes: `IntentSpec`, `get_default_intents()` from Task 1.
- Produces: `def classify(text: str, intents: list[IntentSpec] | None = None) -> tuple[str,
  float]` — same public name/return shape as before; `intents=None` defaults to
  `get_default_intents()`, so every existing caller (`resolver.py`, `eval_understanding.py`,
  every existing test) keeps working with zero call-site changes. Consumed by Tasks 4, 5, 8.

- [ ] **Step 1: Replace `ai/intent_classifier.py`'s full contents**

```python
"""Deterministic rule-based intent classifier — the fast path of the Understanding tier.

Intent data itself (trigger keywords/patterns, whether a pattern is meta-question-immune) now
lives in the Intent Registry (intent_registry.py), loaded from config/intents.json and each
plugin's manifest.json — this module is purely the matching algorithm, data-agnostic, so a new
capability becomes recognisable without editing this file at all.
"""

from __future__ import annotations

import difflib
import re

from intent_registry import IntentSpec, get_default_intents

CONFIDENCE_THRESHOLD = 0.5

FUZZY_MATCH_CUTOFF = 0.7  # verified empirically: catches real typos (staus, ecoh, abuot),
# rejects unrelated short words (sat, at, is, who, what, how) that would otherwise misfire.
MIN_FUZZY_WORD_LENGTH = 3
FUZZY_MATCH_CONFIDENCE = 0.8  # confident but intentionally below the 1.0 of an exact match

# Words that signal the sentence is *asking about* a command rather than invoking it —
# e.g. "what does the echo command do" must not fire ECHO's bare {"echo"} pattern. An intent
# marked meta_question_immune in its IntentSpec (e.g. ABOUT) is exempt: asking about something
# IS what that intent means, so its own bare match is never ambiguous the way an actionable
# intent's bare mention is.
META_QUESTION_WORDS = {"does", "command", "explain", "mean", "meaning"}

# Phrases that signal explicit refusal/cancellation — "don't shut down", "cancel that", "never
# mind the volume" must not fire the intent they'd otherwise match. Checked against the raw
# lowercased text (not the token set) so contractions like "don't"/"won't"/"can't" are caught
# correctly regardless of how the tokenizer splits the apostrophe. Deliberately intent-agnostic
# (not just T3/T4-scoped): a false match here is always worse than falling through to UNKNOWN,
# which just escalates to the LLM tier (INV-8) instead of confidently doing the wrong thing.
_NEGATION_RE = re.compile(
    r"\b(don'?t|doesn'?t|won'?t|can'?t|cannot|never ?mind|never|stop|cancel|do not|does not)\b"
)

_TOKEN_RE = re.compile(r"[a-z0-9]+")


def preprocess(text: str) -> set[str]:
    """Lowercase, strip punctuation, split on whitespace into a set of tokens."""
    return set(_TOKEN_RE.findall(text.lower()))


def classify(text: str, intents: list[IntentSpec] | None = None) -> tuple[str, float]:
    """Map free-form text to (intent, confidence); ("UNKNOWN", 0.0) at or below threshold.

    Two passes. First, a first-word fast path: if the sentence starts with a command keyword
    (or a likely typo of one, from any intent's trigger_keywords), route instantly — no
    ambiguity possible. Otherwise, fall back to phrase-pattern matching for natural-language
    phrasing that doesn't start with the command word itself (e.g. "are you alive", "how long
    have you been running"). An intent's score there is its best-matching pattern, where a
    pattern scores len(pattern & tokens) / len(pattern); a bare single-word pattern is skipped
    when a meta-question word is present, unless the intent is meta_question_immune. A sentence
    containing a negation/cancellation phrase never matches any pattern at all (falls through to
    UNKNOWN, which escalates to the LLM tier — a false match is worse than a missed one). Ties
    resolve to declaration order. The threshold is exclusive so a two-word pattern needs both
    words, not one generic one — the rule path stays high-precision and defers anything
    uncertain to UNKNOWN.

    `intents` defaults to the real production Intent Registry (config/intents.json + every
    plugins/*/manifest.json) when not given explicitly — tests that need an isolated fixture
    set pass their own list.
    """
    if intents is None:
        intents = get_default_intents()

    words = _TOKEN_RE.findall(text.lower())
    if not words:
        return "UNKNOWN", 0.0

    first_word = words[0]
    keyword_to_intent = {kw: spec.name for spec in intents for kw in spec.trigger_keywords}

    if first_word in keyword_to_intent:
        return keyword_to_intent[first_word], 1.0

    if len(first_word) >= MIN_FUZZY_WORD_LENGTH:
        correction = difflib.get_close_matches(
            first_word, keyword_to_intent.keys(), n=1, cutoff=FUZZY_MATCH_CUTOFF
        )
        if correction:
            return keyword_to_intent[correction[0]], FUZZY_MATCH_CONFIDENCE

    if _NEGATION_RE.search(text.lower()):
        return "UNKNOWN", 0.0

    tokens = set(words)
    is_meta_question = bool(META_QUESTION_WORDS & tokens)

    best_intent = "UNKNOWN"
    best_score = 0.0
    for spec in intents:
        score = max(
            (
                len(pattern & tokens) / len(pattern)
                for pattern in spec.trigger_patterns
                if not (is_meta_question and len(pattern) == 1 and not spec.meta_question_immune)
            ),
            default=0.0,
        )
        if score > best_score:
            best_intent = spec.name
            best_score = score

    if best_score > CONFIDENCE_THRESHOLD:
        return best_intent, best_score
    return "UNKNOWN", 0.0
```

- [ ] **Step 2: Run the full existing suite to confirm zero regressions**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_intent_classifier.py -v`
Expected: PASS — all pre-existing test classes (`TestPreprocess`, `TestClassifyPositive`,
`TestClassifyUnknown`, `TestAddressTermDoesNotSkewClassification`, `TestConfidence`,
`TestFirstWordFastPath`, `TestFirstWordTypoCorrection`, `TestMetaQuestionVeto`) pass unchanged,
proving `config/intents.json` (Task 1) is a faithful migration of the old hardcoded data.

- [ ] **Step 3: Append new test classes to `ai/test_intent_classifier.py`**

Add at the end of the file:

```python
class TestNegationVeto:
    """A false match on a destructive/actionable intent is worse than falling through to
    UNKNOWN (which just escalates to the LLM tier) — see _NEGATION_RE."""

    @pytest.mark.parametrize(
        "text",
        [
            "don't shut down the computer",
            "do not shut down",
            "please cancel the shutdown",
            "never mind the volume",
            "stop, don't echo that",
            "won't you tell me the status",
        ],
    )
    def test_negated_request_is_unknown(self, text):
        assert classify(text) == ("UNKNOWN", 0.0)

    def test_non_negated_request_still_fires(self):
        # Regression guard: the negation check must not somehow suppress ordinary requests.
        assert classify("shut down the computer") == ("SHUTDOWN", pytest.approx(1.0))


class TestMetaQuestionImmuneIsDataDriven:
    """The exemption from the meta-question veto now comes from IntentSpec.meta_question_immune
    (set for ABOUT in config/intents.json) rather than a hardcoded intent-name check."""

    def test_non_immune_intent_is_suppressed_by_meta_question(self):
        assert classify("what does volume mean") == ("UNKNOWN", 0.0)

    def test_immune_intent_still_fires_under_meta_question(self):
        intent, confidence = classify("what does about mean")
        assert intent == "ABOUT"
        assert confidence == pytest.approx(1.0)


class TestNewIntentsAreReachable:
    """VOLUME/SHUTDOWN (system-control plugin) and HELP (built-in) were unreachable by natural
    language before this task — this is the regression test proving the concrete gap is closed."""

    @pytest.mark.parametrize(
        "text,expected",
        [
            ("volume", "VOLUME"),
            ("turn the volume up", "VOLUME"),
            ("what's the sound level", "VOLUME"),
            ("shutdown", "SHUTDOWN"),
            ("shut down the machine", "SHUTDOWN"),
            ("power off", "SHUTDOWN"),
            ("help", "HELP"),
            ("what can you do", "HELP"),
        ],
    )
    def test_previously_unreachable_intents_now_match(self, text, expected):
        intent, confidence = classify(text)
        assert intent == expected
        assert confidence > CONFIDENCE_THRESHOLD
```

Add `from intent_classifier import CONFIDENCE_THRESHOLD, classify, preprocess` already exists at
the top of the file (unchanged) — no new import needed since `CONFIDENCE_THRESHOLD` is already
imported there.

- [ ] **Step 4: Run the full suite again**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_intent_classifier.py -v`
Expected: PASS, all cases including the new ones (roughly 20 existing + 16 new = 36 total).

- [ ] **Step 5: Commit**

```bash
git add ai/intent_classifier.py ai/test_intent_classifier.py
git commit -m "feat(ai): make intent_classifier registry-driven, add negation veto"
```

---

### Task 3: `ai/slot_extractor.py` — deterministic argument extraction

**Files:**
- Create: `ai/slot_extractor.py`
- Create: `ai/test_slot_extractor.py`

**Interfaces:**
- Consumes: `IntentSpec`, `SlotRule` from Task 1.
- Produces: `def extract(intent_name: str, text: str, intents: list[IntentSpec]) -> str | None`.
  Consumed by Task 5.

- [ ] **Step 1: Write the failing tests — `ai/test_slot_extractor.py`**

```python
"""Unit tests for deterministic slot extraction. Uses hand-built IntentSpec fixtures, not the
real production registry, so these tests are about the extraction algorithm, not today's actual
slot_rules content."""

from intent_registry import IntentSpec, SlotRule
from slot_extractor import extract


def _volume_spec():
    return IntentSpec(
        name="VOLUME",
        description="d",
        power_tier="T2_SYSTEM_AFFECTING",
        slot_rules=(
            SlotRule(regex=r"\bget\b|what.*volume", payload_template="get"),
            SlotRule(regex=r"(\d{1,3})", payload_template="set {0}"),
        ),
    )


def _shutdown_spec_with_accidental_slot_rules():
    # A plugin author mistakenly declaring slot_rules on a T3 capability — the extractor must
    # refuse regardless (INV-9: this is a code-level short-circuit, not authoring discipline).
    return IntentSpec(
        name="SHUTDOWN",
        description="d",
        power_tier="T3_DESTRUCTIVE",
        slot_rules=(SlotRule(regex=r"(\d+)", payload_template="set {0}"),),
    )


def _echo_spec_no_rules():
    return IntentSpec(name="ECHO", description="d", power_tier="T0_READ_ONLY")


class TestDeterministicExtraction:
    def test_get_pattern_matches(self):
        intents = [_volume_spec()]
        assert extract("VOLUME", "what's the volume", intents) == "get"

    def test_numeric_pattern_matches_and_fills_template(self):
        intents = [_volume_spec()]
        assert extract("VOLUME", "turn it up to 50", intents) == "set 50"

    def test_rules_tried_in_declaration_order_first_match_wins(self):
        intents = [_volume_spec()]
        # Contains both "get" and a number — the "get" rule is declared first and must win.
        assert extract("VOLUME", "get the volume, is it 50 percent?", intents) == "get"

    def test_no_rule_matches_returns_none(self):
        intents = [_volume_spec()]
        assert extract("VOLUME", "turn it up please", intents) is None

    def test_unknown_intent_name_returns_none(self):
        intents = [_volume_spec()]
        assert extract("NOT_REGISTERED", "anything", intents) is None

    def test_intent_with_no_slot_rules_returns_none(self):
        intents = [_echo_spec_no_rules()]
        assert extract("ECHO", "echo this back", intents) is None


class TestDestructiveTierShortCircuit:
    """The actual proof of INV-9's payload-preservation guarantee: even a manifest that
    mistakenly declares slot_rules on a T3/T4 capability must never have them applied."""

    def test_t3_intent_never_extracts_even_with_slot_rules_declared(self):
        intents = [_shutdown_spec_with_accidental_slot_rules()]
        assert extract("SHUTDOWN", "shutdown 5", intents) is None

    def test_t4_intent_never_extracts_even_with_slot_rules_declared(self):
        spec = IntentSpec(
            name="CALL_API", description="d", power_tier="T4_EXTERNAL",
            slot_rules=(SlotRule(regex=r"(\d+)", payload_template="set {0}"),),
        )
        assert extract("CALL_API", "call api 5", [spec]) is None
```

- [ ] **Step 2: Run to verify it fails**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_slot_extractor.py -v`
Expected: FAIL — `slot_extractor` module does not exist yet.

- [ ] **Step 3: Write `ai/slot_extractor.py`**

```python
"""Deterministic slot/argument extraction — tries a matching intent's declared slot_rules in
declaration order. The LLM fallback (llm_backend.llm_extract_slot) is only reached by
resolver.py when this returns None for an intent that DOES declare slot_rules (meaning it
legitimately expects an argument the deterministic patterns didn't catch).
"""

from __future__ import annotations

import re

from intent_registry import IntentSpec


def extract(intent_name: str, text: str, intents: list[IntentSpec]) -> str | None:
    """Returns the extracted payload for `intent_name` given the raw `text`, or None if nothing
    was extracted (no matching rule, unknown intent, or the intent has none declared).

    T3_DESTRUCTIVE/T4_EXTERNAL intents ALWAYS return None here, unconditionally — the full
    original sentence must reach ConsentGate unmodified so its confirm-token check stays
    reliable. This is enforced in code, not left to manifest-authoring discipline (INV-9: a
    capability/tier must not invent or bypass its own guardrail; this module is Understanding-
    tier code, so it holds itself to the same rule).
    """
    spec = next((s for s in intents if s.name == intent_name), None)
    if spec is None or spec.is_destructive_or_external:
        return None

    for rule in spec.slot_rules:
        match = re.search(rule.regex, text, re.IGNORECASE)
        if match is None:
            continue
        groups = [g for g in match.groups() if g is not None]
        return rule.payload_template.format(*groups)

    return None
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_slot_extractor.py -v`
Expected: PASS, all 8 cases.

- [ ] **Step 5: Commit**

```bash
git add ai/slot_extractor.py ai/test_slot_extractor.py
git commit -m "feat(ai): add deterministic slot extraction with a T3/T4 structural refusal"
```

---

### Task 4: `ai/llm_backend.py` — dynamic prompt, `llm_extract_slot`

**Files:**
- Modify: `ai/llm_backend.py` (full rewrite)
- Modify: `ai/test_llm_backend.py` (append new test classes only — do not change any existing
  test)

**Interfaces:**
- Consumes: `IntentSpec`, `get_default_intents()` from Task 1.
- Produces: `def llm_classify(text: str, intents: list[IntentSpec] | None = None) -> tuple[str,
  float]` (same name/shape as before, `intents=None` defaults to `get_default_intents()`).
  `def llm_extract_slot(text: str, intent_spec: IntentSpec) -> str | None`. Both consumed by
  Task 5.

- [ ] **Step 1: Replace `ai/llm_backend.py`'s full contents**

```python
"""LLM-backed intent classification and slot extraction — the escalation tier of the
Understanding layer.

`llm_classify` is consulted only when the rule-based classifier can't match the input.
`llm_extract_slot` is consulted only when a resolved intent declares slot_rules but the
deterministic extractor (slot_extractor.py) found nothing. Both call a local Ollama server; any
failure (unreachable, slow, malformed reply) degrades to a safe empty result rather than
raising — a dead/slow LLM must never block the caller (INV-7).
"""

from __future__ import annotations

import json
import logging
import time
import urllib.error
import urllib.request

from intent_registry import IntentSpec, get_default_intents

logger = logging.getLogger("jarvis_ai_server.llm_backend")

OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "llama3.2:latest"
TIMEOUT_SECONDS = 3.0


def build_prompt(text: str, intents: list[IntentSpec]) -> str:
    """Builds the classification prompt from the live Intent Registry instead of a hardcoded
    template — a new capability's manifest.json description is all that's needed for the LLM
    tier to learn about it too."""
    intent_lines = "\n".join(f"- {spec.name}: {spec.description}" for spec in intents)
    intent_names = " | ".join(f'"{spec.name}"' for spec in intents)
    return (
        "You are an intent classifier for a personal assistant called JARVIS. "
        "JARVIS currently understands exactly these commands:\n\n"
        f"{intent_lines}\n\n"
        "If the user is asking a question about one of these commands (e.g. what it does, how "
        "it works) rather than actually using it, that is NOT a match for that command — "
        "respond UNKNOWN instead. If the user is explicitly refusing or cancelling an action "
        "(e.g. \"don't shut down\", \"cancel that\"), that is NOT a match either — respond "
        "UNKNOWN.\n\n"
        "Given the user's message below, decide which single intent it matches, or UNKNOWN if "
        "it matches none of them. Respond with ONLY a JSON object of the exact form:\n"
        '{"intent": ' + intent_names + ' | "UNKNOWN", "confidence": <number between 0.0 and 1.0>}\n\n'
        f"User message: {text}\n"
    )


def _post_ollama(prompt: str) -> dict | None:
    """Shared HTTP call for both llm_classify and llm_extract_slot. Returns the parsed
    inner `response` JSON dict, or None on any failure (already logged)."""
    payload = json.dumps({
        "model": MODEL,
        "prompt": prompt,
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
        return json.loads(outer["response"])
    except (urllib.error.URLError, TimeoutError, OSError, KeyError, ValueError, TypeError,
            json.JSONDecodeError) as exc:
        logger.warning("Ollama call failed: %s: %s", type(exc).__name__, exc)
        return None


def llm_classify(text: str, intents: list[IntentSpec] | None = None) -> tuple[str, float]:
    """Ask the local Ollama model to classify `text`. Returns ("UNKNOWN", 0.0) on any failure.

    `intents` defaults to the real production Intent Registry when not given explicitly."""
    if intents is None:
        intents = get_default_intents()
    known_intent_names = {spec.name for spec in intents}

    start = time.monotonic()
    parsed = _post_ollama(build_prompt(text, intents))
    latency_ms = (time.monotonic() - start) * 1000

    if parsed is None:
        logger.warning("llm_classify failed latency_ms=%.2f", latency_ms)
        return "UNKNOWN", 0.0

    try:
        intent = parsed["intent"]
        confidence = float(parsed["confidence"])
    except (KeyError, ValueError, TypeError) as exc:
        logger.warning("llm_classify got a malformed reply: %s latency_ms=%.2f", exc, latency_ms)
        return "UNKNOWN", 0.0

    if intent == "UNKNOWN":
        logger.info("llm_classify intent=UNKNOWN latency_ms=%.2f", latency_ms)
        return "UNKNOWN", 0.0

    if intent not in known_intent_names:
        logger.warning(
            "llm_classify returned unrecognised intent=%r latency_ms=%.2f", intent, latency_ms,
        )
        return "UNKNOWN", 0.0

    confidence = max(0.0, min(1.0, confidence))
    logger.info(
        "llm_classify intent=%s confidence=%.2f latency_ms=%.2f", intent, confidence, latency_ms,
    )
    return intent, confidence


def llm_extract_slot(text: str, intent_spec: IntentSpec) -> str | None:
    """Asks the LLM to extract the argument value for `intent_spec` from `text`, when the
    deterministic slot_extractor found nothing. Same failure discipline as llm_classify: any
    error degrades to None, never raises, never blocks."""
    prompt = (
        f"You are extracting a single argument value for the '{intent_spec.name}' command of "
        "a personal assistant called JARVIS. The command's description is:\n"
        f"{intent_spec.description}\n\n"
        "Extract the value being requested from the user's message below (e.g. a number, a "
        "word, whatever the command needs), or null if no clear value is present. Respond with "
        'ONLY a JSON object of the exact form: {"value": <string> | null}\n\n'
        f"User message: {text}\n"
    )

    start = time.monotonic()
    parsed = _post_ollama(prompt)
    latency_ms = (time.monotonic() - start) * 1000

    if parsed is None:
        logger.warning("llm_extract_slot failed intent=%s latency_ms=%.2f", intent_spec.name, latency_ms)
        return None

    try:
        value = parsed["value"]
    except KeyError:
        logger.warning("llm_extract_slot got a malformed reply intent=%s latency_ms=%.2f", intent_spec.name, latency_ms)
        return None

    if value is None:
        logger.info("llm_extract_slot intent=%s value=None latency_ms=%.2f", intent_spec.name, latency_ms)
        return None

    logger.info("llm_extract_slot intent=%s value=%r latency_ms=%.2f", intent_spec.name, value, latency_ms)
    return str(value)
```

- [ ] **Step 2: Run the full existing suite to confirm zero regressions**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_llm_backend.py -v`
Expected: PASS — all pre-existing tests pass unchanged. They mock `llm_backend.urllib.request.urlopen`
directly (not `_post_ollama`), so the refactor into a shared `_post_ollama` helper is transparent
to them; none of them assert on the exact prompt text sent, only on the parsed return value.

- [ ] **Step 3: Append new test classes to `ai/test_llm_backend.py`**

Add near the top (after the existing `_fake_response` helper) and at the end of the file:

```python
from intent_registry import IntentSpec

from llm_backend import llm_extract_slot
```

```python
class TestBuildPromptIsDataDriven:
    def test_prompt_lists_every_intent_description(self):
        from llm_backend import build_prompt
        intents = [
            IntentSpec(name="STATUS", description="Reports uptime.", power_tier="T0_READ_ONLY"),
            IntentSpec(name="VOLUME", description="Controls volume.", power_tier="T2_SYSTEM_AFFECTING"),
        ]
        prompt = build_prompt("hello", intents)
        assert "STATUS: Reports uptime." in prompt
        assert "VOLUME: Controls volume." in prompt
        assert '"STATUS"' in prompt
        assert '"VOLUME"' in prompt


class TestLLMExtractSlotSuccess:
    @patch("llm_backend.urllib.request.urlopen")
    def test_valid_value_is_extracted(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response({"response": json.dumps({"value": "50"})})
        spec = IntentSpec(name="VOLUME", description="d", power_tier="T2_SYSTEM_AFFECTING")
        assert llm_extract_slot("turn it up to fifty", spec) == "50"

    @patch("llm_backend.urllib.request.urlopen")
    def test_null_value_returns_none(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response({"response": json.dumps({"value": None})})
        spec = IntentSpec(name="VOLUME", description="d", power_tier="T2_SYSTEM_AFFECTING")
        assert llm_extract_slot("turn it up", spec) is None


class TestLLMExtractSlotDegradesGracefully:
    @patch("llm_backend.urllib.request.urlopen")
    def test_connection_error_returns_none(self, mock_urlopen):
        mock_urlopen.side_effect = urllib.error.URLError("connection refused")
        spec = IntentSpec(name="VOLUME", description="d", power_tier="T2_SYSTEM_AFFECTING")
        assert llm_extract_slot("anything", spec) is None

    @patch("llm_backend.urllib.request.urlopen")
    def test_malformed_reply_returns_none(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response({"response": "not valid json"})
        spec = IntentSpec(name="VOLUME", description="d", power_tier="T2_SYSTEM_AFFECTING")
        assert llm_extract_slot("anything", spec) is None
```

- [ ] **Step 4: Run the full suite again**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_llm_backend.py -v`
Expected: PASS, all cases including the new ones.

- [ ] **Step 5: Commit**

```bash
git add ai/llm_backend.py ai/test_llm_backend.py
git commit -m "feat(ai): build LLM prompt from the Intent Registry, add llm_extract_slot"
```

---

### Task 5: `ai/resolver.py` — extend to `(intent, confidence, tier, payload)`

**Files:**
- Modify: `ai/resolver.py` (full rewrite)
- Modify: `ai/test_resolver.py` (full rewrite — return-tuple shape changes)

**Interfaces:**
- Consumes: `classify()` (Task 2), `llm_classify()`/`llm_extract_slot()` (Task 4),
  `extract()` (Task 3), `get_default_intents()` (Task 1).
- Produces: `def resolve(text: str) -> tuple[str, float, str, str]`. Consumed by Task 6
  (`ai/jarvis_ai_server.py`).

- [ ] **Step 1: Replace `ai/resolver.py`'s full contents**

```python
"""The tiered Understanding-layer resolver — the single entry point jarvis_ai_server.py calls.

Tries the deterministic rule classifier first (fast, free, high-precision); only escalates to
the local LLM when the rules draw a blank. Once an intent is resolved (by either tier), attempts
to extract a clean capability argument (payload) the same way: deterministic slot_rules first,
LLM extraction only if the intent legitimately declares rules but none matched. Keeping this as
the one fixed entry point is what lets a future third tier slot in later without touching the
gRPC server or anything on the C++ side.
"""

from __future__ import annotations

from intent_classifier import CONFIDENCE_THRESHOLD, classify
from intent_registry import get_default_intents
from llm_backend import llm_classify, llm_extract_slot
from slot_extractor import extract as extract_slot


def _extract_payload(intent: str, text: str) -> str:
    """Best-effort payload extraction for a resolved intent. Returns "" (never None) when
    nothing was extracted, so the caller can always forward it straight into the proto's payload
    field — an empty string there means "C++, fall back to the original request payload"."""
    intents = get_default_intents()
    payload = extract_slot(intent, text, intents)
    if payload:
        return payload

    spec = next((s for s in intents if s.name == intent), None)
    if spec is not None and spec.slot_rules:
        llm_payload = llm_extract_slot(text, spec)
        if llm_payload:
            return llm_payload

    return ""


def resolve(text: str) -> tuple[str, float, str, str]:
    """Classify `text`. Returns (intent, confidence, tier, payload) — tier is "rule", "llm", or
    "none"; payload is "" when nothing was extracted."""
    intent, confidence = classify(text)
    if intent != "UNKNOWN":
        return intent, confidence, "rule", _extract_payload(intent, text)

    intent, confidence = llm_classify(text)
    if intent != "UNKNOWN" and confidence > CONFIDENCE_THRESHOLD:
        return intent, confidence, "llm", _extract_payload(intent, text)

    return "UNKNOWN", 0.0, "none", ""
```

- [ ] **Step 2: Replace `ai/test_resolver.py`'s full contents**

```python
"""Unit tests for the tiered resolver. The LLM tier (both llm_classify and llm_extract_slot) is
always mocked here — these tests prove the *escalation and payload-extraction logic*, not the
LLM's own behaviour (that's test_llm_backend.py's job)."""

from unittest.mock import patch

from resolver import resolve


class TestResolvePrefersRules:
    @patch("resolver.llm_classify")
    def test_rule_hit_never_calls_llm(self, mock_llm):
        assert resolve("status") == ("STATUS", 1.0, "rule", "")
        mock_llm.assert_not_called()

    @patch("resolver.llm_classify")
    def test_rule_hit_echo_never_calls_llm(self, mock_llm):
        assert resolve("say hello") == ("ECHO", 1.0, "rule", "")
        mock_llm.assert_not_called()


class TestResolveEscalatesToLLM:
    @patch("resolver.llm_classify")
    def test_rule_miss_llm_hit(self, mock_llm):
        mock_llm.return_value = ("STATUS", 0.75)
        assert resolve("hows it going") == ("STATUS", 0.75, "llm", "")
        mock_llm.assert_called_once_with("hows it going")

    @patch("resolver.llm_classify")
    def test_rule_miss_llm_also_miss(self, mock_llm):
        mock_llm.return_value = ("UNKNOWN", 0.0)
        assert resolve("the weather today") == ("UNKNOWN", 0.0, "none", "")

    @patch("resolver.llm_classify")
    def test_rule_miss_llm_low_confidence_rejected(self, mock_llm):
        """A non-UNKNOWN LLM guess below the confidence threshold must not pass through as
        tier="llm" — the wire-level invariant "intent != UNKNOWN implies confidence > 0.5" must
        hold for the LLM tier too, not just the rule tier."""
        mock_llm.return_value = ("STATUS", 0.3)
        assert resolve("hows it going") == ("UNKNOWN", 0.0, "none", "")


class TestResolvePayloadExtraction:
    def test_deterministic_extraction_populates_payload(self):
        # "VOLUME" resolves via the rule tier (real production registry, no mocking needed) and
        # its numeric slot_rule matches "50" directly — no LLM call needed for either step.
        with patch("resolver.llm_classify") as mock_classify_llm, \
             patch("resolver.llm_extract_slot") as mock_extract_llm:
            intent, confidence, tier, payload = resolve("set the volume to 50")
            assert intent == "VOLUME"
            assert tier == "rule"
            assert payload == "set 50"
            mock_classify_llm.assert_not_called()
            mock_extract_llm.assert_not_called()

    def test_intent_with_no_slot_rules_gets_empty_payload_no_llm_call(self):
        with patch("resolver.llm_extract_slot") as mock_extract_llm:
            intent, confidence, tier, payload = resolve("status")
            assert intent == "STATUS"
            assert payload == ""
            mock_extract_llm.assert_not_called()

    @patch("resolver.llm_extract_slot")
    def test_deterministic_miss_falls_back_to_llm_extraction(self, mock_extract_llm):
        mock_extract_llm.return_value = "50"
        # "fifty" isn't numeric, so the deterministic \d{1,3} rule can't extract it — this
        # exercises the LLM slot-extraction fallback path.
        intent, confidence, tier, payload = resolve("turn the volume up to fifty")
        assert intent == "VOLUME"
        assert payload == "50"
        mock_extract_llm.assert_called_once()

    @patch("resolver.llm_extract_slot")
    def test_llm_extraction_also_failing_yields_empty_payload(self, mock_extract_llm):
        mock_extract_llm.return_value = None
        intent, confidence, tier, payload = resolve("turn the volume up please")
        assert intent == "VOLUME"
        assert payload == ""

    def test_t3_intent_payload_is_always_the_original_full_sentence_via_empty_extraction(self):
        # SHUTDOWN never gets slot_rules (Task 1's manifest) and slot_extractor refuses T3/T4
        # unconditionally regardless (Task 3) — payload stays "", so jarvis_service.cpp (Task 6)
        # falls back to the ORIGINAL request payload, preserving the confirm-token contract.
        with patch("resolver.llm_extract_slot") as mock_extract_llm:
            intent, confidence, tier, payload = resolve("shutdown please, i confirm")
            assert intent == "SHUTDOWN"
            assert payload == ""
            mock_extract_llm.assert_not_called()
```

- [ ] **Step 3: Run the tests**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/test_resolver.py -v`
Expected: PASS, all 10 cases.

- [ ] **Step 4: Run the full `ai/` suite to confirm no cross-file regressions**

Run: `PYTHONPATH=ai .venv/bin/python -m pytest ai/ -v`
Expected: PASS, everything from Tasks 1-5 combined.

- [ ] **Step 5: Commit**

```bash
git add ai/resolver.py ai/test_resolver.py
git commit -m "feat(ai): resolver returns extracted payload as a 4th tuple element"
```

---

### Task 6: Proto field + C++ payload relay + AI server wiring

**Files:**
- Modify: `proto/ai.proto`
- Regenerate: `generated/cpp/ai.pb.{h,cc}`, `generated/cpp/ai.grpc.pb.{h,cc}`,
  `generated/python/ai_pb2.py`, `generated/python/ai_pb2_grpc.py`
- Modify: `core/ai_client.h`
- Modify: `core/ai_client.cpp`
- Modify: `core/jarvis_service.cpp`
- Modify: `ai/jarvis_ai_server.py`
- Modify: `ai/test_ai_server.py` (append new tests only)
- Modify: `tools/grpc_smoke_test.py` (append new calls only)

**Interfaces:**
- Consumes: `resolve()`'s 4-tuple return (Task 5).
- Produces: `AIResult.payload` (C++), `NaturalLanguageResponse.payload` (proto, field 6).

- [ ] **Step 1: Add the new field to `proto/ai.proto`**

Change:

```proto
message NaturalLanguageResponse {
    bool success = 1;
    string reply = 2;   // The AI's response text
    string error  = 3;  // Error message if success=false
    string intent = 4;  // Classified intent (e.g. "STATUS"), "UNKNOWN" if unresolved
    float confidence = 5; // Classifier confidence for `intent`, 0.0-1.0
}
```

to:

```proto
message NaturalLanguageResponse {
    bool success = 1;
    string reply = 2;   // The AI's response text
    string error  = 3;  // Error message if success=false
    string intent = 4;  // Classified intent (e.g. "STATUS"), "UNKNOWN" if unresolved
    float confidence = 5; // Classifier confidence for `intent`, 0.0-1.0
    // Extracted capability argument, "" if nothing was extracted — the caller (C++) falls back
    // to the original request payload in that case. Append-only field per INV-2.
    string payload = 6;
}
```

- [ ] **Step 2: Regenerate the protobuf stubs**

Run (from the repo root):

```bash
protoc --proto_path=proto \
       --cpp_out=generated/cpp \
       --grpc_out=generated/cpp \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       proto/jarvis.proto proto/ai.proto

.venv/bin/python -m grpc_tools.protoc -Iproto \
       --python_out=generated/python \
       --grpc_python_out=generated/python \
       proto/jarvis.proto proto/ai.proto
```

Expected: both commands exit 0; `git status` shows only `generated/ai.pb.h`, `generated/ai.pb.cc`,
`generated/python/ai_pb2.py` as modified (the `jarvis.proto`-derived files are untouched since that
proto didn't change — if `git status` shows changes there too, something else is wrong; stop and
investigate before continuing).

- [ ] **Step 3: Update `core/ai_client.h`**

Change:

```cpp
struct AIResult {
  bool success;
  std::string reply;
  std::string intent;
  float confidence;
};
```

to:

```cpp
struct AIResult {
  bool success;
  std::string reply;
  std::string intent;
  float confidence;
  // Extracted capability argument from the Understanding tier, "" if nothing was extracted.
  std::string payload;
};
```

- [ ] **Step 4: Update `core/ai_client.cpp`**

Change:

```cpp
  if (!status.ok()) {
    spdlog::warn("AIClient: call failed — {} (code {})", status.error_message(), static_cast<int>(status.error_code()));
    return AIResult{false, "[AI unavailable: " + status.error_message() + "]", "UNKNOWN", 0.0f};
  }

  if (!response.success()) {
    spdlog::warn("AIClient: AI server returned error: {}", response.error());
    return AIResult{false, "[AI error: " + response.error() + "]", "UNKNOWN", 0.0f};
  }

  spdlog::info("AIClient: reply='{}' intent={} confidence={:.2f}", response.reply(), response.intent(), response.confidence());
  return AIResult{true, response.reply(), response.intent(), response.confidence()};
```

to:

```cpp
  if (!status.ok()) {
    spdlog::warn("AIClient: call failed — {} (code {})", status.error_message(), static_cast<int>(status.error_code()));
    return AIResult{false, "[AI unavailable: " + status.error_message() + "]", "UNKNOWN", 0.0f, ""};
  }

  if (!response.success()) {
    spdlog::warn("AIClient: AI server returned error: {}", response.error());
    return AIResult{false, "[AI error: " + response.error() + "]", "UNKNOWN", 0.0f, ""};
  }

  spdlog::info("AIClient: reply='{}' intent={} confidence={:.2f} payload='{}'",
      response.reply(), response.intent(), response.confidence(), response.payload());
  return AIResult{true, response.reply(), response.intent(), response.confidence(), response.payload()};
```

- [ ] **Step 5: Update `core/jarvis_service.cpp`'s re-dispatch branch**

Change:

```cpp
      if (classifiedCmd != CommandType::UNKNOWN) {
        internalCmd = classifiedCmd;
        dispatchResult = registry_.dispatch(classifiedCmd, payload, execContext);
        if (const Capability* capability = registry_.resolve(classifiedCmd)) {
          intentName = capability->intentName;
        }
      } else {
        intentName = normalizedIntent;
        dispatchResult = registry_.dispatch(normalizedIntent, payload, execContext);
      }
```

to:

```cpp
      // The Understanding tier may have extracted a clean argument from the sentence (e.g.
      // "turn it up to 50" -> "set 50") — prefer it when present; an empty AI payload means
      // nothing was extracted, so fall back to the original raw sentence exactly as before
      // this feature existed (INV-5: Understanding decides the argument, not this C++ layer).
      const std::string& dispatchPayload = aiResult.payload.empty() ? payload : aiResult.payload;
      if (classifiedCmd != CommandType::UNKNOWN) {
        internalCmd = classifiedCmd;
        dispatchResult = registry_.dispatch(classifiedCmd, dispatchPayload, execContext);
        if (const Capability* capability = registry_.resolve(classifiedCmd)) {
          intentName = capability->intentName;
        }
      } else {
        intentName = normalizedIntent;
        dispatchResult = registry_.dispatch(normalizedIntent, dispatchPayload, execContext);
      }
```

- [ ] **Step 6: Update `ai/jarvis_ai_server.py`**

Change:

```python
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

to:

```python
    def ProcessNaturalLanguage(self, request, context):
        start = time.monotonic()
        intent, confidence, tier, payload = resolve(request.text)
        latency_ms = (time.monotonic() - start) * 1000

        logger.info(
            "text=%r tier=%s intent=%s confidence=%.2f payload=%r latency_ms=%.2f",
            request.text, tier, intent, confidence, payload, latency_ms,
        )

        reply = f"[detected intent: {intent}, confidence {confidence:.2f}]"
        return ai_pb2.NaturalLanguageResponse(
            success=True,
            reply=reply,
            intent=intent,
            confidence=confidence,
            payload=payload,
        )
```

- [ ] **Step 7: Build the C++ side and run the C++ test suite**

Run: `cmake -S . -B build && cmake --build build --target jarvis_tests jarvis jarvis_grpc_server && ./build/jarvis_tests`
Expected: builds clean (the regenerated stubs must compile — this is the first real check that
Step 2 produced valid code); all existing C++ tests still pass (this task doesn't touch anything
they cover, but a clean full-suite run after a proto regen is the right sanity check).

- [ ] **Step 8: Append new tests to `ai/test_ai_server.py`**

Add to the `TestServicerClassifiesViaRules` class:

```python
    def test_volume_intent_gets_deterministic_payload(self):
        resp = call("set the volume to 42")
        assert resp.success
        assert resp.intent == "VOLUME"
        assert resp.payload == "set 42"

    def test_status_intent_has_empty_payload(self):
        resp = call("how long have you been running")
        assert resp.payload == ""

    def test_shutdown_intent_has_empty_payload_original_text_preserved_by_caller(self):
        # The servicer itself doesn't know about ConsentGate — it just proves the payload
        # field stays empty for a T3 intent, which is what makes core/jarvis_service.cpp's
        # fallback-to-original-payload path (Task 6, Step 5) preserve the confirm-token
        # contract end to end.
        resp = call("shutdown please, i confirm")
        assert resp.intent == "SHUTDOWN"
        assert resp.payload == ""
```

- [ ] **Step 9: Run the Python AI test suite**

Run: `PYTHONPATH=ai:generated/python .venv/bin/python -m pytest ai/ -v`
Expected: PASS, everything from Tasks 1-6 combined.

- [ ] **Step 10: Append new calls to `tools/grpc_smoke_test.py`**

Add at the end of `main()`, before the final `hows it going` call:

```python
    # VOLUME resolves at the rule tier and its numeric slot_rule matches directly — no Ollama
    # needed for this one, unlike the "hows it going" case below.
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "set the volume to 42",
        label="classified -> VOLUME, payload 'set 42' (T2 — expect consent-required unless granted)",
    )
    # Negation veto: must stay UNKNOWN and never reach SHUTDOWN/ConsentGate at all.
    call_command(
        stub, jarvis_pb2.COMMAND_TYPE_UNKNOWN, "don't shut down the computer",
        label="negation veto -> should stay UNKNOWN",
    )
```

- [ ] **Step 11: Manual verification (requires the full stack running)**

Start `python3 ai/jarvis_ai_server.py`, then `build/jarvis_grpc_server`, then run
`.venv/bin/python tools/grpc_smoke_test.py` and read the output for the two new cases: confirm
`set the volume to 42` prints a consent-required message naming `volume` (not a crash, not a
misclassification), and confirm `don't shut down the computer` prints `command_type: UNKNOWN`.

- [ ] **Step 12: Commit**

```bash
git add proto/ai.proto generated/cpp/ai.pb.h generated/cpp/ai.pb.cc generated/cpp/ai.grpc.pb.h generated/cpp/ai.grpc.pb.cc generated/python/ai_pb2.py generated/python/ai_pb2_grpc.py core/ai_client.h core/ai_client.cpp core/jarvis_service.cpp ai/jarvis_ai_server.py ai/test_ai_server.py tools/grpc_smoke_test.py
git commit -m "feat: relay extracted payload from Understanding tier back to C++ (INV-2 append-only field)"
```

---

### Task 7: `ConsentGate` case-insensitive confirm matching

**Files:**
- Modify: `core/consent_gate.cpp`
- Modify: `tests/consent_gate_test.cpp` (append new tests only)

**Interfaces:** None — internal fix, no signature changes.

- [ ] **Step 1: Write the failing tests**

Append to `tests/consent_gate_test.cpp`:

```cpp
TEST_F(ConsentGateFileTest, T3AllowedWithUppercaseConfirmToken) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("delete_files", PowerTier::T3_DESTRUCTIVE), "please Confirm");

    EXPECT_TRUE(result.allowed);
}

TEST_F(ConsentGateFileTest, T4AllowedWithFullyUppercaseConfirmToken) {
    PluginConfig config = PluginConfig::load("does_not_exist.cfg", grants_file_);
    ConsentGate gate(config);

    ConsentResult result =
        gate.check(makeCapability("call_external_api", PowerTier::T4_EXTERNAL), "CONFIRM");

    EXPECT_TRUE(result.allowed);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter='ConsentGateFileTest.T3AllowedWithUppercaseConfirmToken:ConsentGateFileTest.T4AllowedWithFullyUppercaseConfirmToken'`
Expected: FAIL — the current exact-case comparison rejects both.

- [ ] **Step 3: Update `core/consent_gate.cpp`**

Change:

```cpp
#include "consent_gate.h"

#include <sstream>

namespace {

// T3/T4 confirmation is a per-call payload convention, never persisted (INV-9: "explicit
// confirmation regardless of prior permissions"). The word "confirm" must appear as the LAST
// whitespace-delimited token in the payload — not anywhere, and not as a substring (that would
// let something like "reconfirmation" slip through by accident).
bool payloadConfirms(const std::string& payload) {
    std::istringstream stream(payload);
    std::string token;
    std::string lastToken;
    while (stream >> token) {
        lastToken = token;
    }
    return lastToken == "confirm";
}

}  // namespace
```

to:

```cpp
#include "consent_gate.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// T3/T4 confirmation is a per-call payload convention, never persisted (INV-9: "explicit
// confirmation regardless of prior permissions"). The word "confirm" must appear as the LAST
// whitespace-delimited token in the payload — not anywhere, and not as a substring (that would
// let something like "reconfirmation" slip through by accident). Matched case-insensitively:
// a voice transcript can capitalize the word ("Confirm") in ways a literal CLI command never
// would, and there's no safety reason to require a specific case.
bool payloadConfirms(const std::string& payload) {
    std::istringstream stream(payload);
    std::string token;
    std::string lastToken;
    while (stream >> token) {
        lastToken = token;
    }
    return toLowerAscii(lastToken) == "confirm";
}

}  // namespace
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target jarvis_tests && ./build/jarvis_tests --gtest_filter='ConsentGate*'`
Expected: PASS, all cases including the two new ones.

- [ ] **Step 5: Run the full C++ suite to confirm no regressions**

Run: `./build/jarvis_tests`
Expected: PASS, all suites (this change is strictly additive — a payload already using lowercase
`"confirm"` is unaffected).

- [ ] **Step 6: Commit**

```bash
git add core/consent_gate.cpp tests/consent_gate_test.cpp
git commit -m "fix(core): match T3/T4 confirm token case-insensitively"
```

---

### Task 8: Expanded, categorized evaluation harness

**Files:**
- Modify: `tools/eval_understanding.py` (full rewrite)

**Interfaces:** None — standalone script, not imported elsewhere.

- [ ] **Step 1: Replace `tools/eval_understanding.py`'s full contents**

```python
"""Evaluation harness for the Understanding tier (INV-13) — measures accuracy and latency of
each classification approach against a categorized, labeled dataset. A failing category tells
you *what kind* of robustness regressed, not just an aggregate percentage.

Run standalone: `python3 tools/eval_understanding.py`. The rule-only row needs nothing extra;
the llm-only and hybrid rows need a running local Ollama server (see ai/llm_backend.py).
"""

from collections import defaultdict
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
from slot_extractor import extract as extract_slot
from intent_registry import get_default_intents

# (text, expected_intent, category) — categorized so a run reports per-category accuracy, not
# just an aggregate. "typo"/"synonym"/"negation"/"off_topic" are the baseline categories every
# robustness pass should cover; "voice_noise"/"adversarial"/"ambiguous"/"short_long" are the
# scenario classes selected during this work's design (see docs/superpowers/specs/
# 2026-09-03-intent-recognition-robustness-design.md §1).
DATASET = [
    # -- typo --
    ("staus", "STATUS", "typo"),
    ("shutdon the computer", "SHUTDOWN", "typo"),
    ("volme up please", "UNKNOWN", "typo"),  # not a first-word, no phrase pattern catches it
    ("ecoh this", "ECHO", "typo"),
    ("abuot yourself", "ABOUT", "typo"),
    ("wat can you do", "UNKNOWN", "typo"),  # not a recognised first-word keyword or pattern

    # -- synonym / alternate phrasing --
    ("how long have you been running", "STATUS", "synonym"),
    ("are you alive", "STATUS", "synonym"),
    ("repeat after me", "ECHO", "synonym"),
    ("who are you", "ABOUT", "synonym"),
    ("what's the sound level", "VOLUME", "synonym"),
    ("power off", "SHUTDOWN", "synonym"),
    ("what can you do", "HELP", "synonym"),

    # -- negation --
    ("don't shut down the computer", "UNKNOWN", "negation"),
    ("do not shut down", "UNKNOWN", "negation"),
    ("cancel the shutdown", "UNKNOWN", "negation"),
    ("never mind the volume", "UNKNOWN", "negation"),
    ("stop, don't echo that", "UNKNOWN", "negation"),

    # -- off-topic / no match --
    ("what is the meaning of life", "UNKNOWN", "off_topic"),
    ("the weather today", "UNKNOWN", "off_topic"),
    ("tell me a joke", "UNKNOWN", "off_topic"),
    ("what time is it", "UNKNOWN", "off_topic"),

    # -- voice/STT-specific noise --
    ("hey jarvis what's the status", "STATUS", "voice_noise"),
    ("um, jarvis, could you say hello", "ECHO", "voice_noise"),
    ("jarvis who are you", "ABOUT", "voice_noise"),
    ("hello jarvis what's the weather like today", "UNKNOWN", "voice_noise"),  # address term + unrelated question

    # -- adversarial / injection-like --
    ("my confirm number is five", "UNKNOWN", "adversarial"),  # "confirm" present but no command word
    ("please don't shut down, wait, confirm this is not a shutdown request", "UNKNOWN", "adversarial"),
    ("shutdown confirm confirm confirm", "SHUTDOWN", "adversarial"),  # still legitimately SHUTDOWN

    # -- genuinely ambiguous --
    ("status of the volume", "UNKNOWN", "ambiguous"),  # both STATUS and VOLUME words present, neither pattern fully matches
    ("about the shutdown process", "ABOUT", "ambiguous"),

    # -- short / long input --
    ("status", "STATUS", "short_long"),
    ("s", "UNKNOWN", "short_long"),
    ("", "UNKNOWN", "short_long"),
    (
        "so I was thinking earlier today about whether everything is working correctly and I "
        "guess what I really want to know right now, if you don't mind telling me, is just your "
        "current status",
        "STATUS",
        "short_long",
    ),
]


def _run(label, classify_fn, dataset):
    """Run `classify_fn` over `dataset`, print overall + per-category accuracy and latency."""
    correct = 0
    total_latency_ms = 0.0
    by_category = defaultdict(lambda: [0, 0])  # category -> [correct, total]

    for text, expected, category in dataset:
        start = time.monotonic()
        intent, _confidence = classify_fn(text)
        total_latency_ms += (time.monotonic() - start) * 1000
        is_correct = intent == expected
        correct += is_correct
        by_category[category][1] += 1
        by_category[category][0] += is_correct

    accuracy = correct / len(dataset)
    avg_latency_ms = total_latency_ms / len(dataset)
    print(f"{label:10s} accuracy={accuracy:6.1%}  avg_latency_ms={avg_latency_ms:8.2f}  (n={len(dataset)})")
    for category in sorted(by_category):
        cat_correct, cat_total = by_category[category]
        print(f"  {category:14s} {cat_correct}/{cat_total}")
    return accuracy, avg_latency_ms


def _resolve_intent_only(text):
    intent, confidence, _tier, _payload = resolve(text)
    return intent, confidence


def _eval_slot_extraction():
    """Not accuracy-scored like the intent tables above (a payload isn't a fixed label the way
    an intent name is) — prints deterministic-extraction results for a fixed set of volume
    phrasings so a human can eyeball whether the slot_rules in plugins/system-control/manifest.json
    are behaving sanely."""
    intents = get_default_intents()
    cases = [
        "what's the volume",
        "set the volume to 50",
        "turn it up to 7",
        "volume 99",
        "turn it up please",  # no number -- expected to extract nothing deterministically
    ]
    print("\nslot extraction (VOLUME, deterministic only):")
    for text in cases:
        payload = extract_slot("VOLUME", text, intents)
        print(f"  {text!r:45s} -> {payload!r}")


def _eval_t3_payload_preservation():
    """Confirms the structural guarantee from ai/slot_extractor.py: a T3 intent never gets an
    extracted payload, no matter how the sentence is phrased — resolve()'s payload stays ""."""
    print("\nT3 payload preservation (SHUTDOWN must always extract ''):")
    for text in ["shutdown", "shutdown confirm", "shut down the machine, confirm", "shutdown 5"]:
        intent, _confidence, _tier, payload = resolve(text)
        status = "OK" if payload == "" else "FAIL — payload should always be empty for T3"
        print(f"  {text!r:40s} intent={intent:10s} payload={payload!r:10s} {status}")


def main():
    print(f"Evaluating {len(DATASET)} labeled examples across three configurations:\n")
    _run("rule-only", classify, DATASET)
    _run("llm-only", llm_classify, DATASET)
    _run("hybrid", _resolve_intent_only, DATASET)
    _eval_slot_extraction()
    _eval_t3_payload_preservation()


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it**

Run: `PYTHONPATH=ai:generated/python .venv/bin/python tools/eval_understanding.py`
Expected: the script runs to completion without raising (the `llm-only`/`hybrid` rows' accuracy
will vary depending on whether Ollama is reachable in this environment and how the model actually
answers — that's expected and fine, this is a measurement tool, not a pass/fail gate). Read the
`rule-only` row and its per-category breakdown specifically: it should score highly on `typo`,
`synonym`, `negation`, `voice_noise`, and the exact-`"status"`/empty-string `short_long` cases,
and near-0 is expected/correct on the deliberately-UNKNOWN `off_topic`/`adversarial`/`ambiguous`
cases (scoring "correct" there means correctly returning UNKNOWN, so a category showing e.g. 4/4
is exactly right). The two `_eval_*` sections print human-readable tables — eyeball that
`turn it up please` extracts `None` (no number present) and every `SHUTDOWN` payload is `''`.

- [ ] **Step 3: Commit**

```bash
git add tools/eval_understanding.py
git commit -m "test(ai): expand eval_understanding.py into a categorized robustness dataset"
```

---

### Task 9: Docs and QMUL contemporaneous records

**Files:**
- Modify: `docs/features.md`
- Modify: `docs/roadmap.md`
- Modify: `qmul/notes/genai-usage-log.md` (append-only)
- Create: `qmul/logbook/2026-09-03-understanding-tier-robustness.md`

**Interfaces:** None — documentation only.

- [ ] **Step 1: Update `docs/features.md`'s Phase 2.5 section**

Find the Phase 2.5 section's bullet list and:
- Add a new ✅ bullet directly after the existing "Rule classifier precision fix" bullet:

```
- ✅ **Understanding tier made data-driven & robust (2026-09-03)** — `ai/intent_registry.py`
  loads intent trigger phrases/slot-extraction rules from `config/intents.json` (built-ins) and
  every `plugins/*/manifest.json`, replacing the two previously-hardcoded intent lists in
  `ai/intent_classifier.py` and `ai/llm_backend.py`. A new capability's manifest is now the only
  thing needed to make it natural-language-reachable (closes the concrete gap where `volume`/
  `shutdown` were unreachable by any phrasing at all). Added: a data-driven negation veto
  ("don't shut down" no longer fires SHUTDOWN), deterministic-first/LLM-fallback slot extraction
  (`ai/slot_extractor.py` + a new `payload` field on `NaturalLanguageResponse`, INV-2 append-only)
  so natural phrasing like "turn it up to 50" reaches `volume` as `"set 50"`, a structural
  (code-level, not just documented) refusal to ever extract a payload for a T3/T4 capability so
  `ConsentGate`'s confirm-token contract stays reliable, and case-insensitive confirm-token
  matching in `ConsentGate` itself. `tools/eval_understanding.py`'s dataset grew from 16 to a
  categorized set (typo, synonym, negation, off-topic, voice/STT noise, adversarial, ambiguous,
  short/long input).
```

- Update the existing "Safety guardrails" bullet (currently reads something like "Real teeth on
  this depend on Phase 3 having a plugin that can actually do something destructive") to:

```
- 📋 **Safety guardrails, remaining scope** — the negation veto and T3/T4 payload-preservation
  guarantee (above) close the two sharpest false-positive/false-confirmation risks now that
  `shutdown` is a real destructive capability. Still open: any *proactive* confirmation UX
  (JARVIS asking "are you sure?" rather than requiring the word "confirm" already present in the
  utterance) — deliberately out of scope for the 2026-09-03 work since it needs multi-turn
  conversational state, which is its own deferred item below.
```

- [ ] **Step 2: Update `docs/roadmap.md`'s Phase 2.5 section**

Append a new paragraph after the existing Phase 2.5 narrative (matching the file's established
prose style — see the "LLM fallback tier" and "rule classifier precision fix" paragraphs for
tone/format):

```
**Understanding tier robustness (2026-09-03).** Traced the actual data flow and found the
Understanding tier's intent knowledge was 100% hardcoded across two Python files, with zero
visibility into what capabilities existed — meaning `volume`/`shutdown` (Phase 4,
2026-09-02) were completely unreachable by natural language despite being real, dispatchable
capabilities. Replaced both hardcoded lists with a data-driven Intent Registry
(`ai/intent_registry.py`) sourced from `config/intents.json` and every plugin's own
`manifest.json`, added a negation veto, deterministic-first/LLM-fallback slot extraction with a
new append-only proto field carrying the extracted argument back to C++, a structural guarantee
that T3/T4 capabilities never receive an extracted payload, and fixed a case-sensitivity bug in
`ConsentGate`'s confirm-token check. Full design rationale in
`docs/superpowers/specs/2026-09-03-intent-recognition-robustness-design.md`.
```

- [ ] **Step 3: Append a Gen AI usage log entry**

Read `qmul/notes/genai-usage-log.md`'s existing entries for the exact per-entry template (Tool /
Category / What I asked for / What I received / What I did with it / Verification / Defensible?),
then append a new **Category C** entry dated 2026-09-03, before the `<!-- Add new entries above
this line -->` marker at the end of the `## Log` section, covering: the Understanding tier
redesign (Intent Registry, negation veto, slot extraction, the proto field, the ConsentGate
case-fix), how it was verified (the full `ai/` pytest suite plus the C++ suite plus the
categorized eval harness plus manual grpc_smoke_test.py runs), and why it's defensible (every
piece is either a direct, explainable extension of the existing tiered-rule/LLM design or a small,
traceable fix like the case-insensitivity change).

- [ ] **Step 4: Write the engineering logbook entry**

Check `qmul/logbook/README.md`'s conventions and the most recent existing entry's format (e.g.
`qmul/logbook/2026-09-02-t3-t4-consent-system-control.md`) to match structure exactly, then write
`qmul/logbook/2026-09-03-understanding-tier-robustness.md`: what was built (all 9 tasks above),
the decisions and why (data-driven registry over continued hardcoding; deterministic-first slot
extraction; keeping T3/T4 confirmation single-utterance rather than adding conversation state;
scanning the source `plugins/` tree directly rather than the build-staged directory, so `pytest
ai` never depends on a prior C++ build having run — note this as a deliberate refinement over the
design spec's literal wording, and why), problems hit during actual implementation (fill in with
what really happened, not hypothetically), and verification (the real test counts from Tasks 1-8,
run for real).

- [ ] **Step 5: Commit**

```bash
git add docs/features.md docs/roadmap.md qmul/notes/genai-usage-log.md qmul/logbook/2026-09-03-understanding-tier-robustness.md
git commit -m "docs: record Understanding tier robustness work"
```

---

## Self-Review

**Spec coverage:** §3-4 (Intent Registry, all components) → Tasks 1-5. §4.6-4.9 (proto + C++
relay + ConsentGate fix) → Tasks 6-7. §7 (testing) → each task's own test steps plus Task 8's
harness expansion. §8 (migration/compatibility) → verified explicitly at each task's "run existing
suite unchanged" step. The one deliberate deviation from the spec's literal text — scanning
`plugins/` directly instead of resolving plugin locations via `config/plugin_dirs.cfg` — is
called out in Task 1 and Task 9 with its rationale (avoids coupling `pytest ai` to a prior C++
build), not silently substituted.

**Placeholder scan:** no TBD/TODO; every code step has complete, working code; every test has
concrete assertions.

**Type consistency:** `IntentSpec`/`SlotRule` (Task 1) are used with identical field names and
types in Tasks 2-5's consuming code. `classify(text, intents=None)` and `llm_classify(text,
intents=None)` keep identical signatures from their definition (Tasks 2, 4) through every call
site (Task 5's `resolver.py`, Task 8's `eval_understanding.py`). `resolve()`'s 4-tuple return
shape is consistent between its definition (Task 5) and its two consumers (Task 6's
`jarvis_ai_server.py`, Task 8's `_resolve_intent_only`). `AIResult.payload` (Task 6) is set at
every construction site in `ai_client.cpp` (both failure paths get `""`, the success path gets
`response.payload()`) and read at exactly one site in `jarvis_service.cpp`.
