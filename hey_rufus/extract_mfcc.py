import os
import random
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf
from librosa import feature
from sklearn.model_selection import train_test_split
from tqdm import tqdm

# ---- Config ----
DATASET_DIR = "dataset"
SAMPLE_RATE = 16000
TARGET_LEN = SAMPLE_RATE  # 1 second
N_MFCC = 40
MAX_LEN = 49  # MFCC time steps

POS_CLEAN_DIR = f"{DATASET_DIR}/positive"
POS_AUG_DIR = f"{DATASET_DIR}/positive_aug"
NEG_CLEAN_DIR = f"{DATASET_DIR}/negative"
NEG_AUG_DIR = f"{DATASET_DIR}/negative_aug"
NOISE_RAW_DIR = f"{DATASET_DIR}/negative_raw"

os.makedirs(POS_AUG_DIR, exist_ok=True)
os.makedirs(NEG_AUG_DIR, exist_ok=True)


# ---- Audio I/O ----
def load_audio(path):
    y, sr = librosa.load(path, sr=SAMPLE_RATE)
    if len(y) > TARGET_LEN:
        y = y[:TARGET_LEN]
    else:
        y = np.pad(y, (0, TARGET_LEN - len(y)))
    return y


def mix_audio(clean, noise, snr_db):
    noise = noise[:TARGET_LEN]
    rms_clean = np.sqrt(np.mean(clean ** 2))
    rms_noise = np.sqrt(np.mean(noise ** 2))
    desired_rms_noise = rms_clean / (10 ** (snr_db / 20))
    noise = noise * (desired_rms_noise / (rms_noise + 1e-8))
    mixed = clean + noise
    return mixed / max(np.abs(mixed) + 1e-8)


# ---- MFCC Extraction ----
def extract_mfcc(y):
    mfcc = librosa.feature.mfcc(y=y, sr=SAMPLE_RATE, n_mfcc=N_MFCC, n_fft=400, hop_length=160)
    mfcc = mfcc.T  # shape: (time_steps, n_mfcc)
    if mfcc.shape[0] > MAX_LEN:
        mfcc = mfcc[:MAX_LEN, :]
    else:
        mfcc = np.pad(mfcc, ((0, MAX_LEN - mfcc.shape[0]), (0, 0)), mode='constant')
    return mfcc


# ---- 1. Augment Positives ----
def augment_positives():
    pos_files = sorted([f for f in os.listdir(POS_CLEAN_DIR) if f.endswith('.wav')])

    # Gather noise files from all subfolders
    noise_files = []
    for sub in ["speech", "music", "noise"]:
        sub_dir = os.path.join(NOISE_RAW_DIR, sub)
        if os.path.exists(sub_dir):
            noise_files += [
                os.path.join(sub_dir, f) for f in os.listdir(sub_dir) if f.endswith(".wav")
            ]

    if not noise_files:
        raise RuntimeError("No noise files found in any subfolders of dataset/negative_raw/")

    for f in tqdm(pos_files, desc="Augmenting positives"):
        clean = load_audio(os.path.join(POS_CLEAN_DIR, f))
        noise = load_audio(random.choice(noise_files))
        snr = random.uniform(5, 20)
        mixed = mix_audio(clean, noise, snr)
        sf.write(os.path.join(POS_AUG_DIR, f"aug_{f}"), mixed, SAMPLE_RATE)


# ---- 2. Slice Negatives from Noise ----
def augment_negatives(total_target=2000):
    """
    Slice negative samples from musan into 1-second .wav files.
    Pulls from speech, music, and noise folders in a controlled ratio.
    """
    print("Augmenting negative samples from MUSAN (speech, music, noise)...")
    os.makedirs(NEG_AUG_DIR, exist_ok=True)

    # Desired breakdown
    subfolders = ["speech", "music", "noise"]
    alloc = {
        "speech": int(0.3 * total_target),  # 30%
        "music": int(0.3 * total_target),  # 30%
        "noise": int(0.4 * total_target),  # 40%
    }

    for sub in subfolders:
        sub_dir = os.path.join(NOISE_RAW_DIR, sub)
        files = [f for f in os.listdir(sub_dir) if f.endswith('.wav')]
        if not files:
            print(f"Warning: No .wav files found in {sub_dir}")
            continue

        saved_count = 0
        loop_count = 0
        print(f" - Generating {alloc[sub]} samples from {sub}/")

        while saved_count < alloc[sub] and loop_count < 10000:
            loop_count += 1
            file = random.choice(files)
            try:
                y, sr = librosa.load(os.path.join(sub_dir, file), sr=SAMPLE_RATE)
                if len(y) < SAMPLE_RATE:
                    continue

                start = random.randint(0, len(y) - SAMPLE_RATE)
                clip = y[start:start + SAMPLE_RATE]

                out_path = os.path.join(
                    NEG_AUG_DIR,
                    f"neg_{sub}_{Path(file).stem}_{saved_count:04d}.wav"
                )
                sf.write(out_path, clip, SAMPLE_RATE)
                saved_count += 1

            except Exception as e:
                print(f"Error processing {file}: {e}")

        print(f"   → Saved {saved_count} negative samples from {sub}/")

    print(f"Done. Total negative samples saved: {sum(alloc.values())}")


# ---- 3. Collect & Extract All MFCCs ----
def collect_and_extract():
    X, y = [], []

    label_map = [
        (POS_CLEAN_DIR, 1),
        (POS_AUG_DIR, 1),
        (NEG_CLEAN_DIR, 0),
        (NEG_AUG_DIR, 0)
    ]

    for folder, label in label_map:
        files = sorted([f for f in os.listdir(folder) if f.endswith('.wav')])
        for f in tqdm(files, desc=f"Extracting MFCCs from {folder}"):
            try:
                y_audio = load_audio(os.path.join(folder, f))
                mfcc = extract_mfcc(y_audio)
                X.append(mfcc)
                y.append(label)
            except Exception as e:
                print(f"Failed on {f}: {e}")

    X = np.array(X)
    y = np.array(y)
    return X, y


# ---- MAIN ----
if __name__ == "__main__":
    print("=== Augmenting Data ===")
    augment_positives()
    augment_negatives()

    print("=== Extracting MFCCs ===")
    X, y = collect_and_extract()
    print("Final shape:", X.shape, y.shape)

    print("=== Splitting and Saving Dataset ===")
    X_train, X_val, y_train, y_val = train_test_split(X, y, test_size=0.2, stratify=y, random_state=42)
    np.savez("rufus_mfcc_dataset.npz", X_train=X_train, y_train=y_train, X_val=X_val, y_val=y_val)
    print("Saved to rufus_mfcc_dataset.npz")
