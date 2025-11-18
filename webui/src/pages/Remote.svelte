<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { RemoteStatus } from '../lib/types';

  let status: RemoteStatus | null = null;
  let loading = true;
  let error: string | null = null;
  let intervalId: number | null = null;

  // State mappings
  const CLIENT_STATES: Record<number, string> = {
    0: 'Idle',
    1: 'Resolving',
    2: 'Connecting',
    3: 'Handshake',
    4: 'Connected',
    5: 'Error',
  };

  const SERVER_STATES: Record<number, string> = {
    0: 'Idle',
    1: 'Listening',
    2: 'Handshake',
    3: 'Connected',
    4: 'Error',
  };

  async function loadStatus() {
    try {
      error = null;
      status = await api.getRemoteStatus();
    } catch (e) {
      error = (e as Error).message;
      console.error('Failed to load remote status:', e);
    } finally {
      loading = false;
    }
  }

  async function handleClientStart() {
    try {
      await api.startRemoteClient();
      // Quick refresh
      setTimeout(loadStatus, 200);
    } catch (e) {
      error = 'Failed to start client: ' + (e as Error).message;
    }
  }

  async function handleClientStop() {
    try {
      await api.stopRemoteClient();
      setTimeout(loadStatus, 200);
    } catch (e) {
      error = 'Failed to stop client: ' + (e as Error).message;
    }
  }

  async function handleServerStart() {
    try {
      await api.startRemoteServer();
      setTimeout(loadStatus, 200);
    } catch (e) {
      error = 'Failed to start server: ' + (e as Error).message;
    }
  }

  async function handleServerStop() {
    try {
      await api.stopRemoteServer();
      setTimeout(loadStatus, 200);
    } catch (e) {
      error = 'Failed to stop server: ' + (e as Error).message;
    }
  }

  onMount(() => {
    loadStatus();
    intervalId = setInterval(loadStatus, 1000) as unknown as number;
  });

  onDestroy(() => {
    if (intervalId !== null) {
      clearInterval(intervalId);
    }
  });

  // Computed properties
  $: clientStateName = status ? CLIENT_STATES[status.client.state] || 'Unknown' : 'Unknown';
  $: serverStateName = status ? SERVER_STATES[status.server.state] || 'Unknown' : 'Unknown';

  $: clientStateClass =
    clientStateName === 'Idle'
      ? 'state-idle'
      : clientStateName === 'Connected'
      ? 'state-connected'
      : clientStateName === 'Error'
      ? 'state-error'
      : 'state-connecting';

  $: serverStateClass =
    serverStateName === 'Idle'
      ? 'state-idle'
      : serverStateName === 'Connected'
      ? 'state-connected'
      : serverStateName === 'Listening'
      ? 'state-listening'
      : serverStateName === 'Error'
      ? 'state-error'
      : 'state-connecting';

  $: clientCanStart = status && (status.client.state === 0 || status.client.state === 5);
  $: clientCanStop = status && status.client.state !== 0 && status.client.state !== 5;
  $: serverCanStart = status && (status.server.state === 0 || status.server.state === 4);
  $: serverCanStop = status && status.server.state !== 0 && status.server.state !== 4;
</script>

<div class="remote-page">
  <div class="header">
    <h1>Remote CW Keying</h1>
    <div class="subtitle">CWNet Client & Server Status</div>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    {#if error}
      <div class="error-banner">{error}</div>
    {/if}

    {#if loading && !status}
      <div class="loading">Loading remote status...</div>
    {:else if status}
      <div class="grid">
        <!-- Client Status Card -->
        <div class="card">
          <h2>
            <span class="icon icon-client"></span>
            Remote Client
          </h2>

          <div class="status-grid">
            <div class="status-item">
              <span class="status-label">State</span>
              <span class="status-value {clientStateClass}">{clientStateName}</span>
            </div>
            <div class="status-item">
              <span class="status-label">Server</span>
              <span class="status-value">
                {#if status.client.server_host}
                  {status.client.server_host}:{status.client.server_port}
                {:else}
                  -
                {/if}
              </span>
            </div>
            <div class="status-item">
              <span class="status-label">Latency</span>
              <span class="status-value">
                {#if status.client.state === 4 && status.client.latency_ms > 0}
                  {status.client.latency_ms} ms
                {:else}
                  - ms
                {/if}
              </span>
            </div>
            <div class="status-item">
              <span class="status-label">PTT Tail</span>
              <span class="status-value">
                {#if status.client.state === 4 && status.client.latency_ms > 0}
                  {status.client.ptt_tail_base_ms + status.client.latency_ms} ms
                  ({status.client.ptt_tail_base_ms}+{status.client.latency_ms})
                {:else}
                  {status.client.ptt_tail_base_ms} ms (base)
                {/if}
              </span>
            </div>
          </div>

          <div class="buttons">
            <button
              class="btn btn-primary"
              on:click={handleClientStart}
              disabled={!clientCanStart}
            >
              Start
            </button>
            <button
              class="btn btn-danger"
              on:click={handleClientStop}
              disabled={!clientCanStop}
            >
              Stop
            </button>
          </div>
        </div>

        <!-- Server Status Card -->
        <div class="card">
          <h2>
            <span class="icon icon-server"></span>
            Remote Server
          </h2>

          <div class="status-grid">
            <div class="status-item">
              <span class="status-label">State</span>
              <span class="status-value {serverStateClass}">{serverStateName}</span>
            </div>
            <div class="status-item">
              <span class="status-label">Listen Port</span>
              <span class="status-value">{status.server.listen_port || '-'}</span>
            </div>
            <div class="status-item">
              <span class="status-label">Client IP</span>
              <span class="status-value">
                {#if status.server.state === 3 && status.server.client_ip}
                  {status.server.client_ip}
                {:else}
                  -
                {/if}
              </span>
            </div>
            <div class="status-item">
              <span class="status-label">PTT Tail</span>
              <span class="status-value">{status.server.ptt_tail_ms} ms</span>
            </div>
          </div>

          <div class="buttons">
            <button
              class="btn btn-primary"
              on:click={handleServerStart}
              disabled={!serverCanStart}
            >
              Start
            </button>
            <button
              class="btn btn-danger"
              on:click={handleServerStop}
              disabled={!serverCanStop}
            >
              Stop
            </button>
          </div>
        </div>
      </div>

      <!-- Configuration Card -->
      <div class="card">
        <h2>Configuration</h2>
        <div class="config-grid">
          <div class="config-section">
            <h3>Client Settings</h3>
            <div class="config-items">
              <div class="config-item">
                <span class="config-label">Enabled</span>
                <span class="config-value">{status.config.client_enabled ? 'Yes' : 'No'}</span>
              </div>
              <div class="config-item">
                <span class="config-label">Server Host</span>
                <span class="config-value">{status.config.client_server_host || '(not set)'}</span>
              </div>
              <div class="config-item">
                <span class="config-label">Server Port</span>
                <span class="config-value">{status.config.client_server_port || '-'}</span>
              </div>
              <div class="config-item">
                <span class="config-label">Auto-Reconnect</span>
                <span class="config-value">{status.config.client_auto_reconnect ? 'Yes' : 'No'}</span>
              </div>
            </div>
          </div>

          <div class="config-section">
            <h3>Server Settings</h3>
            <div class="config-items">
              <div class="config-item">
                <span class="config-label">Enabled</span>
                <span class="config-value">{status.config.server_enabled ? 'Yes' : 'No'}</span>
              </div>
              <div class="config-item">
                <span class="config-label">Listen Port</span>
                <span class="config-value">{status.config.server_listen_port || '-'}</span>
              </div>
            </div>
          </div>
        </div>

        <div class="info-note">
          <strong>Note:</strong> Use the <a href="/config">Configuration</a> page to change remote keying settings.
        </div>
      </div>
    {/if}
  </div>
</div>

<style>
  .remote-page {
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
    max-width: 1200px;
    margin: 0 auto;
    padding: 1rem;
  }

  .error-banner {
    background: #f8d7da;
    color: #721c24;
    padding: 1rem;
    border-radius: 6px;
    margin: 1rem 0;
    border: 1px solid #f5c6cb;
  }

  .loading {
    color: #7f8c8d;
    font-style: italic;
    padding: 2rem;
    text-align: center;
  }

  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(500px, 1fr));
    gap: 1.5rem;
    margin-bottom: 1.5rem;
  }

  .card {
    background: white;
    border: 1px solid #ddd;
    border-radius: 8px;
    padding: 1.5rem;
    margin: 1rem 0;
  }

  .card h2 {
    font-size: 1.3rem;
    margin-bottom: 1rem;
    color: #2c3e50;
    border-bottom: 2px solid #667eea;
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

  .icon-client {
    background: linear-gradient(135deg, #667eea, #764ba2);
  }

  .icon-server {
    background: linear-gradient(135deg, #27ae60, #2ecc71);
  }

  .status-grid {
    display: grid;
    gap: 1rem;
    margin-bottom: 1rem;
  }

  .status-item {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 0.75rem;
    background: #f8f9fa;
    border-radius: 6px;
  }

  .status-label {
    font-weight: 500;
    color: #7f8c8d;
    font-size: 0.9rem;
  }

  .status-value {
    font-weight: 600;
    font-size: 1rem;
  }

  .state-idle {
    color: #95a5a6;
  }

  .state-connecting {
    color: #f39c12;
  }

  .state-connected {
    color: #27ae60;
  }

  .state-error {
    color: #e74c3c;
  }

  .state-listening {
    color: #3498db;
  }

  .buttons {
    display: flex;
    gap: 0.75rem;
    margin-top: 1rem;
  }

  .btn {
    padding: 0.75rem 1.5rem;
    border: none;
    border-radius: 6px;
    font-size: 0.95rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.2s;
    flex: 1;
  }

  .btn-primary {
    background: #667eea;
    color: white;
  }

  .btn-primary:hover:not(:disabled) {
    background: #5568d3;
  }

  .btn-danger {
    background: #e74c3c;
    color: white;
  }

  .btn-danger:hover:not(:disabled) {
    background: #c0392b;
  }

  .btn:disabled {
    background: #95a5a6;
    cursor: not-allowed;
    opacity: 0.6;
  }

  .config-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
    gap: 2rem;
    margin: 1rem 0;
  }

  .config-section h3 {
    font-size: 1.1rem;
    color: #667eea;
    margin-bottom: 1rem;
    font-weight: 600;
  }

  .config-items {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }

  .config-item {
    display: flex;
    justify-content: space-between;
    padding: 0.5rem;
    background: #f8f9fa;
    border-radius: 4px;
  }

  .config-label {
    font-weight: 500;
    color: #7f8c8d;
    font-size: 0.9rem;
  }

  .config-value {
    font-weight: 600;
    color: #2c3e50;
  }

  .info-note {
    margin-top: 1rem;
    padding: 0.75rem;
    background: #e7f3ff;
    border-left: 4px solid #3498db;
    border-radius: 4px;
    font-size: 0.9rem;
    color: #2980b9;
  }

  .info-note a {
    color: #2980b9;
    text-decoration: underline;
  }

  @media (max-width: 768px) {
    .grid {
      grid-template-columns: 1fr;
    }

    .config-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
