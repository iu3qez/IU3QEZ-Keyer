<script lang="ts">
  import { api } from '../lib/api';

  let statusMessage = '';
  let statusType: 'success' | 'error' | 'info' = 'info';
  let showStatus = false;
  let buttonDisabled = false;

  function displayStatus(message: string, type: 'success' | 'error' | 'info') {
    statusMessage = message;
    statusType = type;
    showStatus = true;
  }

  async function handleEnterBootloader() {
    const confirmed = confirm(
      'Are you sure you want to enter bootloader mode?\n\n' +
        'The device will disconnect from WiFi and restart as a USB drive.\n\n' +
        'Make sure the device is connected via USB cable.'
    );

    if (!confirmed) return;

    buttonDisabled = true;
    displayStatus('Sending bootloader entry command...', 'info');

    try {
      await api.enterBootloader();
      displayStatus(
        '✓ Device is restarting into bootloader mode. USB drive "KEYERBOOT" should appear shortly...',
        'success'
      );

      setTimeout(() => {
        displayStatus(
          'This page will become unavailable as the device has entered bootloader mode. Close this window and look for the KEYERBOOT USB drive.',
          'info'
        );
      }, 3000);
    } catch (error) {
      displayStatus('✗ Failed to enter bootloader mode: ' + (error as Error).message, 'error');
      buttonDisabled = false;
    }
  }
</script>

<div class="firmware-page">
  <div class="header">
    <h1>🔄 Firmware Update</h1>
    <div class="subtitle">UF2 Bootloader - USB Drive Method</div>
    <div class="nav"><a href="/">← Back to Home</a></div>
  </div>

  <div class="container">
    <div class="card">
      <h2>How to Update Firmware</h2>
      <p>
        This device uses the UF2 bootloader for easy firmware updates via USB. Follow the steps
        below to update your device.
      </p>

      <div class="steps">
        <div class="step">
          <div class="step-number">1</div>
          <div class="step-content">
            <h3>Enter Bootloader Mode</h3>
            <p>
              Click the button below to restart the device into bootloader mode. The device will
              disconnect from WiFi and appear as a USB drive.
            </p>
          </div>
        </div>

        <div class="step">
          <div class="step-number">2</div>
          <div class="step-content">
            <h3>USB Drive Appears</h3>
            <p>
              After restart, a USB drive named <strong>KEYERBOOT</strong> will appear on your
              computer. Open it to see README.TXT with instructions.
            </p>
          </div>
        </div>

        <div class="step">
          <div class="step-number">3</div>
          <div class="step-content">
            <h3>Drag Firmware File</h3>
            <p>
              Download the latest <code>firmware.uf2</code> file and drag it onto the KEYERBOOT
              drive. The device will automatically flash and restart.
            </p>
          </div>
        </div>

        <div class="step">
          <div class="step-number">4</div>
          <div class="step-content">
            <h3>Update Complete</h3>
            <p>
              The device will restart with the new firmware. Your configuration (WiFi, keyer
              settings) will be preserved.
            </p>
          </div>
        </div>
      </div>

      <div class="warning-box">
        <strong>⚠️ Important Notes:</strong>
        <ul>
          <li>Do NOT unplug the device during the update process</li>
          <li>Only use official <code>.uf2</code> firmware files for this device</li>
          <li>The device must be connected via USB cable (not just WiFi)</li>
          <li>Invalid firmware files will be automatically rejected</li>
        </ul>
      </div>

      <div class="info-box">
        <strong>ℹ️ Configuration Preservation:</strong><br />
        Your NVS configuration (WiFi credentials, keyer settings, etc.) will be preserved during
        the update. Only the application firmware is replaced.
      </div>

      <button class="btn" on:click={handleEnterBootloader} disabled={buttonDisabled}>
        Enter Bootloader Mode
      </button>
      <a href="/" class="btn btn-secondary">Cancel</a>

      {#if showStatus}
        <div class="status-message status-{statusType}">{statusMessage}</div>
      {/if}
    </div>

    <div class="card">
      <h2>Build Your Own Firmware</h2>
      <p>If you want to build firmware from source code:</p>
      <div class="code-block">
        idf.py build<br />
        ninja -C build uf2
      </div>
      <p class="build-info">
        The generated <code>build/firmware.uf2</code> file can be used for updates.
      </p>
    </div>

    <div class="card">
      <h2>Troubleshooting</h2>
      <p>
        <strong>Device stuck in bootloop?</strong><br />
        The bootloader has automatic recovery after 3 consecutive failed boots. Alternatively,
        delete <code>FACTORY_RESET.TXT</code> from the KEYERBOOT drive to manually trigger NVS
        reset.
      </p>
      <p class="troubleshoot-item">
        <strong>USB drive not appearing?</strong><br />
        Ensure USB cable supports data transfer (not charge-only). Try a different USB port or
        cable.
      </p>
      <p class="troubleshoot-item">
        <strong>Update failed?</strong><br />
        The device will reject invalid firmware files. Verify you downloaded a valid
        <code>.uf2</code>
        file for this device model (ESP32-S3 Keyer QRS2HST).
      </p>
    </div>
  </div>

  <div class="footer">
    Keyer QRS2HST &copy; 2025 - For more info, visit
    <a href="https://github.com/iu3qez/keyer_qrs2hst" target="_blank">GitHub</a>
  </div>
</div>

<style>
  .firmware-page {
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    background: #f5f7fa;
  }

  .header {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    padding: 2rem 1.5rem;
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
    text-align: center;
  }

  .header h1 {
    font-size: 2rem;
    font-weight: 600;
    margin-bottom: 0.5rem;
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
    max-width: 800px;
    margin: 0 auto;
    padding: 2rem 1rem;
    flex: 1;
  }

  .card {
    background: white;
    border-radius: 8px;
    padding: 2rem;
    margin-bottom: 2rem;
    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
  }

  .card h2 {
    font-size: 1.5rem;
    margin-bottom: 1rem;
    color: #2c3e50;
  }

  .card p {
    color: #7f8c8d;
    margin-bottom: 1rem;
    line-height: 1.8;
  }

  .steps {
    margin: 2rem 0;
  }

  .step {
    display: flex;
    align-items: flex-start;
    margin-bottom: 1.5rem;
  }

  .step-number {
    background: #667eea;
    color: white;
    width: 2rem;
    height: 2rem;
    border-radius: 50%;
    display: flex;
    align-items: center;
    justify-content: center;
    font-weight: 600;
    flex-shrink: 0;
    margin-right: 1rem;
  }

  .step-content h3 {
    font-size: 1rem;
    margin-bottom: 0.3rem;
    color: #2c3e50;
  }

  .step-content p {
    font-size: 0.9rem;
    color: #7f8c8d;
  }

  .warning-box {
    background: #fff3cd;
    border-left: 4px solid #ffc107;
    padding: 1rem;
    margin: 1.5rem 0;
    border-radius: 4px;
  }

  .warning-box strong {
    color: #856404;
    display: block;
    margin-bottom: 0.5rem;
  }

  .warning-box ul {
    margin-left: 1.5rem;
    color: #856404;
  }

  .warning-box li {
    margin: 0.3rem 0;
  }

  .info-box {
    background: #d1ecf1;
    border-left: 4px solid #17a2b8;
    padding: 1rem;
    margin: 1.5rem 0;
    border-radius: 4px;
    color: #0c5460;
  }

  .btn {
    background: #667eea;
    color: white;
    border: none;
    padding: 1rem 2rem;
    border-radius: 6px;
    font-size: 1rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.3s;
    display: inline-block;
    text-align: center;
    text-decoration: none;
  }

  .btn:hover:not(:disabled) {
    background: #5568d3;
    transform: translateY(-2px);
    box-shadow: 0 4px 8px rgba(102, 126, 234, 0.3);
  }

  .btn:disabled {
    background: #95a5a6;
    cursor: not-allowed;
    transform: none;
  }

  .btn-secondary {
    background: #6c757d;
    margin-left: 0.5rem;
  }

  .btn-secondary:hover {
    background: #5a6268;
  }

  .status-message {
    padding: 1rem;
    border-radius: 6px;
    margin-top: 1rem;
  }

  .status-success {
    background: #d4edda;
    color: #155724;
    border: 1px solid #c3e6cb;
  }

  .status-error {
    background: #f8d7da;
    color: #721c24;
    border: 1px solid #f5c6cb;
  }

  .status-info {
    background: #d1ecf1;
    color: #0c5460;
    border: 1px solid #bee5eb;
  }

  .code-block {
    background: #2c3e50;
    color: #ecf0f1;
    padding: 1rem;
    border-radius: 4px;
    font-family: 'Courier New', monospace;
    font-size: 0.85rem;
    overflow-x: auto;
    margin: 0.5rem 0;
  }

  .build-info {
    margin-top: 1rem;
    font-size: 0.9rem;
    color: #7f8c8d;
  }

  .troubleshoot-item {
    margin-top: 1rem;
  }

  .footer {
    background: white;
    padding: 1rem;
    text-align: center;
    color: #7f8c8d;
    font-size: 0.85rem;
    box-shadow: 0 -2px 4px rgba(0, 0, 0, 0.05);
  }

  .footer a {
    color: #667eea;
    text-decoration: none;
  }

  .footer a:hover {
    text-decoration: underline;
  }

  @media (max-width: 768px) {
    .container {
      padding: 1rem;
    }

    .header h1 {
      font-size: 1.5rem;
    }

    .btn {
      width: 100%;
      margin-bottom: 0.5rem;
    }

    .btn-secondary {
      margin-left: 0;
    }
  }
</style>
