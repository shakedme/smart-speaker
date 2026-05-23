export type SessionState =
  | 'IDLE'
  | 'LISTENING'
  | 'PROCESSING_INTENT'
  | 'STREAMING_RESPONSE'
  | 'HANDLING_COMMAND'
  | 'WAITING_FOR_USER';

export function isRecordingState(state: SessionState): boolean {
  return state === 'LISTENING' || state === 'WAITING_FOR_USER';
}
