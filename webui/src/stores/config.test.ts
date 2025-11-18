import { describe, it, expect, vi, beforeEach } from 'vitest';
import { get } from 'svelte/store';
import { configStore } from './config';
import type { ConfigSchema, DeviceConfig } from '../lib/types';

describe('configStore', () => {
  beforeEach(() => {
    configStore.reset();
  });

  it('initializes with empty state', () => {
    const state = get(configStore);
    expect(state.schema).toBeNull();
    expect(state.config).toEqual({});
    expect(state.loading).toBe(false);
  });

  it('sets schema', () => {
    const mockSchema: ConfigSchema = {
      subsystems: {
        audio: {
          freq: {
            name: 'freq',
            type: 'UINT16',
            min: 100,
            max: 2000,
            description: 'Frequency',
            unit: 'Hz',
            reset_required: false,
            category: 'normal',
          },
        },
      },
    };

    configStore.setSchema(mockSchema);
    const state = get(configStore);
    expect(state.schema).toEqual(mockSchema);
  });

  it('tracks dirty parameters', () => {
    configStore.setConfig({ audio: { freq: 700 } });
    configStore.markDirty('audio.freq', 700, 800);

    const state = get(configStore);
    expect(state.dirtyParams.has('audio.freq')).toBe(true);
    expect(state.dirtyParams.get('audio.freq')).toEqual({
      oldValue: 700,
      newValue: 800,
    });
  });

  it('tracks parameters requiring reset', () => {
    const mockSchema: ConfigSchema = {
      subsystems: {
        hardware: {
          dit_gpio: {
            name: 'dit_gpio',
            type: 'INT32',
            min: 0,
            max: 48,
            description: 'Dit GPIO',
            unit: '',
            reset_required: true,
            category: 'advanced',
          },
        },
      },
    };

    configStore.setSchema(mockSchema);
    configStore.markDirty('hardware.dit_gpio', 15, 18);

    const state = get(configStore);
    expect(state.resetRequired).toBe(true);
  });
});
