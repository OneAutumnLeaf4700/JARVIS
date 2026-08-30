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
