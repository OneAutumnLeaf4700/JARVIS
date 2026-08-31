"""LLM-backed intent classification — the escalation tier of the Understanding layer.

Consulted only when the rule-based classifier (intent_classifier.classify) can't match the
input. Calls a local Ollama server; any failure (unreachable, slow, malformed reply) degrades to
("UNKNOWN", 0.0) rather than raising — a dead/slow LLM must never block the caller.
"""

from __future__ import annotations

import json
import logging
import time
import urllib.error
import urllib.request

logger = logging.getLogger("jarvis_ai_server.llm_backend")

OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "llama3.2:latest"
TIMEOUT_SECONDS = 3.0

KNOWN_INTENTS = {"STATUS", "ECHO", "ABOUT", "SYSTEM_INFO"}

PROMPT_TEMPLATE = """You are an intent classifier for a personal assistant called JARVIS. \
JARVIS currently understands exactly these commands:

- STATUS: the user is asking about JARVIS's uptime, whether it is running, or its current state.
- ECHO: the user wants JARVIS to repeat/say something back.
- ABOUT: the user is asking who or what JARVIS is.
- SYSTEM_INFO: the user is asking about the local machine's operating system, architecture,
  compiler, or hardware threads.

If the user is asking a question about one of these commands (e.g. what it does, how it \
works) rather than actually using it, that is NOT a match for that command — respond \
UNKNOWN instead.

Given the user's message below, decide which single intent it matches, or UNKNOWN if it \
matches none of them. Respond with ONLY a JSON object of the exact form:
{{"intent": "STATUS" | "ECHO" | "ABOUT" | "SYSTEM_INFO" | "UNKNOWN", "confidence": <number between 0.0 and 1.0>}}

User message: {text}
"""


def llm_classify(text: str) -> tuple[str, float]:
    """Ask the local Ollama model to classify `text`. Returns ("UNKNOWN", 0.0) on any failure."""
    payload = json.dumps({
        "model": MODEL,
        "prompt": PROMPT_TEMPLATE.format(text=text),
        "format": "json",
        "stream": False,
    }).encode("utf-8")

    request = urllib.request.Request(
        OLLAMA_URL,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    start = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
            outer = json.loads(response.read().decode("utf-8"))
        parsed = json.loads(outer["response"])
        intent = parsed["intent"]
        confidence = float(parsed["confidence"])
    except (urllib.error.URLError, TimeoutError, OSError, KeyError, ValueError, TypeError,
            json.JSONDecodeError) as exc:
        latency_ms = (time.monotonic() - start) * 1000
        logger.warning(
            "llm_classify failed: %s: %s latency_ms=%.2f",
            type(exc).__name__, exc, latency_ms,
        )
        return "UNKNOWN", 0.0

    latency_ms = (time.monotonic() - start) * 1000

    if intent == "UNKNOWN":
        logger.info("llm_classify intent=UNKNOWN latency_ms=%.2f", latency_ms)
        return "UNKNOWN", 0.0

    if intent not in KNOWN_INTENTS:
        logger.warning(
            "llm_classify returned unrecognised intent=%r latency_ms=%.2f", intent, latency_ms,
        )
        return "UNKNOWN", 0.0

    confidence = max(0.0, min(1.0, confidence))
    logger.info(
        "llm_classify intent=%s confidence=%.2f latency_ms=%.2f", intent, confidence, latency_ms,
    )
    return intent, confidence
