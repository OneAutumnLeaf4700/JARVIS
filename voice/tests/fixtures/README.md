# Voice test fixtures

`voice/tests/test_tts.py` needs a real Piper voice model on disk. The model weights
(`en_US-lessac-low.onnx`, ~61MB) and its config (`en_US-lessac-low.onnx.json`) are **not**
committed to git — no ML model weight file is checked into this repo (same reasoning as
`voice/tts_models/` being gitignored for the runtime voice model). Download it once before
running the tests:

```bash
.venv/bin/python -m piper.download_voices en_US-lessac-low
```

Run that command from this directory (`voice/tests/fixtures/`), or run it elsewhere and move
the two produced files (`en_US-lessac-low.onnx`, `en_US-lessac-low.onnx.json`) here. Both files
are gitignored (`voice/tests/fixtures/*.onnx`, `voice/tests/fixtures/*.onnx.json`) so they stay
untracked once downloaded.
