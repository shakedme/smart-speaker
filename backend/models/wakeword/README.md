# Okay Rufus Wake Word Model

Custom wake word classifier trained with [livekit-wakeword](https://github.com/livekit/livekit-wakeword).

## Prerequisites

```bash
# macOS
brew install espeak-ng ffmpeg portaudio

# Ubuntu/Debian
sudo apt install espeak-ng libsndfile1 ffmpeg sox portaudio19-dev
```

## Setup Python environment

From `backend/`:

```bash
python3.11 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pip install -r models/wakeword/requirements-train.txt   # training only
```

## Production training (required for real detection)

```bash
cd backend
npm run train:wakeword:prod
# or: bash models/wakeword/train-prod.sh okay_rufus.yaml
```

This runs generate → augment → train (100k steps, conv-attention medium) → export → eval.
Expect several hours on CPU; GPU/MPS speeds it up significantly.

Artifacts in `output/okay_rufus/`:
- `okay_rufus.onnx` — runtime classifier
- `okay_rufus_eval.json` — threshold, recall, FPPH (used automatically by the backend)

## Backend runtime

The live server auto-loads `WAKEWORD_THRESHOLD` from `okay_rufus_eval.json` unless overridden in `.env`.

```bash
cd backend
npm run start:live
```

Optional debug logging: `VOICE_DEBUG=1 npm run dev:live`

## Quick smoke test (pipeline validation only)

```bash
cd backend/models/wakeword
../.venv/bin/livekit-wakeword run okay_rufus_test.yaml
```

The test model is **not** usable for production — scores do not separate wake word from other speech.

## Offline score check

```bash
cd backend
npm run test:wakeword-offline
npm run test:wakeword-offline -- path/to/recording.wav
```

## Capturing real MEMS examples

Enable capture mode while testing with the ESP32. The backend writes decoded
16 kHz mono s16le PCM exactly as the wake-word detector receives it, and
skips VAD/wake-word/Gemini processing so recordings are not interrupted.

```bash
cd backend
WAKEWORD_CAPTURE_AUDIO=1 WAKEWORD_CAPTURE_DIR=models/wakeword/captures/raw npm run dev:live
```

PCM is appended on every uplink packet (~every 250 ms from the ESP32). By default the
backend `fsync`s to disk every 1 second and updates the sidecar `.json` metadata, so
you lose at most ~1 s of audio if the process crashes.

Say "Okay Rufus" many times with a short pause between attempts. Vary distance,
angle, volume, and background noise. Stop the server or disconnect the session
when done.

Split the long PCM captures into reviewable 2-second WAV clips:

```bash
cd backend
npm run capture:wakeword:segment
# optional threshold override if segmentation is too sensitive or misses clips:
npm run capture:wakeword:segment -- models/wakeword/captures/raw models/wakeword/captures/positive --threshold=500
```

Review the WAV clips before using them for additional training. Delete bad cuts,
partial wake words, and non-wake clips from the positive set.

Segment negatives the same way (point the output dir at `captures/negative`):

```bash
npm run capture:wakeword:segment -- models/wakeword/captures/raw/negative models/wakeword/captures/negative
```

## Retrain with device captures

After reviewing clips in `captures/positive/` and `captures/negative/`, merge them
into the existing production splits and retrain (skips TTS generation; re-augment
through eval only):

```bash
cd backend
npm run train:wakeword:prod:captures
```

This copies device WAVs into `output/okay_rufus/{positive,negative}_train/` as
`clip_XXXXXX.wav`, then runs augment → feature extraction → train → export → eval.
Expect several hours on CPU.
