# Voice test fixtures

`voice/tests/test_tts.py` needs a real Piper voice model on disk. The model weights
(`en_US-lessac-low.onnx`, ~61MB) and its config (`en_US-lessac-low.onnx.json`) are **not**
committed to git — no ML model weight file is checked into this repo (same reasoning as
`voice/tts_models/` being gitignored for the runtime voice model). Download it once before
running the tests:

```bash
.venv/bin/python -m piper.download_voices --download-dir voice/tests/fixtures en_US-lessac-low
```

Run that command from the repo root — same form as the runtime voice download in `README.md`'s
Voice (optional) section, just pointed at `voice/tests/fixtures` instead of `voice/tts_models`.
Both produced files (`en_US-lessac-low.onnx`, `en_US-lessac-low.onnx.json`) are gitignored
(`voice/tests/fixtures/*.onnx`, `voice/tests/fixtures/*.onnx.json`) so they stay untracked once
downloaded.
