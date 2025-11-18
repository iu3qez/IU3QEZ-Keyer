<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { DecoderStatus } from '../lib/types';

  let status: DecoderStatus | null = null;
  let loading = true;
  let error: string | null = null;
  let intervalId: number | null = null;
  let alertMessage = '';
  let alertType: 'info' | 'warning' = 'info';
  let showAlert = false;
  let lastText = '';

  async function loadStatus() {
    try {
      error = null;
      status = await api.getDecoderStatus();

      // Auto-scroll decoded text if it changed
      if (status.text !== lastText) {
        lastText = status.text;
        setTimeout(() => {
          const textDisplay = document.getElementById('decoded-text');
          if (textDisplay) {
            textDisplay.scrollTop = textDisplay.scrollHeight;
          }
        }, 10);
      }
    } catch (e) {
      error = (e as Error).message;
      console.error('Failed to load decoder status:', e);
    } finally {
      loading = false;
    }
  }

  async function handleEnableToggle() {
    if (!status) return;

    const newState = !status.enabled;

    try {
      const result = await api.setDecoderEnabled(newState);
      displayAlert(
        result.message || (newState ? 'Decoder enabled' : 'Decoder disabled'),
        'info'
      );
      await loadStatus();
    } catch (e) {
      displayAlert('Failed to toggle decoder: ' + (e as Error).message, 'warning');
      // Revert on error (will be corrected by next poll)
    }
  }

  async function handleReset() {
    if (!status?.enabled) {
      displayAlert('Decoder is disabled. Enable it first.', 'warning');
      return;
    }

    try {
      // Reset by disabling and re-enabling
      await api.setDecoderEnabled(false);
      await new Promise((resolve) => setTimeout(resolve, 100));
      await api.setDecoderEnabled(true);
      displayAlert('Decoder buffer reset', 'info');
      lastText = '';
      await loadStatus();
    } catch (e) {
      displayAlert('Failed to reset decoder: ' + (e as Error).message, 'warning');
    }
  }

  function displayAlert(message: string, type: 'info' | 'warning') {
    alertMessage = message;
    alertType = type;
    showAlert = true;

    setTimeout(() => {
      showAlert = false;
    }, 3000);
  }

  onMount(() => {
    loadStatus();
    intervalId = setInterval(loadStatus, 500) as unknown as number;
  });

  onDestroy(() => {
    if (intervalId !== null) {
      clearInterval(intervalId);
    }
  });

  // Computed properties
  $: wpm = status?.wpm || 0;
  $: wpmDisplay = wpm > 0 ? wpm.toString() : '--';
  $: wpmPercent = Math.min(100, Math.max(0, ((wpm - 10) / 50) * 100));
  $: pattern = status?.pattern || '';
  $: patternDisplay = pattern.length > 0 ? pattern : '--';
  $: patternEmpty = pattern.length === 0;

  $: statusText = status?.enabled
    ? wpm > 0
      ? 'Active'
      : 'Waiting'
    : 'Disabled';
  $: statusColor = status?.enabled
    ? wpm > 0
      ? '#27ae60'
      : '#f39c12'
    : '#95a5a6';

  $: decodedText = status
    ? status.text.length > 0
      ? status.text
      : status.enabled
      ? 'Ready to decode...'
      : 'Decoder disabled'
    : 'Loading...';
</script>

<div class="decoder-page">
  <div class="header">
    <h1>Morse Decoder</h1>
    <div class="subtitle">Adaptive CW Decoder with Timing Analysis</div>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    {#if showAlert}
      <div class="alert alert-{alertType}">{alertMessage}</div>
    {/if}

    {#if loading && !status}
      <div class="loading">Loading decoder...</div>
    {:else if status}
      <!-- Decoder Status Card -->
      <div class="card">
        <h2>
          <span class="icon icon-decoder"></span>
          Decoder Status
        </h2>

        <div class="controls">
          <label class="toggle-wrapper" on:click={handleEnableToggle}>
            <input type="checkbox" checked={status.enabled} readonly />
            <span class="toggle-label">Enable Decoder</span>
          </label>
          <button class="btn btn-danger" on:click={handleReset}>Reset Buffer</button>
          <a href="/config" class="btn btn-secondary">Configuration</a>
        </div>

        <div class="status-grid">
          <div class="status-item">
            <div class="status-label">WPM</div>
            <div class="status-value wpm-value">{wpmDisplay}</div>
          </div>
          <div class="status-item">
            <div class="status-label">Current Pattern</div>
            <div class="pattern-display" class:empty={patternEmpty}>
              {patternDisplay}
            </div>
          </div>
          <div class="status-item">
            <div class="status-label">Status</div>
            <div class="status-value" style="color: {statusColor}">{statusText}</div>
          </div>
        </div>

        <div class="wpm-gauge">
          <div class="wpm-gauge-fill" style="width: {wpmPercent}%">
            {wpm > 0 ? wpm + ' WPM' : ''}
          </div>
        </div>
        <div class="wpm-gauge-labels">
          <span>10 WPM</span>
          <span>20 WPM</span>
          <span>30 WPM</span>
          <span>40 WPM</span>
          <span>50 WPM</span>
          <span>60 WPM</span>
        </div>
      </div>

      <!-- Decoded Text Card -->
      <div class="card">
        <h2>Decoded Text</h2>
        <div class="text-display" id="decoded-text">{decodedText}</div>
      </div>
    {/if}

    {#if error}
      <div class="error-banner">{error}</div>
    {/if}
  </div>
</div>

<style>
  .decoder-page {
    min-height: 100vh;
    background: #f5f7fa;
  }

  .header {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    padding: 1.5rem;
    text-align: center;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
  }

  .header h1 {
    font-size: 1.8rem;
    font-weight: 600;
    margin-bottom: 0.3rem;
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

  .loading {
    color: #7f8c8d;
    font-style: italic;
    padding: 2rem;
    text-align: center;
  }

  .alert {
    padding: 1rem;
    border-radius: 6px;
    margin-bottom: 1rem;
    font-size: 0.9rem;
  }

  .alert-info {
    background: #e7f3ff;
    border-left: 4px solid #3498db;
    color: #2980b9;
  }

  .alert-warning {
    background: #fff3cd;
    border-left: 4px solid #f39c12;
    color: #d68910;
  }

  .error-banner {
    background: #f8d7da;
    color: #721c24;
    padding: 1rem;
    border-radius: 6px;
    margin: 1rem 0;
    border: 1px solid #f5c6cb;
  }

  .card {
    background: white;
    border-radius: 8px;
    padding: 1.5rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
    margin-bottom: 1.5rem;
  }

  .card h2 {
    font-size: 1.3rem;
    margin-bottom: 1rem;
    color: #2c3e50;
    border-bottom: 2px solid #ecf0f1;
    padding-bottom: 0.5rem;
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .icon {
    width: 20px;
    height: 20px;
    display: inline-block;
    border-radius: 50%;
  }

  .icon-decoder {
    background: linear-gradient(135deg, #2ecc71, #27ae60);
  }

  .controls {
    display: flex;
    gap: 1rem;
    flex-wrap: wrap;
    align-items: center;
    margin-bottom: 1.5rem;
  }

  .toggle-wrapper {
    display: flex;
    align-items: center;
    gap: 0.75rem;
    padding: 0.75rem 1rem;
    background: #f8f9fa;
    border-radius: 6px;
    cursor: pointer;
    transition: background 0.2s;
  }

  .toggle-wrapper:hover {
    background: #ecf0f1;
  }

  .toggle-wrapper input[type='checkbox'] {
    width: 44px;
    height: 24px;
    position: relative;
    cursor: pointer;
    appearance: none;
    background: #95a5a6;
    border-radius: 12px;
    transition: background 0.3s;
  }

  .toggle-wrapper input[type='checkbox']::before {
    content: '';
    position: absolute;
    width: 20px;
    height: 20px;
    border-radius: 50%;
    background: white;
    top: 2px;
    left: 2px;
    transition: transform 0.3s;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
  }

  .toggle-wrapper input[type='checkbox']:checked {
    background: #27ae60;
  }

  .toggle-wrapper input[type='checkbox']:checked::before {
    transform: translateX(20px);
  }

  .toggle-label {
    font-weight: 600;
    font-size: 0.95rem;
    color: #2c3e50;
  }

  .btn {
    padding: 0.75rem 1.5rem;
    border: none;
    border-radius: 6px;
    font-size: 0.95rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    text-decoration: none;
    display: inline-block;
    text-align: center;
  }

  .btn-danger {
    background: #e74c3c;
    color: white;
  }

  .btn-danger:hover {
    background: #c0392b;
  }

  .btn-secondary {
    background: #95a5a6;
    color: white;
  }

  .btn-secondary:hover {
    background: #7f8c8d;
  }

  .status-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 1rem;
    margin-bottom: 1.5rem;
  }

  .status-item {
    padding: 0.75rem;
    background: #f8f9fa;
    border-radius: 6px;
  }

  .status-label {
    font-size: 0.75rem;
    text-transform: uppercase;
    color: #7f8c8d;
    font-weight: 600;
    letter-spacing: 0.5px;
    margin-bottom: 0.3rem;
  }

  .status-value {
    font-size: 1.4rem;
    font-weight: 600;
    color: #2c3e50;
  }

  .wpm-value {
    color: #667eea;
  }

  .pattern-display {
    display: inline-block;
    background: #ecf0f1;
    padding: 0.5rem 1rem;
    border-radius: 4px;
    font-family: 'Courier New', monospace;
    font-size: 1.2rem;
    font-weight: 600;
    color: #2c3e50;
    min-width: 80px;
    text-align: center;
  }

  .pattern-display.empty {
    color: #95a5a6;
    font-style: italic;
  }

  .wpm-gauge {
    position: relative;
    height: 120px;
    margin: 1rem 0;
    background: #f8f9fa;
    border-radius: 8px;
    overflow: hidden;
  }

  .wpm-gauge-fill {
    height: 100%;
    background: linear-gradient(
      90deg,
      #27ae60 0%,
      #2ecc71 50%,
      #f39c12 80%,
      #e74c3c 100%
    );
    width: 0%;
    transition: width 0.5s ease-out;
    display: flex;
    align-items: center;
    justify-content: center;
    color: white;
    font-weight: 700;
    font-size: 2rem;
  }

  .wpm-gauge-labels {
    display: flex;
    justify-content: space-between;
    margin-top: 0.5rem;
    font-size: 0.85rem;
    color: #7f8c8d;
  }

  .text-display {
    background: #2c3e50;
    color: #2ecc71;
    font-family: 'Courier New', monospace;
    padding: 1rem;
    border-radius: 6px;
    min-height: 150px;
    font-size: 1.1rem;
    line-height: 1.8;
    word-wrap: break-word;
    overflow-y: auto;
    max-height: 300px;
    white-space: pre-wrap;
  }

  .text-display::-webkit-scrollbar {
    width: 8px;
  }

  .text-display::-webkit-scrollbar-track {
    background: #1a252f;
    border-radius: 4px;
  }

  .text-display::-webkit-scrollbar-thumb {
    background: #667eea;
    border-radius: 4px;
  }

  @media (max-width: 768px) {
    .container {
      padding: 1rem 0.5rem;
    }

    .status-grid {
      grid-template-columns: 1fr;
    }

    .controls {
      flex-direction: column;
      align-items: stretch;
    }

    .btn {
      width: 100%;
    }
  }
</style>
