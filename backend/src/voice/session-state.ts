export type SessionState =
  | 'IDLE'
  | 'LISTENING'
  | 'LISTENING_FOR_COMMAND'
  | 'PROCESSING_INTENT'
  | 'STREAMING_RESPONSE'
  | 'HANDLING_COMMAND'
  | 'WAITING_FOR_USER';

export function isRecordingState(state: SessionState): boolean {
  return state === 'LISTENING' || state === 'LISTENING_FOR_COMMAND' || state === 'WAITING_FOR_USER';
}
