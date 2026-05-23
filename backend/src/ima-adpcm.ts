const INDEX_TABLE = [
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8,
];

const STEP_TABLE = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
  19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
  5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

export interface ImaAdpcmState {
  predictor: number;
  index: number;
}

export function createImaAdpcmState(): ImaAdpcmState {
  return { predictor: 0, index: 0 };
}

function clamp16(value: number): number {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return value;
}

function decodeNibble(nibble: number, state: ImaAdpcmState): void {
  const step: number = STEP_TABLE[state.index] ?? STEP_TABLE[0] ?? 7;
  let delta = step >> 3;
  if (nibble & 4) delta += step;
  if (nibble & 2) delta += step >> 1;
  if (nibble & 1) delta += step >> 2;
  if (nibble & 8) delta = -delta;

  state.predictor = clamp16(state.predictor + delta);
  const indexDelta = INDEX_TABLE[nibble] ?? 0;
  state.index = Math.max(0, Math.min(88, state.index + indexDelta));
}

export function decodeImaAdpcmToPcm(adpcm: Buffer, state: ImaAdpcmState): Buffer {
  // Each ADPCM byte encodes two 16-bit samples (low/high nibble).
  const samples = Buffer.alloc(adpcm.length * 4);
  let writeOffset = 0;

  for (let i = 0; i < adpcm.length; i++) {
    const byte = adpcm[i] ?? 0;
    decodeNibble(byte & 0x0f, state);
    samples.writeInt16LE(state.predictor, writeOffset);
    writeOffset += 2;
    decodeNibble((byte >> 4) & 0x0f, state);
    samples.writeInt16LE(state.predictor, writeOffset);
    writeOffset += 2;
  }

  return samples.subarray(0, writeOffset);
}

export function decodeImaAdpcmBlockToPcm(block: Buffer, sampleCount: number): Buffer | null {
  if (sampleCount <= 0 || block.length < 4) {
    return null;
  }

  const out = Buffer.alloc(sampleCount * 2);
  const state: ImaAdpcmState = {
    predictor: block.readInt16LE(0),
    index: Math.max(0, Math.min(88, block.readUInt8(2))),
  };

  out.writeInt16LE(state.predictor, 0);
  let writeOffset = 2;
  let samplesWritten = 1;

  for (let i = 4; i < block.length && samplesWritten < sampleCount; i++) {
    const byte = block[i] ?? 0;
    decodeNibble(byte & 0x0f, state);
    out.writeInt16LE(state.predictor, writeOffset);
    writeOffset += 2;
    samplesWritten++;

    if (samplesWritten >= sampleCount) {
      break;
    }

    decodeNibble((byte >> 4) & 0x0f, state);
    out.writeInt16LE(state.predictor, writeOffset);
    writeOffset += 2;
    samplesWritten++;
  }

  return samplesWritten === sampleCount ? out : null;
}

export const UPLINK_FORMAT_PCM = 0;
export const UPLINK_FORMAT_IMA_ADPCM = 1;
