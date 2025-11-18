import { describe, it, expect } from 'vitest';
import { validateParameter } from './validators';
import type { ParameterMeta } from './types';

describe('validateParameter', () => {
  it('validates number within range', () => {
    const param: ParameterMeta = {
      name: 'wpm',
      type: 'UINT32',
      min: 5,
      max: 80,
      description: 'Speed',
      unit: 'WPM',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, 25);
    expect(result.valid).toBe(true);
    expect(result.error).toBeUndefined();
  });

  it('rejects number below min', () => {
    const param: ParameterMeta = {
      name: 'wpm',
      type: 'UINT32',
      min: 5,
      max: 80,
      description: 'Speed',
      unit: 'WPM',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, 3);
    expect(result.valid).toBe(false);
    expect(result.error).toBe('Value must be between 5 and 80');
  });

  it('rejects number above max', () => {
    const param: ParameterMeta = {
      name: 'wpm',
      type: 'UINT32',
      min: 5,
      max: 80,
      description: 'Speed',
      unit: 'WPM',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, 100);
    expect(result.valid).toBe(false);
    expect(result.error).toBe('Value must be between 5 and 80');
  });

  it('validates string length', () => {
    const param: ParameterMeta = {
      name: 'callsign',
      type: 'STRING',
      min: 3,
      max: 15,
      description: 'Callsign',
      unit: '',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, 'IU3QEZ');
    expect(result.valid).toBe(true);
  });

  it('rejects string too short', () => {
    const param: ParameterMeta = {
      name: 'callsign',
      type: 'STRING',
      min: 3,
      max: 15,
      description: 'Callsign',
      unit: '',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, 'AB');
    expect(result.valid).toBe(false);
    expect(result.error).toContain('at least 3 characters');
  });

  it('validates boolean', () => {
    const param: ParameterMeta = {
      name: 'enabled',
      type: 'BOOL',
      description: 'Enabled',
      unit: '',
      reset_required: false,
      category: 'normal',
    };

    const result = validateParameter(param, true);
    expect(result.valid).toBe(true);
  });
});
