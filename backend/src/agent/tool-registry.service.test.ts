import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { ToolRegistryService } from './tool-registry.service';
import { ToolNotFoundError } from '../errors/agent-errors';

describe('tool-registry', () => {
  it('throws ToolNotFoundError for unknown tool', async () => {
    const registry = new ToolRegistryService();
    await assert.rejects(
      () => registry.execute('nonexistent', {}, { sessionId: 's1', timezone: 'UTC' }),
      ToolNotFoundError,
    );
  });

  it('executes registered tool', async () => {
    const registry = new ToolRegistryService();
    registry.register({
      name: 'echo',
      description: 'echo test',
      execute: async (args: { msg: string }) => ({ echoed: args.msg }),
    });
    const result = await registry.execute('echo', { msg: 'hello' }, {
      sessionId: 's1',
      timezone: 'UTC',
    });
    assert.deepEqual(result, { echoed: 'hello' });
  });
});
