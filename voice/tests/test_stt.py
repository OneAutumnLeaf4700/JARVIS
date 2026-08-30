from pathlib import Path
from unittest.mock import MagicMock, patch

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


def test_transcribe_defaults_to_english_language():
    # Regression: without pinning a language, Whisper auto-detects per utterance and can
    # mis-detect short/ambiguous clips as a different language entirely (observed live:
    # a short "hello" transcribed as Arabic). Pinning "en" removes that guesswork.
    with patch("voice.stt.WhisperModel") as MockModel:
        mock_instance = MockModel.return_value
        mock_instance.transcribe.return_value = ([], {})

        stt = SpeechToText(model_size="base")
        stt.transcribe(np.zeros(1600, dtype=np.int16))

        _, kwargs = mock_instance.transcribe.call_args
        assert kwargs.get("language") == "en"


def test_transcribe_language_is_configurable():
    with patch("voice.stt.WhisperModel") as MockModel:
        mock_instance = MockModel.return_value
        mock_instance.transcribe.return_value = ([], {})

        stt = SpeechToText(model_size="base", language="fr")
        stt.transcribe(np.zeros(1600, dtype=np.int16))

        _, kwargs = mock_instance.transcribe.call_args
        assert kwargs.get("language") == "fr"
