"""Speech-to-text via faster-whisper - local CTranslate2 Whisper, no API key.

Whisper doesn't expose a single first-class "confidence" field; this module derives one from
each segment's avg_logprob (log-probability of the decoded tokens) and no_speech_prob
(probability the segment is actually silence/noise), converting log-probability to a 0-1ish
probability via exp() and discounting by no_speech_prob. This is documented here rather than
treated as a black box because it's an approximation, not a value faster-whisper guarantees.
"""

from dataclasses import dataclass
import math

import numpy as np
from faster_whisper import WhisperModel


@dataclass(frozen=True)
class TranscriptResult:
    text: str
    confidence: float


class SpeechToText:
    def __init__(self, model_size: str = "base", language: str = "en"):
        """language: forced ISO-639-1 code (default "en"). Without pinning this, Whisper
        auto-detects language per utterance and can mis-detect short/ambiguous clips as an
        entirely different language (observed live: a short "hello" transcribed as Arabic).
        Pass None to restore auto-detection if that's ever actually wanted."""
        self._model = WhisperModel(model_size, device="cpu", compute_type="int8")
        self._language = language

    def transcribe(self, audio: np.ndarray) -> TranscriptResult:
        """audio: int16 mono numpy array containing one whole utterance."""
        float_audio = audio.astype(np.float32) / 32768.0

        segments, _info = self._model.transcribe(float_audio, beam_size=5, language=self._language)
        segments = list(segments)

        if not segments:
            return TranscriptResult(text="", confidence=0.0)

        text = " ".join(segment.text.strip() for segment in segments).strip()
        avg_logprob = sum(s.avg_logprob for s in segments) / len(segments)
        avg_no_speech_prob = sum(s.no_speech_prob for s in segments) / len(segments)

        confidence = math.exp(avg_logprob) * (1.0 - avg_no_speech_prob)
        confidence = max(0.0, min(1.0, confidence))

        return TranscriptResult(text=text, confidence=confidence)
