export function createWavBuffer(pcmData: Buffer, sampleRate = 16000): Buffer {
  const numChannels = 1;
  const bitsPerSample = 16;
  const byteRate = sampleRate * numChannels * (bitsPerSample / 8);
  const blockAlign = numChannels * (bitsPerSample / 8);
  const dataSize = pcmData.length;
  const fileSize = 36 + dataSize;

  const buffer = Buffer.alloc(44);

  buffer.write('RIFF', 0);
  buffer.writeUInt32LE(fileSize, 4);
  buffer.write('WAVE', 8);
  buffer.write('fmt ', 12);
  buffer.writeUInt32LE(16, 16);
  buffer.writeUInt16LE(1, 20);
  buffer.writeUInt16LE(numChannels, 22);
  buffer.writeUInt32LE(sampleRate, 24);
  buffer.writeUInt32LE(byteRate, 28);
  buffer.writeUInt16LE(blockAlign, 32);
  buffer.writeUInt16LE(bitsPerSample, 34);
  buffer.write('data', 36);
  buffer.writeUInt32LE(dataSize, 40);

  return Buffer.concat([buffer, pcmData]);
}

export const UPLINK_PCM_SAMPLE_RATE = 16000;
export const GEMINI_RESPONSE_PCM_SAMPLE_RATE = 24000;
export const GEMINI_RESPONSE_VOICE = 'Sulafat';
export const CONVERSATION_TIMEOUT_MS = 5000;
export const WAKE_PHRASE_HINT = 'okay rufus or ok rufus';
