/**
 * @file captive_portal_manager.hpp
 * @brief Captive portal manager that coordinates DNS and HTTP servers.
 * @details
 *  This file defines the main controller for the captive portal feature.
 *  The manager coordinates the lifecycle of the DNS server and minimal HTTP
 *  server based on WiFi mode and connected client count.
 *
 *  Activation Logic:
 *  - Enable() when: WiFi in AP mode AND at least one client connected
 *  - Disable() when: WiFi in STA mode OR no clients connected
 *
 *  The manager is initialized during the CaptivePortalPhase of the
 *  application initialization pipeline.
 *
 * @author Simone Fabris
 * @date 2025-11-15
 * @version 1.0
 */

#pragma once

namespace wifi_subsystem {
class WiFiSubsystem;  // Forward declaration
}

namespace config {
class DeviceConfig;  // Forward declaration
class Storage;       // Forward declaration (ConfigStorage is actually called Storage)
}

namespace diagnostics_subsystem {
class StatusLED;      // Forward declaration
}

namespace ui {
class HttpServer;  // Forward declaration
}

namespace captive_portal {

class DnsServer;
class MinimalHttpServer;

/**
 * @brief Captive portal manager that coordinates DNS and HTTP servers.
 *
 * This class manages the captive portal lifecycle, enabling/disabling
 * the DNS and HTTP servers based on WiFi mode and client connection status.
 */
class CaptivePortalManager {
 public:
  /**
   * @brief  Constructs a captive portal manager instance.
   * @param  wifi_subsystem Pointer to WiFi subsystem (for mode detection and scanning).
   * @param  device_config Pointer to device configuration (for WiFi credentials).
   * @param  config_storage Pointer to config storage (for NVS persistence).
   * @param  status_led Pointer to status LED (for rainbow pattern indication).
   * @param  main_http_server Pointer to main HTTP server (to stop/start when captive portal activates).
   */
  CaptivePortalManager(wifi_subsystem::WiFiSubsystem* wifi_subsystem,
                       config::DeviceConfig* device_config,
                       config::Storage* config_storage,
                       diagnostics_subsystem::StatusLED* status_led = nullptr,
                       ui::HttpServer* main_http_server = nullptr);

  /**
   * @brief  Destructor, ensures captive portal is disabled.
   */
  ~CaptivePortalManager();

  /**
   * @brief  Initializes the captive portal (creates DNS and HTTP server instances).
   * @return True if initialization succeeded, false otherwise.
   * @note   Does NOT start the servers. Call Enable() to activate.
   */
  bool Initialize();

  /**
   * @brief  Enables the captive portal (starts DNS and HTTP servers).
   * @return True if both servers started successfully, false otherwise.
   * @note   Should be called when WiFi is in AP mode with clients connected.
   */
  bool Enable();

  /**
   * @brief  Disables the captive portal (stops DNS and HTTP servers).
   * @return True if both servers stopped successfully, false otherwise.
   * @note   Should be called when WiFi enters STA mode or all clients disconnect.
   */
  bool Disable();

  /**
   * @brief  Checks if the captive portal is currently active.
   * @return True if DNS and HTTP servers are running, false otherwise.
   */
  bool IsActive() const;

 private:
  wifi_subsystem::WiFiSubsystem* wifi_subsystem_;      ///< WiFi subsystem for mode detection.
  config::DeviceConfig* device_config_;                ///< Device configuration for WiFi credentials.
  config::Storage* config_storage_;                    ///< Config storage for NVS persistence.
  diagnostics_subsystem::StatusLED* status_led_;       ///< Status LED for rainbow pattern.
  ui::HttpServer* main_http_server_;                   ///< Main HTTP server (to stop/start when captive portal activates).

  DnsServer* dns_server_;                              ///< DNS server instance (nullptr if not initialized).
  MinimalHttpServer* http_server_;                     ///< HTTP server instance (nullptr if not initialized).
  bool initialized_;                                   ///< True if Initialize() succeeded.
  bool active_;                                        ///< True if Enable() succeeded and servers are running.
  bool main_server_was_running_;                       ///< True if main HTTP server was running before captive portal enabled.
};

}  // namespace captive_portal
