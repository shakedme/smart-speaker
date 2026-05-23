import {
  createImaAdpcmState,
  decodeImaAdpcmToPcm,
  ImaAdpcmState,
  UPLINK_FORMAT_IMA_ADPCM,
  UPLINK_FORMAT_PCM,
} from '../ima-adpcm';

export class UplinkDecoder {
  private adpcmState: ImaAdpcmState = createImaAdpcmState();

  decode(data: Buffer): Buffer {
    if (data.length === 0) {
      return data;
    }

    const format = data[0]!;
    if (format === UPLINK_FORMAT_IMA_ADPCM && data.length > 1) {
      return decodeImaAdpcmToPcm(data.subarray(1), this.adpcmState);
    }
    if (format === UPLINK_FORMAT_PCM && data.length > 1) {
      const payload = data.subarray(1);
      if (payload.length % 2 !== 0) {
        return data;
      }
      return payload;
    }

    return data;
  }

  resetAdpcmState(): void {
    this.adpcmState = createImaAdpcmState();
  }
}
