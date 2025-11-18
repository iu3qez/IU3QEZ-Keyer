<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { SystemStats, TaskInfo } from '../lib/types';

  let stats: SystemStats | null = null;
  let loading = true;
  let error: string | null = null;
  let autoRefresh = true;
  let intervalId: number | null = null;

  function formatBytes(bytes: number): string {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(2) + ' MB';
  }

  function getStateBadgeClass(state: string): string {
    if (state === 'Running') return 'state-running';
    if (state === 'Blocked') return 'state-blocked';
    if (state === 'Suspended') return 'state-suspended';
    return '';
  }

  async function loadSystemStats() {
    try {
      loading = true;
      error = null;
      stats = await api.getSystemStats();
    } catch (e) {
      error = (e as Error).message;
      console.error('Failed to load system stats:', e);
    } finally {
      loading = false;
    }
  }

  function handleRefresh() {
    loadSystemStats();
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
    intervalId = setInterval(loadSystemStats, 2000) as unknown as number;
  }

  function stopAutoRefresh() {
    if (intervalId !== null) {
      clearInterval(intervalId);
      intervalId = null;
    }
  }

  onMount(() => {
    loadSystemStats();
    if (autoRefresh) {
      startAutoRefresh();
    }
  });

  onDestroy(() => {
    stopAutoRefresh();
  });

  $: memoryUsedPercent = stats
    ? (
        ((stats.heap.total_bytes - stats.heap.free_bytes) /
          stats.heap.total_bytes) *
        100
      ).toFixed(1)
    : '0';

  $: sortedTasksByCpu = stats?.tasks
    ? [...stats.tasks].sort(
        (a, b) => (b.cpu_percent || 0) - (a.cpu_percent || 0)
      )
    : [];
</script>

<div class="system-page">
  <div class="header">
    <h1>System Monitor</h1>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    <!-- Controls -->
    <div class="card">
      <div class="controls">
        <div class="auto-refresh">
          <input
            type="checkbox"
            id="autoRefresh"
            bind:checked={autoRefresh}
            on:change={handleAutoRefreshToggle}
          />
          <label for="autoRefresh">Auto-refresh every 2 seconds</label>
        </div>
        <button class="refresh-btn" on:click={handleRefresh}>🔄 Refresh Now</button>
      </div>
    </div>

    {#if error}
      <div class="card">
        <div class="error">{error}</div>
      </div>
    {/if}

    {#if loading && !stats}
      <div class="card">
        <div class="loading">Loading system statistics...</div>
      </div>
    {:else if stats}
      <!-- System Uptime -->
      <div class="card">
        <h2>System Uptime</h2>
        <div class="stats-grid">
          <div class="stat-box">
            <div class="stat-label">Hours</div>
            <div class="stat-value">{stats.uptime.hours}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Minutes</div>
            <div class="stat-value">{stats.uptime.minutes}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Seconds</div>
            <div class="stat-value">{stats.uptime.seconds}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Total Seconds</div>
            <div class="stat-value">{stats.uptime.total_seconds}</div>
          </div>
        </div>
      </div>

      <!-- Heap Memory -->
      <div class="card">
        <h2>Heap Memory</h2>
        <div class="stats-grid">
          <div class="stat-box">
            <div class="stat-label">Free</div>
            <div class="stat-value">{formatBytes(stats.heap.free_bytes)}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Minimum Free</div>
            <div class="stat-value">{formatBytes(stats.heap.minimum_free_bytes)}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Total</div>
            <div class="stat-value">{formatBytes(stats.heap.total_bytes)}</div>
          </div>
          <div class="stat-box">
            <div class="stat-label">Largest Block</div>
            <div class="stat-value">{formatBytes(stats.heap.largest_free_block)}</div>
          </div>
        </div>
        <h3>Memory Usage</h3>
        <div class="progress-bar">
          <div class="progress-fill" style="width: {memoryUsedPercent}%">
            {memoryUsedPercent}% Used
          </div>
        </div>
        <div class="fragmentation">
          Fragmentation: {stats.heap.fragmentation_percent.toFixed(1)}%
        </div>
      </div>

      <!-- CPU Usage -->
      <div class="card">
        <h2>CPU Usage by Task</h2>
        {#if sortedTasksByCpu.length > 0}
          <table>
            <thead>
              <tr>
                <th>Task Name</th>
                <th>State</th>
                <th>Priority</th>
                <th>Runtime (µs)</th>
                <th>CPU %</th>
                <th style="width: 200px;">Usage</th>
              </tr>
            </thead>
            <tbody>
              {#each sortedTasksByCpu as task}
                <tr>
                  <td><strong>{task.name}</strong></td>
                  <td>
                    <span class="state-badge {getStateBadgeClass(task.state)}">
                      {task.state}
                    </span>
                  </td>
                  <td>{task.priority}</td>
                  <td>{task.runtime_us?.toLocaleString() || 'N/A'}</td>
                  <td>
                    <strong
                      >{task.cpu_percent !== undefined
                        ? task.cpu_percent.toFixed(2)
                        : 'N/A'}%</strong
                    >
                  </td>
                  <td>
                    <div class="progress-bar" style="height: 16px;">
                      <div
                        class="progress-fill"
                        style="width: {Math.min(
                          task.cpu_percent || 0,
                          100
                        )}%; font-size: 0.65rem;"
                      >
                        {task.cpu_percent !== undefined
                          ? task.cpu_percent.toFixed(2)
                          : 'N/A'}%
                      </div>
                    </div>
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        {:else}
          <div class="loading">No CPU statistics available</div>
        {/if}
      </div>

      <!-- Task List -->
      <div class="card">
        <h2>Task Information</h2>
        {#if stats.tasks.length > 0}
          <table>
            <thead>
              <tr>
                <th>Task Name</th>
                <th>State</th>
                <th>Priority</th>
                <th>Stack HWM (bytes)</th>
                <th>Task Number</th>
              </tr>
            </thead>
            <tbody>
              {#each stats.tasks as task}
                <tr>
                  <td><strong>{task.name}</strong></td>
                  <td>
                    <span class="state-badge {getStateBadgeClass(task.state)}">
                      {task.state}
                    </span>
                  </td>
                  <td>{task.priority}</td>
                  <td>{task.stack_hwm}</td>
                  <td>{task.task_number}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        {:else}
          <div class="loading">No task information available</div>
        {/if}
      </div>
    {/if}
  </div>
</div>

<style>
  .system-page {
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
    max-width: 1280px;
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

  .card h3 {
    margin-top: 1.5rem;
    margin-bottom: 0.75rem;
    color: #2c3e50;
    font-size: 1.1rem;
  }

  .controls {
    display: flex;
    justify-content: space-between;
    align-items: center;
    flex-wrap: wrap;
    gap: 1rem;
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

  .refresh-btn {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    border: none;
    padding: 0.6rem 1.2rem;
    border-radius: 4px;
    cursor: pointer;
    font-size: 0.9rem;
  }

  .refresh-btn:hover {
    opacity: 0.9;
  }

  .stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 1rem;
    margin: 1rem 0;
  }

  .stat-box {
    background: #f8f9fa;
    border-left: 4px solid #667eea;
    padding: 1rem;
    border-radius: 4px;
  }

  .stat-label {
    font-size: 0.75rem;
    text-transform: uppercase;
    color: #7f8c8d;
    font-weight: 600;
    letter-spacing: 0.5px;
    margin-bottom: 0.5rem;
  }

  .stat-value {
    font-size: 1.5rem;
    font-weight: 600;
    color: #2c3e50;
  }

  .progress-bar {
    background: #e9ecef;
    height: 20px;
    border-radius: 10px;
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
    font-size: 0.75rem;
    font-weight: 600;
  }

  .fragmentation {
    margin-top: 0.5rem;
    font-size: 0.85rem;
    color: #7f8c8d;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    margin: 1rem 0;
  }

  th {
    background: #f8f9fa;
    padding: 0.75rem;
    text-align: left;
    border-bottom: 2px solid #667eea;
    font-weight: 600;
    color: #2c3e50;
    font-size: 0.9rem;
  }

  td {
    padding: 0.75rem;
    border-bottom: 1px solid #e9ecef;
    font-family: 'Courier New', monospace;
    font-size: 0.85rem;
  }

  tr:hover {
    background: #f8f9fa;
  }

  .state-badge {
    display: inline-block;
    padding: 0.25rem 0.5rem;
    border-radius: 4px;
    font-size: 0.75rem;
    font-weight: 600;
  }

  .state-running {
    background: #d4edda;
    color: #155724;
  }

  .state-blocked {
    background: #fff3cd;
    color: #856404;
  }

  .state-suspended {
    background: #f8d7da;
    color: #721c24;
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
    .stats-grid {
      grid-template-columns: 1fr;
    }

    table {
      font-size: 0.75rem;
    }

    .controls {
      flex-direction: column;
      align-items: stretch;
    }

    .refresh-btn {
      width: 100%;
    }
  }
</style>
