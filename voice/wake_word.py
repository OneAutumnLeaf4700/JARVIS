"""Wake-word detection via openWakeWord - fully local, ONNX model, no account/API key."""

import numpy as np
from openwakeword.model import Model


class WakeWordDetector:
    def __init__(self, sensitivity: float = 0.5, model_name: str = "hey_jarvis"):
        self._model = Model(wakeword_models=[model_name], inference_framework="onnx")
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
