/**
 * @file minimal_http_server.hpp
 * @brief Minimal HTTP server for captive portal (dedicated, not reusing components/ui).
 * @details
 *  This file defines a lightweight HTTP server specifically for the captive portal.
 *  It handles only 9 endpoints:
 *  - Captive portal detection (iOS, Android, Windows, Firefox)
 *  - Setup page serving
 *  - WiFi scan and configuration APIs
 *  - Wildcard redirect to /setup
 *
 *  The server is active ONLY in AP mode and is completely isolated from the
 *  main Web UI (components/ui/http_server.cpp) to ensure zero conflicts.
 *
 * @author Simone Fabris
 * @date 2025-11-15
 * @version 1.0
 */

#pragma once

#include <esp_http_server.h>

namespace wifi_subsystem {
class WiFiSubsystem;  // Forward declaration
}

namespace config {
class DeviceConfig;  // Forward declaration
class Storage;       // Forward declaration (ConfigStorage is actually called Storage)
}

namespace captive_portal {

/**
 * @brief Minimal HTTP server for captive portal.
 *
 * This class implements a dedicated HTTP server for the captive portal,
 * completely separate from the main Web UI. It handles captive portal
 * detection endpoints, setup page serving, and WiFi configuration APIs.
 */
class MinimalHttpServer {
 public:
  /**
   * @brief  Constructs a minimal HTTP server instance.
   * @param  wifi_subsystem Pointer to WiFi subsystem (for scanning and client count).
   * @param  device_config Pointer to device configuration (for reading/writing WiFi credentials).
   * @param  config_storage Pointer to config storage (for persisting to NVS).
   */
  MinimalHttpServer(wifi_subsystem::WiFiSubsystem* wifi_subsystem,
                    config::DeviceConfig* device_config,
                    config::Storage* config_storage);

  /**
   * @brief  Destructor, ensures server is stopped.
   */
  ~MinimalHttpServer();

  /**
   * @brief  Starts the HTTP server on port 80.
   * @return True if the server started successfully, false otherwise.
   * @note   Registers all handlers (detection endpoints, setup page, WiFi APIs).
   */
  bool Start();

  /**
   * @brief  Stops the HTTP server and frees resources.
   * @return True if the server stopped successfully, false otherwise.
   */
  bool Stop();

  /**
   * @brief  Checks if the HTTP server is currently running.
   * @return True if running, false otherwise.
   */
  bool IsRunning() const;

 private:
  // ===== Captive Portal Detection Endpoints =====

  /**
   * @brief  Handler for Apple iOS/macOS detection: GET /hotspot-detect.html
   * @return ESP_OK on success.
   */
  static esp_err_t HandleAppleDetection(httpd_req_t* req);

  /**
   * @brief  Handler for Apple alternative endpoint: GET /library/test/success.html
   * @return ESP_OK on success.
   */
  static esp_err_t HandleAppleAlt(httpd_req_t* req);

  /**
   * @brief  Handler for Android detection: GET /generate_204
   * @return ESP_OK on success (HTTP 204 No Content).
   */
  static esp_err_t HandleAndroidDetection(httpd_req_t* req);

  /**
   * @brief  Handler for Android alternative endpoint: GET /gen_204
   * @return ESP_OK on success (HTTP 204 No Content).
   */
  static esp_err_t HandleAndroidAlt(httpd_req_t* req);

  /**
   * @brief  Handler for Windows detection: GET /connecttest.txt
   * @return ESP_OK on success.
   */
  static esp_err_t HandleWindowsDetection(httpd_req_t* req);

  /**
   * @brief  Handler for Windows NCSI: GET /ncsi.txt
   * @return ESP_OK on success.
   */
  static esp_err_t HandleWindowsNcsi(httpd_req_t* req);

  /**
   * @brief  Handler for Firefox detection: GET /success.txt
   * @return ESP_OK on success.
   */
  static esp_err_t HandleFirefoxDetection(httpd_req_t* req);

  // ===== Setup Page and WiFi APIs =====

  /**
   * @brief  Handler for setup page: GET /setup
   * @return ESP_OK on success (returns hardcoded HTML from setup_page.cpp).
   */
  static esp_err_t HandleSetupPage(httpd_req_t* req);

  /**
   * @brief  Handler for WiFi scan API: GET /api/wifi/scan
   * @return ESP_OK on success (returns JSON with network list).
   */
  static esp_err_t HandleWifiScan(httpd_req_t* req);

  /**
   * @brief  Handler for WiFi configuration API: POST /api/wifi/configure
   * @return ESP_OK on success (saves credentials to NVS, schedules reboot).
   */
  static esp_err_t HandleWifiConfigure(httpd_req_t* req);

  /**
   * @brief  Wildcard handler for unknown paths: redirect to /setup
   * @return ESP_OK on success (HTTP 302 redirect).
   */
  static esp_err_t HandleWildcardRedirect(httpd_req_t* req);

  // ===== Helper Methods =====

  /**
   * @brief  Retrieves MinimalHttpServer instance from HTTP request context.
   * @param  req HTTP request object.
   * @return Pointer to MinimalHttpServer instance, or nullptr if not found.
   */
  static MinimalHttpServer* GetInstance(httpd_req_t* req);

  /**
   * @brief  Registers all HTTP handlers with the ESP-IDF HTTP server.
   * @return True if all handlers registered successfully, false otherwise.
   */
  bool RegisterHandlers();

  wifi_subsystem::WiFiSubsystem* wifi_subsystem_;  ///< WiFi subsystem for scanning and client count.
  config::DeviceConfig* device_config_;            ///< Device configuration for WiFi credentials.
  config::Storage* config_storage_;                ///< Config storage for NVS persistence.
  httpd_handle_t server_handle_;                   ///< ESP-IDF HTTP server handle (nullptr if not started).

  static constexpr uint16_t kHttpPort = 80;  ///< HTTP server port number.
};

}  // namespace captive_portal
