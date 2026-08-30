import os
from unittest.mock import MagicMock

import numpy as np

from voice.stt import TranscriptResult
from voice.voice_client import (
    build_request,
    capture_utterance,
    capture_utterance_bounded,
    dispatch_transcript,
)


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


def test_capture_utterance_does_not_discard_speech_after_leading_pause():
    loud = np.full(1280, 5000, dtype=np.int16)
    silent = np.zeros(1280, dtype=np.int16)
    # 20 blocks of leading silence (more than max_silence_blocks=15) before the user actually
    # starts speaking, then speech, then trailing silence. Without arming the trailing-silence
    # stop condition on speech_started, this would previously return before the loud blocks
    # were ever captured.
    frame_sequence = [silent] * 20 + [loud, loud, loud, loud, loud] + [silent] * 16

    result = capture_utterance(
        iter(frame_sequence),
        silence_rms_threshold=300.0,
        max_silence_blocks=15,
        max_leading_silence_blocks=80,
        max_blocks=150,
    )

    # The bug discarded the utterance as silence before any loud block was ever captured, so
    # the strongest signal is that the loud samples actually made it into the buffer.
    assert len(result) > 0
    assert int(np.max(result)) == 5000


def test_capture_utterance_respects_max_blocks():
    loud = np.full(1280, 5000, dtype=np.int16)
    frame_sequence = [loud] * 1000  # never goes silent

    result = capture_utterance(iter(frame_sequence), max_blocks=10)

    assert len(result) == 10 * 1280


def test_capture_utterance_bounded_stops_on_second_enter():
    read_fd, write_fd = os.pipe()
    read_file = os.fdopen(read_fd, "r")
    loud = np.full(1280, 5000, dtype=np.int16)

    def frame_gen():
        for i in range(10):
            yield loud
            if i == 2:
                # Simulate the user pressing Enter again after the 3rd frame.
                os.write(write_fd, b"\n")

    result = capture_utterance_bounded(frame_gen(), stdin=read_file)
    os.close(write_fd)
    read_file.close()

    # Should stop shortly after frame index 2 (once select() sees the pipe is ready), not
    # consume all 10 frames — proves the second-Enter signal actually stops capture.
    assert 3 * 1280 <= len(result) < 10 * 1280


def test_capture_utterance_bounded_returns_everything_if_iterator_ends_first():
    read_fd, write_fd = os.pipe()
    read_file = os.fdopen(read_fd, "r")
    loud = np.full(1280, 5000, dtype=np.int16)
    frame_sequence = [loud] * 5  # never signals on the pipe

    result = capture_utterance_bounded(iter(frame_sequence), stdin=read_file)
    os.close(write_fd)
    read_file.close()

    assert len(result) == 5 * 1280


def test_capture_utterance_bounded_respects_max_blocks_safety_cap():
    read_fd, write_fd = os.pipe()
    read_file = os.fdopen(read_fd, "r")
    loud = np.full(1280, 5000, dtype=np.int16)
    frame_sequence = [loud] * 1000  # never signals, would otherwise run forever

    result = capture_utterance_bounded(iter(frame_sequence), stdin=read_file, max_blocks=10)
    os.close(write_fd)
    read_file.close()

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


def test_dispatch_transcript_verbose_logs_heard_text_above_threshold(capsys):
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_STATUS, message="Engine: running"
    )
    result = TranscriptResult(text="status", confidence=0.87)

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55, verbose=True)

    captured = capsys.readouterr()
    assert "heard: 'status'" in captured.out
    assert "0.87" in captured.out


def test_dispatch_transcript_verbose_logs_heard_text_below_threshold(capsys):
    stub = MagicMock()
    result = TranscriptResult(text="mumble", confidence=0.2)

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55, verbose=True)

    stub.ProcessCommand.assert_not_called()
    captured = capsys.readouterr()
    assert "heard: 'mumble'" in captured.out
    assert "0.20" in captured.out


def test_dispatch_transcript_not_verbose_omits_heard_line_above_threshold(capsys):
    import jarvis_pb2

    stub = MagicMock()
    stub.ProcessCommand.return_value = jarvis_pb2.ExecuteCommandResponse(
        command_type=jarvis_pb2.COMMAND_TYPE_STATUS, message="Engine: running"
    )
    result = TranscriptResult(text="status", confidence=0.87)

    dispatch_transcript(result, stub, stt_confidence_threshold=0.55, verbose=False)

    captured = capsys.readouterr()
    assert "heard:" not in captured.out


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
