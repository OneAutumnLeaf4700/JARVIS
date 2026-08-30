"""Voice client - the microphone/wake-word/STT equivalent of tools/interactive_client.py.

Same architectural role as interactive_client.py: a thin gRPC client that turns some input
(here, transcribed speech instead of typed text) into an ExecuteCommandRequest and calls
JarvisService.ProcessCommand. Everything downstream of that call is the existing, unmodified
pipeline - this file adds no business logic of its own (INV-3).
"""

import argparse
from pathlib import Path
import re
import select
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

# Deliberately duplicated from tools/interactive_client.py's identical dict rather than
# imported — these are two independent thin-client surfaces (INV-3); importing across them
# would couple surfaces that should stay decoupled, which is worse than the duplication.
KNOWN_COMMANDS = {
    "echo": jarvis_pb2.COMMAND_TYPE_ECHO,
    "status": jarvis_pb2.COMMAND_TYPE_STATUS,
    "about": jarvis_pb2.COMMAND_TYPE_ABOUT,
    "help": jarvis_pb2.COMMAND_TYPE_HELP,
}

# Matches the literal bracketed format ai/jarvis_ai_server.py emits:
# f"[detected intent: {intent}, confidence {confidence:.2f}]". Tightened to the full literal
# (rather than a bare "confidence \d+\.\d+" scan) so a capability's own output text can't
# accidentally false-positive-match this pattern.
_CONFIDENCE_RE = re.compile(r"\[detected intent: \w+, confidence (\d+\.\d+)\]")


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
    max_leading_silence_blocks: int = 80,
    max_blocks: int = 150,
) -> np.ndarray:
    """Buffer frames from frame_iter until trailing silence (max_silence_blocks consecutive
    quiet blocks after speech has actually started) or max_blocks is reached, whichever comes
    first. Before any speech is observed, only max_leading_silence_blocks of quiet is tolerated
    (covers a press-to-talk pause before the user actually starts speaking) — otherwise a press
    with no speech at all would hang until max_blocks. Returns the concatenated int16 utterance."""
    buffer = []
    silence_count = 0
    speech_started = False

    for frame in frame_iter:
        buffer.append(frame)

        rms = float(np.sqrt(np.mean(frame.astype(np.float32) ** 2)))
        if rms < silence_rms_threshold:
            silence_count += 1
        else:
            silence_count = 0
            speech_started = True

        if len(buffer) >= max_blocks:
            break
        if speech_started:
            if silence_count >= max_silence_blocks:
                break
        else:
            if silence_count >= max_leading_silence_blocks:
                break

    if not buffer:
        return np.array([], dtype=np.int16)
    return np.concatenate(buffer)


def capture_utterance_bounded(
    frame_iter,
    stdin=None,
    max_blocks: int = 750,
) -> np.ndarray:
    """Push-to-talk capture bracketed by two explicit Enter presses: the caller has already
    consumed the first (the "start" signal) before calling this; this buffers frames until a
    line becomes available on stdin (the "stop" signal) or frame_iter is exhausted, whichever
    comes first. No silence detection — the user controls the boundary directly, which is what
    push-to-talk asked for after capture_utterance's silence-based heuristic proved fragile
    against real speech. max_blocks is a safety net only (default ~60s at 1280 samples/16kHz),
    not expected to trigger in normal use — it guards against a forgotten stop press."""
    if stdin is None:
        stdin = sys.stdin

    buffer = []
    for frame in frame_iter:
        buffer.append(frame)

        if len(buffer) >= max_blocks:
            break

        ready, _, _ = select.select([stdin], [], [], 0)
        if ready:
            stdin.readline()
            break

    if not buffer:
        return np.array([], dtype=np.int16)
    return np.concatenate(buffer)


def dispatch_transcript(
    result: TranscriptResult,
    stub,
    stt_confidence_threshold: float = 0.55,
    ai_confidence_threshold: float = 0.5,
    verbose: bool = False,
    tts=None,
) -> None:
    """Implements the two-signal low-confidence handling from the design spec §6: low STT
    confidence skips the round-trip entirely; low AI/intent confidence (parsed out of the
    existing response message) prints a clarification after the round-trip completes.

    verbose (--log): always echoes exactly what STT heard and its confidence, before the
    threshold decision, regardless of whether the utterance ends up dispatched or rejected —
    a debugging aid for calibrating stt.confidence_threshold / mic choice against real speech.

    tts: optional TextToSpeech (voice.tts) — when provided, spoken alongside each printed
    message using a plain-language rendering (no leading indentation/markdown-style punctuation
    from the print() strings)."""
    if verbose:
        print(f"  heard: '{result.text}' (confidence {result.confidence:.2f})")

    if result.confidence < stt_confidence_threshold:
        print(f"  Didn't catch that clearly — heard: '{result.text}'. Try again?\n")
        if tts is not None:
            try:
                tts.speak(f"Didn't catch that clearly. Heard: {result.text}. Try again?")
            except Exception as exc:
                print(f"  [TTS error: {exc}]")
        return

    request = build_request(result.text)

    try:
        response = stub.ProcessCommand(request, timeout=10)
    except grpc.RpcError as exc:
        print(f"  [gRPC error: {exc.code()} — {exc.details()}]\n")
        return

    print(f"  command_type: {jarvis_pb2.CommandType.Name(response.command_type)}")
    print(f"  {response.message}\n")

    if tts is not None:
        try:
            tts.speak(response.message)
        except Exception as exc:
            print(f"  [TTS error: {exc}]")

    match = _CONFIDENCE_RE.search(response.message)
    if match and float(match.group(1)) < ai_confidence_threshold:
        print("  Not sure I understood — could you rephrase that?\n")
        if tts is not None:
            try:
                tts.speak("Not sure I understood. Could you rephrase that?")
            except Exception as exc:
                print(f"  [TTS error: {exc}]")


def _run_push_to_talk(stub, stt: SpeechToText, config, verbose: bool = False, tts=None) -> None:
    print("Push-to-talk mode. Press Enter, then speak. Press Enter again to stop. Ctrl+C to quit.\n")
    while True:
        try:
            input("Press Enter to speak > ")
        except (EOFError, KeyboardInterrupt):
            print()
            return

        print("  (recording — press Enter to stop)")
        utterance = capture_utterance_bounded(frames(device=config.audio_device))
        result = stt.transcribe(utterance)
        dispatch_transcript(result, stub, config.stt_confidence_threshold, verbose=verbose, tts=tts)


def _run_always_listen(
    stub, detector: WakeWordDetector, stt: SpeechToText, config, verbose: bool = False, tts=None
) -> None:
    print("Always-listen mode. Say the wake word, then speak. Ctrl+C to quit.\n")
    frame_iter = frames(device=config.audio_device)
    try:
        for frame in frame_iter:
            if not detector.detect(frame):
                continue

            print("  (wake word detected — listening...)")
            utterance = capture_utterance(frame_iter)
            result = stt.transcribe(utterance)
            dispatch_transcript(result, stub, config.stt_confidence_threshold, verbose=verbose, tts=tts)
            detector.reset()
    except KeyboardInterrupt:
        print()
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="JARVIS voice client")
    parser.add_argument("--config", default=None, help="Path to voice_config.yaml")
    parser.add_argument(
        "--log",
        action="store_true",
        help="Echo every transcript STT hears, with its confidence, before the threshold "
        "decision — useful for calibrating stt.confidence_threshold or diagnosing "
        "inconsistent recognition.",
    )
    args = parser.parse_args()

    config = load_config(args.config)

    channel = grpc.insecure_channel("localhost:50051")
    stub = jarvis_pb2_grpc.JarvisServiceStub(channel)
    stt = SpeechToText(model_size=config.stt_model_size, language=config.stt_language)

    tts = None
    if config.tts_enabled:
        from voice.tts import TextToSpeech, resolve_voice_model_path
        tts = TextToSpeech(resolve_voice_model_path(config.tts_voice))

    if config.mode == "push_to_talk":
        _run_push_to_talk(stub, stt, config, verbose=args.log, tts=tts)
    else:
        detector = WakeWordDetector(sensitivity=config.wake_word_sensitivity)
        _run_always_listen(stub, detector, stt, config, verbose=args.log, tts=tts)


if __name__ == "__main__":
    main()
