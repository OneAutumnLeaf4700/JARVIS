# Voice Input ("Ears") — Design Spec

**Status:** approved (blanket approval given in-conversation, 2026-08-30 — no per-section
sign-off required).
**Phase:** Phase 3 — Voice I/O (first sub-project; Voice Output/TTS is a separate, later spec).

## 1. Scope

**In scope:**
- Microphone capture, wake-word detection ("Jarvis"), and speech-to-text — the "hearing" half
  of Phase 3.
- Push-to-talk vs. always-listen mode switching, config-driven.
- Graceful low-confidence handling (terminal-only for this pass — see §6).
- A small voice-only config file.

**Out of scope (deliberately, separate future spec):**
- Text-to-speech / Voice Output. `roadmap.md` already notes TTS is orthogonal to STT with no
  dependency between them — this sub-project produces no output audio at all.
- A general-purpose JARVIS configuration system. The config file introduced here is scoped
  narrowly to voice settings; when a real config system arrives (Phase 1's still-open item) it
  should absorb this file, not the other way around.
- Custom wake-word training (stretch goal in `features.md`).
- Any change to `core/` (C++). This is a pure Python surface addition.

## 2. Where this sits in the pipeline

Per the fixed 7-stage pipeline, this is **Capture** (new modality) feeding the *existing*
**Ingress** unchanged. It does not introduce a parallel path (project rule §1): the voice
client is architecturally identical to `tools/interactive_client.py` — both are thin Python
gRPC clients that turn some input into an `ExecuteCommandRequest` and call
`JarvisService.ProcessCommand` on `localhost:50051`. Voice replaces "typing a line" with
"speaking a line"; everything downstream (parsing, classification, dispatch, response) is
untouched.

Concretely: `voice_client.py`'s dispatch logic reuses the exact same known-command-first-word
check that `interactive_client.py::parse_line()` already implements (mirroring the CLI's own
`COMMAND_MAP` lookup client-side) — a known command word routes directly; anything else goes
as `COMMAND_TYPE_UNKNOWN` with the full transcript as payload, flowing through the real
classify-then-escalate pipeline exactly like typed natural language does today. This satisfies
INV-3 (thin surface, zero business logic, no `core/` edits) by construction: it's the same
proven client pattern, just fed by a microphone instead of a keyboard.

## 3. Approach: modular components + thin orchestrator

Mirrors how `ai/` is already organized (`intent_classifier.py`, `llm_backend.py`,
`resolver.py` as separate single-purpose modules) rather than one monolithic script. Each
component is independently testable without a real microphone (INV-13) — feed it a wav file or
mocked audio frames, not a live device.

```
voice/
├── __init__.py
├── config.py          # load/validate voice_config.yaml
├── audio_capture.py    # mic -> raw audio frames, via sounddevice
├── wake_word.py         # openWakeWord wrapper: frames -> wake/no-wake
├── stt.py                # faster-whisper wrapper: frames -> (text, confidence)
├── voice_client.py       # orchestrator: wires the above + the gRPC call, mirrors
│                          # tools/interactive_client.py's dispatch logic
├── requirements.txt       # voice-specific deps, separate from the core ai/ deps
├── voice_config.example.yaml
└── tests/
    ├── test_config.py
    ├── test_wake_word.py
    ├── test_stt.py
    └── test_voice_client.py   # dispatch-logic tests, gRPC call mocked
```

`voice/voice_client.py` is the only entry point a user runs (`python3 voice/voice_client.py`),
same launch shape as `tools/interactive_client.py`. `start_jarvis.sh` gets a new optional flag
to launch it instead of (or alongside) the text client — see §8.

## 4. Components

### 4.1 `audio_capture.py`
Wraps `sounddevice` (numpy-based, cross-platform, simpler dependency footprint than PyAudio) to
pull fixed-size audio frames from the default input device at 16kHz mono (the sample rate both
openWakeWord and faster-whisper expect natively, avoiding a resampling step). Exposes a small
generator interface — `frames() -> Iterator[np.ndarray]` — so `wake_word.py`/`stt.py` never
touch `sounddevice` directly, and tests can substitute a wav-file-backed generator with the same
interface.

### 4.2 `wake_word.py`
Wraps `openWakeWord`. Loads its stock "hey jarvis"-style pretrained model (or nearest available
stock phrase if an exact "Jarvis" model isn't in its pretrained set — confirmed at
implementation time; custom training is explicitly out of scope per §1). Exposes
`detect(frame) -> bool`. Runs entirely offline via a local ONNX model — no account, no API key,
consistent with INV-11.

### 4.3 `stt.py`
Wraps `faster-whisper` (CTranslate2 Whisper reimplementation — local, no API key, meaningfully
faster than reference Whisper on CPU). Exposes `transcribe(audio) -> TranscriptResult{text,
confidence}`. Confidence is derived from faster-whisper's segment-level `avg_logprob` /
`no_speech_prob` output (converted to a normalized 0–1-ish score) — there's no single
first-class "confidence" field in Whisper's output, so this mapping is documented inline in the
module rather than treated as a black box. Model size defaults to `base` (small enough to be
responsive on CPU, matches the "tiny/base models are small and fast enough for a personal
assistant" reasoning from the brainstorm) and is a config value, not hardcoded (§5).

### 4.4 `config.py`
Loads and validates `voice/voice_config.yaml` (path overridable via `--config` flag or
`JARVIS_VOICE_CONFIG` env var). Schema:

```yaml
mode: push_to_talk        # push_to_talk | always_listen
wake_word:
  sensitivity: 0.5         # openWakeWord threshold, 0-1
stt:
  model_size: base          # tiny | base | small | medium
  confidence_threshold: 0.55  # below this -> clarification path (§6)
audio:
  device: null               # null = system default input device
```

A `voice_config.example.yaml` ships committed; the real `voice_config.yaml` is gitignored (same
pattern as the two `.jarvis-*.log` files) so per-machine tuning (mic device, sensitivity) never
pollutes the repo. `voice_client.py` fails fast with a clear message if no config file is found
in either location, pointing at the example file.

### 4.5 `voice_client.py` (orchestrator)
- **Push-to-talk mode:** waits for a keypress (simple blocking `input()`-driven trigger, no
  wake-word model needed in this mode — matches the roadmap's framing of PTT vs. always-listen
  as alternatives, not layered), then captures until a short trailing silence, then transcribes.
- **Always-listen mode:** runs a blocking loop over `audio_capture.frames()`, feeding each frame
  to `wake_word.detect()`; on trigger, captures the following utterance (same
  capture-until-silence logic as PTT) and transcribes. No async/threading needed for a first
  pass — approach C (event-driven/async) was explicitly rejected in the brainstorm as overkill
  for this phase.
- After transcription: apply the dispatch logic from §2 and call `ProcessCommand`, printing the
  response exactly like `interactive_client.py` does.

## 5. Config-driven mode switching

Settled in the brainstorm: a small voice-only YAML config now (§4.4), not a CLI flag and not
blocked on the general config system. `mode: push_to_talk | always_listen` is the switch;
`voice_client.py` reads it once at startup (no live hot-reload — restart to change mode, matches
this project's current level of runtime dynamism everywhere else).

## 6. Low-confidence handling

Settled in the brainstorm: **print a clarification to the terminal**, not silent guessing and
not deferred to Voice Output. Two independent confidence signals, either of which can trigger
this path:

1. **STT confidence** below `stt.confidence_threshold` (§4.4) — the transcript itself is
   unreliable. Printed message: `"Didn't catch that clearly — heard: '<best-effort transcript>'.
   Try again?"` and the utterance is **not** sent to `ProcessCommand` at all (sending a garbled
   transcript into the classifier would just produce a confusing `UNKNOWN` round-trip for no
   benefit).
2. **Intent confidence** — the *existing* `resolve()`/classifier confidence, already returned
   over gRPC today (`AIResult.confidence`, INV-8's stable interface — unchanged by this spec).
   When a response comes back with low confidence, print `"Not sure I understood — did you
   mean: <best guess if available>?"` This is purely a Render-side addition to how
   `voice_client.py` prints an existing field; no change to the Understanding tier itself.

This is written so it becomes an audio prompt "for free" once Voice Output lands — same
decision logic, the `print()` call is the only thing that changes to a TTS call.

## 7. What doesn't change

- `core/` — zero changes. No new `CommandType`, no proto changes, no registry changes.
- `ai/` — zero changes. The classifier/resolver/LLM-fallback stack is consumed exactly as-is
  through the existing gRPC contract.
- The existing `tools/interactive_client.py` keeps working unmodified; this is a new, separate
  entry point, not a replacement.

## 8. Setup / launch

- New dependencies (`sounddevice`, `openwakeword`, `faster-whisper`, `pyyaml`) live in
  `voice/requirements.txt`, kept separate from `ai/requirements-dev.txt` since these are
  runtime deps for an optional surface, not core JARVIS deps — installing JARVIS without
  microphone support shouldn't require them. README gets a new "Voice (optional)" setup section
  documenting this, following the existing prose-based dependency documentation convention (no
  project-wide `requirements.txt` exists today).
- `start_jarvis.sh` gets a `--voice` flag: when passed, launches `voice/voice_client.py` instead
  of `tools/interactive_client.py` after the two servers are up. Default behavior (no flag)
  unchanged.

## 9. Testing plan (INV-13)

- `voice/tests/test_config.py` — valid/invalid YAML, missing file, env var override.
- `voice/tests/test_wake_word.py` — feed known wav fixtures (a clip containing the wake phrase,
  a clip that doesn't) through `wake_word.detect()`, assert true/false. No live mic.
- `voice/tests/test_stt.py` — feed a short wav fixture with known transcript through
  `stt.transcribe()`, assert the text is close (exact match is brittle across model versions) and
  confidence is in a sane range. A second fixture (silence or noise) asserts low confidence.
- `voice/tests/test_voice_client.py` — dispatch-logic tests only (mirroring
  `interactive_client.py`'s own untested-but-simple `parse_line`): given a transcript string,
  assert the correct `ExecuteCommandRequest` is built; gRPC call itself is mocked, no real
  server needed. Also covers the two low-confidence branches from §6 (assert `ProcessCommand` is
  *not* called on low STT confidence; assert the clarification message is printed).
- No test requires a live microphone or a live model download to run in CI-like conditions —
  wav fixtures are small, committed sample audio; model weights (openWakeWord's ONNX file,
  faster-whisper's model) are downloaded once and cached locally, not committed to the repo.

## 10. Deferred / explicitly not now

- Voice Output (TTS) — separate future spec, per the brainstorm's sub-project split.
- Custom wake-word training — stretch goal, unchanged.
- Hot-reloading config without restart.
- Async/concurrent audio pipeline (approach C) — revisit only if push-to-talk/always-listen's
  blocking-loop model proves too limiting in practice.
- Folding `voice_config.yaml` into a project-wide config system — happens whenever that system
  is actually built, not here.
