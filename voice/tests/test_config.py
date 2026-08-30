import os
import textwrap

import pytest

from voice.config import VoiceConfig, load_config

VALID_YAML = textwrap.dedent("""\
    mode: push_to_talk
    wake_word:
      sensitivity: 0.6
    stt:
      model_size: small
      confidence_threshold: 0.5
    audio:
      device: null
    """)

INVALID_MODE_YAML = textwrap.dedent("""\
    mode: sleepwalking
    wake_word:
      sensitivity: 0.5
    stt:
      model_size: base
      confidence_threshold: 0.55
    audio:
      device: null
    """)


def test_load_config_from_explicit_path(tmp_path):
    config_file = tmp_path / "voice_config.yaml"
    config_file.write_text(VALID_YAML)

    config = load_config(str(config_file))

    assert config == VoiceConfig(
        mode="push_to_talk",
        wake_word_sensitivity=0.6,
        stt_model_size="small",
        stt_confidence_threshold=0.5,
        audio_device=None,
    )


def test_load_config_from_env_var(tmp_path, monkeypatch):
    config_file = tmp_path / "voice_config.yaml"
    config_file.write_text(VALID_YAML)
    monkeypatch.setenv("JARVIS_VOICE_CONFIG", str(config_file))

    config = load_config()

    assert config.mode == "push_to_talk"


def test_load_config_missing_file_raises(tmp_path, monkeypatch):
    monkeypatch.delenv("JARVIS_VOICE_CONFIG", raising=False)
    missing_path = str(tmp_path / "does_not_exist.yaml")

    with pytest.raises(FileNotFoundError, match="voice_config.example.yaml"):
        load_config(missing_path)


def test_load_config_invalid_mode_raises(tmp_path):
    config_file = tmp_path / "voice_config.yaml"
    config_file.write_text(INVALID_MODE_YAML)

    with pytest.raises(ValueError, match="mode"):
        load_config(str(config_file))
