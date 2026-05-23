import { Injectable } from '@nestjs/common';
import { AgentTool, ToolExecutionContext } from './agent-types';
import { ToolNotFoundError } from '../errors/agent-errors';

@Injectable()
export class ToolRegistryService {
  private readonly tools = new Map<string, AgentTool>();

  register(tool: AgentTool): void {
    this.tools.set(tool.name, tool);
  }

  get(name: string): AgentTool | undefined {
    return this.tools.get(name);
  }

  has(name: string): boolean {
    return this.tools.has(name);
  }

  async execute(
    name: string,
    args: Record<string, unknown>,
    context: ToolExecutionContext,
  ): Promise<unknown> {
    const tool = this.tools.get(name);
    if (!tool) {
      throw new ToolNotFoundError(name);
    }
    return tool.execute(args, context);
  }
}
