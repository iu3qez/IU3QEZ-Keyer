<script lang="ts">
  import { resetRequired, resetRequiredCount, configStore } from '../stores/config';

  export let onReboot: () => void = () => {};
  export let onCancel: () => void = () => {};

  $: params = Array.from($configStore.dirtyParams.entries())
    .filter(([paramPath]) => {
      const [subsystem, paramName] = paramPath.split('.');
      return $configStore.schema?.subsystems[subsystem]?.[paramName]?.reset_required;
    })
    .map(([paramPath, change]) => ({
      path: paramPath,
      oldValue: change.oldValue,
      newValue: change.newValue,
    }));
</script>

{#if $resetRequired}
  <div class="status-banner warning">
    <div class="banner-content">
      <span class="icon">⚠️</span>
      <div class="message">
        <strong>{$resetRequiredCount} parameter{$resetRequiredCount > 1 ? 's' : ''} modified require reboot</strong>
        <div class="param-list">
          {#each params as param}
            <span class="param-item">
              {param.path} ({param.oldValue} → {param.newValue})
            </span>
          {/each}
        </div>
      </div>
      <div class="actions">
        <button class="btn-reboot" on:click={onReboot}>Reboot Now</button>
        <button class="btn-cancel" on:click={onCancel}>Cancel</button>
      </div>
    </div>
  </div>
{/if}

<style>
  .status-banner {
    position: sticky;
    top: 0;
    z-index: 100;
    padding: 1rem;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
    animation: slideDown 0.3s ease-out;
  }

  @keyframes slideDown {
    from {
      transform: translateY(-100%);
      opacity: 0;
    }
    to {
      transform: translateY(0);
      opacity: 1;
    }
  }

  .warning {
    background-color: #fff3cd;
    border-bottom: 3px solid #ffc107;
  }

  .banner-content {
    display: flex;
    align-items: center;
    gap: 1rem;
    max-width: 1200px;
    margin: 0 auto;
  }

  .icon {
    font-size: 1.5rem;
  }

  .message {
    flex: 1;
  }

  .param-list {
    display: flex;
    flex-wrap: wrap;
    gap: 0.5rem;
    margin-top: 0.5rem;
    font-size: 0.85rem;
  }

  .param-item {
    background-color: rgba(255, 193, 7, 0.2);
    padding: 0.25rem 0.5rem;
    border-radius: 3px;
    font-family: monospace;
  }

  .actions {
    display: flex;
    gap: 0.5rem;
  }

  button {
    padding: 0.5rem 1rem;
    border: none;
    border-radius: 4px;
    font-size: 0.9rem;
    cursor: pointer;
    transition: all 0.2s;
  }

  .btn-reboot {
    background-color: #dc3545;
    color: white;
  }

  .btn-reboot:hover {
    background-color: #c82333;
  }

  .btn-cancel {
    background-color: #6c757d;
    color: white;
  }

  .btn-cancel:hover {
    background-color: #5a6268;
  }
</style>
