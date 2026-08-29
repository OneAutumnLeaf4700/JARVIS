"""Deterministic rule-based intent classifier — the fast path of the Understanding tier."""

from __future__ import annotations

import re

# Intent -> token-set patterns, as data (not code) so new intents are edits here alone.
# Keys are UPPERCASE to match the internal/proto CommandType names for direct re-dispatch later.
INTENT_PATTERNS: dict[str, list[set[str]]] = {
    "STATUS": [{"status"}, {"uptime"}, {"how", "long", "running"}, {"alive"}],
    "ECHO": [{"echo"}, {"repeat", "after"}, {"say"}],
    "ABOUT": [{"about"}, {"who", "are", "you"}, {"what", "jarvis"}],
}

CONFIDENCE_THRESHOLD = 0.5

_TOKEN_RE = re.compile(r"[a-z0-9]+")


def preprocess(text: str) -> set[str]:
    """Lowercase, strip punctuation, split on whitespace into a set of tokens."""
    return set(_TOKEN_RE.findall(text.lower()))


def classify(text: str) -> tuple[str, float]:
    """Map free-form text to (intent, confidence); ("UNKNOWN", 0.0) at or below threshold.

    An intent's score is its best-matching pattern, where a pattern scores
    len(pattern & tokens) / len(pattern). Ties resolve to declaration order. The
    threshold is exclusive so a two-word pattern needs both words, not one generic
    one — the rule path stays high-precision and defers anything uncertain to UNKNOWN.
    """
    tokens = preprocess(text)

    best_intent = "UNKNOWN"
    best_score = 0.0
    for intent, patterns in INTENT_PATTERNS.items():
        score = max((len(pattern & tokens) / len(pattern) for pattern in patterns), default=0.0)
        if score > best_score:
            best_intent = intent
            best_score = score

    if best_score > CONFIDENCE_THRESHOLD:
        return best_intent, best_score
    return "UNKNOWN", 0.0
