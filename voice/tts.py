"""Text-to-speech via Piper - local ONNX-based TTS, no API key.

Piper synthesizes at whatever samplerate the loaded voice model specifies (commonly 16kHz or
22.05kHz depending on the voice) - always read from the voice's own config, never assumed.

Note on Piper's real API (piper-tts==1.7.0, verified by inspection - the older
`synthesize_stream_raw` method some docs/examples reference does not exist in this version):
`PiperVoice.synthesize(text)` returns an `Iterable[AudioChunk]` (one chunk per sentence), and
each `AudioChunk` exposes `.audio_int16_array` (already-decoded int16 PCM) and its own
`.sample_rate`. We concatenate chunks' int16 arrays and take the samplerate from
`voice.config.sample_rate`, which does exist on the loaded voice and matches each chunk's rate.
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
        chunks = [chunk.audio_int16_array for chunk in self._voice.synthesize(text)]
        audio = np.concatenate(chunks) if chunks else np.array([], dtype=np.int16)
        return SynthesisResult(audio=audio, samplerate=self._voice.config.sample_rate)

    def speak(
        self,
        text: str,
        player: Optional[Callable[[np.ndarray, int], None]] = None,
    ) -> None:
        result = self.synthesize(text)
        player_fn = player if player is not None else _default_play
        player_fn(result.audio, result.samplerate)
