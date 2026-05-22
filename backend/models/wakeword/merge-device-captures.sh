#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

POSITIVE_SRC="${1:-./captures/positive}"
NEGATIVE_SRC="${2:-./captures/negative}"
OUTPUT_DIR="${3:-./output/okay_rufus}"
MANIFEST="${4:-./captures/.merged-manifest.json}"

positive_train="${OUTPUT_DIR}/positive_train"
negative_train="${OUTPUT_DIR}/negative_train"

if [[ ! -d "${positive_train}" || ! -d "${negative_train}" ]]; then
  echo "Missing training dirs under ${OUTPUT_DIR}. Run production generate first."
  exit 1
fi

python3 - <<PY
import json
import re
import shutil
from pathlib import Path

positive_src = Path("${POSITIVE_SRC}")
negative_src = Path("${NEGATIVE_SRC}")
positive_train = Path("${positive_train}")
negative_train = Path("${negative_train}")
manifest_path = Path("${MANIFEST}")
orig_re = re.compile(r"^clip_\d{6}\.wav$")

if manifest_path.exists():
    manifest = json.loads(manifest_path.read_text())
else:
    manifest = {"positive_train": {}, "negative_train": {}}

def next_index(dest_dir: Path) -> int:
    mx = max(
        (int(p.stem.split("_")[1]) for p in dest_dir.iterdir() if orig_re.match(p.name)),
        default=-1,
    )
    return mx + 1

def merge_split(src_dir: Path, dest_dir: Path, split_key: str, label: str) -> None:
    if not src_dir.is_dir():
        print(f"Skipping {label}: {src_dir} not found")
        return

    files = sorted(src_dir.glob("*.wav"))
    if not files:
        print(f"Skipping {label}: no .wav files in {src_dir}")
        return

    split_manifest = manifest.setdefault(split_key, {})
    idx = next_index(dest_dir)
    copied = 0
    skipped = 0

    for src in files:
        src_key = str(src.resolve())
        existing = split_manifest.get(src_key)
        if existing:
            if Path(existing).exists():
                skipped += 1
                continue
            split_manifest.pop(src_key, None)

        dest = dest_dir / f"clip_{idx:06d}.wav"
        shutil.copy2(src, dest)
        split_manifest[src_key] = str(dest)
        idx += 1
        copied += 1

    if copied:
        print(f"Merged {copied} new {label} clips into {dest_dir}")
    if skipped:
        print(f"Skipped {skipped} already-merged {label} clips")
    if copied == 0 and skipped == 0:
        print(f"No {label} clips to merge")

merge_split(positive_src, positive_train, "positive_train", "positive")
merge_split(negative_src, negative_train, "negative_train", "device negative")

manifest_path.parent.mkdir(parents=True, exist_ok=True)
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
PY
