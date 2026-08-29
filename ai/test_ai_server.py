"""Tests that the AI server's servicer classifies input and populates the response fields."""

import pytest

# Importing the server module sets up sys.path for the generated stubs and pulls in ai_pb2.
from jarvis_ai_server import JarvisAIServicer, ai_pb2


def call(text):
    servicer = JarvisAIServicer()
    request = ai_pb2.NaturalLanguageRequest(text=text)
    return servicer.ProcessNaturalLanguage(request, context=None)


class TestServicerClassifies:
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
