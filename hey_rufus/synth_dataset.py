import os
import random

from google.cloud import texttospeech

os.environ["GOOGLE_APPLICATION_CREDENTIALS"] = "../backend/google_api_service_account.json"

SAVE_DIR = "dataset/positive"
os.makedirs(SAVE_DIR, exist_ok=True)

client = texttospeech.TextToSpeechClient()


def synthesize_speech(text, pitch=0.0, speed=1.0, voice_name="en-US-Wavenet-D", filename="output.wav"):
    synthesis_input = texttospeech.SynthesisInput(text=text)

    voice = texttospeech.VoiceSelectionParams(
        language_code=voice_name[:5],  # Extract language code from voice name
        name=voice_name,
    )

    audio_config = texttospeech.AudioConfig(
        audio_encoding=texttospeech.AudioEncoding.LINEAR16,
        pitch=pitch,
        speaking_rate=speed
    )

    response = client.synthesize_speech(
        input=synthesis_input,
        voice=voice,
        audio_config=audio_config
    )

    if not response.audio_content:
        raise ValueError("No audio content returned from synthesis.")

    with open(filename, "wb") as out:
        out.write(response.audio_content)


# Generate samples
NUM_SAMPLES = 1000
total = 173
while total < NUM_SAMPLES:
    pitch = round(random.uniform(-5.0, 5.0), 2)  # Vary pitch
    speed = round(random.uniform(0.85, 1.2), 2)  # Vary speaking rate
    voices_res = client.list_voices(language_code="en-US")

    voice = random.choice(voices_res.voices)

    fname = f"hey_rufus_{total:04d}.wav"
    out_path = os.path.join(SAVE_DIR, fname)

    try:
        synthesize_speech("Hey Rufus", pitch, speed, voice.name, out_path)
        total += 1
        print(f"[{total + 1}/{NUM_SAMPLES}] Saved: {fname} | Voice: {voice}, Pitch: {pitch}, Speed: {speed}")
    except Exception as e:
        print(f"Failed to generate {fname}: {e}")

if __name__ == "__main__":
    print(f"Generated {NUM_SAMPLES} samples in {SAVE_DIR}")
