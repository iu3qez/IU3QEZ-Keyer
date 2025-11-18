<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { DeviceStatus, DeviceConfig } from '../lib/types';

  let status: DeviceStatus | null = null;
  let keyingPreset = '...';
  let keyingWpm = '...';
  let intervalId: number | null = null;

  async function loadStatus() {
    try {
      const [statusData, config] = await Promise.all([
        api.getStatus(),
        api.getConfig(),
      ]);

      status = statusData;

      // Extract keying info from config
      if (config.keying) {
        keyingPreset = config.keying.preset?.toString() || '...';
        keyingWpm = config.keying.wpm?.toString() || '...';
      }
    } catch (error) {
      console.error('Failed to load status:', error);
    }
  }

  onMount(() => {
    loadStatus();
    // Refresh every 3 seconds
    intervalId = setInterval(loadStatus, 3000) as unknown as number;
  });

  onDestroy(() => {
    if (intervalId !== null) {
      clearInterval(intervalId);
    }
  });
</script>

<div class="home-page">
  <div class="header">
    <h1>Keyer QRS2HST</h1>
    <div class="subtitle">Precision Iambic Keyer Web Interface</div>
  </div>

  <div class="container">
    <div class="status-bar">
      <div class="status-item">
        <div class="status-label">WiFi</div>
        <div
          class="status-value"
          class:status-ready={status?.ready}
          class:status-connecting={!status?.ready}
        >
          {status?.mode || 'Loading...'}
        </div>
      </div>
      <div class="status-item">
        <div class="status-label">IP Address</div>
        <div class="status-value">{status?.ip || '...'}</div>
      </div>
      <div class="status-item">
        <div class="status-label">Keying Mode</div>
        <div class="status-value">{keyingPreset}</div>
      </div>
      <div class="status-item">
        <div class="status-label">WPM</div>
        <div class="status-value">{keyingWpm}</div>
      </div>
    </div>

    <div class="welcome">
      <h2>Welcome to Your Morse Code Keyer</h2>
      <p>
        Select a section below to configure your device, monitor keying activity,
        control remote operations, or decode morse code signals.
      </p>
    </div>

    <div class="cards">
      <a href="/config" class="card">
        <div class="card-icon">⚙️</div>
        <h3>Configuration</h3>
        <p>
          Configure all device parameters including keying settings, audio, WiFi,
          and hardware options.
        </p>
      </a>
      <a href="/timeline" class="card">
        <div class="card-icon">📊</div>
        <h3>Timeline</h3>
        <p>
          View real-time keying events and timing analysis with graphical timeline
          visualization.
        </p>
      </a>
      <a href="/remote" class="card">
        <div class="card-icon">📡</div>
        <h3>Remote Keying</h3>
        <p>
          Control and monitor remote keying operations, manage client and server
          connections.
        </p>
      </a>
      <a href="/decoder" class="card">
        <div class="card-icon">🔤</div>
        <h3>Morse Decoder</h3>
        <p>
          Decode incoming morse code signals in real-time with adjustable tolerance
          and buffering.
        </p>
      </a>
      <a href="/system" class="card">
        <div class="card-icon">💻</div>
        <h3>System Monitor</h3>
        <p>
          Monitor CPU usage, task states, memory consumption, and system uptime in
          real-time.
        </p>
      </a>
      <a href="/firmware" class="card">
        <div class="card-icon">🔄</div>
        <h3>Firmware Update</h3>
        <p>
          Update device firmware via USB using the UF2 bootloader with drag-and-drop
          convenience.
        </p>
      </a>
      <a href="/keyer" class="card">
        <div class="card-icon">⌨️</div>
        <h3>Text Keyer</h3>
        <p>
          Send morse code from keyboard or stored messages. Perfect for CQ calls and
          contest messages.
        </p>
      </a>
    </div>
  </div>

  <div class="footer">
    Keyer QRS2HST &copy; 2025 - Precision Morse Code Keying System
  </div>
</div>

<style>
  .home-page {
    min-height: 100vh;
    display: flex;
    flex-direction: column;
  }

  .header {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    padding: 2rem 1.5rem;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
    text-align: center;
  }

  .header h1 {
    font-size: 2.5rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
  }

  .header .subtitle {
    font-size: 1rem;
    opacity: 0.9;
  }

  .container {
    max-width: 1200px;
    margin: 0 auto;
    padding: 3rem 1rem;
    flex: 1;
  }

  .status-bar {
    background: white;
    border-radius: 8px;
    padding: 1rem;
    margin-bottom: 2rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
    display: flex;
    flex-wrap: wrap;
    gap: 1.5rem;
    align-items: center;
    justify-content: center;
  }

  .status-item {
    display: flex;
    flex-direction: column;
    gap: 0.2rem;
    align-items: center;
  }

  .status-label {
    font-size: 0.75rem;
    text-transform: uppercase;
    color: #7f8c8d;
    font-weight: 600;
    letter-spacing: 0.5px;
  }

  .status-value {
    font-size: 1rem;
    font-weight: 600;
    color: #2c3e50;
  }

  .status-ready {
    color: #27ae60;
  }

  .status-connecting {
    color: #f39c12;
  }

  .welcome {
    background: white;
    border-radius: 8px;
    padding: 2rem;
    margin-bottom: 2rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
    text-align: center;
  }

  .welcome h2 {
    font-size: 1.5rem;
    margin-bottom: 1rem;
    color: #2c3e50;
  }

  .welcome p {
    color: #7f8c8d;
    font-size: 1rem;
    line-height: 1.8;
  }

  .cards {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 1.5rem;
    margin-top: 2rem;
  }

  .card {
    background: white;
    border-radius: 8px;
    padding: 2rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
    transition: all 0.3s;
    cursor: pointer;
    text-decoration: none;
    color: inherit;
    display: block;
  }

  .card:hover {
    transform: translateY(-4px);
    box-shadow: 0 8px 16px rgba(102, 126, 234, 0.3);
  }

  .card-icon {
    font-size: 3rem;
    margin-bottom: 1rem;
  }

  .card h3 {
    font-size: 1.3rem;
    margin-bottom: 0.5rem;
    color: #2c3e50;
  }

  .card p {
    color: #7f8c8d;
    font-size: 0.9rem;
    line-height: 1.6;
  }

  .footer {
    background: white;
    padding: 1.5rem;
    text-align: center;
    color: #7f8c8d;
    font-size: 0.85rem;
    box-shadow: 0 -2px 4px rgba(0, 0, 0, 0.05);
  }

  @media (max-width: 768px) {
    .container {
      padding: 1.5rem 1rem;
    }

    .header h1 {
      font-size: 2rem;
    }

    .cards {
      grid-template-columns: 1fr;
    }
  }
</style>
