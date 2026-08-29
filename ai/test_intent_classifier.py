"""Unit tests for the rule-based intent classifier."""

import pytest

from intent_classifier import CONFIDENCE_THRESHOLD, classify, preprocess


class TestPreprocess:
    def test_lowercases(self):
        assert preprocess("STATUS Report") == {"status", "report"}

    def test_strips_punctuation(self):
        assert preprocess("status?") == {"status"}
        assert preprocess("who are you?!") == {"who", "are", "you"}

    def test_splits_on_whitespace(self):
        assert preprocess("how   long\trunning") == {"how", "long", "running"}

    def test_empty_string(self):
        assert preprocess("") == set()

    def test_dedupes_repeats(self):
        assert preprocess("echo echo echo") == {"echo"}


# (text, expected_intent) — several distinct phrasings per intent to prove the abstraction
# lives above surface form: different words, same intent.
POSITIVE_CASES = [
    ("status", "STATUS"),
    ("what's your uptime", "STATUS"),
    ("how long have you been running", "STATUS"),
    ("are you alive", "STATUS"),
    ("echo this back", "ECHO"),
    ("repeat after me", "ECHO"),
    ("say hello", "ECHO"),
    ("about", "ABOUT"),
    ("who are you", "ABOUT"),
    ("what is jarvis", "ABOUT"),
]


class TestClassifyPositive:
    @pytest.mark.parametrize("text,expected", POSITIVE_CASES)
    def test_maps_phrasing_to_intent(self, text, expected):
        intent, confidence = classify(text)
        assert intent == expected
        assert confidence >= CONFIDENCE_THRESHOLD


class TestClassifyUnknown:
    @pytest.mark.parametrize(
        "text",
        [
            "what is the meaning of life",
            "",
            "how",  # 1/3 of {how, long, running} = 0.33, below threshold
            "the weather today",
        ],
    )
    def test_returns_unknown_zero(self, text):
        assert classify(text) == ("UNKNOWN", 0.0)


class TestConfidence:
    def test_exact_single_word_is_full_confidence(self):
        assert classify("status") == ("STATUS", 1.0)

    def test_partial_multiword_above_threshold(self):
        intent, confidence = classify("how long running")
        assert intent == "STATUS"
        assert confidence == pytest.approx(1.0)

    def test_partial_two_of_three_above_threshold(self):
        # "how long" matches 2/3 of {how, long, running} = 0.667, still >= 0.5
        intent, confidence = classify("how long")
        assert intent == "STATUS"
        assert confidence == pytest.approx(2 / 3)

    def test_unknown_confidence_is_exactly_zero(self):
        _, confidence = classify("nonsense input here")
        assert confidence == 0.0

    def test_threshold_is_exclusive(self):
        # One generic word ("what") matches 1/2 of {"what", "jarvis"} = 0.5, which must
        # NOT fire — a two-word pattern needs both words. Keeps the rule path high-precision.
        assert classify("what") == ("UNKNOWN", 0.0)
