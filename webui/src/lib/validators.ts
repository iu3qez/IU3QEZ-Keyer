import type { ParameterMeta } from './types';

export interface ValidationResult {
  valid: boolean;
  error?: string;
}

export function validateParameter(
  param: ParameterMeta,
  value: string | number | boolean
): ValidationResult {
  // Type checking
  if (param.type === 'BOOL') {
    // Accept boolean, or truthy string/number values
    if (typeof value === 'boolean') {
      return { valid: true };
    }
    if (typeof value === 'string') {
      const lower = value.toLowerCase();
      if (lower === 'true' || lower === 'false' || lower === '1' || lower === '0') {
        return { valid: true };
      }
    }
    if (typeof value === 'number' && (value === 0 || value === 1)) {
      return { valid: true };
    }
    // If value is undefined, it's still valid (will use default)
    if (value === undefined) {
      return { valid: true };
    }
    return { valid: false, error: 'Value must be true or false' };
  }

  if (param.type === 'STRING') {
    // Allow undefined (will use default or empty string)
    if (value === undefined || value === null) {
      return { valid: true };
    }

    if (typeof value !== 'string') {
      // Try to convert to string
      value = String(value);
    }

    if (param.min !== undefined && value.length < param.min) {
      return {
        valid: false,
        error: `Value must be at least ${param.min} characters`,
      };
    }

    if (param.max !== undefined && value.length > param.max) {
      return {
        valid: false,
        error: `Value must be at most ${param.max} characters`,
      };
    }

    return { valid: true };
  }

  // Numeric types
  if (
    param.type === 'INT32' ||
    param.type === 'UINT32' ||
    param.type === 'UINT16' ||
    param.type === 'UINT8' ||
    param.type === 'INT8' ||
    param.type === 'FLOAT'
  ) {
    // Allow undefined (will use default)
    if (value === undefined || value === null) {
      return { valid: true };
    }

    // Allow empty string during editing (user is typing)
    if (typeof value === 'string' && value.trim() === '') {
      return { valid: true };
    }

    const numValue = typeof value === 'number' ? value : parseFloat(value as string);

    if (isNaN(numValue)) {
      return { valid: false, error: 'Value must be a number' };
    }

    if (param.min !== undefined && numValue < param.min) {
      return {
        valid: false,
        error: `Value must be between ${param.min} and ${param.max}`,
      };
    }

    if (param.max !== undefined && numValue > param.max) {
      return {
        valid: false,
        error: `Value must be between ${param.min} and ${param.max}`,
      };
    }

    return { valid: true };
  }

  if (param.type === 'ENUM') {
    // Allow undefined (will use default)
    if (value === undefined || value === null) {
      return { valid: true };
    }

    if (typeof value !== 'string') {
      return { valid: false, error: 'Value must be a string' };
    }

    if (param.enum_values) {
      const validValues = param.enum_values.map((ev) => ev.name);
      if (!validValues.includes(value)) {
        return {
          valid: false,
          error: `Value must be one of: ${validValues.join(', ')}`,
        };
      }
    }

    return { valid: true };
  }

  return { valid: true };
}
