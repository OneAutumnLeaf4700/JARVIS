"""Tests that the AI server's servicer resolves input (via the tiered resolver) and populates
the response fields. The LLM tier is mocked by default (autouse fixture) so this suite stays
offline; individual tests override it to prove the LLM-escalation wiring works end-to-end
through the servicer."""

from unittest.mock import Mock

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

    def test_llm_also_misses_escalates_and_stays_unknown(self, monkeypatch):
        """Proves a rule-miss genuinely triggers escalation to the LLM tier (not just that the
        end result happens to be UNKNOWN either way)."""
        mock_llm = Mock(return_value=("UNKNOWN", 0.0))
        monkeypatch.setattr(resolver, "llm_classify", mock_llm)
        resp = call("the weather today")
        assert resp.success
        assert resp.intent == "UNKNOWN"
        assert resp.confidence == 0.0
        mock_llm.assert_called_once_with("the weather today")
