from pathlib import Path

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
