#!/usr/bin/env python3
"""
Long-lived wake word inference worker for the Node.js backend.

Reads newline-delimited JSON requests on stdin, writes responses on stdout.

Request:  {"cmd": "predict", "pcm_b64": "<base64 s16le mono 16kHz PCM>"}
Response: {"ok": true, "score": 0.95, "model": "okay_rufus"}
         {"ok": false, "error": "..."}

Request:  {"cmd": "ping"}
Response: {"ok": true, "model": "okay_rufus"}
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
from pathlib import Path

import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LiveKit wakeword inference worker")
    parser.add_argument(
        "--model",
        required=True,
        help="Path to exported wake word classifier ONNX file",
    )
    return parser.parse_args()


def load_model(model_path: str):
    from livekit.wakeword import WakeWordModel

    path = Path(model_path)
    if not path.is_file():
        raise FileNotFoundError(f"Wake word model not found: {path}")

    model = WakeWordModel(models=[str(path)])
    model_name = path.stem
    if model_name.endswith(".int8"):
        model_name = model_name[: -len(".int8")]
    return model, model_name


def pcm_b64_to_int16(pcm_b64: str) -> np.ndarray:
    raw = base64.b64decode(pcm_b64)
    if len(raw) % 2 != 0:
        raise ValueError("PCM byte length must be even (16-bit samples)")
    if len(raw) == 0:
        raise ValueError("Empty PCM payload")
    return np.frombuffer(raw, dtype=np.int16)


def handle_predict(model, model_name: str, pcm_b64: str) -> dict:
    samples = pcm_b64_to_int16(pcm_b64)
    scores = model.predict(samples)
    score = float(scores.get(model_name, next(iter(scores.values()))))
    return {"ok": True, "score": score, "model": model_name}


def main() -> int:
    args = parse_args()
    try:
        model, model_name = load_model(args.model)
    except Exception as exc:  # noqa: BLE001 - report to parent process
        print(json.dumps({"ok": False, "error": str(exc)}), flush=True)
        return 1

    print(json.dumps({"ok": True, "ready": True, "model": model_name}), flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            cmd = req.get("cmd")
            if cmd == "ping":
                resp = {"ok": True, "model": model_name}
            elif cmd == "predict":
                resp = handle_predict(model, model_name, req["pcm_b64"])
            else:
                resp = {"ok": False, "error": f"Unknown cmd: {cmd}"}
        except Exception as exc:  # noqa: BLE001
            resp = {"ok": False, "error": str(exc)}
        print(json.dumps(resp), flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
