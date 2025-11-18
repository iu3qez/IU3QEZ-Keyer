<script lang="ts">
  import { onMount } from 'svelte';
  import { configStore } from '../stores/config';
  import { api } from '../lib/api';
  import StatusBanner from '../components/StatusBanner.svelte';
  import ParameterSection from '../components/ParameterSection.svelte';

  let activeTab = '';
  let showAdvanced: Record<string, boolean> = {};

  onMount(async () => {
    try {
      configStore.setLoading(true);

      // Load schema and config
      const [schema, config] = await Promise.all([
        api.getSchema(),
        api.getConfig(),
      ]);

      console.log('Loaded schema:', schema);
      console.log('Loaded config:', config);

      configStore.setSchema(schema);
      configStore.setConfig(config);

      // Set activeTab to first subsystem
      if (schema?.subsystems) {
        const subsystemKeys = Object.keys(schema.subsystems);
        console.log('Available subsystems:', subsystemKeys);
        const firstSubsystem = subsystemKeys[0];
        if (firstSubsystem) {
          activeTab = firstSubsystem;
          console.log('Set activeTab to:', activeTab);
        }
      } else {
        console.warn('No subsystems in schema:', schema);
      }
    } catch (error) {
      console.error('Error loading configuration:', error);
      configStore.setError((error as Error).message);
    } finally {
      configStore.setLoading(false);
    }
  });

  async function handleSave() {
    try {
      // Save all dirty parameters
      for (const [paramPath, change] of $configStore.dirtyParams) {
        await api.setParameter({
          param: paramPath,
          value: change.newValue,
        });
      }

      // Persist to NVS
      await api.saveConfig();

      // Clear dirty flags
      configStore.clearDirty();

      alert('Configuration saved successfully!');
    } catch (error) {
      alert(`Error saving config: ${(error as Error).message}`);
    }
  }

  async function handleReboot() {
    if (confirm('Reboot device now? This will disconnect the web interface.')) {
      try {
        await api.reboot();
        alert('Device is rebooting. Please reconnect in 10-15 seconds.');
      } catch (error) {
        alert(`Error rebooting: ${(error as Error).message}`);
      }
    }
  }

  async function handleDiscardChanges() {
    if ($configStore.dirtyParams.size === 0) {
      return; // Nothing to discard
    }

    if (!confirm('Discard all unsaved changes and reload configuration from device?')) {
      return;
    }

    try {
      // Reload config from device to get original values
      const config = await api.getConfig();
      configStore.setConfig(config);

      // Clear all dirty flags
      configStore.clearDirty();
    } catch (error) {
      alert(`Error reloading config: ${(error as Error).message}`);
    }
  }

  function handleCancelReset() {
    // Clear dirty flags for reset-required parameters only
    for (const [paramPath] of $configStore.dirtyParams) {
      const [subsystem, paramName] = paramPath.split('.');
      const paramMeta = $configStore.schema?.subsystems[subsystem]?.[paramName];
      if (paramMeta?.reset_required) {
        configStore.clearDirty(paramPath);
      }
    }
  }

  $: subsystems = $configStore.schema?.subsystems
    ? Object.keys($configStore.schema.subsystems)
    : [];
</script>

<div class="config-page">
  <div class="header">
    <h1>Configuration</h1>
    <div class="subtitle">Device Parameter Editor</div>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <StatusBanner onReboot={handleReboot} onCancel={handleCancelReset} />

  <div class="container">

    {#if $configStore.loading}
      <div class="loading">Loading configuration...</div>
    {:else if $configStore.error}
      <div class="error">Error: {$configStore.error}</div>
    {:else if $configStore.schema}
      <div class="tabs">
        {#each subsystems as subsystem}
          <button
            class="tab"
            class:active={activeTab === subsystem}
            on:click={() => (activeTab = subsystem)}
          >
            {subsystem.charAt(0).toUpperCase() + subsystem.slice(1)}
          </button>
        {/each}
      </div>

      <div class="tab-content">
        {#if activeTab && $configStore.schema.subsystems[activeTab]}
          <ParameterSection
            subsystemName={activeTab}
            subsystemSchema={$configStore.schema.subsystems[activeTab]}
            subsystemConfig={$configStore.config[activeTab] || {}}
            showAdvanced={showAdvanced[activeTab] || false}
          />
        {:else if !activeTab}
          <div class="no-subsystems">No subsystems available</div>
        {/if}
      </div>

      <div class="actions">
        <button class="btn-save" on:click={handleSave}>
          Save Changes
        </button>
        <button class="btn-reset" on:click={handleDiscardChanges}>
          Discard Changes
        </button>
      </div>
    {/if}
  </div>
</div>

<style>
  .config-page {
    min-height: 100vh;
    background: #f0f2f5;
  }

  .header {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    padding: 1.5rem;
    text-align: center;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
  }

  .header h1 {
    font-size: 2rem;
    font-weight: 600;
    margin-bottom: 0.3rem;
    color: white;
  }

  .header .subtitle {
    font-size: 0.9rem;
    opacity: 0.9;
  }

  .header .nav {
    margin-top: 1rem;
  }

  .header .nav a {
    color: white;
    text-decoration: none;
    opacity: 0.9;
    font-size: 0.9rem;
  }

  .header .nav a:hover {
    opacity: 1;
  }

  .container {
    max-width: 1000px;
    margin: 0 auto;
    padding: 2rem 1rem;
  }

  .loading,
  .error {
    padding: 2rem;
    text-align: center;
    font-size: 1.1rem;
    color: #7f8c8d;
  }

  .error {
    color: #c0392b;
    background: #f8d7da;
    border-left: 4px solid #e74c3c;
    border-radius: 6px;
  }

  .tabs {
    display: flex;
    gap: 0.5rem;
    margin-bottom: 1.5rem;
    flex-wrap: wrap;
  }

  .tab {
    background: white;
    border: none;
    padding: 0.75rem 1.5rem;
    border-radius: 6px;
    cursor: pointer;
    font-size: 0.95rem;
    font-weight: 500;
    color: #7f8c8d;
    transition: all 0.2s;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
  }

  .tab:hover {
    background: #ecf0f1;
    color: #34495e;
  }

  .tab.active {
    background: #667eea;
    color: white;
    box-shadow: 0 4px 8px rgba(102, 126, 234, 0.3);
  }

  .tab-content {
    background: white;
    border-radius: 8px;
    padding: 1.5rem;
    margin-bottom: 1.5rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
  }

  .no-subsystems {
    padding: 2rem;
    text-align: center;
    color: #7f8c8d;
  }

  .actions {
    display: flex;
    gap: 1rem;
    margin-top: 2rem;
    flex-wrap: wrap;
  }

  button {
    padding: 0.875rem 2rem;
    border: none;
    border-radius: 6px;
    font-size: 1rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    flex: 1;
    min-width: 150px;
  }

  .btn-save {
    background: #667eea;
    color: white;
    box-shadow: 0 4px 8px rgba(102, 126, 234, 0.3);
  }

  .btn-save:hover {
    background: #5568d3;
    transform: translateY(-1px);
    box-shadow: 0 6px 12px rgba(102, 126, 234, 0.4);
  }

  .btn-reset {
    background: #95a5a6;
    color: white;
  }

  .btn-reset:hover {
    background: #7f8c8d;
  }

  @media (max-width: 768px) {
    .container {
      padding: 1rem 0.5rem;
    }

    .actions {
      flex-direction: column;
    }

    button {
      width: 100%;
    }
  }
</style>
