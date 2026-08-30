"""Evaluation harness for the Understanding tier (INV-13) — measures accuracy and latency of
each classification approach against a small labeled dataset.

Run standalone: `python3 tools/eval_understanding.py`. The rule-only row needs nothing extra;
the llm-only and hybrid rows need a running local Ollama server (see ai/llm_backend.py).
"""

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

# (text, expected_intent) — includes phrasings the rule classifier is expected to miss but a
# reasonable LLM should catch, so the three configurations below are meaningfully different.
DATASET = [
    ("status", "STATUS"),
    ("what's your uptime", "STATUS"),
    ("how long have you been running", "STATUS"),
    ("are you alive", "STATUS"),
    ("hows it going", "STATUS"),
    ("you doing okay up there", "STATUS"),
    ("echo this back", "ECHO"),
    ("repeat after me", "ECHO"),
    ("say hello", "ECHO"),
    ("can you repeat what I just said", "ECHO"),
    ("about", "ABOUT"),
    ("who are you", "ABOUT"),
    ("what is jarvis", "ABOUT"),
    ("tell me what you are", "ABOUT"),
    ("what is the meaning of life", "UNKNOWN"),
    ("the weather today", "UNKNOWN"),
]


def _run(label, classify_fn, dataset):
    """Run `classify_fn` over `dataset`, print and return (accuracy, avg_latency_ms)."""
    correct = 0
    total_latency_ms = 0.0
    for text, expected in dataset:
        start = time.monotonic()
        intent, _confidence = classify_fn(text)
        total_latency_ms += (time.monotonic() - start) * 1000
        if intent == expected:
            correct += 1
    accuracy = correct / len(dataset)
    avg_latency_ms = total_latency_ms / len(dataset)
    print(f"{label:10s} accuracy={accuracy:6.1%}  avg_latency_ms={avg_latency_ms:8.2f}")
    return accuracy, avg_latency_ms


def _resolve_intent_only(text):
    intent, confidence, _tier = resolve(text)
    return intent, confidence


def main():
    print(f"Evaluating {len(DATASET)} labeled examples across three configurations:\n")
    _run("rule-only", classify, DATASET)
    _run("llm-only", llm_classify, DATASET)
    _run("hybrid", _resolve_intent_only, DATASET)


if __name__ == "__main__":
    main()
