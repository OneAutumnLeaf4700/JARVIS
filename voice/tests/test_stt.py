from pathlib import Path

import numpy as np

from voice.audio_capture import frames_from_wav
from voice.stt import SpeechToText

FIXTURES = Path(__file__).parent / "fixtures"

# Update this to match exactly what was spoken in Step 1's recording.
EXPECTED_TRANSCRIPT_SUBSTRING = "status"


def _load_full_clip(wav_name):
    return np.concatenate(list(frames_from_wav(str(FIXTURES / wav_name))))


def test_transcribes_known_speech():
    stt = SpeechToText(model_size="base")
    audio = _load_full_clip("speech_known_transcript.wav")

    result = stt.transcribe(audio)

    assert EXPECTED_TRANSCRIPT_SUBSTRING in result.text.lower()
    assert 0.0 <= result.confidence <= 1.0
    assert result.confidence > 0.3  # clear, deliberate speech should score reasonably high


def test_transcribes_silence_with_low_confidence():
    stt = SpeechToText(model_size="base")
    silence = np.zeros(16000 * 2, dtype=np.int16)  # 2 seconds of digital silence

    result = stt.transcribe(silence)

    assert result.confidence < 0.3
