import numpy as np
import sounddevice as sd
import librosa
import tensorflow as tf
import time
import sys

# ── Config ─────────────────────────────────────────
MODEL_PATH    = "rufus_model_quant.tflite"
SAMPLE_RATE   = 16000
DURATION      = 1.0      # Window length (s)
N_MFCC        = 40
MAX_LEN       = 49       # MFCC frames
THRESHOLD     = 0.20     # From your threshold sweep
DETECTION_REQ = 1        # Number of consecutive hits to trigger

# ── Initialize TFLite Interpreter ─────────────────
print("Loading TFLite model…", flush=True)
interp = tf.lite.Interpreter(model_path=MODEL_PATH)
interp.allocate_tensors()
inp_det = interp.get_input_details()[0]
out_det = interp.get_output_details()[0]
print(f"Model input dtype: {inp_det['dtype']}, shape: {inp_det['shape']}", flush=True)

# ── MFCC Extraction ────────────────────────────────
def extract_mfcc(y):
    mfcc = librosa.feature.mfcc(
        y=y, sr=SAMPLE_RATE,
        n_mfcc=N_MFCC, n_fft=400, hop_length=160
    ).T
    if mfcc.shape[0] > MAX_LEN:
        mfcc = mfcc[:MAX_LEN]
    else:
        mfcc = np.pad(mfcc, ((0, MAX_LEN-mfcc.shape[0]), (0,0)), mode="constant")
    return mfcc

# ── Audio Callback & Detection Loop ───────────────
def listen_and_detect():
    frame_count = int(DURATION * SAMPLE_RATE)
    buffer = np.zeros(frame_count, dtype=np.float32)
    hit_count = 0
    loop = 0

    print("Starting live detection. Press Ctrl+C to exit.\n", flush=True)
    while True:
        loop += 1
        # Debug: print loop number
        print(f"[{loop}] Recording {frame_count} frames…", flush=True)
        audio = sd.rec(frames=frame_count, samplerate=SAMPLE_RATE, channels=1, dtype='float32')
        sd.wait()

        # Debug: show audio stats
        audio_flat = audio.flatten()
        print(f"  Audio: min={audio_flat.min():.3f}, max={audio_flat.max():.3f}, mean={audio_flat.mean():.3f}", flush=True)

        # Extract MFCC
        mfcc = extract_mfcc(audio_flat)
        print(f"  MFCC shape: {mfcc.shape}", flush=True)

        inp = mfcc.astype(np.float32).reshape((1, MAX_LEN, N_MFCC))
        print("  Running inference…", flush=True)
        interp.set_tensor(inp_det['index'], inp)
        interp.invoke()
        prob = float(interp.get_tensor(out_det['index'])[0][0])

        print(f"  Probability = {prob:.3f}", flush=True)

        # Detection FSM
        if prob >= THRESHOLD:
            hit_count += 1
            print(f"    → hit {hit_count}/{DETECTION_REQ}", flush=True)
        else:
            if hit_count > 0:
                print("    → reset hit_count", flush=True)
            hit_count = 0

        if hit_count >= DETECTION_REQ:
            print("\n🎉 Hey Rufus DETECTED!\n", flush=True)
            hit_count = 0

        # Small sleep to avoid busy spin
        time.sleep(0.1)

if __name__ == "__main__":
    try:
        listen_and_detect()
    except KeyboardInterrupt:
        print("\nExiting live test.", flush=True)
        sys.exit(0)
