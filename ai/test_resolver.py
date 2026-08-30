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

    @patch("resolver.llm_classify")
    def test_rule_miss_llm_low_confidence_rejected(self, mock_llm):
        """A non-UNKNOWN LLM guess below the confidence threshold must not pass through as
        tier="llm" — the wire-level invariant "intent != UNKNOWN implies confidence > 0.5" must
        hold for the LLM tier too, not just the rule tier."""
        mock_llm.return_value = ("STATUS", 0.3)
        assert resolve("hows it going") == ("UNKNOWN", 0.0, "none")
