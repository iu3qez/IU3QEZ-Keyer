<script lang="ts">
  import ParameterInput from './ParameterInput.svelte';
  import { configStore } from '../stores/config';
  import { api } from '../lib/api';
  import { validateParameter } from '../lib/validators';
  import type { SubsystemSchema } from '../lib/types';

  export let subsystemName: string;
  export let subsystemSchema: SubsystemSchema;
  export let subsystemConfig: Record<string, any>;
  export let showAdvanced: boolean = false;

  async function handleParamChange(paramName: string, newValue: any) {
    const paramPath = `${subsystemName}.${paramName}`;
    const oldValue = subsystemConfig[paramName];
    const paramMeta = subsystemSchema[paramName];

    // Update the store's config so the UI reflects the new value (allow editing)
    const updatedConfig = {
      ...$configStore.config,
      [subsystemName]: {
        ...($configStore.config[subsystemName] || {}),
        [paramName]: newValue,
      },
    };
    configStore.setConfig(updatedConfig);

    // Don't send empty strings or invalid values to server
    // (but keep them in UI to allow editing)
    if (typeof newValue === 'string' && newValue.trim() === '') {
      console.log(`Skipping server update for ${paramPath}: value is empty (editing in progress)`);
      return;
    }

    // Validate before sending to server
    const validation = validateParameter(paramMeta, newValue);
    if (!validation.valid) {
      console.log(`Skipping server update for ${paramPath}: ${validation.error}`);
      return;
    }

    // Mark as dirty
    configStore.markDirty(paramPath, oldValue, newValue);

    // Send parameter update to device only if validation passed
    try {
      await api.setParameter({
        param: paramPath,
        value: newValue,
      });
    } catch (error) {
      console.error(`Failed to update ${paramPath}:`, error);
      // Revert to old value on error
      const revertedConfig = {
        ...$configStore.config,
        [subsystemName]: {
          ...($configStore.config[subsystemName] || {}),
          [paramName]: oldValue,
        },
      };
      configStore.setConfig(revertedConfig);
      configStore.clearDirty(paramPath);
      alert(`Failed to update ${paramName}: ${(error as Error).message}`);
    }
  }

  $: normalParams = Object.entries(subsystemSchema).filter(
    ([_, meta]) => meta.category === 'normal'
  );

  $: advancedParams = Object.entries(subsystemSchema).filter(
    ([_, meta]) => meta.category === 'advanced'
  );
</script>

<div class="parameter-section">
  <h3>{subsystemName.charAt(0).toUpperCase() + subsystemName.slice(1)}</h3>

  <div class="params-normal">
    {#each normalParams as [paramName, paramMeta]}
      <ParameterInput
        param={paramMeta}
        value={subsystemConfig[paramName]}
        onChange={(newValue) => handleParamChange(paramName, newValue)}
        dirty={$configStore.dirtyParams.has(`${subsystemName}.${paramName}`)}
      />
    {/each}
  </div>

  {#if advancedParams.length > 0}
    <button
      class="toggle-advanced"
      on:click={() => (showAdvanced = !showAdvanced)}
    >
      {showAdvanced ? '▲' : '▼'} {showAdvanced ? 'Hide' : 'Show'} Advanced Parameters
    </button>

    {#if showAdvanced}
      <div class="params-advanced">
        <div class="advanced-header">Advanced Parameters</div>
        {#each advancedParams as [paramName, paramMeta]}
          <ParameterInput
            param={paramMeta}
            value={subsystemConfig[paramName]}
            onChange={(newValue) => handleParamChange(paramName, newValue)}
            dirty={$configStore.dirtyParams.has(`${subsystemName}.${paramName}`)}
          />
        {/each}
      </div>
    {/if}
  {/if}
</div>

<style>
  .parameter-section {
    margin-bottom: 2rem;
  }

  h3 {
    font-size: 1.3rem;
    margin-bottom: 1rem;
    color: #2c3e50;
    border-bottom: 2px solid #ecf0f1;
    padding-bottom: 0.5rem;
  }

  .params-normal,
  .params-advanced {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }

  .toggle-advanced {
    margin: 1.5rem 0;
    padding: 0.75rem 1.5rem;
    background: white;
    border: 2px solid #e0e6ed;
    border-radius: 6px;
    cursor: pointer;
    font-size: 0.95rem;
    font-weight: 500;
    color: #34495e;
    transition: all 0.2s;
  }

  .toggle-advanced:hover {
    background: #ecf0f1;
    border-color: #667eea;
  }

  .params-advanced {
    background: #f8f9fa;
    padding: 1.5rem;
    border-radius: 6px;
    border: 2px solid #e0e6ed;
    margin-top: 1rem;
  }

  .advanced-header {
    font-weight: 600;
    color: #7f8c8d;
    margin-bottom: 1rem;
    font-size: 0.95rem;
    text-transform: uppercase;
    letter-spacing: 0.5px;
  }
</style>
