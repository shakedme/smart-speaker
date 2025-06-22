import numpy as np
import tensorflow as tf
from sklearn.metrics import classification_report, f1_score

# 1️⃣ Load TFLite model
interpreter = tf.lite.Interpreter(model_path="rufus_model_dr_quant.tflite")
interpreter.allocate_tensors()

input_details  = interpreter.get_input_details()[0]
output_details = interpreter.get_output_details()[0]

# 2️⃣ Print I/O info (for sanity)
print("── INPUT ──", input_details["dtype"], input_details["quantization"])
print("── OUTPUT ──", output_details["dtype"], output_details["quantization"])

# 3️⃣ Load validation data
data = np.load("rufus_mfcc_dataset.npz")
X_val, y_val = data['X_val'], data['y_val']

def preprocess(mfcc):
    """
    Return a tensor ready for interpreter.set_tensor(),
    honoring the model's expected input dtype.
    """
    if input_details["dtype"] == np.float32:
        # Model expects float32 MFCCs
        return mfcc.astype(np.float32).reshape((1,) + mfcc.shape)
    else:
        # Model expects int8 (shouldn't happen now)
        scale, zp = input_details["quantization"]
        mfcc_int8 = mfcc.astype(np.float32) / scale + zp
        mfcc_int8 = np.clip(mfcc_int8, -128, 127).astype(np.int8)
        return mfcc_int8.reshape((1,) + mfcc.shape)

# 4️⃣ Run inference over the validation set
y_pred = []
for mfcc in X_val:
    inp = preprocess(mfcc)
    interpreter.set_tensor(input_details["index"], inp)
    interpreter.invoke()
    out = interpreter.get_tensor(output_details["index"])[0][0]
    # Dequantize output if needed
    if output_details["dtype"] == np.float32:
        prob = float(out)
    else:
        scale, zp = output_details["quantization"]
        prob = (int(out) - zp) * scale
    y_pred.append(prob)

y_pred = np.array(y_pred)

# 5️⃣ Classification report at 0.5
threshold = 0.5
y_pred_label = (y_pred >= threshold).astype(int)
print(f"\nThreshold = {threshold}")
print(classification_report(y_val, y_pred_label, digits=4))

# 6️⃣ Threshold sweep
best = (0, 0)
for thr in np.linspace(0.2, 0.8, 13):
    preds = (y_pred >= thr).astype(int)
    f1 = f1_score(y_val, preds)
    if f1 > best[1]:
        best = (thr, f1)
    print(f"Thr={thr:.2f} → F1={f1:.4f}")
print(f"\nBest F1={best[1]:.4f} at threshold {best[0]:.2f}")
