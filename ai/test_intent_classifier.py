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
    ("show system information", "SYSTEM_INFO"),
    ("machine info", "SYSTEM_INFO"),
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


class TestAddressTermDoesNotSkewClassification:
    """Regression: voice input naturally addresses the assistant by name ("Jarvis, ...")
    in a way typed CLI input never does. Bag-of-words scoring must not let the address term
    itself act as intent signal — see the comment on ABOUT's patterns in intent_classifier.py."""

    @pytest.mark.parametrize(
        "text",
        [
            "hello jarvis what's the weather like today",
            "jarvis what time is it",
            "hey jarvis what should I have for lunch",
        ],
    )
    def test_jarvis_address_term_plus_unrelated_question_is_unknown(self, text):
        assert classify(text) == ("UNKNOWN", 0.0)

    def test_who_are_you_still_fires_about_even_when_addressed(self):
        # Confirms the fix didn't collaterally break genuine identity questions that use a
        # pattern unrelated to the word "jarvis" itself.
        intent, confidence = classify("jarvis who are you")
        assert intent == "ABOUT"
        assert confidence >= CONFIDENCE_THRESHOLD


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
        # One word ("repeat") matches 1/2 of ECHO's {"repeat", "after"} = 0.5, which must
        # NOT fire — a two-word pattern needs both words. Keeps the rule path high-precision.
        assert classify("repeat") == ("UNKNOWN", 0.0)


class TestFirstWordFastPath:
    """A literal (or near-literal) command word as the first word routes instantly,
    mirroring how the C++ CLI already treats a typed first-word command."""

    @pytest.mark.parametrize(
        "text,expected",
        [
            ("status update please", "STATUS"),
            ("echo apparently works now", "ECHO"),
            ("about that thing you mentioned", "ABOUT"),
        ],
    )
    def test_exact_first_word_routes_instantly(self, text, expected):
        assert classify(text) == (expected, 1.0)


class TestFirstWordTypoCorrection:
    """A misspelt first word that's still a clear near-match to a command keyword
    still routes correctly, just at a lower (but still confident) score."""

    @pytest.mark.parametrize(
        "text,expected",
        [
            ("staus report", "STATUS"),
            ("ecoh this back", "ECHO"),
            ("abuot yourself", "ABOUT"),
            ("statuss", "STATUS"),
        ],
    )
    def test_typo_first_word_still_routes(self, text, expected):
        intent, confidence = classify(text)
        assert intent == expected
        assert confidence == pytest.approx(0.8)

    def test_unrelated_short_word_is_not_corrected(self):
        # "sat" is superficially close to "status" by edit distance, but it's a real,
        # unrelated word — must not misfire. This is exactly the false-positive risk
        # fuzzy correction has to guard against.
        assert classify("sat there quietly") == ("UNKNOWN", 0.0)


class TestMetaQuestionVeto:
    """Asking *about* a command (not starting with that command word) must not be
    misread as invoking it — the bug found in live testing: 'what does the echo
    command do' was getting classified as ECHO and literally echoing the question
    back, instead of being recognised as a question about the command."""

    @pytest.mark.parametrize(
        "text",
        [
            "what does the echo command do",
            "can you explain the status command",
            "what does status mean",
        ],
    )
    def test_meta_question_about_a_command_is_unknown(self, text):
        # Falls through to UNKNOWN here (not ABOUT) because these particular phrasings
        # don't contain "about" or the ABOUT patterns — genuinely ambiguous phrasing
        # like this is exactly what should escalate to the LLM tier, not get a
        # confident-but-wrong rule-tier answer.
        assert classify(text) == ("UNKNOWN", 0.0)

    def test_about_is_exempt_from_its_own_veto(self):
        # "about" is present alongside "command" (a meta-question word), but ABOUT's
        # own bare pattern must still fire — asking about something IS what ABOUT means,
        # so it's never ambiguous the way an actionable intent's bare mention is.
        intent, confidence = classify("tell me about the echo command")
        assert intent == "ABOUT"
        assert confidence == pytest.approx(1.0)

    def test_veto_does_not_affect_multiword_patterns(self):
        # "what's your uptime" contains "what" but the match here is the bare
        # single-word {"uptime"} pattern with no other meta-question word present —
        # must still fire normally (regression guard for the fix above).
        assert classify("what's your uptime") == ("STATUS", 1.0)
