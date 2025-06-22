import tensorflow as tf

# 1. Load best Keras model
model = tf.keras.models.load_model("best_rufus_model.h5")

# 2. Configure dynamic-range quantization
converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]

# ⚠️ **Do not** set representative_dataset or inference_input/output_type.

# 3. Convert & save
tflite_model = converter.convert()
with open("rufus_model_dr_quant.tflite", "wb") as f:
    f.write(tflite_model)

print("✅ Wrote weights-only dynamic-range model to rufus_model_dr_quant.tflite")
