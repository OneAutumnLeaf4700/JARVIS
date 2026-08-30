# Voice Input ("Ears") Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a thin Python voice client (`voice/`) that captures the microphone, detects a wake
word, transcribes speech locally, and sends the transcript through the existing gRPC pipeline
exactly like `tools/interactive_client.py` does — no changes to `core/` or `ai/`.

**Architecture:** Four small, independently testable modules (`config`, `audio_capture`,
`wake_word`, `stt`) wired together by a thin orchestrator (`voice_client.py`) that mirrors
`tools/interactive_client.py`'s dispatch logic. Push-to-talk and always-listen are both simple
blocking loops — no async/threading.

**Tech Stack:** `sounddevice` (mic capture), `openwakeword` (wake word, local ONNX),
`faster-whisper` (STT, local CTranslate2 Whisper), `pyyaml` (config).

**Spec:** [`docs/superpowers/specs/2026-08-30-voice-input-design.md`](../specs/2026-08-30-voice-input-design.md)

## Global Constraints

- No changes to `core/` (C++) or `ai/` (Python Understanding tier) — this plan only adds files
  under `voice/` and touches `start_jarvis.sh`/`README.md`.
- Audio format: 16kHz mono, `int16` PCM is the format `audio_capture.py` produces and
  `wake_word.py` consumes natively; `stt.py` is responsible for converting to the `float32`
  range faster-whisper expects — no other module does that conversion.
- `voice_config.yaml` is gitignored; `voice_config.example.yaml` is the committed template.
  Real config values (device, sensitivity) never go in git.
- Every module exposes a small class/function interface consumable without a live microphone —
  tests must pass with wav fixtures or mocks only, never require real audio hardware.
- New runtime deps go in `voice/requirements.txt`, not `ai/requirements-dev.txt` (voice is
  optional; installing core JARVIS should not require audio libraries).

---

### Task 1: Voice-only config module

**Files:**
- Create: `voice/__init__.py` (empty)
- Create: `voice/config.py`
- Create: `voice/voice_config.example.yaml`
- Create: `voice/requirements.txt`
- Modify: `.gitignore` (add `voice/voice_config.yaml`)
- Test: `voice/tests/__init__.py` (empty)
- Test: `voice/tests/test_config.py`

**Interfaces:**
- Produces: `voice.config.VoiceConfig` dataclass with fields `mode: str`,
  `wake_word_sensitivity: float`, `stt_model_size: str`, `stt_confidence_threshold: float`,
  `audio_device: str | None`. Produces `voice.config.load_config(path: str | None = None) ->
  VoiceConfig`, which resolves the path in this order: explicit `path` argument, then
  `JARVIS_VOICE_CONFIG` env var, then `voice/voice_config.yaml` relative to the repo root.
  Raises `FileNotFoundError` with a message pointing at `voice_config.example.yaml` if none of
  those exist. Raises `ValueError` if `mode` is not one of `push_to_talk`/`always_listen`.

- [ ] **Step 1: Write the failing tests**

```python
# voice/tests/test_config.py
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m pytest voice/tests/test_config.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice'` (or `voice.config`).

- [ ] **Step 3: Write `voice/voice_config.example.yaml`**

```yaml
# Copy this file to voice_config.yaml (gitignored) and adjust for your machine.
mode: push_to_talk        # push_to_talk | always_listen
wake_word:
  sensitivity: 0.5         # openWakeWord threshold, 0.0-1.0
stt:
  model_size: base          # tiny | base | small | medium
  confidence_threshold: 0.55  # below this -> clarification path, transcript is discarded
audio:
  device: null               # null = system default input device; or a sounddevice device index/name
```

- [ ] **Step 4: Write `voice/requirements.txt`**

```
sounddevice==0.5.1
openwakeword==0.6.0
faster-whisper==1.1.0
pyyaml==6.0.2
```

- [ ] **Step 5: Write `voice/__init__.py` and `voice/tests/__init__.py`**

Both empty files — they exist only so `voice` and `voice.tests` are importable packages.

- [ ] **Step 6: Write minimal `voice/config.py`**

```python
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
```

- [ ] **Step 7: Run tests to verify they pass**

Run: `python3 -m pytest voice/tests/test_config.py -v`
Expected: 4 passed

- [ ] **Step 8: Add the gitignore entry**

Add this line to `.gitignore` (near the existing `.jarvis-*.log` entries):
```
voice/voice_config.yaml
```

- [ ] **Step 9: Commit**

```bash
git add voice/__init__.py voice/config.py voice/voice_config.example.yaml voice/requirements.txt voice/tests/__init__.py voice/tests/test_config.py .gitignore
git commit -m "feat(voice): add voice-only config loader"
```

---

### Task 2: Audio capture module

**Files:**
- Create: `voice/audio_capture.py`
- Test: `voice/tests/test_audio_capture.py`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `voice.audio_capture.frames(samplerate: int = 16000, blocksize: int = 1280, device:
  str | None = None) -> Iterator[np.ndarray]` — a generator yielding `int16` numpy arrays of
  shape `(blocksize,)`, one per audio block, pulled live from the microphone via
  `sounddevice.InputStream`. Also produces `voice.audio_capture.frames_from_wav(path: str,
  blocksize: int = 1280) -> Iterator[np.ndarray]` — same yield shape/dtype, but reads a `.wav`
  file instead of a live device, resampling to 16kHz mono `int16` if the file isn't already in
  that format. This is the seam later tasks/tests use to avoid touching real hardware.

- [ ] **Step 1: Write the failing test**

```python
# voice/tests/test_audio_capture.py
import numpy as np
import wave

from voice.audio_capture import frames_from_wav


def _write_test_wav(path, samplerate=16000, seconds=1.0):
    n_samples = int(samplerate * seconds)
    # A simple 440Hz tone, well within int16 range.
    t = np.linspace(0, seconds, n_samples, endpoint=False)
    tone = (np.sin(2 * np.pi * 440 * t) * 10000).astype(np.int16)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # int16
        wf.setframerate(samplerate)
        wf.writeframes(tone.tobytes())
    return tone


def test_frames_from_wav_yields_int16_blocks(tmp_path):
    wav_path = tmp_path / "tone.wav"
    tone = _write_test_wav(wav_path)

    blocks = list(frames_from_wav(str(wav_path), blocksize=1280))

    assert all(block.dtype == np.int16 for block in blocks)
    reconstructed = np.concatenate(blocks)
    # Last block may be shorter (not a multiple of blocksize) - compare the overlap only.
    assert np.array_equal(reconstructed[: len(tone)], tone)


def test_frames_from_wav_blocksize_shape(tmp_path):
    wav_path = tmp_path / "tone.wav"
    _write_test_wav(wav_path, seconds=0.5)

    blocks = list(frames_from_wav(str(wav_path), blocksize=1280))

    # All but possibly the last block are exactly blocksize long.
    for block in blocks[:-1]:
        assert block.shape == (1280,)
    assert len(blocks[-1]) <= 1280
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest voice/tests/test_audio_capture.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice.audio_capture'`

- [ ] **Step 3: Write `voice/audio_capture.py`**

```python
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest voice/tests/test_audio_capture.py -v`
Expected: 2 passed

- [ ] **Step 5: Commit**

```bash
git add voice/audio_capture.py voice/tests/test_audio_capture.py
git commit -m "feat(voice): add microphone/wav audio capture module"
```

---

### Task 3: Wake word detection module

**Files:**
- Create: `voice/wake_word.py`
- Create: `voice/tests/fixtures/` directory with two committed wav fixtures (see Step 1)
- Test: `voice/tests/test_wake_word.py`

**Interfaces:**
- Consumes: `voice.audio_capture.frames_from_wav` (Task 2) to build test fixtures' frame
  streams.
- Produces: `voice.wake_word.WakeWordDetector` class with constructor
  `WakeWordDetector(sensitivity: float = 0.5, model_name: str = "hey_jarvis")` and method
  `detect(frame: np.ndarray) -> bool`, where `frame` is an `int16` numpy array of the same shape
  `audio_capture.frames()` yields.

- [ ] **Step 1: Generate two short wav fixtures with local TTS (no human recording needed)**

`espeak-ng` is available on this machine and can synthesize the fixtures directly — no
microphone or human recording required. First confirm the exact stock wake-word model name
(the spec flagged this as unconfirmed until implementation time):

```bash
python3 -c "import openwakeword; openwakeword.utils.download_models()"
python3 -c "from openwakeword.model import Model; print(list(Model().models.keys()))"
```

Use whichever listed name is closest to "jarvis" (e.g. `hey_jarvis`) as `model_name`'s default
in Step 4 below. Then synthesize the fixtures at 16kHz mono 16-bit PCM directly:

```bash
mkdir -p voice/tests/fixtures
espeak-ng "hey jarvis" -w voice/tests/fixtures/wake_word_present.wav -s 150
espeak-ng "what is the weather like today" -w voice/tests/fixtures/wake_word_absent.wav -s 150
ffmpeg -y -i voice/tests/fixtures/wake_word_present.wav -ar 16000 -ac 1 -sample_fmt s16 /tmp/p.wav && mv /tmp/p.wav voice/tests/fixtures/wake_word_present.wav
ffmpeg -y -i voice/tests/fixtures/wake_word_absent.wav -ar 16000 -ac 1 -sample_fmt s16 /tmp/a.wav && mv /tmp/a.wav voice/tests/fixtures/wake_word_absent.wav
```

(Swap in whichever wake phrase matches the model name confirmed above if it isn't literally
"jarvis".) **Known risk:** openWakeWord's models are trained on natural human speech; a
robotic TTS voice may score lower than real speech and could fail to cross the sensitivity
threshold reliably. After generating the fixtures, run the model manually against them once
before writing the test:

```bash
python3 -c "
from openwakeword.model import Model
import numpy as np, wave
m = Model()
for name in ('wake_word_present', 'wake_word_absent'):
    with wave.open(f'voice/tests/fixtures/{name}.wav', 'rb') as wf:
        audio = np.frombuffer(wf.readframes(wf.getnframes()), dtype=np.int16)
    scores = [m.predict(audio[i:i+1280]) for i in range(0, len(audio) - 1280, 1280)]
    print(name, max(s[list(s.keys())[0]] for s in scores) if scores else 'no frames')
"
```

If the present clip's max score isn't clearly higher than the absent clip's, the test in Step 2
below should compare **relative** scores between the two clips rather than an absolute
threshold — swap in the relative-comparison version noted inline in Step 2 if so. If synthetic
TTS turns out unusable even for a relative comparison, fall back to asking the user for one
short (~3s) recorded clip saying the wake phrase — flag this back rather than guessing.

- [ ] **Step 2: Write the failing test**

```python
# voice/tests/test_wake_word.py
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
```

**If Step 1's manual scoring showed the absolute-threshold version above is unreliable on
synthetic TTS audio** (present clip doesn't clearly cross 0.5, or the absent clip does), use
this relative-comparison version instead — it only requires the present clip to score higher
than the absent clip, which is robust to a TTS voice generally under-triggering the model:

```python
def test_present_clip_scores_higher_than_absent_clip():
    from openwakeword.model import Model

    model = Model()
    model_name = list(model.models.keys())[0]

    def max_score(wav_name):
        scores = [
            model.predict(frame)[model_name]
            for frame in frames_from_wav(str(FIXTURES / wav_name))
        ]
        return max(scores) if scores else 0.0

    present_score = max_score("wake_word_present.wav")
    absent_score = max_score("wake_word_absent.wav")

    assert present_score > absent_score
```

Use exactly one of the two versions (whichever Step 1's manual check indicated), not both.

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 -m pytest voice/tests/test_wake_word.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice.wake_word'`

- [ ] **Step 4: Write `voice/wake_word.py`**

```python
"""Wake-word detection via openWakeWord — fully local, ONNX model, no account/API key."""

import numpy as np
from openwakeword.model import Model


class WakeWordDetector:
    def __init__(self, sensitivity: float = 0.5, model_name: str = "hey_jarvis"):
        self._model = Model(wakeword_models=[model_name])
        self._model_name = model_name
        self._sensitivity = sensitivity

    def detect(self, frame: np.ndarray) -> bool:
        """frame: int16 mono numpy array, same shape audio_capture.frames() yields."""
        scores = self._model.predict(frame)
        score = scores.get(self._model_name)
        if score is None:
            # Model key naming can vary by openwakeword version - fall back to the only
            # entry if there's exactly one, since we only ever load one model.
            score = next(iter(scores.values()))
        return score >= self._sensitivity
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest voice/tests/test_wake_word.py -v`
Expected: 2 passed

If the stock model name isn't exactly `hey_jarvis` (confirm during Step 1), update the default
in `WakeWordDetector.__init__` to match and re-run.

- [ ] **Step 6: Commit**

```bash
git add voice/wake_word.py voice/tests/test_wake_word.py voice/tests/fixtures/wake_word_present.wav voice/tests/fixtures/wake_word_absent.wav
git commit -m "feat(voice): add openWakeWord-based wake word detector"
```

---

### Task 4: Speech-to-text module

**Files:**
- Create: `voice/stt.py`
- Create: `voice/tests/fixtures/speech_known_transcript.wav` (see Step 1)
- Test: `voice/tests/test_stt.py`

**Interfaces:**
- Consumes: `voice.audio_capture.frames_from_wav` (Task 2).
- Produces: `voice.stt.TranscriptResult` dataclass with fields `text: str`, `confidence:
  float`. Produces `voice.stt.SpeechToText` class with constructor
  `SpeechToText(model_size: str = "base")` and method `transcribe(audio: np.ndarray) ->
  TranscriptResult`, where `audio` is a single concatenated `int16` numpy array (a whole
  utterance, not one block).

- [ ] **Step 1: Generate a wav fixture with a known transcript via local TTS**

No human recording needed — `espeak-ng` is available on this machine. Whisper-family models
are trained partly on synthetic/varied speech and are generally far more robust to a TTS voice
than the wake-word model is (this is a full transcription task, not a narrow trigger-word
match), so this is lower-risk than Task 3's fixture generation:

```bash
espeak-ng "what is the status" -w voice/tests/fixtures/speech_known_transcript.wav -s 150
ffmpeg -y -i voice/tests/fixtures/speech_known_transcript.wav -ar 16000 -ac 1 -sample_fmt s16 /tmp/s.wav && mv /tmp/s.wav voice/tests/fixtures/speech_known_transcript.wav
```

The sentence is "what is the status" — matches `EXPECTED_TRANSCRIPT_SUBSTRING` in Step 2 below.
If you synthesize a different sentence, update that constant to match.

- [ ] **Step 2: Write the failing test**

```python
# voice/tests/test_stt.py
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
```

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 -m pytest voice/tests/test_stt.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice.stt'`

- [ ] **Step 4: Write `voice/stt.py`**

```python
"""Speech-to-text via faster-whisper - local CTranslate2 Whisper, no API key.

Whisper doesn't expose a single first-class "confidence" field; this module derives one from
each segment's avg_logprob (log-probability of the decoded tokens) and no_speech_prob
(probability the segment is actually silence/noise), converting log-probability to a 0-1ish
probability via exp() and discounting by no_speech_prob. This is documented here rather than
treated as a black box because it's an approximation, not a value faster-whisper guarantees.
"""

from dataclasses import dataclass
import math

import numpy as np
from faster_whisper import WhisperModel


@dataclass(frozen=True)
class TranscriptResult:
    text: str
    confidence: float


class SpeechToText:
    def __init__(self, model_size: str = "base"):
        self._model = WhisperModel(model_size, device="cpu", compute_type="int8")

    def transcribe(self, audio: np.ndarray) -> TranscriptResult:
        """audio: int16 mono numpy array containing one whole utterance."""
        float_audio = audio.astype(np.float32) / 32768.0

        segments, _info = self._model.transcribe(float_audio, beam_size=5)
        segments = list(segments)

        if not segments:
            return TranscriptResult(text="", confidence=0.0)

        text = " ".join(segment.text.strip() for segment in segments).strip()
        avg_logprob = sum(s.avg_logprob for s in segments) / len(segments)
        avg_no_speech_prob = sum(s.no_speech_prob for s in segments) / len(segments)

        confidence = math.exp(avg_logprob) * (1.0 - avg_no_speech_prob)
        confidence = max(0.0, min(1.0, confidence))

        return TranscriptResult(text=text, confidence=confidence)
```

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest voice/tests/test_stt.py -v`
Expected: 2 passed

If `test_transcribes_known_speech` fails because the transcript doesn't contain the expected
substring, first check the fixture actually contains clear speech of the sentence you recorded
(play it back) before assuming the confidence math is wrong.

- [ ] **Step 6: Commit**

```bash
git add voice/stt.py voice/tests/test_stt.py voice/tests/fixtures/speech_known_transcript.wav
git commit -m "feat(voice): add faster-whisper speech-to-text module"
```

---

### Task 5: Voice client orchestrator (dispatch + push-to-talk + always-listen + low-confidence handling)

**Files:**
- Create: `voice/voice_client.py`
- Test: `voice/tests/test_voice_client.py`

**Interfaces:**
- Consumes: `voice.config.load_config`/`VoiceConfig` (Task 1), `voice.audio_capture.frames`
  (Task 2), `voice.wake_word.WakeWordDetector` (Task 3), `voice.stt.SpeechToText`/
  `TranscriptResult` (Task 4), and the generated `jarvis_pb2`/`jarvis_pb2_grpc` stubs the same
  way `tools/interactive_client.py` does.
- Produces: `voice.voice_client.build_request(transcript: str) ->
  jarvis_pb2.ExecuteCommandRequest` (the dispatch logic, directly testable without gRPC or
  audio); `voice.voice_client.capture_utterance(frame_iter: Iterator[np.ndarray],
  silence_rms_threshold: float = 300.0, max_silence_blocks: int = 15, max_blocks: int = 150) ->
  np.ndarray` (buffers frames from an iterator until trailing silence or a max length, returns
  the concatenated utterance); a `main()` entry point wiring everything together for
  `python3 voice/voice_client.py` (not unit tested directly — it's the thin orchestration layer
  covered by manual testing per Step 7).

- [ ] **Step 1: Write the failing tests**

```python
# voice/tests/test_voice_client.py
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

from voice.stt import TranscriptResult
from voice.voice_client import build_request, capture_utterance, dispatch_transcript

KNOWN_COMMANDS = {"echo", "status", "about", "help"}


def test_build_request_known_command_routes_directly():
    import jarvis_pb2

    request = build_request("status")

    assert request.command == jarvis_pb2.COMMAND_TYPE_STATUS
    assert request.payload == ""


def test_build_request_known_command_with_payload():
    import jarvis_pb2

    request = build_request("echo hello there")

    assert request.command == jarvis_pb2.COMMAND_TYPE_ECHO
    assert request.payload == "hello there"


def test_build_request_unknown_first_word_sends_full_text():
    import jarvis_pb2

    request = build_request("what does the echo command do")

    assert request.command == jarvis_pb2.COMMAND_TYPE_UNKNOWN
    assert request.payload == "what does the echo command do"


def test_capture_utterance_stops_on_trailing_silence():
    loud = np.full(1280, 5000, dtype=np.int16)
    silent = np.zeros(1280, dtype=np.int16)
    # 3 loud blocks, then enough silent blocks to trip the stop condition.
    frame_sequence = [loud, loud, loud] + [silent] * 16

    result = capture_utterance(
        iter(frame_sequence), silence_rms_threshold=300.0, max_silence_blocks=15, max_blocks=150
    )

    # Should stop shortly after the 15th consecutive silent block, not consume the whole list.
    assert len(result) < len(frame_sequence) * 1280


def test_capture_utterance_respects_max_blocks():
    loud = np.full(1280, 5000, dtype=np.int16)
    frame_sequence = [loud] * 1000  # never goes silent

    result = capture_utterance(iter(frame_sequence), max_blocks=10)

    assert len(result) == 10 * 1280


def test_dispatch_transcript_skips_low_stt_confidence(capsys):
    stub = MagicMock()
    low_confidence_result = TranscriptResult(text="garbled mumble", confidence=0.1)

    dispatch_transcript(low_confidence_result, stub, stt_confidence_threshold=0.55)

    stub.ProcessCommand.assert_not_called()
    captured = capsys.readouterr()
    assert "Didn't catch that clearly" in captured.out


def test_dispatch_transcript_calls_process_command_above_threshold():
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_STATUS, message="Engine: running"
    )
    result = TranscriptResult(text="status", confidence=0.9)

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55)

    stub.ProcessCommand.assert_called_once()


def test_dispatch_transcript_prints_clarification_on_low_ai_confidence(capsys):
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_UNKNOWN,
        message="[detected intent: STATUS, confidence 0.30]",
    )
    result = TranscriptResult(text="some ambiguous mumble", confidence=0.9)

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55, ai_confidence_threshold=0.5)

    captured = capsys.readouterr()
    assert "Not sure I understood" in captured.out
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m pytest voice/tests/test_voice_client.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice.voice_client'`

- [ ] **Step 3: Write `voice/voice_client.py`**

```python
"""Voice client - the microphone/wake-word/STT equivalent of tools/interactive_client.py.

Same architectural role as interactive_client.py: a thin gRPC client that turns some input
(here, transcribed speech instead of typed text) into an ExecuteCommandRequest and calls
JarvisService.ProcessCommand. Everything downstream of that call is the existing, unmodified
pipeline - this file adds no business logic of its own (INV-3).
"""

import argparse
from pathlib import Path
import re
import sys

import grpc
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
GENERATED_PY = ROOT / "generated" / "python"
if str(GENERATED_PY) not in sys.path:
    sys.path.insert(0, str(GENERATED_PY))

import jarvis_pb2
import jarvis_pb2_grpc

from voice.audio_capture import frames
from voice.config import load_config
from voice.stt import SpeechToText, TranscriptResult
from voice.wake_word import WakeWordDetector

KNOWN_COMMANDS = {
    "echo": jarvis_pb2.COMMAND_TYPE_ECHO,
    "status": jarvis_pb2.COMMAND_TYPE_STATUS,
    "about": jarvis_pb2.COMMAND_TYPE_ABOUT,
    "help": jarvis_pb2.COMMAND_TYPE_HELP,
}

_CONFIDENCE_RE = re.compile(r"confidence (\d+\.\d+)")


def build_request(transcript: str) -> "jarvis_pb2.ExecuteCommandRequest":
    """Mirrors tools/interactive_client.py's parse_line(): a known first word routes directly,
    anything else keeps the full transcript as payload for the classify-then-escalate pipeline."""
    stripped = transcript.strip()
    first_word = stripped.split(" ", 1)[0].lower() if stripped else ""

    if first_word in KNOWN_COMMANDS:
        payload = stripped[len(first_word):].strip()
        return jarvis_pb2.ExecuteCommandRequest(command=KNOWN_COMMANDS[first_word], payload=payload)

    return jarvis_pb2.ExecuteCommandRequest(command=jarvis_pb2.COMMAND_TYPE_UNKNOWN, payload=stripped)


def capture_utterance(
    frame_iter,
    silence_rms_threshold: float = 300.0,
    max_silence_blocks: int = 15,
    max_blocks: int = 150,
) -> np.ndarray:
    """Buffer frames from frame_iter until trailing silence (max_silence_blocks consecutive
    quiet blocks, only checked once at least a few blocks have been captured) or max_blocks is
    reached, whichever comes first. Returns the concatenated int16 utterance."""
    buffer = []
    silence_count = 0

    for frame in frame_iter:
        buffer.append(frame)

        rms = float(np.sqrt(np.mean(frame.astype(np.float32) ** 2)))
        if rms < silence_rms_threshold:
            silence_count += 1
        else:
            silence_count = 0

        if len(buffer) >= max_blocks:
            break
        if len(buffer) > 3 and silence_count >= max_silence_blocks:
            break

    if not buffer:
        return np.array([], dtype=np.int16)
    return np.concatenate(buffer)


def dispatch_transcript(
    result: TranscriptResult,
    stub,
    stt_confidence_threshold: float = 0.55,
    ai_confidence_threshold: float = 0.5,
) -> None:
    """Implements the two-signal low-confidence handling from the design spec §6: low STT
    confidence skips the round-trip entirely; low AI/intent confidence (parsed out of the
    existing response message) prints a clarification after the round-trip completes."""
    if result.confidence < stt_confidence_threshold:
        print(f"  Didn't catch that clearly — heard: '{result.text}'. Try again?\n")
        return

    request = build_request(result.text)

    try:
        response = stub.ProcessCommand(request, timeout=10)
    except grpc.RpcError as exc:
        print(f"  [gRPC error: {exc.code()} — {exc.details()}]\n")
        return

    print(f"  command_type: {jarvis_pb2.CommandType.Name(response.command_type)}")
    print(f"  {response.message}\n")

    match = _CONFIDENCE_RE.search(response.message)
    if match and float(match.group(1)) < ai_confidence_threshold:
        print("  Not sure I understood — could you rephrase that?\n")


def _run_push_to_talk(stub, wake_config, stt: SpeechToText, config) -> None:
    print("Push-to-talk mode. Press Enter, then speak. Ctrl+C to quit.\n")
    while True:
        try:
            input("Press Enter to speak > ")
        except (EOFError, KeyboardInterrupt):
            print()
            return

        utterance = capture_utterance(frames(device=config.audio_device))
        result = stt.transcribe(utterance)
        dispatch_transcript(result, stub, config.stt_confidence_threshold)


def _run_always_listen(stub, detector: WakeWordDetector, stt: SpeechToText, config) -> None:
    print("Always-listen mode. Say the wake word, then speak. Ctrl+C to quit.\n")
    frame_iter = frames(device=config.audio_device)
    try:
        for frame in frame_iter:
            if not detector.detect(frame):
                continue

            print("  (wake word detected — listening...)")
            utterance = capture_utterance(frame_iter)
            result = stt.transcribe(utterance)
            dispatch_transcript(result, stub, config.stt_confidence_threshold)
    except KeyboardInterrupt:
        print()
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="JARVIS voice client")
    parser.add_argument("--config", default=None, help="Path to voice_config.yaml")
    args = parser.parse_args()

    config = load_config(args.config)

    channel = grpc.insecure_channel("localhost:50051")
    stub = jarvis_pb2_grpc.JarvisServiceStub(channel)
    stt = SpeechToText(model_size=config.stt_model_size)

    if config.mode == "push_to_talk":
        _run_push_to_talk(stub, None, stt, config)
    else:
        detector = WakeWordDetector(sensitivity=config.wake_word_sensitivity)
        _run_always_listen(stub, detector, stt, config)


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m pytest voice/tests/test_voice_client.py -v`
Expected: 7 passed

- [ ] **Step 5: Commit**

```bash
git add voice/voice_client.py voice/tests/test_voice_client.py
git commit -m "feat(voice): add voice client orchestrator with push-to-talk/always-listen modes"
```

- [ ] **Step 6: Manual smoke test (requires real hardware, do this once implementation lands)**

With `ai/jarvis_ai_server.py` and `build/jarvis_grpc_server` both running (see
`guides/testing-guide.md`), run `python3 voice/voice_client.py` with a real
`voice/voice_config.yaml` (copied from the example) and confirm:
- Push-to-talk mode: pressing Enter then saying "status" prints the real engine status.
- Saying something off-topic prints a sensible `UNKNOWN`/AI-classified response, not a crash.
- Speaking too quietly/unclearly triggers the "Didn't catch that clearly" message and does not
  make a gRPC call (check server-side logs show no new request for that attempt).

This step has no automated pass/fail — record the outcome in the task's review notes.

---

### Task 6: Wire into `start_jarvis.sh` and document setup

**Files:**
- Modify: `start_jarvis.sh`
- Modify: `README.md`

**Interfaces:**
- Consumes: `voice/voice_client.py`'s `main()` (Task 5) as the process this script can launch.
- Produces: nothing consumed by other tasks — this is the last task in the plan.

- [ ] **Step 1: Add a `--voice` flag to `start_jarvis.sh`**

Read the current file first (`cat start_jarvis.sh`) to match its existing argument-parsing and
process-launch style exactly — it already backgrounds the two servers and traps `EXIT` for
cleanup; the only change is which client process runs in the foreground at the end. Add:
- Argument parsing for a `--voice` flag (alongside whatever flags already exist, or as the
  first flag if none do yet).
- When `--voice` is passed, after both servers are confirmed up, run
  `python3 voice/voice_client.py` in the foreground instead of
  `python3 tools/interactive_client.py`.
- Default behavior (no flag) is unchanged — still launches the text client.
- Print a one-line reminder if `voice/voice_config.yaml` doesn't exist yet, pointing at
  `voice/voice_config.example.yaml`, before attempting to launch (fail fast with a clear
  message rather than letting `voice_client.py`'s own `FileNotFoundError` surface as a raw
  traceback in a shell script context).

- [ ] **Step 2: Manually verify the script still works in default (no-flag) mode**

Run: `./start_jarvis.sh` (without `--voice`)
Expected: behaves exactly as before this task — launches the text `interactive_client.py`.
Ctrl+C to exit once confirmed.

- [ ] **Step 3: Manually verify `--voice` mode launches the voice client**

Run: `./start_jarvis.sh --voice` (with `voice/voice_config.yaml` already copied from the
example per Task 5 Step 6)
Expected: both servers start as usual, then `voice/voice_client.py` launches in the foreground
instead of the text client.

- [ ] **Step 4: Add a "Voice (optional)" section to `README.md`**

Read the current README's dependency/setup section first (`grep -n "grpcio\|pip install"
README.md`) to match its existing style. Add a new section documenting:
- Voice support is optional and requires the packages in `voice/requirements.txt` (`pip install
  -r voice/requirements.txt`), separate from core JARVIS's Python deps.
- Copy `voice/voice_config.example.yaml` to `voice/voice_config.yaml` and adjust `mode` and
  `audio.device` for your machine before first use.
- Launch with `./start_jarvis.sh --voice` (or `python3 voice/voice_client.py` directly if the
  two servers are already running some other way).
- First run downloads the `openwakeword` ONNX model and the `faster-whisper` model weights
  (not committed to the repo) — this needs network access once, then works fully offline
  (INV-11).

- [ ] **Step 5: Commit**

```bash
git add start_jarvis.sh README.md
git commit -m "feat(voice): wire voice client into start_jarvis.sh, document setup"
```

---

## Plan self-review notes

- **Spec coverage:** §3 (module layout) → Tasks 1-5. §4.1-4.4 (each component) → Tasks 2-4 and
  part of Task 1. §4.5 (orchestrator) → Task 5. §5 (config-driven mode) → Task 1 (schema) +
  Task 5 (`main()` branching on `config.mode`). §6 (low-confidence handling) → Task 5's
  `dispatch_transcript`. §7 (no `core`/`ai` changes) → enforced by Global Constraints, nothing
  in any task touches those directories. §8 (setup/launch) → Task 6. §9 (testing plan) → a test
  file per component across Tasks 1-5, all fixture/mock-based, no live hardware required except
  the one manual smoke step in Task 5 and the two manual verification steps in Task 6.
- **Fixture generation (Task 3 Step 1, Task 4 Step 1)** uses `espeak-ng` (confirmed present on
  this machine) to synthesize test audio rather than requiring a human recording — this keeps
  the whole plan subagent-executable end to end. Task 3's fixture carries a known accuracy risk
  (a wake-word model trained on human speech may under-score a synthetic voice), handled with a
  manual verification sub-step and a relative-scoring test fallback; if even that proves
  unusable, the step explicitly says to flag back for a user-recorded clip rather than guess
  silently. The only unavoidable manual/human steps are Task 5 Step 6 and Task 6 Steps 2-3
  (live smoke tests against real hardware and a running stack) — those can't be fixture-faked
  and are expected to happen once, after implementation, not blocking task-by-task progress.
