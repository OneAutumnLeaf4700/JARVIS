from pathlib import Path
from unittest.mock import MagicMock

import numpy as np
import pytest

from voice.tts import SynthesisResult, TextToSpeech, resolve_voice_model_path

VOICE_MODEL_PATH = str(Path(__file__).parent / "fixtures" / "en_US-lessac-low.onnx")


def test_synthesize_returns_nonempty_int16_audio():
    tts = TextToSpeech(VOICE_MODEL_PATH)

    result = tts.synthesize("hello")

    assert isinstance(result, SynthesisResult)
    assert result.audio.dtype == np.int16
    assert len(result.audio) > 0
    assert result.samplerate > 0


def test_speak_calls_default_player_with_synthesis_result():
    tts = TextToSpeech(VOICE_MODEL_PATH)
    mock_player = MagicMock()

    tts.speak("hello", player=mock_player)

    mock_player.assert_called_once()
    called_audio, called_samplerate = mock_player.call_args[0]
    assert isinstance(called_audio, np.ndarray)
    assert called_audio.dtype == np.int16
    assert called_samplerate > 0


def test_speak_uses_audio_playback_play_when_no_player_given(monkeypatch):
    import voice.tts as tts_module

    mock_play = MagicMock()
    monkeypatch.setattr(tts_module, "_default_play", mock_play)

    tts = TextToSpeech(VOICE_MODEL_PATH)
    tts.speak("hello")

    mock_play.assert_called_once()


def test_resolve_voice_model_path_builds_expected_path(tmp_path, monkeypatch):
    import voice.tts as tts_module

    models_dir = tmp_path / "tts_models"
    models_dir.mkdir()
    (models_dir / "en_US-lessac-medium.onnx").touch()
    monkeypatch.setattr(tts_module, "_TTS_MODELS_DIR", models_dir)

    path = resolve_voice_model_path("en_US-lessac-medium")

    assert path.endswith("en_US-lessac-medium.onnx")
    assert "tts_models" in path


def test_resolve_voice_model_path_raises_clear_error_when_missing(tmp_path, monkeypatch):
    import voice.tts as tts_module

    monkeypatch.setattr(tts_module, "_TTS_MODELS_DIR", tmp_path)

    with pytest.raises(FileNotFoundError, match="en_US-lessac-medium"):
        resolve_voice_model_path("en_US-lessac-medium")
