"""Blocking audio playback via sounddevice - reused from Voice Input, output instead of input."""

import numpy as np
import sounddevice as sd


def play(audio: np.ndarray, samplerate: int) -> None:
    """Blocks until playback finishes, so the caller doesn't move on mid-sentence.

    Known limitation: sd.wait() has no timeout, so a wedged audio device would block
    indefinitely; not addressed here, flagged for a future pass.
    """
    sd.play(audio, samplerate)
    sd.wait()
