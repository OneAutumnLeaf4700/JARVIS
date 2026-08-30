"""Unit tests for the LLM fallback classifier. Every test mocks the Ollama HTTP call — this
module must never make a real network call, so the suite stays fast and doesn't depend on a
running Ollama server."""

import json
import urllib.error
from unittest.mock import MagicMock, patch

from llm_backend import llm_classify


def _fake_response(body: dict) -> MagicMock:
    mock_cm = MagicMock()
    mock_cm.__enter__.return_value.read.return_value = json.dumps(body).encode("utf-8")
    return mock_cm


class TestLLMClassifySuccess:
    @patch("llm_backend.urllib.request.urlopen")
    def test_valid_json_reply_is_parsed(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "STATUS", "confidence": 0.82})}
        )
        assert llm_classify("how long you been up") == ("STATUS", 0.82)

    @patch("llm_backend.urllib.request.urlopen")
    def test_model_reports_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "UNKNOWN", "confidence": 0.0})}
        )
        assert llm_classify("the weather today") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_confidence_is_clamped_above_one(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "ECHO", "confidence": 1.5})}
        )
        assert llm_classify("say something") == ("ECHO", 1.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_confidence_is_clamped_below_zero(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "ECHO", "confidence": -0.3})}
        )
        assert llm_classify("say something") == ("ECHO", 0.0)


class TestLLMClassifyDegradesGracefully:
    @patch("llm_backend.urllib.request.urlopen")
    def test_connection_error_returns_unknown(self, mock_urlopen):
        mock_urlopen.side_effect = urllib.error.URLError("connection refused")
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_timeout_returns_unknown(self, mock_urlopen):
        mock_urlopen.side_effect = TimeoutError()
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_malformed_outer_json_returns_unknown(self, mock_urlopen):
        mock_cm = MagicMock()
        mock_cm.__enter__.return_value.read.return_value = b"not json"
        mock_urlopen.return_value = mock_cm
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_malformed_inner_json_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response({"response": "not valid json"})
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_missing_intent_key_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"confidence": 0.9})}
        )
        assert llm_classify("anything") == ("UNKNOWN", 0.0)

    @patch("llm_backend.urllib.request.urlopen")
    def test_unrecognised_intent_name_returns_unknown(self, mock_urlopen):
        mock_urlopen.return_value = _fake_response(
            {"response": json.dumps({"intent": "WEATHER", "confidence": 0.9})}
        )
        assert llm_classify("anything") == ("UNKNOWN", 0.0)
