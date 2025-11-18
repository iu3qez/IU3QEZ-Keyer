/**
 * @file setup_page.cpp
 * @brief Hardcoded HTML/CSS/JS for captive portal setup page (manual entry only).
 * @details
 *  Simplified WiFi setup page with manual SSID/password entry.
 *  No network scanning - user must enter credentials manually.
 * @author Simone Fabris
 * @date 2025-11-16
 * @version 3.0
 */

namespace captive_portal {

/**
 * @brief Complete setup page HTML with embedded CSS and JavaScript.
 * @details
 *  Features:
 *  - Responsive design (320px-1920px)
 *  - Manual SSID and password entry
 *  - Password show/hide toggle
 *  - Form validation
 *  - Loading states and error handling
 */
constexpr const char* kSetupPageHtml = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Keyer QRS2HST Setup</title>
  <style>
    /* ========== CSS Reset & Base Styles ========== */
    * {
      margin: 0;
      padding: 0;
      box-sizing: border-box;
    }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
      color: #333;
      line-height: 1.6;
    }

    /* ========== Container ========== */
    .container {
      max-width: 500px;
      margin: 40px auto;
      background: #ffffff;
      border-radius: 12px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.2);
      overflow: hidden;
    }

    /* ========== Header ========== */
    .header {
      background: linear-gradient(135deg, #1976d2 0%, #1565c0 100%);
      color: #ffffff;
      padding: 32px 24px;
      text-align: center;
    }

    .header h1 {
      font-size: 26px;
      font-weight: 600;
      margin-bottom: 8px;
    }

    .header p {
      font-size: 15px;
      opacity: 0.95;
    }

    /* ========== Content Area ========== */
    .content {
      padding: 32px 24px;
    }

    /* ========== Form Styles ========== */
    .form-group {
      margin-bottom: 24px;
    }

    label {
      display: block;
      font-weight: 600;
      color: #333;
      margin-bottom: 8px;
      font-size: 14px;
    }

    input[type="text"],
    input[type="password"] {
      width: 100%;
      padding: 12px 16px;
      border: 2px solid #e0e0e0;
      border-radius: 8px;
      font-size: 15px;
      transition: border-color 0.3s;
    }

    input[type="text"]:focus,
    input[type="password"]:focus {
      outline: none;
      border-color: #1976d2;
    }

    .input-group {
      position: relative;
    }

    .toggle-password {
      position: absolute;
      right: 12px;
      top: 50%;
      transform: translateY(-50%);
      background: none;
      border: none;
      cursor: pointer;
      font-size: 18px;
      color: #666;
      padding: 4px;
    }

    .toggle-password:hover {
      color: #1976d2;
    }

    .help-text {
      font-size: 13px;
      color: #666;
      margin-top: 6px;
    }

    /* ========== Buttons ========== */
    .btn {
      width: 100%;
      padding: 14px 24px;
      background: linear-gradient(135deg, #1976d2 0%, #1565c0 100%);
      color: #ffffff;
      border: none;
      border-radius: 8px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s;
      margin-top: 8px;
    }

    .btn:hover:not(:disabled) {
      transform: translateY(-2px);
      box-shadow: 0 4px 12px rgba(25, 118, 210, 0.4);
    }

    .btn:disabled {
      opacity: 0.6;
      cursor: not-allowed;
    }

    /* ========== Status Messages ========== */
    .status {
      padding: 14px 16px;
      border-radius: 8px;
      margin-bottom: 20px;
      font-size: 14px;
      display: none;
    }

    .status.info {
      background: #e3f2fd;
      color: #1565c0;
      border-left: 4px solid #1976d2;
    }

    .status.success {
      background: #e8f5e9;
      color: #2e7d32;
      border-left: 4px solid #4caf50;
    }

    .status.error {
      background: #ffebee;
      color: #c62828;
      border-left: 4px solid #f44336;
    }

    /* ========== Loading Spinner ========== */
    .spinner {
      display: inline-block;
      width: 16px;
      height: 16px;
      border: 3px solid #ffffff;
      border-top-color: transparent;
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
      margin-right: 8px;
      vertical-align: middle;
    }

    @keyframes spin {
      to { transform: rotate(360deg); }
    }

    /* ========== Responsive Design ========== */
    @media (max-width: 480px) {
      body {
        padding: 10px;
      }

      .container {
        margin: 20px auto;
      }

      .header {
        padding: 24px 16px;
      }

      .header h1 {
        font-size: 22px;
      }

      .content {
        padding: 24px 16px;
      }
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>🔑 Keyer QRS2HST</h1>
      <p>WiFi Configuration</p>
    </div>

    <div class="content">
      <div id="status" class="status"></div>

      <form id="wifiForm">
        <div class="form-group">
          <label for="ssid">Network Name (SSID)</label>
          <input
            type="text"
            id="ssid"
            name="ssid"
            placeholder="Enter WiFi network name"
            required
            maxlength="32"
            autocomplete="off"
          />
          <div class="help-text">Enter the exact name of your WiFi network</div>
        </div>

        <div class="form-group">
          <label for="password">Password</label>
          <div class="input-group">
            <input
              type="password"
              id="password"
              name="password"
              placeholder="Enter WiFi password"
              maxlength="63"
              autocomplete="off"
            />
            <button type="button" class="toggle-password" onclick="togglePassword()">
              👁️
            </button>
          </div>
          <div class="help-text">Leave empty for open networks</div>
        </div>

        <button type="submit" class="btn" id="submitBtn">
          Save & Connect
        </button>
      </form>
    </div>
  </div>

  <script>
    // Form elements
    const form = document.getElementById('wifiForm');
    const ssidInput = document.getElementById('ssid');
    const passwordInput = document.getElementById('password');
    const submitBtn = document.getElementById('submitBtn');
    const statusDiv = document.getElementById('status');

    // Toggle password visibility
    function togglePassword() {
      const type = passwordInput.type === 'password' ? 'text' : 'password';
      passwordInput.type = type;
    }

    // Show status message
    function showStatus(message, type = 'info') {
      statusDiv.textContent = message;
      statusDiv.className = 'status ' + type;
      statusDiv.style.display = 'block';
    }

    // Hide status message
    function hideStatus() {
      statusDiv.style.display = 'none';
    }

    // Form validation
    function validateForm() {
      const ssid = ssidInput.value.trim();

      if (ssid.length === 0) {
        showStatus('Please enter a network name', 'error');
        return false;
      }

      if (ssid.length > 32) {
        showStatus('Network name too long (max 32 characters)', 'error');
        return false;
      }

      const password = passwordInput.value;
      if (password.length > 63) {
        showStatus('Password too long (max 63 characters)', 'error');
        return false;
      }

      return true;
    }

    // Form submission
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      hideStatus();

      if (!validateForm()) {
        return;
      }

      const ssid = ssidInput.value.trim();
      const password = passwordInput.value;

      // Disable form
      submitBtn.disabled = true;
      submitBtn.innerHTML = '<span class="spinner"></span> Saving...';

      try {
        const response = await fetch('/api/wifi/configure', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            sta_ssid: ssid,
            sta_password: password
          })
        });

        const data = await response.json();

        if (response.ok) {
          showStatus('✓ Configuration saved! Device will reboot and connect...', 'success');

          // Countdown timer
          let countdown = 3;
          const countdownInterval = setInterval(() => {
            countdown--;
            if (countdown > 0) {
              showStatus(`✓ Configuration saved! Rebooting in ${countdown} seconds...`, 'success');
            } else {
              clearInterval(countdownInterval);
              showStatus('✓ Device rebooting now. Please reconnect to your WiFi network.', 'info');
            }
          }, 1000);
        } else {
          showStatus('✗ Error: ' + (data.message || 'Failed to save configuration'), 'error');
          submitBtn.disabled = false;
          submitBtn.textContent = 'Save & Connect';
        }
      } catch (error) {
        showStatus('✗ Connection error. Please try again.', 'error');
        submitBtn.disabled = false;
        submitBtn.textContent = 'Save & Connect';
      }
    });

    // Enable submit button on input
    ssidInput.addEventListener('input', () => {
      hideStatus();
    });

    passwordInput.addEventListener('input', () => {
      hideStatus();
    });
  </script>
</body>
</html>
)HTMLPAGE";

/**
 * @brief Get setup page HTML.
 * @return Pointer to HTML string.
 */
const char* GetSetupPageHtml() {
  return kSetupPageHtml;
}

}  // namespace captive_portal
