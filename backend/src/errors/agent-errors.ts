export class AgentError extends Error {
  constructor(
    message: string,
    readonly userMessage: string,
    readonly recoverable = true,
  ) {
    super(message);
    this.name = new.target.name;
  }
}

export class ToolNotFoundError extends AgentError {
  constructor(toolName: string) {
    super(
      `Unknown tool: ${toolName}`,
      'I could not handle that request.',
      true,
    );
  }
}

export class CalendarNotConnectedError extends AgentError {
  constructor() {
    super(
      'Calendar provider is not connected',
      'Your calendar is not connected yet.',
      true,
    );
  }
}

export class CalendarProviderError extends AgentError {
  constructor(cause?: unknown) {
    super(
      `Calendar provider error: ${cause}`,
      'I could not reach your calendar right now.',
      true,
    );
  }
}

export class IntentClassificationError extends AgentError {
  constructor(reason: string) {
    super(
      `Intent classification failed: ${reason}`,
      'I could not understand that request.',
      true,
    );
  }
}

export class SpotifyResolutionError extends AgentError {
  constructor(query: string, cause?: unknown) {
    super(
      `Spotify resolution failed for "${query}": ${cause}`,
      'I could not find that on Spotify.',
      true,
    );
  }
}
