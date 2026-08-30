# Voice Output ("Voice") — Design Spec

**Status:** approved (blanket approval given in-conversation, 2026-08-30 — no per-section
sign-off required, matching the process used for Voice Input).
**Phase:** Phase 3 — Voice I/O (second and final sub-project; Voice Input/"Ears" already
merged and live-validated).

## 1. Scope

**In scope:**
- Text-to-speech synthesis via Piper, fully local.
- Audio playback of synthesized speech.
- Speaking both a dispatched command's response AND the existing low-confidence clarification
  messages in `voice/voice_client.py`'s `dispatch_transcript` — the Voice Input spec's §6
  explicitly anticipated this: *"written so it becomes an audio prompt for free once Voice
  Output lands — same decision logic, the `print()` call is the only thing that changes to a
  TTS call."*
- Config-driven enable/disable and voice model selection.

**Out of scope:**
- Any change to `tools/interactive_client.py` (the text client) or `core/`/`ai/`. This stays a
  `voice/`-only addition, same discipline as Voice Input.
- Streaming/incremental synthesis (speaking partial sentences as they're generated). JARVIS's
  responses are short, already-complete strings by the time they reach the client — there is
  nothing to stream.
- Interrupting playback (e.g. a wake word barging in mid-sentence to cut off speech).
  Push-to-talk's next "Press Enter to speak" naturally waits for playback to finish; always-listen
  would need this eventually but it's not blocking a first version.
- Voice selection beyond picking a downloaded Piper voice model by name — no runtime
  voice-mixing, speed/pitch controls, or SSML.

## 2. Where this sits in the pipeline

This is **Render** — the mirror image of what Voice Input added to Capture. `dispatch_transcript`
already owns exactly two places a response reaches the user: the two `print()` calls after a
successful dispatch, and the `print()` in the low-confidence branches. Voice Output adds a
`tts.speak(text)` call alongside each, not instead of — the terminal output is unchanged,
speech is additive. No new decision logic: `dispatch_transcript` already decided *what* to say
(that's Understanding + the two-signal confidence logic from Voice Input); this only changes
*how it's rendered*, which is squarely INV-3/INV-4 territory (thin surface, adapter at the
boundary) and touches nothing upstream.

## 3. Approach: mirror Ears' module shape

Two small, single-purpose modules, consistent with `audio_capture.py`/`wake_word.py`/`stt.py`'s
existing shape:

```
voice/
├── tts.py              # Piper wrapper: text -> (audio, samplerate)
├── audio_playback.py    # sounddevice wrapper: (audio, samplerate) -> speaker
└── tests/
    ├── test_tts.py
    └── (no test_audio_playback.py — see §6)
```

`tts.py` and `audio_playback.py` are deliberately separate so `tts.py`'s synthesis logic is
testable without a speaker or any audio hardware — mirrors why `audio_capture.py`'s
`frames_from_wav()` exists alongside `frames()`.

## 4. Components

### 4.1 `tts.py`
Wraps Piper (ONNX-based, ships as a Python package with a `PiperVoice` load-from-file API).
Exposes:
```python
@dataclass(frozen=True)
class SynthesisResult:
    audio: np.ndarray   # int16 mono PCM
    samplerate: int

class TextToSpeech:
    def __init__(self, voice_model_path: str): ...
    def synthesize(self, text: str) -> SynthesisResult: ...
    def speak(self, text: str, player=None) -> None:
        """synthesize() then hand the result to audio_playback.play() (or an injected
        player callable, for testing) — the convenience path voice_client.py actually calls."""
```
Fully local — Piper's voice models are `.onnx` + a `.onnx.json` config file, downloaded once
(same INV-11 exception as openWakeWord/faster-whisper's weights) and cached on disk; synthesis
itself never touches the network.

### 4.2 `audio_playback.py`
```python
def play(audio: np.ndarray, samplerate: int) -> None:
    """Blocking playback via sounddevice — waits for speech to finish before returning,
    so dispatch_transcript's caller doesn't move on mid-sentence."""
```
Thin enough that it's one function, not a class — no state to hold between calls.

### 4.3 `voice_client.py` changes
- `dispatch_transcript` gains an optional `tts: TextToSpeech | None = None` parameter. When not
  `None`, it calls `tts.speak(...)` immediately after each of the three existing `print()` calls
  that render something to the user (the dispatched response, the STT-low-confidence message,
  the AI-low-confidence clarification).
- `main()` constructs a `TextToSpeech` instance (or `None`, if `config.tts_enabled` is `False`)
  and passes it through to `_run_push_to_talk`/`_run_always_listen`, which forward it to
  `dispatch_transcript` exactly as `config`/`stt` already flow today.

### 4.4 Config additions
```yaml
tts:
  enabled: true            # false = voice-in-text-out only, no speaker required
  voice: en_US-lessac-medium   # a Piper voice model name (downloaded to a local cache dir)
```
Both keys are optional with defaults (`enabled: true`, a specific default voice name), following
the same `raw["tts"].get(...)` backward-compatibility pattern `stt.language` established — an
existing `voice_config.yaml` without a `tts` section keeps working unchanged.

## 5. Error handling

If Piper's voice model file isn't present at startup (not yet downloaded), `TextToSpeech.__init__`
raises a clear `FileNotFoundError` naming the model path and pointing at the README's setup
instructions — same fail-fast pattern `config.py`'s `load_config` already uses for a missing
`voice_config.yaml`. This only happens when `tts.enabled: true`; with `false`, `main()` never
constructs a `TextToSpeech` at all, so a user without a speaker (or who hasn't downloaded the
voice model yet) isn't blocked from using push-to-talk/always-listen text-only.

## 6. Testing plan (INV-13)

- `voice/tests/test_tts.py`: `synthesize()` tested for real against Piper (deterministic, no
  hardware needed) — assert a short input string produces non-empty `int16` audio and a sane
  samplerate (Piper's models are commonly 16kHz or 22.05kHz; whichever the chosen voice model
  actually reports, asserted against, not assumed). `speak()` tested with an injected `player`
  callable (a `MagicMock`) asserting it's called with `synthesize()`'s exact output — no real
  playback in this test either.
- `audio_playback.py`'s `play()` has no automated test — there is no wav-file-equivalent
  substitute for "did a speaker make sound," the same reason `audio_capture.py`'s live-mic
  `frames()` path has none. Verified manually once implementation lands, alongside Voice Input's
  own still-outstanding live microphone smoke test.
- `voice_client.py`'s `dispatch_transcript` gets its existing three tests (skip-low-confidence,
  dispatch-above-threshold, ai-low-confidence-clarification) extended with a `tts` mock,
  asserting `speak()` is called with the right text in each of the three branches — and a
  fourth test confirming `tts=None` (the default) behaves exactly as before, with zero calls.

## 7. What doesn't change

- `core/`, `ai/`, `tools/interactive_client.py` — untouched.
- Voice Input's modules (`config.py` gains fields but keeps its existing ones unchanged;
  `audio_capture.py`, `wake_word.py`, `stt.py` — all untouched).
- The two-signal low-confidence decision logic itself (§6 of the Voice Input spec) — unchanged;
  this spec only adds a render call alongside each existing `print()`.

## 8. Setup

- New dependency: `piper-tts` (or whichever exact PyPI package name resolves at implementation
  time — Piper's Python packaging has moved between a couple of names historically; verify at
  Task 1 and use the exact name, same "confirm the real one, don't guess" precedent as Voice
  Input's Task 3 verifying openWakeWord's actual stock model name). Added to
  `voice/requirements.txt`.
- Voice model download is one-time, documented in the README's existing "Voice (optional)"
  section (extend it, don't duplicate it).

## 9. Deferred / explicitly not now

- Playback interruption (barge-in) for always-listen mode.
- Streaming/incremental synthesis.
- Multiple simultaneous voices, SSML, prosody control.
- Making TTS available to `tools/interactive_client.py` or any non-voice surface — if ever
  wanted, that's a separate, later decision, not an extension of this branch's scope.
