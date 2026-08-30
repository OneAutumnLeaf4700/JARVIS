"""Unit tests for the eval harness's scoring logic. Uses fake classify functions — never
imports the real classifier/LLM/resolver, so this stays fast and Ollama-independent."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS_DIR = ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from eval_understanding import _run


def test_perfect_classifier_scores_full_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ({"a": "X", "b": "Y"}[text], 1.0)

    accuracy, avg_latency_ms = _run("perfect", fake_classify, dataset)
    assert accuracy == 1.0
    assert avg_latency_ms >= 0.0


def test_wrong_classifier_scores_zero_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ("UNKNOWN", 0.0)

    accuracy, _ = _run("wrong", fake_classify, dataset)
    assert accuracy == 0.0


def test_partial_accuracy():
    dataset = [("a", "X"), ("b", "Y")]

    def fake_classify(text):
        return ("X", 1.0) if text == "a" else ("UNKNOWN", 0.0)

    accuracy, _ = _run("partial", fake_classify, dataset)
    assert accuracy == 0.5
