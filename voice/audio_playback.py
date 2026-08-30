"""Blocking audio playback via sounddevice - reused from Voice Input, output instead of input."""

import numpy as np
import sounddevice as sd


def play(audio: np.ndarray, samplerate: int) -> None:
    """Blocks until playback finishes, so the caller doesn't move on mid-sentence."""
    sd.play(audio, samplerate)
    sd.wait()
