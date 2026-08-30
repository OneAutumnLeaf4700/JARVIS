"""The tiered Understanding-layer resolver — the single entry point jarvis_ai_server.py calls.

Tries the deterministic rule classifier first (fast, free, high-precision); only escalates to
the local LLM when the rules draw a blank. Keeping this as the one fixed entry point is what lets
a future third tier (e.g. escalating further for genuinely complex requests) slot in later
without touching the gRPC server or anything on the C++ side.
"""

from __future__ import annotations

from intent_classifier import classify
from llm_backend import llm_classify


def resolve(text: str) -> tuple[str, float, str]:
    """Classify `text`. Returns (intent, confidence, tier) — tier is "rule", "llm", or "none"."""
    intent, confidence = classify(text)
    if intent != "UNKNOWN":
        return intent, confidence, "rule"

    intent, confidence = llm_classify(text)
    if intent != "UNKNOWN":
        return intent, confidence, "llm"

    return "UNKNOWN", 0.0, "none"
