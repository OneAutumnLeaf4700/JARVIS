from pathlib import Path

import numpy as np

from voice.audio_capture import frames_from_wav
from voice.wake_word import WakeWordDetector

FIXTURES = Path(__file__).parent / "fixtures"


def test_detects_wake_word_in_present_clip():
    detector = WakeWordDetector(sensitivity=0.5)

    triggered = any(
        detector.detect(frame)
        for frame in frames_from_wav(str(FIXTURES / "wake_word_present.wav"))
    )

    assert triggered is True


def test_does_not_trigger_on_absent_clip():
    detector = WakeWordDetector(sensitivity=0.5)

    triggered = any(
        detector.detect(frame)
        for frame in frames_from_wav(str(FIXTURES / "wake_word_absent.wav"))
    )

    assert triggered is False


def test_reset_clears_stale_buffer_state_after_detection():
    """Regression test: without calling reset() after a real detection, the model's internal
    streaming feature buffer still holds the triggering audio, and detect() can keep firing
    True on subsequent silent frames even though no new wake word was said. reset() must clear
    that state so silence afterward reliably does not trigger."""
    detector = WakeWordDetector(sensitivity=0.5)

    for frame in frames_from_wav(str(FIXTURES / "wake_word_present.wav")):
        detector.detect(frame)

    detector.reset()

    silence = np.zeros(1280, dtype=np.int16)
    post_reset_triggers = [detector.detect(silence) for _ in range(10)]

    assert not any(post_reset_triggers)
