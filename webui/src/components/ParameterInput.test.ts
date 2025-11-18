import { describe, it, expect } from 'vitest';
import { render, fireEvent } from '@testing-library/svelte';
import ParameterInput from './ParameterInput.svelte';
import type { ParameterMeta } from '../lib/types';

describe('ParameterInput', () => {
  it('renders number input with slider', () => {
    const param: ParameterMeta = {
      name: 'wpm',
      type: 'UINT32',
      min: 5,
      max: 80,
      description: 'Keying speed',
      unit: 'WPM',
      reset_required: false,
      category: 'normal',
    };

    const { getByLabelText, getAllByDisplayValue } = render(ParameterInput, {
      props: { param, value: 25 },
    });

    expect(getByLabelText(/keying speed/i)).toBeInTheDocument();
    const inputs = getAllByDisplayValue('25');
    expect(inputs).toHaveLength(2); // slider and number input
  });

  it('shows error for invalid value', () => {
    const param: ParameterMeta = {
      name: 'wpm',
      type: 'UINT32',
      min: 5,
      max: 80,
      description: 'Keying speed',
      unit: 'WPM',
      reset_required: false,
      category: 'normal',
    };

    const { getByText, getByDisplayValue } = render(ParameterInput, {
      props: { param, value: 100 },
    });

    expect(getByText(/must be between 5 and 80/i)).toBeInTheDocument();
  });

  it('shows reset required badge', () => {
    const param: ParameterMeta = {
      name: 'dit_gpio',
      type: 'INT32',
      min: 0,
      max: 48,
      description: 'Dit GPIO pin',
      unit: '',
      reset_required: true,
      category: 'advanced',
    };

    const { getByText } = render(ParameterInput, {
      props: { param, value: 15 },
    });

    expect(getByText(/⚠️/)).toBeInTheDocument();
  });

  it('renders checkbox for boolean', () => {
    const param: ParameterMeta = {
      name: 'enabled',
      type: 'BOOL',
      description: 'Sidetone enabled',
      unit: '',
      reset_required: false,
      category: 'normal',
    };

    const { getByRole } = render(ParameterInput, {
      props: { param, value: true },
    });

    const checkbox = getByRole('checkbox');
    expect(checkbox).toBeChecked();
  });
});
