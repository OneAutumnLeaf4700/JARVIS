"""Microphone capture -> fixed-size int16 audio blocks at 16kHz mono.

wake_word.py and stt.py both consume the frames() / frames_from_wav() generators here and never
touch sounddevice or the wave module directly - this is the only module that knows how audio
actually gets into the process.
"""

from typing import Iterator, Optional
import wave

import numpy as np
import sounddevice as sd

SAMPLE_RATE = 16000


def frames(
    samplerate: int = SAMPLE_RATE,
    blocksize: int = 1280,
    device: Optional[str] = None,
) -> Iterator[np.ndarray]:
    """Yield int16 mono audio blocks from the live microphone, forever, until the caller stops
    iterating (e.g. via `break`)."""
    with sd.InputStream(
        samplerate=samplerate,
        channels=1,
        dtype="int16",
        blocksize=blocksize,
        device=device,
    ) as stream:
        while True:
            data, _overflowed = stream.read(blocksize)
            yield data.reshape(-1)


def frames_from_wav(path: str, blocksize: int = 1280) -> Iterator[np.ndarray]:
    """Yield int16 mono audio blocks read from a wav file, for tests and offline use. Raises
    ValueError if the file isn't 16-bit PCM mono - resampling/format conversion is explicitly
    out of scope for this helper (test fixtures are written in the target format directly)."""
    with wave.open(path, "rb") as wf:
        if wf.getsampwidth() != 2:
            raise ValueError(f"{path}: expected 16-bit PCM audio, got sampwidth={wf.getsampwidth()}")
        if wf.getnchannels() != 1:
            raise ValueError(f"{path}: expected mono audio, got {wf.getnchannels()} channels")

        raw = wf.readframes(wf.getnframes())

    samples = np.frombuffer(raw, dtype=np.int16)
    for start in range(0, len(samples), blocksize):
        yield samples[start : start + blocksize]
