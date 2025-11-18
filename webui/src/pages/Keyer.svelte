<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { KeyerStatus } from '../lib/types';

  let status: KeyerStatus | null = null;
  let loading = true;
  let error: string | null = null;
  let autoRefresh = true;
  let intervalId: number | null = null;

  let textToSend = '';
  let wpm = 20;
  let sending = false;
  let responseMessage = '';

  async function loadKeyerStatus() {
    try {
      error = null;
      status = await api.getKeyerStatus();
    } catch (e) {
      error = (e as Error).message;
      console.error('Failed to load keyer status:', e);
    } finally {
      loading = false;
    }
  }

  async function handleSendText() {
    if (!textToSend.trim()) {
      responseMessage = 'Please enter text to send';
      return;
    }

    try {
      sending = true;
      responseMessage = '';
      const result = await api.sendText(textToSend, wpm);
      responseMessage = result.message || 'Text sent successfully';
      // Don't clear textToSend - user might want to resend
    } catch (e) {
      responseMessage = 'Error: ' + (e as Error).message;
    } finally {
      sending = false;
    }
  }

  async function handleSendMessage(messageNum: number) {
    try {
      sending = true;
      responseMessage = '';
      const result = await api.sendMessage(messageNum);
      responseMessage = result.message || `Message F${messageNum} sent`;
    } catch (e) {
      responseMessage = 'Error: ' + (e as Error).message;
    } finally {
      sending = false;
    }
  }

  async function handleAbort() {
    try {
      const result = await api.abortTransmission();
      responseMessage = result.message || 'Transmission aborted';
    } catch (e) {
      responseMessage = 'Error: ' + (e as Error).message;
    }
  }

  function handleAutoRefreshToggle() {
    if (autoRefresh) {
      startAutoRefresh();
    } else {
      stopAutoRefresh();
    }
  }

  function startAutoRefresh() {
    stopAutoRefresh();
    intervalId = setInterval(loadKeyerStatus, 500) as unknown as number;
  }

  function stopAutoRefresh() {
    if (intervalId !== null) {
      clearInterval(intervalId);
      intervalId = null;
    }
  }

  onMount(() => {
    loadKeyerStatus();
    if (autoRefresh) {
      startAutoRefresh();
    }
  });

  onDestroy(() => {
    stopAutoRefresh();
  });

  $: progressPercent = status && status.total > 0
    ? ((status.progress / status.total) * 100).toFixed(1)
    : '0';

  $: isSending = status?.state === 'sending';
</script>

<div class="keyer-page">
  <div class="header">
    <h1>CW Keyer</h1>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    <!-- Status Display -->
    <div class="card">
      <h2>Keyer Status</h2>

      {#if error}
        <div class="error">{error}</div>
      {/if}

      {#if loading && !status}
        <div class="loading">Loading keyer status...</div>
      {:else if status}
        <div class="status-grid">
          <div class="status-item">
            <div class="status-label">State</div>
            <div class="status-value">
              <span class="state-badge {isSending ? 'state-sending' : 'state-idle'}">
                {status.state.toUpperCase()}
              </span>
            </div>
          </div>
          <div class="status-item">
            <div class="status-label">Speed</div>
            <div class="status-value">{status.wpm} WPM</div>
          </div>
          <div class="status-item">
            <div class="status-label">Progress</div>
            <div class="status-value">{status.progress} / {status.total}</div>
          </div>
        </div>

        {#if isSending}
          <div class="progress-section">
            <div class="progress-bar">
              <div class="progress-fill" style="width: {progressPercent}%">
                {progressPercent}%
              </div>
            </div>
          </div>
        {/if}
      {/if}

      <div class="controls-row">
        <div class="auto-refresh">
          <input
            type="checkbox"
            id="autoRefresh"
            bind:checked={autoRefresh}
            on:change={handleAutoRefreshToggle}
          />
          <label for="autoRefresh">Auto-refresh (500ms)</label>
        </div>
      </div>
    </div>

    <!-- Send Text -->
    <div class="card">
      <h2>Send CW Text</h2>

      <div class="send-form">
        <div class="form-group">
          <label for="textToSend">Text to Send:</label>
          <textarea
            id="textToSend"
            bind:value={textToSend}
            placeholder="Enter text to send in Morse code..."
            rows="4"
            disabled={sending || isSending}
          ></textarea>
        </div>

        <div class="form-group">
          <label for="wpm">Speed (WPM):</label>
          <input
            type="number"
            id="wpm"
            bind:value={wpm}
            min="5"
            max="60"
            disabled={sending || isSending}
          />
        </div>

        <div class="button-group">
          <button
            class="btn btn-primary"
            on:click={handleSendText}
            disabled={sending || isSending || !textToSend.trim()}
          >
            {sending ? 'Sending...' : 'Send Text'}
          </button>

          <button
            class="btn btn-danger"
            on:click={handleAbort}
            disabled={!isSending}
          >
            Abort
          </button>
        </div>

        {#if responseMessage}
          <div class="response-message {responseMessage.startsWith('Error') ? 'error' : 'success'}">
            {responseMessage}
          </div>
        {/if}
      </div>
    </div>

    <!-- Stored Messages -->
    <div class="card">
      <h2>Stored Messages (F1-F10)</h2>
      <p class="hint">Click to send a stored message. Configure messages in the Config page.</p>

      <div class="message-buttons">
        {#each Array(10) as _, i}
          <button
            class="btn btn-message"
            on:click={() => handleSendMessage(i + 1)}
            disabled={sending || isSending}
          >
            F{i + 1}
          </button>
        {/each}
      </div>
    </div>
  </div>
</div>

<style>
  .keyer-page {
    min-height: 100vh;
    background: #f0f2f5;
  }

  .header {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    padding: 1.5rem;
    text-align: center;
  }

  .header h1 {
    font-size: 2rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
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
    max-width: 900px;
    margin: 0 auto;
    padding: 1rem;
  }

  .card {
    background: #fff;
    border: 1px solid #ddd;
    border-radius: 8px;
    padding: 1.5rem;
    margin: 1rem 0;
  }

  .card h2 {
    margin-bottom: 1rem;
    color: #2c3e50;
    border-bottom: 2px solid #667eea;
    padding-bottom: 0.5rem;
  }

  .hint {
    color: #7f8c8d;
    font-size: 0.9rem;
    margin-bottom: 1rem;
  }

  .status-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
    gap: 1rem;
    margin: 1rem 0;
  }

  .status-item {
    background: #f8f9fa;
    border-left: 4px solid #667eea;
    padding: 1rem;
    border-radius: 4px;
  }

  .status-label {
    font-size: 0.75rem;
    text-transform: uppercase;
    color: #7f8c8d;
    font-weight: 600;
    letter-spacing: 0.5px;
    margin-bottom: 0.5rem;
  }

  .status-value {
    font-size: 1.25rem;
    font-weight: 600;
    color: #2c3e50;
  }

  .state-badge {
    display: inline-block;
    padding: 0.25rem 0.75rem;
    border-radius: 4px;
    font-size: 0.9rem;
    font-weight: 600;
  }

  .state-idle {
    background: #d4edda;
    color: #155724;
  }

  .state-sending {
    background: #fff3cd;
    color: #856404;
  }

  .progress-section {
    margin-top: 1rem;
  }

  .progress-bar {
    background: #e9ecef;
    height: 24px;
    border-radius: 12px;
    overflow: hidden;
    position: relative;
  }

  .progress-fill {
    background: linear-gradient(90deg, #667eea 0%, #764ba2 100%);
    height: 100%;
    transition: width 0.3s;
    display: flex;
    align-items: center;
    justify-content: center;
    color: white;
    font-size: 0.8rem;
    font-weight: 600;
  }

  .controls-row {
    margin-top: 1rem;
    padding-top: 1rem;
    border-top: 1px solid #e9ecef;
  }

  .auto-refresh {
    display: flex;
    align-items: center;
    gap: 0.5rem;
  }

  .auto-refresh input {
    width: 18px;
    height: 18px;
    cursor: pointer;
  }

  .send-form {
    margin-top: 1rem;
  }

  .form-group {
    margin-bottom: 1.5rem;
  }

  .form-group label {
    display: block;
    font-weight: 600;
    margin-bottom: 0.5rem;
    color: #2c3e50;
  }

  textarea {
    width: 100%;
    padding: 0.75rem;
    border: 2px solid #e0e6ed;
    border-radius: 6px;
    font-size: 0.95rem;
    font-family: 'Courier New', monospace;
    transition: border-color 0.2s;
    background: white;
    resize: vertical;
  }

  textarea:focus {
    outline: none;
    border-color: #667eea;
  }

  textarea:disabled {
    background: #f8f9fa;
    cursor: not-allowed;
  }

  .button-group {
    display: flex;
    gap: 1rem;
    flex-wrap: wrap;
  }

  .btn {
    padding: 0.75rem 1.5rem;
    border: none;
    border-radius: 6px;
    font-size: 0.95rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.2s;
  }

  .btn:disabled {
    opacity: 0.5;
    cursor: not-allowed;
  }

  .btn-primary {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
  }

  .btn-primary:hover:not(:disabled) {
    opacity: 0.9;
  }

  .btn-danger {
    background: #dc3545;
    color: white;
  }

  .btn-danger:hover:not(:disabled) {
    opacity: 0.9;
  }

  .btn-message {
    background: #e9ecef;
    color: #2c3e50;
    padding: 0.6rem 1.2rem;
    min-width: 60px;
  }

  .btn-message:hover:not(:disabled) {
    background: #d4d7db;
  }

  .message-buttons {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(80px, 1fr));
    gap: 0.75rem;
    margin-top: 1rem;
  }

  .response-message {
    margin-top: 1rem;
    padding: 0.75rem;
    border-radius: 4px;
    font-weight: 500;
  }

  .response-message.success {
    background: #d4edda;
    color: #155724;
    border: 1px solid #c3e6cb;
  }

  .response-message.error {
    background: #f8d7da;
    color: #721c24;
    border: 1px solid #f5c6cb;
  }

  .loading {
    color: #7f8c8d;
    font-style: italic;
    padding: 1rem;
    text-align: center;
  }

  .error {
    color: #dc3545;
    padding: 1rem;
    background: #f8d7da;
    border-radius: 4px;
    margin: 1rem 0;
  }

  @media (max-width: 768px) {
    .status-grid {
      grid-template-columns: 1fr;
    }

    .button-group {
      flex-direction: column;
    }

    .btn {
      width: 100%;
    }

    .message-buttons {
      grid-template-columns: repeat(auto-fill, minmax(60px, 1fr));
    }
  }
</style>
