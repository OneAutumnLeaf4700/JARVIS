"""Deterministic rule-based intent classifier — the fast path of the Understanding tier."""

from __future__ import annotations

import difflib
import re

# Intent -> token-set patterns, as data (not code) so new intents are edits here alone.
# Keys are UPPERCASE to match the internal/proto CommandType names for direct re-dispatch later.
INTENT_PATTERNS: dict[str, list[set[str]]] = {
    "STATUS": [{"status"}, {"uptime"}, {"how", "long", "running"}, {"alive"}],
    "ECHO": [{"echo"}, {"repeat", "after"}, {"say"}],
    "ABOUT": [{"about"}, {"who", "are", "you"}, {"what", "jarvis"}],
}

CONFIDENCE_THRESHOLD = 0.5

# First-word fast path: if the sentence literally starts with (or is a likely typo of)
# one of these, route instantly — mirrors how the C++ CLI already treats a typed
# first-word command. Values are exactly the lowercase form of an INTENT_PATTERNS key.
COMMAND_KEYWORDS = {"status", "echo", "about"}
FUZZY_MATCH_CUTOFF = 0.7  # verified empirically: catches real typos (staus, ecoh, abuot),
# rejects unrelated short words (sat, at, is, who, what, how) that would otherwise misfire.
MIN_FUZZY_WORD_LENGTH = 3
FUZZY_MATCH_CONFIDENCE = 0.8  # confident but intentionally below the 1.0 of an exact match

# Words that signal the sentence is *asking about* a command rather than invoking it —
# e.g. "what does the echo command do" must not fire ECHO's bare {"echo"} pattern.
# Bare "about" is exempt: asking about something IS what the ABOUT intent means, so its
# own bare match is never ambiguous the way an actionable intent's bare mention is.
META_QUESTION_WORDS = {"does", "command", "explain", "mean", "meaning"}

_TOKEN_RE = re.compile(r"[a-z0-9]+")


def preprocess(text: str) -> set[str]:
    """Lowercase, strip punctuation, split on whitespace into a set of tokens."""
    return set(_TOKEN_RE.findall(text.lower()))


def classify(text: str) -> tuple[str, float]:
    """Map free-form text to (intent, confidence); ("UNKNOWN", 0.0) at or below threshold.

    Two passes. First, a first-word fast path: if the sentence starts with a command
    keyword (or a likely typo of one), route instantly — no ambiguity possible. Otherwise,
    fall back to phrase-pattern matching for natural-language phrasing that doesn't start
    with the command word itself (e.g. "are you alive", "how long have you been running").
    An intent's score there is its best-matching pattern, where a pattern scores
    len(pattern & tokens) / len(pattern); a bare single-word pattern (other than ABOUT's)
    is skipped when a meta-question word is present, since that reads as discussion, not
    invocation. Ties resolve to declaration order. The threshold is exclusive so a
    two-word pattern needs both words, not one generic one — the rule path stays
    high-precision and defers anything uncertain to UNKNOWN.
    """
    words = _TOKEN_RE.findall(text.lower())
    if not words:
        return "UNKNOWN", 0.0

    first_word = words[0]
    if first_word in COMMAND_KEYWORDS:
        return first_word.upper(), 1.0

    if len(first_word) >= MIN_FUZZY_WORD_LENGTH:
        correction = difflib.get_close_matches(
            first_word, COMMAND_KEYWORDS, n=1, cutoff=FUZZY_MATCH_CUTOFF
        )
        if correction:
            return correction[0].upper(), FUZZY_MATCH_CONFIDENCE

    tokens = set(words)
    is_meta_question = bool(META_QUESTION_WORDS & tokens)

    best_intent = "UNKNOWN"
    best_score = 0.0
    for intent, patterns in INTENT_PATTERNS.items():
        score = max(
            (
                len(pattern & tokens) / len(pattern)
                for pattern in patterns
                if not (is_meta_question and len(pattern) == 1 and pattern != {"about"})
            ),
            default=0.0,
        )
        if score > best_score:
            best_intent = intent
            best_score = score

    if best_score > CONFIDENCE_THRESHOLD:
        return best_intent, best_score
    return "UNKNOWN", 0.0
