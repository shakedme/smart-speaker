export interface ToolExecutionContext {
  sessionId: string;
  timezone: string;
  userId?: string;
  now?: Date;
}

export interface AgentTool<TArgs = unknown, TResult = unknown> {
  name: string;
  description: string;
  execute(args: TArgs, context: ToolExecutionContext): Promise<TResult>;
}

export interface AgentContext {
  sessionId: string;
  timezone: string;
  userId?: string;
}

export type DeviceCommand = {
  type: 'intent';
  intent: 'spotify' | 'playback_control';
  data: string;
};

export type AgentResult =
  | {
      type: 'device_command';
      command: DeviceCommand;
    }
  | {
      type: 'spoken_response';
      text: string;
    }
  | {
      type: 'live_response';
      prompt: string;
    }
  | {
      type: 'error';
      message: string;
      recoverable: boolean;
    };
