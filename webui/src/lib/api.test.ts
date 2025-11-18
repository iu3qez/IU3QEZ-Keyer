import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClient } from './api';
import type { ConfigSchema, DeviceConfig } from './types';

describe('ApiClient', () => {
  let client: ApiClient;

  beforeEach(() => {
    client = new ApiClient('http://test.local');
    global.fetch = vi.fn();
  });

  it('fetches config schema successfully', async () => {
    const mockSchema: ConfigSchema = {
      subsystems: {
        audio: {
          freq: {
            name: 'freq',
            type: 'UINT16',
            min: 100,
            max: 2000,
            description: 'Sidetone frequency',
            unit: 'Hz',
            reset_required: false,
            category: 'normal',
          },
        },
      },
    };

    (global.fetch as any).mockResolvedValueOnce({
      ok: true,
      json: async () => mockSchema,
    });

    const schema = await client.getSchema();
    expect(schema).toEqual(mockSchema);
    expect(global.fetch).toHaveBeenCalledWith('http://test.local/api/config/schema');
  });

  it('handles fetch errors gracefully', async () => {
    (global.fetch as any).mockRejectedValueOnce(new Error('Network error'));

    await expect(client.getSchema()).rejects.toThrow('Network error');
  });
});
