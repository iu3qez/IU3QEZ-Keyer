<script lang="ts">
  import { validateParameter } from '../lib/validators';
  import type { ParameterMeta } from '../lib/types';

  export let param: ParameterMeta;
  export let value: string | number | boolean;
  export let onChange: (newValue: string | number | boolean) => void = () => {};
  export let dirty: boolean = false;

  $: validation = validateParameter(param, value);

  function handleChange(event: Event) {
    const target = event.target as HTMLInputElement | HTMLSelectElement;
    let newValue: string | number | boolean;

    if (param.type === 'BOOL') {
      newValue = (target as HTMLInputElement).checked;
    } else if (param.type === 'STRING' || param.type === 'ENUM') {
      // Pass string value as-is
      newValue = target.value;
    } else {
      // Numeric types: pass raw string value
      // Validator will handle parsing and validation, including empty strings during editing
      newValue = target.value;
    }

    onChange(newValue);
  }
</script>

<div class="parameter-input" class:error={!validation.valid} class:dirty>
  <label for={param.name}>
    <span class="label-text">
      {param.description}
      {#if param.reset_required}
        <span class="badge-reset" title="Requires device reboot">⚠️</span>
      {/if}
    </span>

    {#if param.type === 'BOOL'}
      <input
        id={param.name}
        type="checkbox"
        checked={value === true}
        on:change={handleChange}
      />
    {:else if param.type === 'STRING'}
      <input
        id={param.name}
        type="text"
        value={value ?? ''}
        minlength={param.min}
        maxlength={param.max}
        on:input={handleChange}
      />
    {:else if param.type === 'ENUM'}
      <select id={param.name} value={value ?? ''} on:change={handleChange}>
        {#each param.enum_values || [] as enumVal}
          <option value={enumVal.name}>{enumVal.name} - {enumVal.description}</option>
        {/each}
      </select>
    {:else if param.widget_hint === 'number_input'}
      <!-- Number input only (no slider) for ports, addresses, etc. -->
      <div class="number-input-only">
        <input
          id={param.name}
          type="number"
          min={param.min}
          max={param.max}
          step={param.type === 'FLOAT' ? 0.1 : 1}
          value={value ?? param.min ?? 0}
          on:input={handleChange}
        />
        {#if param.unit}
          <span class="unit">{param.unit}</span>
        {/if}
      </div>
    {:else}
      <!-- Slider + number input for ranges like WPM, volume, etc. -->
      <div class="number-input">
        <input
          id={param.name}
          type="range"
          min={param.min}
          max={param.max}
          step={param.type === 'FLOAT' ? 0.1 : 1}
          value={value ?? param.min ?? 0}
          on:input={handleChange}
        />
        <input
          type="number"
          min={param.min}
          max={param.max}
          step={param.type === 'FLOAT' ? 0.1 : 1}
          value={value ?? param.min ?? 0}
          on:input={handleChange}
          class="number-display"
        />
        {#if param.unit}
          <span class="unit">{param.unit}</span>
        {/if}
      </div>
    {/if}
  </label>

  {#if !validation.valid}
    <p class="error-message">{validation.error}</p>
  {/if}
</div>

<style>
  .parameter-input {
    margin-bottom: 1.5rem;
    transition: all 0.2s;
  }

  .parameter-input.dirty {
    background: #fff3cd;
    padding: 0.75rem;
    border-radius: 6px;
    border-left: 4px solid #f39c12;
  }

  .parameter-input.error {
    background: #f8d7da;
    padding: 0.75rem;
    border-radius: 6px;
    border-left: 4px solid #e74c3c;
  }

  label {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }

  .label-text {
    display: block;
    margin-bottom: 0.5rem;
    color: #34495e;
    font-weight: 500;
    font-size: 0.95rem;
  }

  .badge-reset {
    margin-left: 0.5rem;
    font-size: 1rem;
    color: #f39c12;
  }

  /* Checkbox styling */
  input[type="checkbox"] {
    width: 20px;
    height: 20px;
    cursor: pointer;
  }

  /* Number input only (no slider) */
  .number-input-only {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .number-input-only input[type="number"] {
    max-width: 200px;
  }

  /* Number input with slider */
  .number-input {
    display: flex;
    align-items: center;
    gap: 1rem;
  }

  input[type="range"] {
    flex: 1;
    height: 6px;
    border-radius: 3px;
    background: #e0e6ed;
    outline: none;
    -webkit-appearance: none;
  }

  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: #667eea;
    cursor: pointer;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
  }

  input[type="range"]::-moz-range-thumb {
    width: 18px;
    height: 18px;
    border-radius: 50%;
    background: #667eea;
    cursor: pointer;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
    border: none;
  }

  .number-display {
    min-width: 80px;
    padding: 0.5rem;
    background: #ecf0f1;
    border-radius: 4px;
    text-align: center;
    font-weight: 600;
    font-size: 0.9rem;
    border: 2px solid #e0e6ed;
  }

  .unit {
    color: #7f8c8d;
    font-size: 0.85rem;
    min-width: 50px;
    font-weight: 500;
  }

  .error-message {
    color: #c0392b;
    font-size: 0.85rem;
    margin-top: 0.25rem;
    font-weight: 500;
  }
</style>
