<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import { api } from '../lib/api';
  import type { TimelineEvent } from '../lib/types';

  // Configuration
  let canvasWidth = 1200;
  let canvasHeight = 400;
  let trackHeight = 80;
  let pollInterval = 100;
  let maxEvents = 10000;
  let durationSeconds = 3.0;

  // State
  let enabled = true;
  let lastTimestamp = 0;
  let eventBuffer: TimelineEvent[] = [];
  let currentWPM = 20;
  let wpmSource = 'keying_config';
  let gridDirty = true;

  // Visualization options
  let showMemoryWindow = true;
  let showLatch = true;
  let showSqueeze = true;
  let showGapMarkers = true;
  let alignDecodedText = true;

  // Canvas references
  let canvas: HTMLCanvasElement;
  let ctx: CanvasRenderingContext2D | null = null;
  let offscreenCanvas: HTMLCanvasElement;
  let offscreenCtx: CanvasRenderingContext2D | null = null;

  // Intervals
  let eventPollIntervalId: number | null = null;
  let configPollIntervalId: number | null = null;
  let animationFrameId: number | null = null;

  async function pollEvents() {
    if (!enabled) return;

    try {
      const data = await api.getTimelineEvents(lastTimestamp, 100);

      if (data.events && Array.isArray(data.events) && data.events.length > 0) {
        eventBuffer.push(...data.events);

        const latestEvent = data.events[data.events.length - 1];
        lastTimestamp = latestEvent.timestamp_us;

        const pruneThreshold = lastTimestamp - durationSeconds * 2 * 1000000;
        eventBuffer = eventBuffer.filter((evt) => evt.timestamp_us > pruneThreshold);

        if (eventBuffer.length > maxEvents) {
          eventBuffer = eventBuffer.slice(-maxEvents);
        }
      }
    } catch (error) {
      console.error('[Timeline] Poll error:', error);
    }
  }

  async function pollConfig() {
    try {
      const data = await api.getTimelineConfig();

      if (data.wpm && data.wpm !== currentWPM) {
        currentWPM = data.wpm;
        wpmSource = data.wpm_source || 'unknown';
        gridDirty = true;
      }
    } catch (error) {
      console.error('[Timeline] Config poll error:', error);
    }
  }

  function renderCanvas() {
    if (!ctx || !offscreenCtx) return;

    ctx.clearRect(0, 0, canvasWidth, canvasHeight);

    drawGrid();
    drawTracks();
    drawLogicOverlay();
    drawDecodedText();
  }

  function drawGrid() {
    if (!offscreenCtx || !ctx) return;

    if (gridDirty) {
      const ditDurationMs = 1200 / currentWPM;
      const durationMs = durationSeconds * 1000;
      const pixelPerMs = canvasWidth / durationMs;
      const pixelPerDit = ditDurationMs * pixelPerMs;

      offscreenCtx.clearRect(0, 0, canvasWidth, canvasHeight);

      for (let x = 0; x < canvasWidth; x += pixelPerDit) {
        const ditCount = Math.round(x / pixelPerDit);

        if (ditCount % 7 === 0) {
          offscreenCtx.strokeStyle = '#a0a0a0';
          offscreenCtx.lineWidth = 2;
        } else if (ditCount % 3 === 0) {
          offscreenCtx.strokeStyle = '#c0c0c0';
          offscreenCtx.lineWidth = 1;
        } else {
          offscreenCtx.strokeStyle = '#e0e0e0';
          offscreenCtx.lineWidth = 0.5;
        }

        offscreenCtx.beginPath();
        offscreenCtx.moveTo(x, 0);
        offscreenCtx.lineTo(x, canvasHeight);
        offscreenCtx.stroke();
      }

      gridDirty = false;
    }

    ctx.drawImage(offscreenCanvas, 0, 0);
  }

  function drawTracks() {
    if (!ctx) return;

    const durationUs = durationSeconds * 1000000;
    const currentTimeUs = lastTimestamp;
    const startTimeUs = currentTimeUs - durationUs;

    const visibleEvents = eventBuffer.filter(
      (evt) => evt.timestamp_us >= startTimeUs && evt.timestamp_us <= currentTimeUs
    );

    const trackY = {
      DOT: 0,
      DASH: trackHeight,
      OUT: trackHeight * 2,
      LOGIC: trackHeight * 3,
    };

    ctx.fillStyle = '#f5f5f5';
    Object.values(trackY).forEach((y) => {
      ctx!.fillRect(0, y, canvasWidth, trackHeight);
    });

    ctx.font = 'bold 14px Arial';
    ctx.fillStyle = '#000';
    ctx.fillText('DOT', 10, trackY.DOT + 20);
    ctx.fillText('DASH', 10, trackY.DASH + 20);
    ctx.fillText('OUT', 10, trackY.OUT + 20);
    ctx.fillText('LOGIC', 10, trackY.LOGIC + 20);

    const timestampToX = (ts: number) =>
      ((ts - startTimeUs) / durationUs) * canvasWidth;

    drawPaddleTrack(visibleEvents, trackY.DOT, timestampToX, 0, '#4169E1');
    drawPaddleTrack(visibleEvents, trackY.DASH, timestampToX, 1, '#DC143C');
    drawKeyingTrack(visibleEvents, trackY.OUT, timestampToX, '#32CD32');
  }

  function drawPaddleTrack(
    events: TimelineEvent[],
    trackY: number,
    timestampToX: (ts: number) => number,
    paddleType: number,
    color: string
  ) {
    if (!ctx) return;

    const paddleEvents = events.filter(
      (evt) => evt.type === 'paddle_edge' && evt.arg0 === paddleType
    );

    let pressStart: number | null = null;

    paddleEvents.forEach((evt) => {
      const isPressed = evt.arg1 === 1;

      if (isPressed) {
        pressStart = evt.timestamp_us;
      } else if (pressStart !== null) {
        const x1Raw = timestampToX(pressStart);
        const x2Raw = timestampToX(evt.timestamp_us);
        const x1 = Math.max(0, Math.min(x1Raw, canvasWidth));
        const x2 = Math.max(0, Math.min(x2Raw, canvasWidth));
        const width = Math.max(x2 - x1, 1);

        ctx!.fillStyle = color;
        ctx!.fillRect(x1, trackY, width, trackHeight);

        pressStart = null;
      }
    });

    if (pressStart !== null) {
      const x1Raw = timestampToX(pressStart);
      const x1 = Math.max(0, Math.min(x1Raw, canvasWidth));
      const width = canvasWidth - x1;

      if (width > 0) {
        ctx!.fillStyle = color;
        ctx!.fillRect(x1, trackY, width, trackHeight);
      }
    }
  }

  function drawKeyingTrack(
    events: TimelineEvent[],
    trackY: number,
    timestampToX: (ts: number) => number,
    color: string
  ) {
    if (!ctx) return;

    const keyingEvents = events.filter((evt) => evt.type === 'keying');

    const ditEvents = keyingEvents.filter((evt) => evt.arg0 === 0);
    const dahEvents = keyingEvents.filter((evt) => evt.arg0 === 1);

    const allKeyingEvents = [...ditEvents, ...dahEvents].sort(
      (a, b) => a.timestamp_us - b.timestamp_us
    );

    let activeStart: number | null = null;

    allKeyingEvents.forEach((evt) => {
      const isActive = evt.arg1 === 1;

      if (isActive) {
        activeStart = evt.timestamp_us;
      } else if (activeStart !== null) {
        const x1Raw = timestampToX(activeStart);
        const x2Raw = timestampToX(evt.timestamp_us);
        const x1 = Math.max(0, Math.min(x1Raw, canvasWidth));
        const x2 = Math.max(0, Math.min(x2Raw, canvasWidth));
        const width = Math.max(x2 - x1, 1);

        ctx!.fillStyle = color;
        ctx!.fillRect(x1, trackY, width, trackHeight);

        activeStart = null;
      }
    });

    if (activeStart !== null) {
      const x1Raw = timestampToX(activeStart);
      const x1 = Math.max(0, Math.min(x1Raw, canvasWidth));
      const width = canvasWidth - x1;

      if (width > 0) {
        ctx!.fillStyle = color;
        ctx!.fillRect(x1, trackY, width, trackHeight);
      }
    }
  }

  function drawLogicOverlay() {
    if (!ctx) return;

    const durationUs = durationSeconds * 1000000;
    const currentTimeUs = lastTimestamp;
    const startTimeUs = currentTimeUs - durationUs;

    const visibleEvents = eventBuffer.filter(
      (evt) => evt.timestamp_us >= startTimeUs && evt.timestamp_us <= currentTimeUs
    );

    const trackY = {
      LOGIC: trackHeight * 3,
    };

    const timestampToX = (ts: number) =>
      ((ts - startTimeUs) / durationUs) * canvasWidth;

    // Memory window shading
    if (showMemoryWindow) {
      const memoryWindowEvents = visibleEvents.filter(
        (evt) => evt.type === 'memory_window'
      );

      let ditWindowStart: number | null = null;
      let dahWindowStart: number | null = null;

      memoryWindowEvents.forEach((evt) => {
        const isDah = evt.arg0 === 1;
        const isOpen = evt.arg1 === 1;

        if (isDah) {
          if (isOpen) {
            dahWindowStart = evt.timestamp_us;
          } else if (dahWindowStart !== null) {
            const x1 = Math.max(0, Math.min(timestampToX(dahWindowStart), canvasWidth));
            const x2 = Math.max(0, Math.min(timestampToX(evt.timestamp_us), canvasWidth));
            const width = x2 - x1;

            if (width > 0) {
              ctx!.globalAlpha = 0.2;
              ctx!.fillStyle = 'rgba(255, 136, 0, 1)';
              ctx!.fillRect(x1, trackY.LOGIC, width, trackHeight);
              ctx!.globalAlpha = 1.0;
            }

            dahWindowStart = null;
          }
        } else {
          if (isOpen) {
            ditWindowStart = evt.timestamp_us;
          } else if (ditWindowStart !== null) {
            const x1 = Math.max(0, Math.min(timestampToX(ditWindowStart), canvasWidth));
            const x2 = Math.max(0, Math.min(timestampToX(evt.timestamp_us), canvasWidth));
            const width = x2 - x1;

            if (width > 0) {
              ctx!.globalAlpha = 0.2;
              ctx!.fillStyle = 'rgba(255, 255, 0, 1)';
              ctx!.fillRect(x1, trackY.LOGIC, width, trackHeight);
              ctx!.globalAlpha = 1.0;
            }

            ditWindowStart = null;
          }
        }
      });
    }

    // Latch shading and symbols
    if (showLatch) {
      const latchEvents = visibleEvents.filter((evt) => evt.type === 'latch');

      let latchStart: number | null = null;

      latchEvents.forEach((evt) => {
        const isActive = evt.arg1 === 1;

        if (isActive) {
          latchStart = evt.timestamp_us;
        } else if (latchStart !== null) {
          const x1 = Math.max(0, Math.min(timestampToX(latchStart), canvasWidth));
          const x2 = Math.max(0, Math.min(timestampToX(evt.timestamp_us), canvasWidth));
          const width = x2 - x1;

          if (width > 0) {
            ctx!.globalAlpha = 0.15;
            ctx!.fillStyle = 'rgba(76, 175, 80, 1)';
            ctx!.fillRect(x1, trackY.LOGIC, width, trackHeight);
            ctx!.globalAlpha = 1.0;

            if (x1 > 0 && x1 < canvasWidth) {
              ctx!.font = '18px Arial';
              ctx!.fillStyle = '#4CAF50';
              const y = trackY.LOGIC + trackHeight / 2 + 7;
              ctx!.fillText('🔒', x1 - 9, y);
            }
          }

          latchStart = null;
        }
      });
    }

    // Squeeze symbols
    if (showSqueeze) {
      const squeezeEvents = visibleEvents.filter((evt) => evt.type === 'squeeze');

      ctx.font = '24px Arial';
      ctx.fillStyle = '#FFD700';
      ctx.globalAlpha = 1.0;

      squeezeEvents.forEach((evt) => {
        const x = timestampToX(evt.timestamp_us);
        const y = trackHeight - 5;
        ctx!.fillText('⚡', x - 12, y);
      });
    }

    // Gap markers
    if (showGapMarkers) {
      const gapEvents = visibleEvents.filter((evt) => evt.type === 'gap_marker');

      gapEvents.forEach((evt) => {
        const x = timestampToX(evt.timestamp_us);
        const gapType = evt.arg0;

        let lineWidth: number;
        let color: string;
        if (gapType === 2) {
          lineWidth = 3;
          color = '#CC0000';
        } else if (gapType === 1) {
          lineWidth = 2;
          color = '#FF0000';
        } else {
          lineWidth = 1;
          color = '#FF6666';
        }

        ctx!.strokeStyle = color;
        ctx!.lineWidth = lineWidth;
        ctx!.globalAlpha = 0.6;

        ctx!.beginPath();
        ctx!.moveTo(x, 0);
        ctx!.lineTo(x, canvasHeight);
        ctx!.stroke();

        ctx!.globalAlpha = 1.0;
      });
    }

    ctx.globalAlpha = 1.0;
  }

  function drawDecodedText() {
    if (!ctx || !alignDecodedText) return;

    const durationUs = durationSeconds * 1000000;
    const currentTimeUs = lastTimestamp;
    const startTimeUs = currentTimeUs - durationUs;

    const decodedEvents = eventBuffer.filter(
      (evt) =>
        evt.type === 'decoded_char' &&
        evt.timestamp_us >= startTimeUs &&
        evt.timestamp_us <= currentTimeUs
    );

    if (decodedEvents.length === 0) return;

    const timestampToX = (ts: number) =>
      ((ts - startTimeUs) / durationUs) * canvasWidth;

    ctx.font = '16px "Courier New", monospace';
    ctx.fillStyle = '#000';
    ctx.textBaseline = 'top';

    const baseY = trackHeight * 4 + 10;

    let lastX = -1000;
    const minSpacing = 10;

    decodedEvents.forEach((evt) => {
      const char = String.fromCharCode(evt.arg0);
      const x = timestampToX(evt.timestamp_us);

      let yOffset = 0;
      if (x - lastX < minSpacing) {
        yOffset = 20;
      }

      const y = baseY + yOffset;

      ctx!.fillText(char, x, y);

      lastX = x + ctx!.measureText(char).width;
    });

    ctx.textBaseline = 'alphabetic';
  }

  function saveConfig() {
    const config = {
      durationSeconds,
      enabled,
      visualizationOptions: {
        showMemoryWindow,
        showLatch,
        showSqueeze,
        showGapMarkers,
        alignDecodedText,
      },
    };

    localStorage.setItem('keyer_timeline_config', JSON.stringify(config));
    alert('Configuration saved!');
  }

  function loadConfig() {
    const saved = localStorage.getItem('keyer_timeline_config');
    if (!saved) return;

    try {
      const config = JSON.parse(saved);

      durationSeconds = config.durationSeconds || 3.0;
      enabled = config.enabled !== undefined ? config.enabled : true;

      if (config.visualizationOptions) {
        showMemoryWindow = config.visualizationOptions.showMemoryWindow ?? true;
        showLatch = config.visualizationOptions.showLatch ?? true;
        showSqueeze = config.visualizationOptions.showSqueeze ?? true;
        showGapMarkers = config.visualizationOptions.showGapMarkers ?? true;
        alignDecodedText = config.visualizationOptions.alignDecodedText ?? true;
      }
    } catch (error) {
      console.error('[Timeline] Failed to load config:', error);
    }
  }

  function resetDefaults() {
    durationSeconds = 3.0;
    enabled = true;
    showMemoryWindow = true;
    showLatch = true;
    showSqueeze = true;
    showGapMarkers = true;
    alignDecodedText = true;

    saveConfig();
  }

  onMount(() => {
    loadConfig();

    ctx = canvas.getContext('2d');
    offscreenCanvas = document.createElement('canvas');
    offscreenCanvas.width = canvasWidth;
    offscreenCanvas.height = canvasHeight;
    offscreenCtx = offscreenCanvas.getContext('2d');

    eventPollIntervalId = setInterval(pollEvents, pollInterval) as unknown as number;
    configPollIntervalId = setInterval(pollConfig, 1000) as unknown as number;

    pollEvents();
    pollConfig();

    function renderLoop() {
      renderCanvas();
      animationFrameId = requestAnimationFrame(renderLoop) as unknown as number;
    }
    animationFrameId = requestAnimationFrame(renderLoop) as unknown as number;
  });

  onDestroy(() => {
    if (eventPollIntervalId !== null) clearInterval(eventPollIntervalId);
    if (configPollIntervalId !== null) clearInterval(configPollIntervalId);
    if (animationFrameId !== null) cancelAnimationFrame(animationFrameId);
  });

  $: {
    if (durationSeconds) {
      gridDirty = true;
    }
  }

  $: {
    if (!enabled) {
      console.log('[Timeline] Logging disabled (frozen)');
    } else {
      console.log('[Timeline] Logging enabled (resuming)');
      lastTimestamp = 0;
    }
  }
</script>

<div class="timeline-page">
  <div class="header">
    <h1>Real-Time Timeline Visualization</h1>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    <div class="canvas-container">
      <canvas bind:this={canvas} width={canvasWidth} height={canvasHeight}></canvas>
    </div>

    <div class="card controls-card">
      <h2>Controls</h2>
      <div class="control-row">
        <label>
          <input type="checkbox" bind:checked={enabled} />
          Enable Timeline Logging
        </label>
      </div>
      <div class="control-row">
        <label for="durationSlider">Duration:</label>
        <input
          type="range"
          id="durationSlider"
          bind:value={durationSeconds}
          min="1"
          max="10"
          step="0.5"
        />
        <span>{durationSeconds.toFixed(1)}s</span>
      </div>

      <h3>Visualization Options</h3>
      <div class="checkbox-grid">
        <label>
          <input type="checkbox" bind:checked={showMemoryWindow} />
          Show Memory Window
        </label>
        <label>
          <input type="checkbox" bind:checked={showLatch} />
          Show Latch
        </label>
        <label>
          <input type="checkbox" bind:checked={showSqueeze} />
          Show Squeeze
        </label>
        <label>
          <input type="checkbox" bind:checked={showGapMarkers} />
          Show Gap Markers
        </label>
        <label>
          <input type="checkbox" bind:checked={alignDecodedText} />
          Align Decoded Text
        </label>
      </div>

      <div class="control-row button-row">
        <button on:click={saveConfig}>Save Config</button>
        <button on:click={resetDefaults}>Reset to Defaults</button>
      </div>
    </div>

    <div class="card legend-card">
      <h3>Legend</h3>
      <div class="legend-grid">
        <div class="legend-item">
          <span class="legend-color" style="background: #4169E1;"></span>
          <span>Blue (DOT): Dit paddle</span>
        </div>
        <div class="legend-item">
          <span class="legend-color" style="background: #DC143C;"></span>
          <span>Red (DASH): Dah paddle</span>
        </div>
        <div class="legend-item">
          <span class="legend-color" style="background: #32CD32;"></span>
          <span>Green (OUT): TX output</span>
        </div>
        <div class="legend-item">
          <span class="legend-color" style="background: rgba(255,255,0,0.2);"></span>
          <span>Yellow: Dit memory</span>
        </div>
        <div class="legend-item">
          <span class="legend-color" style="background: rgba(255,136,0,0.2);"></span>
          <span>Orange: Dah memory</span>
        </div>
        <div class="legend-item">
          <span style="font-size: 1.2em;">⏱</span>
          <span>Late release active</span>
        </div>
        <div class="legend-item">
          <span style="font-size: 1.2em;">🔒</span>
          <span>Latch active</span>
        </div>
        <div class="legend-item">
          <span style="font-size: 1.2em;">⚡</span>
          <span>Squeeze detected</span>
        </div>
        <div class="legend-item">
          <span style="color: #ff0000; font-weight: bold;">|</span>
          <span>Gap markers (thin=element, medium=char, thick=word)</span>
        </div>
      </div>
    </div>
  </div>
</div>

<style>
  .timeline-page {
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

  .canvas-container {
    background: #fff;
    border: 1px solid #ddd;
    border-radius: 8px;
    padding: 1rem;
    margin: 1rem 0;
    overflow-x: auto;
  }

  canvas {
    display: block;
    max-width: 100%;
    height: auto;
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
    font-size: 1.1rem;
    color: #667eea;
    margin: 1rem 0 0.75rem 0;
  }

  .control-row {
    display: flex;
    align-items: center;
    gap: 1rem;
    margin: 0.75rem 0;
    flex-wrap: wrap;
  }

  .control-row input[type='range'] {
    flex: 1;
    min-width: 200px;
  }

  .button-row {
    margin-top: 1.5rem;
    gap: 0.75rem;
  }

  button {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    border: none;
    padding: 0.6rem 1.2rem;
    border-radius: 4px;
    cursor: pointer;
    font-weight: 600;
  }

  button:hover {
    opacity: 0.9;
  }

  .checkbox-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 0.75rem;
    margin: 1rem 0;
  }

  .legend-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
    gap: 0.75rem;
  }

  .legend-item {
    display: flex;
    align-items: center;
    gap: 0.75rem;
  }

  .legend-color {
    width: 30px;
    height: 20px;
    border: 1px solid #ddd;
  }

  @media (max-width: 768px) {
    .control-row {
      flex-direction: column;
    }

    .legend-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
