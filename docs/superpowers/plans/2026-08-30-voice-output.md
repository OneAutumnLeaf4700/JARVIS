# Voice Output ("Voice") Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add local text-to-speech (Piper) and playback (`sounddevice`) to the voice client, so
`dispatch_transcript` speaks its responses and clarification messages alongside the existing
terminal output — completing the voice loop Voice Input ("Ears") started.

**Architecture:** Two small modules mirroring Ears' shape — `voice/tts.py` (Piper synthesis)
and `voice/audio_playback.py` (thin `sounddevice` playback wrapper) — wired into
`voice_client.py`'s existing `dispatch_transcript` as an additive render call alongside each
`print()`, never replacing it.

**Tech Stack:** Piper (ONNX-based local TTS — exact PyPI package name to be confirmed at Task 1,
see its verification step), `sounddevice` (already a dependency from Ears, reused for output).

**Spec:** [`docs/superpowers/specs/2026-08-30-voice-output-design.md`](../specs/2026-08-30-voice-output-design.md)

## Global Constraints

- No changes to `core/`, `ai/`, or `tools/interactive_client.py` — this plan only adds/modifies
  files under `voice/`, plus `README.md` and `voice/voice_config.example.yaml`.
- TTS is additive: every existing `print()` call in `dispatch_transcript` stays exactly as-is;
  a `tts.speak(...)` call is added alongside it, never replacing it.
- `tts.enabled: false` (or omitting the whole `tts` section, for backward compatibility with an
  existing `voice_config.yaml`) must mean `main()` never constructs a `TextToSpeech` at all —
  no speaker, no downloaded voice model, and no `FileNotFoundError` required to use push-to-talk
  or always-listen text-only.
- Every module must be testable without real audio hardware — `audio_playback.play()` is the
  sole deliberate exception (documented, not tested, same precedent as `audio_capture.frames()`'s
  live-mic path).
- New runtime deps go in `voice/requirements.txt`, matching Ears' existing convention.

---

### Task 1: TTS synthesis + playback modules

**Files:**
- Create: `voice/tts.py`
- Create: `voice/audio_playback.py`
- Modify: `voice/requirements.txt` (add the Piper package)
- Test: `voice/tests/test_tts.py`

**Interfaces:**
- Consumes: nothing from earlier Voice Output tasks (this is the first one). Does NOT import
  from `voice/voice_client.py`, `voice/config.py`, or any other Ears module.
- Produces: `voice.tts.SynthesisResult` dataclass with fields `audio: np.ndarray` (int16 mono
  PCM) and `samplerate: int`. `voice.tts.TextToSpeech` class with constructor
  `TextToSpeech(voice_model_path: str)` and methods `synthesize(text: str) -> SynthesisResult`
  and `speak(text: str, player: Callable[[np.ndarray, int], None] | None = None) -> None`
  (calls `synthesize()` then hands the result to `player` if given, else to
  `voice.audio_playback.play`). Produces `voice.audio_playback.play(audio: np.ndarray,
  samplerate: int) -> None`.

- [ ] **Step 1: Confirm the real Piper package name and API**

Piper's Python packaging has moved between names historically — don't assume. Run:

```bash
.venv/bin/pip index versions piper-tts 2>&1 | head -5 || echo "piper-tts not found on index"
.venv/bin/pip install piper-tts
.venv/bin/python -c "import piper; print(piper.__file__)"
.venv/bin/python -c "from piper import PiperVoice; help(PiperVoice.load)"
.venv/bin/python -c "from piper import PiperVoice; help(PiperVoice.synthesize_stream_raw)"
```

If `piper-tts` isn't the right package (check PyPI directly if the above fails), find the
correct one and use it instead — but keep the module's own public interface
(`TextToSpeech`/`SynthesisResult`/`play`, as specified above) exactly as this task defines,
regardless of what Piper's actual internal API turns out to be. Everything below assumes
`PiperVoice.load(model_path) -> PiperVoice` and `voice_obj.synthesize_stream_raw(text) ->
Iterator[bytes]` (raw int16 PCM chunks) plus `voice_obj.config.sample_rate: int` — adjust the
implementation in Step 5 to match whatever you actually find, and note any deviation in your
report.

- [ ] **Step 2: Download a small voice model for testing**

Piper voice models are not bundled — find and download the smallest available English voice
(favor one like `en_US-lessac-low` or similar over `-medium`/`-high` purely for faster test
runs; exact naming may differ, check what's actually available):

```bash
.venv/bin/python -m piper.download_voices en_US-lessac-low 2>&1 | tail -20 || \
  echo "download_voices module not found — check piper's actual CLI/docs for the right download mechanism"
```

If that exact command doesn't exist, find Piper's real download mechanism (a CLI entry point,
or a documented direct URL under `rhasspy/piper-voices` on Hugging Face) and use that instead.
Note the exact model file paths (both the `.onnx` and its matching `.onnx.json` config) you end
up with — Step 4's test needs them.

- [ ] **Step 3: Write `voice/audio_playback.py`**

```python
"""Blocking audio playback via sounddevice - reused from Voice Input, output instead of input."""

import numpy as np
import sounddevice as sd


def play(audio: np.ndarray, samplerate: int) -> None:
    """Blocks until playback finishes, so the caller doesn't move on mid-sentence."""
    sd.play(audio, samplerate)
    sd.wait()
```

- [ ] **Step 4: Write the failing tests**

```python
# voice/tests/test_tts.py
from pathlib import Path
from unittest.mock import MagicMock

import numpy as np

from voice.tts import SynthesisResult, TextToSpeech

# Update these two paths to match whatever Step 2 actually downloaded.
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
```

Place the downloaded voice model files (from Step 2) at
`voice/tests/fixtures/en_US-lessac-low.onnx` and its matching `.onnx.json` — adjust
`VOICE_MODEL_PATH` above if you used a different voice name. These are small binary model files
(a few MB); commit them the same way Voice Input committed its wav fixtures.

- [ ] **Step 5: Run tests to verify they fail**

Run: `.venv/bin/python -m pytest voice/tests/test_tts.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'voice.tts'`

- [ ] **Step 6: Write `voice/tts.py`**

```python
"""Text-to-speech via Piper - local ONNX-based TTS, no API key.

Piper synthesizes at whatever samplerate the loaded voice model specifies (commonly 16kHz or
22.05kHz depending on the voice) - always read from the voice's own config, never assumed.
"""

from dataclasses import dataclass
from typing import Callable, Optional

import numpy as np
from piper import PiperVoice

from voice.audio_playback import play as _default_play


@dataclass(frozen=True)
class SynthesisResult:
    audio: np.ndarray  # int16 mono PCM
    samplerate: int


class TextToSpeech:
    def __init__(self, voice_model_path: str):
        self._voice = PiperVoice.load(voice_model_path)

    def synthesize(self, text: str) -> SynthesisResult:
        chunks = list(self._voice.synthesize_stream_raw(text))
        raw = b"".join(chunks)
        audio = np.frombuffer(raw, dtype=np.int16)
        return SynthesisResult(audio=audio, samplerate=self._voice.config.sample_rate)

    def speak(
        self,
        text: str,
        player: Optional[Callable[[np.ndarray, int], None]] = None,
    ) -> None:
        result = self.synthesize(text)
        player_fn = player if player is not None else _default_play
        player_fn(result.audio, result.samplerate)
```

If Step 1 found a different real API shape than assumed here (different method name than
`synthesize_stream_raw`, different config attribute path than `.config.sample_rate`, etc.),
adjust this implementation to match reality — keep the module's public interface
(`SynthesisResult`, `TextToSpeech.__init__(voice_model_path)`, `.synthesize()`, `.speak()`)
exactly as specified, but the internals must reflect what the installed package actually does.

- [ ] **Step 7: Run tests to verify they pass**

Run: `.venv/bin/python -m pytest voice/tests/test_tts.py -v`
Expected: 3 passed

- [ ] **Step 8: Add the confirmed package to requirements**

Add the exact package name and version you confirmed in Step 1 to `voice/requirements.txt`
(matching the existing `name==version` format the file already uses).

- [ ] **Step 9: Commit**

```bash
git add voice/tts.py voice/audio_playback.py voice/requirements.txt voice/tests/test_tts.py voice/tests/fixtures/en_US-lessac-low.onnx voice/tests/fixtures/en_US-lessac-low.onnx.json
git commit -m "feat(voice): add Piper TTS synthesis and playback modules"
```
(Adjust the fixture filenames in this command if Step 2 used a different voice name.)

---

### Task 2: Config additions for TTS

**Files:**
- Modify: `voice/config.py`
- Modify: `voice/voice_config.example.yaml`
- Test: `voice/tests/test_config.py`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `VoiceConfig` gains two new fields — `tts_enabled: bool` and `tts_voice: str` —
  alongside its existing five. `load_config()`'s behavior for existing config files (no `tts`
  section at all) must default `tts_enabled=True` and `tts_voice="en_US-lessac-medium"` (the
  spec's documented default voice — a reasonable general-purpose choice; Task 1's test fixture
  voice is a separate, smaller one chosen only for fast tests, not this default).

- [ ] **Step 1: Write the failing tests**

```python
# add to voice/tests/test_config.py — do not remove any existing test in this file
def test_load_config_tts_defaults_when_omitted(tmp_path):
    # VALID_YAML (defined earlier in this file) has no tts section at all — an existing
    # user's voice_config.yaml (written before this field existed) must keep working.
    config_file = tmp_path / "voice_config.yaml"
    config_file.write_text(VALID_YAML)

    config = load_config(str(config_file))

    assert config.tts_enabled is True
    assert config.tts_voice == "en_US-lessac-medium"


def test_load_config_tts_is_overridable(tmp_path):
    config_file = tmp_path / "voice_config.yaml"
    config_file.write_text(
        VALID_YAML + "tts:\n  enabled: false\n  voice: en_US-amy-medium\n"
    )

    config = load_config(str(config_file))

    assert config.tts_enabled is False
    assert config.tts_voice == "en_US-amy-medium"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `.venv/bin/python -m pytest voice/tests/test_config.py -k tts -v`
Expected: FAIL — `VoiceConfig` has no `tts_enabled` field yet (existing tests that construct
`VoiceConfig(...)` explicitly, like `test_load_config_from_explicit_path`, will also start
failing at this point since the dataclass gains required fields — that's expected and Step 4
fixes it too).

- [ ] **Step 3: Update `voice/config.py`**

Add to the `VoiceConfig` dataclass (after the existing `stt_language` field, before
`audio_device`):

```python
    tts_enabled: bool
    tts_voice: str
```

Add to `load_config()`'s return, after the existing `stt_language` line:

```python
        tts_enabled=bool(raw.get("tts", {}).get("enabled", True)),
        tts_voice=raw.get("tts", {}).get("voice", "en_US-lessac-medium"),
```

- [ ] **Step 4: Update the existing exact-equality test**

`test_load_config_from_explicit_path` in `voice/tests/test_config.py` constructs a full
`VoiceConfig(...)` and asserts equality — add the two new fields to that constructor call
(`tts_enabled=True, tts_voice="en_US-lessac-medium"`, matching the defaults since `VALID_YAML`
has no `tts` section) so it still passes.

- [ ] **Step 5: Run tests to verify they pass**

Run: `.venv/bin/python -m pytest voice/tests/test_config.py -v`
Expected: all tests in the file pass (existing ones plus the 2 new ones)

- [ ] **Step 6: Update `voice/voice_config.example.yaml`**

Add, after the existing `stt:` section:

```yaml
tts:
  enabled: true             # false = voice-in, text-out only — no speaker/voice model needed
  voice: en_US-lessac-medium  # a Piper voice model name; see README for how to download one
```

- [ ] **Step 7: Commit**

```bash
git add voice/config.py voice/voice_config.example.yaml voice/tests/test_config.py
git commit -m "feat(voice): add tts.enabled/tts.voice to voice config"
```

---

### Task 3: Wire TTS into `dispatch_transcript` and the run loops

**Files:**
- Modify: `voice/voice_client.py`
- Test: `voice/tests/test_voice_client.py`

**Interfaces:**
- Consumes: `voice.tts.TextToSpeech` (Task 1), `VoiceConfig.tts_enabled`/`.tts_voice`
  (Task 2).
- Produces: `dispatch_transcript` gains an optional `tts: TextToSpeech | None = None` parameter,
  fifth in its signature (after the existing `stt_confidence_threshold`, `ai_confidence_threshold`,
  `verbose` — matching the order those were added in). `main()` constructs a `TextToSpeech` (or
  `None`) and threads it through `_run_push_to_talk`/`_run_always_listen` the same way `verbose`
  already flows.

- [ ] **Step 1: Write the failing tests**

Add to `voice/tests/test_voice_client.py`:

```python
from voice.tts import SynthesisResult  # add to the existing import block if not already there


def test_dispatch_transcript_speaks_response_above_threshold():
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_STATUS, message="Engine: running"
    )
    result = TranscriptResult(text="status", confidence=0.9)
    tts = MagicMock()

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55, tts=tts)

    tts.speak.assert_called_once_with("Engine: running")


def test_dispatch_transcript_speaks_stt_low_confidence_message():
    stub = MagicMock()
    low_confidence_result = TranscriptResult(text="garbled mumble", confidence=0.1)
    tts = MagicMock()

    dispatch_transcript(low_confidence_result, stub, stt_confidence_threshold=0.55, tts=tts)

    tts.speak.assert_called_once()
    spoken_text = tts.speak.call_args[0][0]
    assert "garbled mumble" in spoken_text


def test_dispatch_transcript_speaks_ai_low_confidence_clarification():
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_UNKNOWN,
        message="[detected intent: STATUS, confidence 0.30]",
    )
    result = TranscriptResult(text="some ambiguous mumble", confidence=0.9)
    tts = MagicMock()

    dispatch_transcript(
        result, stub, stt_confidence_threshold=0.55, ai_confidence_threshold=0.5, tts=tts
    )

    # Two speak() calls: the dispatched response, then the clarification.
    assert tts.speak.call_count == 2
    second_call_text = tts.speak.call_args_list[1][0][0]
    assert "Not sure I understood" in second_call_text


def test_dispatch_transcript_with_no_tts_makes_no_speak_calls():
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_STATUS, message="Engine: running"
    )
    result = TranscriptResult(text="status", confidence=0.9)

    # tts defaults to None — must not raise, must not attempt to speak.
    dispatch_transcript(result, stub, stt_confidence_threshold=0.55)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `.venv/bin/python -m pytest voice/tests/test_voice_client.py -k tts -v`
Expected: FAIL — `dispatch_transcript()` doesn't accept a `tts` keyword argument yet.

- [ ] **Step 3: Update `dispatch_transcript` in `voice/voice_client.py`**

Change the signature (add `tts` as the last parameter):

```python
def dispatch_transcript(
    result: TranscriptResult,
    stub,
    stt_confidence_threshold: float = 0.55,
    ai_confidence_threshold: float = 0.5,
    verbose: bool = False,
    tts=None,
) -> None:
```

In the low-STT-confidence branch, add a `speak()` call alongside the existing `print()`:

```python
    if result.confidence < stt_confidence_threshold:
        message = f"  Didn't catch that clearly — heard: '{result.text}'. Try again?\n"
        print(message)
        if tts is not None:
            tts.speak(f"Didn't catch that clearly. Heard: {result.text}. Try again?")
        return
```

After the two existing response `print()` calls (`command_type` and `response.message`), add:

```python
    if tts is not None:
        tts.speak(response.message)
```

In the AI-low-confidence branch, add a `speak()` call alongside its existing `print()`:

```python
    match = _CONFIDENCE_RE.search(response.message)
    if match and float(match.group(1)) < ai_confidence_threshold:
        clarification = "  Not sure I understood — could you rephrase that?\n"
        print(clarification)
        if tts is not None:
            tts.speak("Not sure I understood. Could you rephrase that?")
```

(Note: the spoken text is a plain-language version, not the raw `print()` string with its
leading spaces/formatting — TTS shouldn't read out literal indentation or markdown-style
punctuation.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `.venv/bin/python -m pytest voice/tests/test_voice_client.py -v`
Expected: all tests pass, including the 4 new ones

- [ ] **Step 5: Thread `tts` through the run loops and `main()`**

Update `_run_push_to_talk` and `_run_always_listen` signatures to accept `tts=None` and pass it
to their `dispatch_transcript(...)` calls, the same way `verbose` already flows through both.

In `main()`, after constructing `stt`, add:

```python
    tts = None
    if config.tts_enabled:
        from voice.tts import TextToSpeech
        tts = TextToSpeech(config.tts_voice)
```

Note: this imports `TextToSpeech` lazily (inside the `if`), not at module top-level alongside
the other `from voice.X import Y` lines at the top of the file — so a user running with
`tts.enabled: false` never pays Piper's import/model-loading cost at all, consistent with the
Global Constraint that disabling TTS requires no speaker/model. Pass `tts=tts` into both
`_run_push_to_talk(...)` and `_run_always_listen(...)` calls.

- [ ] **Step 6: Manual smoke check (no automated test — needs real audio)**

This step has no pass/fail assertion; note the outcome in your report. With
`voice/voice_config.yaml` set to `tts.enabled: true` and a real voice model downloaded at the
path `tts_voice` resolves to (see Task 4 for exactly how that resolution/download works — if
Task 4 hasn't landed yet when you reach this step, it's fine to skip this manual check and note
that it's deferred), run the full stack and confirm a dispatched command produces audible
speech, not just text.

- [ ] **Step 7: Commit**

```bash
git add voice/voice_client.py voice/tests/test_voice_client.py
git commit -m "feat(voice): wire TTS into dispatch_transcript and the run loops"
```

---

### Task 4: Voice model resolution, README, and setup docs

**Files:**
- Modify: `voice/tts.py` (voice-name-to-path resolution — see Step 1)
- Modify: `README.md`
- Test: `voice/tests/test_tts.py` (extend)

**Interfaces:**
- Consumes: `TextToSpeech.__init__(voice_model_path: str)` from Task 1 — unchanged signature.
- Produces: a new `voice.tts.resolve_voice_model_path(voice_name: str) -> str` helper that
  `main()` (Task 3) should actually be calling instead of passing `config.tts_voice` straight
  through as a raw path — this task closes that gap. `TextToSpeech.__init__` itself keeps
  taking a literal path; the resolution step happens at the call site.

Task 3's `main()` currently does `TextToSpeech(config.tts_voice)`, treating the config's voice
*name* (e.g. `"en_US-lessac-medium"`) as if it were already a file path. This task fixes that by
adding a resolution step: voice model files live in a local cache directory
(`voice/tts_models/`, gitignored — same treatment as `voice_config.yaml`), and a voice *name*
maps to `voice/tts_models/<name>.onnx`.

- [ ] **Step 1: Write the failing test**

```python
# add to voice/tests/test_tts.py
from voice.tts import resolve_voice_model_path


def test_resolve_voice_model_path_builds_expected_path():
    path = resolve_voice_model_path("en_US-lessac-medium")

    assert path.endswith("en_US-lessac-medium.onnx")
    assert "tts_models" in path


def test_resolve_voice_model_path_raises_clear_error_when_missing(tmp_path, monkeypatch):
    import voice.tts as tts_module

    monkeypatch.setattr(tts_module, "_TTS_MODELS_DIR", tmp_path)

    with pytest.raises(FileNotFoundError, match="en_US-lessac-medium"):
        resolve_voice_model_path("en_US-lessac-medium")
```

(Add `import pytest` to the top of `voice/tests/test_tts.py` if not already present.)

- [ ] **Step 2: Run tests to verify they fail**

Run: `.venv/bin/python -m pytest voice/tests/test_tts.py -k resolve -v`
Expected: FAIL — `resolve_voice_model_path` doesn't exist yet.

- [ ] **Step 3: Add the resolver to `voice/tts.py`**

```python
from pathlib import Path

_TTS_MODELS_DIR = Path(__file__).resolve().parent / "tts_models"


def resolve_voice_model_path(voice_name: str) -> str:
    """Maps a config voice name (e.g. "en_US-lessac-medium") to its local model file path.
    Model files are downloaded once (see README) into voice/tts_models/, gitignored — same
    treatment as voice_config.yaml, since they're per-machine, not repo content."""
    path = _TTS_MODELS_DIR / f"{voice_name}.onnx"
    if not path.is_file():
        raise FileNotFoundError(
            f"Voice model '{voice_name}' not found at '{path}'. "
            "Download it first — see README.md's Voice (optional) setup section."
        )
    return str(path)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `.venv/bin/python -m pytest voice/tests/test_tts.py -v`
Expected: all tests pass, including the 2 new ones

- [ ] **Step 5: Update `main()` in `voice/voice_client.py` to use the resolver**

Change the Task 3 Step 5 snippet from `TextToSpeech(config.tts_voice)` to:

```python
    tts = None
    if config.tts_enabled:
        from voice.tts import TextToSpeech, resolve_voice_model_path
        tts = TextToSpeech(resolve_voice_model_path(config.tts_voice))
```

- [ ] **Step 6: Add `voice/tts_models/` to `.gitignore`**

Add this line near the existing `voice/voice_config.yaml` entry:

```
voice/tts_models/
```

- [ ] **Step 7: Extend the README's "Voice (optional)" section**

Read the section first (`grep -n "Voice (optional)" -A 30 README.md`) to match its existing
structure and style. Add, after the existing STT/wake-word model download note: instructions
for downloading a Piper voice model into `voice/tts_models/` (using whichever download
mechanism Task 1 Step 1/2 confirmed actually works — reference the exact command, don't
re-describe it vaguely), and a one-line note that `tts.enabled: false` in `voice_config.yaml`
skips this requirement entirely for text-only voice use.

- [ ] **Step 8: Manual smoke check (no automated test)**

Note the outcome in your report, not a pass/fail assertion. With a real voice model downloaded
to `voice/tts_models/en_US-lessac-medium.onnx` (or whatever `voice_config.yaml`'s `tts.voice`
names) and `tts.enabled: true`, run `./start_jarvis.sh --voice`, dispatch a known command (e.g.
say "status"), and confirm you hear it spoken, not just printed.

- [ ] **Step 9: Commit**

```bash
git add voice/tts.py voice/voice_client.py voice/tests/test_tts.py README.md .gitignore
git commit -m "feat(voice): resolve TTS voice names to local model paths, document setup"
```

---

## Plan self-review notes

- **Spec coverage:** §4.1 (`tts.py`) → Task 1 + Task 4's resolver addition. §4.2
  (`audio_playback.py`) → Task 1. §4.3 (`voice_client.py` wiring) → Task 3. §4.4 (config) →
  Task 2. §5 (error handling — fail-fast on missing model) → Task 4's
  `resolve_voice_model_path`. §6 (testing plan) → a test file/extension per task, `play()`
  deliberately untested per the spec's own stated precedent. §7 (what doesn't change) →
  enforced by Global Constraints; no task touches `core/`/`ai/`/`tools/`. §8 (setup) → Task 1
  Steps 1-2 (package/model verification) + Task 4 Step 7 (README).
- **The Piper API is the plan's single biggest unknown**, flagged explicitly at Task 1 Step 1
  rather than assumed — same honesty precedent as Voice Input's wake-word-model-name
  verification. If the real API differs substantially from what Task 1's reference code
  assumes, the implementer adjusts internals while preserving the specified public interface;
  this is called out explicitly in Step 6's text, not left implicit.
- **Task ordering:** Task 1 before Task 2 before Task 3 before Task 4 is required — Task 3
  imports `TextToSpeech` (Task 1) and consumes `tts_enabled`/`tts_voice` (Task 2); Task 4
  modifies code Task 3 just wrote (the `main()` TTS construction line). This plan is more
  linear than Ears' was (which had more independent leaf modules); no parallelization
  opportunity to note here.
