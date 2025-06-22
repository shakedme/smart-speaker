# train_rufus_model.py

import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models
from tensorflow.keras.callbacks import ModelCheckpoint, EarlyStopping
from sklearn.metrics import classification_report
import matplotlib.pyplot as plt

# ---- 1. Load Dataset ----
data = np.load("rufus_mfcc_dataset.npz")
X_train, y_train = data['X_train'], data['y_train']
X_val, y_val     = data['X_val'],   data['y_val']

print(f"Training shape:   {X_train.shape}, {y_train.shape}")
print(f"Validation shape: {X_val.shape},   {y_val.shape}")

# ---- 2. Build Model ----
def build_model(input_shape=(49, 40)):
    model = models.Sequential([
        layers.Input(shape=input_shape),
        layers.Conv1D(32, kernel_size=3, activation='relu'),
        layers.BatchNormalization(),
        layers.MaxPooling1D(pool_size=2),

        layers.Conv1D(64, kernel_size=3, activation='relu'),
        layers.BatchNormalization(),
        layers.MaxPooling1D(pool_size=2),

        layers.Flatten(),
        layers.Dense(64, activation='relu'),
        layers.Dropout(0.3),
        layers.Dense(1, activation='sigmoid'),  # binary output
    ])
    model.compile(
        optimizer='adam',
        loss='binary_crossentropy',
        metrics=['accuracy']
    )
    return model

model = build_model()

# ---- 3. Callbacks for Saving Best Model ----
checkpoint_cb = ModelCheckpoint(
    "best_rufus_model.h5",   # file will contain the best weights
    monitor="val_accuracy",   # metric to monitor
    mode="max",               # we want to maximize val_accuracy
    save_best_only=True,      # only save when val_accuracy improves
    verbose=1
)

earlystop_cb = EarlyStopping(
    monitor="val_accuracy",
    mode="max",
    patience=3,               # stop after 3 epochs without improvement
    restore_best_weights=True,# reload best weights after stopping
    verbose=1
)

# ---- 4. Train ----
history = model.fit(
    X_train, y_train,
    validation_data=(X_val, y_val),
    epochs=50,                # increase if you want more training
    batch_size=32,
    callbacks=[checkpoint_cb, earlystop_cb]
)

# ---- 5. Evaluate on Validation Set ----
val_probs   = model.predict(X_val).flatten()
val_preds   = (val_probs > 0.5).astype(int)
print("\nClassification Report (val set):")
print(classification_report(y_val, val_preds, digits=4))

# ---- 6. Plot Training History ----
plt.figure(figsize=(8,5))
plt.plot(history.history['accuracy'],    label="Train Acc")
plt.plot(history.history['val_accuracy'],label="Val Acc")
plt.title("Training History")
plt.xlabel("Epoch")
plt.ylabel("Accuracy")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()

if __name__ == "__main__":
    print("Training..")