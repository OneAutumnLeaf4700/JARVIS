"""Load and validate voice/voice_config.yaml — see voice_config.example.yaml for the schema."""

from dataclasses import dataclass
import os
from pathlib import Path

import yaml

_VALID_MODES = {"push_to_talk", "always_listen"}

_REPO_ROOT = Path(__file__).resolve().parents[1]
_DEFAULT_CONFIG_PATH = _REPO_ROOT / "voice" / "voice_config.yaml"
_EXAMPLE_CONFIG_PATH = _REPO_ROOT / "voice" / "voice_config.example.yaml"


@dataclass(frozen=True)
class VoiceConfig:
    mode: str
    wake_word_sensitivity: float
    stt_model_size: str
    stt_confidence_threshold: float
    audio_device: str | None


def load_config(path: str | None = None) -> VoiceConfig:
    resolved_path = path or os.environ.get("JARVIS_VOICE_CONFIG") or str(_DEFAULT_CONFIG_PATH)

    if not os.path.isfile(resolved_path):
        raise FileNotFoundError(
            f"Voice config not found at '{resolved_path}'. "
            f"Copy {_EXAMPLE_CONFIG_PATH} to voice/voice_config.yaml and adjust it, "
            "or set JARVIS_VOICE_CONFIG to point elsewhere."
        )

    with open(resolved_path, "r", encoding="utf-8") as f:
        raw = yaml.safe_load(f)

    mode = raw["mode"]
    if mode not in _VALID_MODES:
        raise ValueError(f"Invalid mode '{mode}' — must be one of {sorted(_VALID_MODES)}")

    return VoiceConfig(
        mode=mode,
        wake_word_sensitivity=float(raw["wake_word"]["sensitivity"]),
        stt_model_size=raw["stt"]["model_size"],
        stt_confidence_threshold=float(raw["stt"]["confidence_threshold"]),
        audio_device=raw["audio"]["device"],
    )
