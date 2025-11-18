#pragma once

/**
 * @file wifi_subsystem.hpp
 * @brief WiFi subsystem with STA + AP fallback modes
 *
 * Manages WiFi connectivity with two operational modes:
 * - Station (STA) mode: Connect to existing WiFi network
 * - Access Point (AP) mode: Fallback when STA fails or is not configured
 *
 * ARCHITECTURE RATIONALE:
 * - STA mode prioritized for normal operation (lowest power, better range)
 * - AP mode provides configuration access when STA unavailable
 * - Automatic fallback on connection timeout or credential failure
 * - Event-driven state machine for robust connection management
 * - NeoPixel animation signals WiFi readiness (per PRD requirement)
 *
 * USAGE:
 * 1. Initialize() with WiFiConfig from NVS
 * 2. Tick() in main loop for connection monitoring
 * 3. Query status via GetStatus()
 * 4. Reconfigure via SetConfig() + Reconnect()
 *
 * THREAD SAFETY:
 * - Initialize/Deinitialize: Must be called from main task
 * - Tick: Must be called from main loop (not ISR-safe)
 * - GetStatus: Thread-safe (atomic state access)
 * - SetConfig: Not thread-safe (requires external synchronization)
 */

#include <cstdint>
#include <atomic>
#include <vector>

extern "C" {
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
}

#include "config/device_config.hpp"

// Forward declaration to avoid circular dependency
namespace diagnostics_subsystem {
class DiagnosticsSubsystem;
}

namespace wifi_subsystem {

/**
 * @brief WiFi scan result entry
 */
struct WifiScanResult {
  char ssid[33];              ///< Network SSID (null-terminated, max 32 chars + null)
  int8_t rssi;                ///< Signal strength in dBm (e.g., -45 = strong, -80 = weak)
  wifi_auth_mode_t authmode;  ///< Authentication mode (OPEN, WPA2-PSK, etc.)
  uint8_t channel;            ///< WiFi channel (1-14 for 2.4GHz)
};

/**
 * @brief WiFi subsystem operational modes
 */
enum class WiFiMode : uint8_t {
  kIdle = 0,       ///< WiFi not initialized
  kStaConnecting,  ///< Attempting STA connection
  kStaConnected,   ///< STA mode active, connected to AP
  kApActive,       ///< AP mode active (fallback or primary)
};

/**
 * @brief WiFi connection status for monitoring
 */
struct WiFiStatus {
  WiFiMode mode = WiFiMode::kIdle;
  char ip_address[16] = "";   ///< Current IP address (empty if not connected)
  int8_t rssi_dbm = 0;        ///< Signal strength in dBm (STA mode only)
  uint16_t clients = 0;       ///< Number of connected clients (AP mode only)
  bool ready = false;         ///< True when network stack is ready for HTTP server
};

/**
 * @brief WiFi subsystem manages network connectivity
 *
 * Encapsulates ESP-IDF WiFi stack with simplified configuration and status API.
 * Handles automatic mode transitions: STA → AP fallback → STA retry.
 */
class WiFiSubsystem {
 public:
  WiFiSubsystem() = default;
  ~WiFiSubsystem();

  WiFiSubsystem(const WiFiSubsystem&) = delete;
  WiFiSubsystem& operator=(const WiFiSubsystem&) = delete;

  /**
   * @brief Initialize WiFi subsystem with configuration
   *
   * @param config WiFi configuration from NVS
   * @param diagnostics Optional pointer to DiagnosticsSubsystem for LED animation (Task 1.7)
   * @return ESP_OK on success, error code otherwise
   *
   * Side effects:
   * - Initializes ESP-IDF netif, event loop, WiFi driver
   * - Registers event handlers for connection/disconnection
   * - Starts WiFi in STA or AP mode based on config
   *
   * Error conditions:
   * - ESP_ERR_INVALID_STATE: Already initialized
   * - ESP_ERR_NO_MEM: Insufficient heap for WiFi stack
   * - ESP_FAIL: WiFi driver init failed
   */
  esp_err_t Initialize(const config::WiFiConfig& config,
                       diagnostics_subsystem::DiagnosticsSubsystem* diagnostics = nullptr);

  /**
   * @brief Deinitialize WiFi subsystem and release resources
   *
   * Stops WiFi, unregisters event handlers, deinitializes driver.
   * Safe to call multiple times (idempotent).
   */
  void Deinitialize();

  /**
   * @brief Tick function for main loop (monitors connection state)
   *
   * Call periodically (e.g., every 100ms) to:
   * - Check STA connection timeout
   * - Trigger AP fallback if needed
   * - Update status cache
   *
   * @param now_ms Current time in milliseconds (monotonic)
   */
  void Tick(uint32_t now_ms);

  /**
   * @brief Get current WiFi status (thread-safe)
   *
   * @return WiFiStatus structure with current mode, IP, RSSI, etc.
   */
  WiFiStatus GetStatus() const;

  /**
   * @brief Update WiFi configuration (does not reconnect)
   *
   * Call Reconnect() after SetConfig() to apply changes.
   *
   * @param config New WiFi configuration
   */
  void SetConfig(const config::WiFiConfig& config);

  /**
   * @brief Disconnect and reconnect with current configuration
   *
   * Forces STA connection attempt even if AP mode is active.
   * Useful after SetConfig() or manual reconnect request.
   *
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t Reconnect();

  /**
   * @brief Check if WiFi is initialized
   *
   * @return true if Initialize() succeeded and Deinitialize() not called yet
   */
  bool IsInitialized() const { return initialized_; }

  /**
   * @brief Scan for available WiFi networks
   *
   * Performs a blocking WiFi scan (blocks for 3-5 seconds).
   * Returns networks sorted by RSSI (strongest first).
   * Filters out duplicate SSIDs (keeps strongest signal) and 5GHz networks.
   *
   * @param results Output vector of scan results (cleared before filling)
   * @param max_results Maximum number of results to return (default: 20)
   * @return ESP_OK on success, ESP_ERR_INVALID_STATE if scan already in progress,
   *         ESP_ERR_TIMEOUT if scan times out (>5 seconds)
   *
   * @note This method is NOT thread-safe. Do not call concurrently.
   * @note Scan timeout is 5 seconds. If no results after 5s, returns empty vector.
   */
  esp_err_t ScanNetworks(std::vector<WifiScanResult>& results, uint16_t max_results = 20);

  /**
   * @brief Get number of clients connected to AP
   *
   * Only valid when WiFi is in AP mode. Returns 0 in STA mode.
   *
   * @return Number of connected clients (0-4)
   */
  uint8_t GetConnectedClientCount() const;

 private:
  /**
   * @brief Start WiFi in Station mode (connect to existing network)
   *
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t StartStaMode();

  /**
   * @brief Start WiFi in Access Point mode (create network)
   *
   * @return ESP_OK on success, error code otherwise
   */
  esp_err_t StartApMode();

  /**
   * @brief Stop WiFi and disconnect
   */
  void StopWiFi();

  /**
   * @brief Event handler for WiFi events (connection/disconnection)
   *
   * @param arg Pointer to WiFiSubsystem instance
   * @param event_base Event base (WIFI_EVENT or IP_EVENT)
   * @param event_id Event ID (e.g., WIFI_EVENT_STA_CONNECTED)
   * @param event_data Event data payload
   */
  static void EventHandler(void* arg, esp_event_base_t event_base,
                           int32_t event_id, void* event_data);

  /**
   * @brief Transition to AP fallback mode
   *
   * Called when STA connection times out or fails.
   */
  void FallbackToAp();

  config::WiFiConfig config_{};
  std::atomic<WiFiMode> mode_{WiFiMode::kIdle};
  std::atomic<bool> ready_{false};
  bool initialized_ = false;

  // Diagnostics subsystem for LED animation on WiFi events (Task 1.7)
  diagnostics_subsystem::DiagnosticsSubsystem* diagnostics_ = nullptr;
  uint32_t sta_connect_start_ms_ = 0;
  uint32_t sta_retry_count_ = 0;

  esp_netif_t* netif_sta_ = nullptr;
  esp_netif_t* netif_ap_ = nullptr;
  esp_event_handler_instance_t wifi_event_handler_ = nullptr;
  esp_event_handler_instance_t ip_event_handler_ = nullptr;

  // WiFi scan state tracking
  std::atomic<bool> scan_in_progress_{false};  ///< True if scan is currently running
  uint32_t last_client_check_ms_ = 0;          ///< Last time client count was checked
  std::atomic<uint8_t> connected_clients_{0};  ///< Cached client count (updated every 5s)
};

}  // namespace wifi_subsystem
