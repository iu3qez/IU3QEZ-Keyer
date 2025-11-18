#pragma once

/**
 * @file http_server.hpp
 * @brief HTTP server for Web UI configuration interface
 *
 * Provides REST API and static web pages for device configuration.
 * Runs on port 80 when WiFi is ready (STA or AP mode).
 *
 * ARCHITECTURE RATIONALE:
 * - Primary configuration interface (console eliminated per PRD)
 * - RESTful API using ParameterRegistry for dynamic config
 * - WebSocket for real-time status updates
 * - Static assets embedded in flash (SPIFFS or string literals)
 *
 * ENDPOINTS (Task 5.4.2 - REST API, Task 5.4.3 - Web UI):
 * - GET  /                            -> Complete configuration page (HTML)
 * - GET  /api/status                  -> WiFi/system status (mode, IP, RSSI, keying)
 * - GET  /api/config/schema           -> Parameter schema with widget hints (JSON)
 * - GET  /api/config                  -> Current config (all or filtered by subsystem)
 * - POST /api/parameter               -> Update parameter (body: {"param":"name","value":"..."})
 * - POST /api/config/save             -> Persist config to NVS
 *
 * THREAD SAFETY:
 * - Initialize/Deinitialize: Must be called from main task
 * - HTTP handlers: Run in httpd context (thread-safe if using locks)
 * - Access to DeviceConfig requires external synchronization
 *
 * USAGE:
 * 1. Initialize() after WiFi is ready
 * 2. Server runs until Deinitialize() or WiFi disconnects
 * 3. Access via http://<device-ip>/ or http://192.168.4.1/ (AP mode)
 */


#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" {
#include "esp_err.h"
#include "esp_http_server.h"
}

#include "config/device_config.hpp"
#include "config/parameter_registry.hpp"
#include "wifi_subsystem/wifi_subsystem.hpp"

namespace app {
class ApplicationController;
}  // namespace app

namespace ui {

/**
 * @brief HTTP server for Web UI
 *
 * Manages ESP-IDF http_server instance with handlers for configuration
 * and status endpoints.
 */
class HttpServer {
 public:
  HttpServer() = default;
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  /**
   * @brief Initialize and start HTTP server
   *
   * @param config Pointer to DeviceConfig (must remain valid)
   * @param wifi Pointer to WiFiSubsystem (must remain valid)
   * @param storage Pointer to Storage (must remain valid)
   * @param param_registry Pointer to ParameterRegistry (must remain valid)
   * @param app_controller Pointer to ApplicationController for hot-reload (must remain valid)
   * @return ESP_OK on success, error code otherwise
   *
   * Prerequisites:
   * - WiFi must be initialized and ready (STA connected or AP active)
   * - Pointers must remain valid for server lifetime
   *
   * Side effects:
   * - Starts HTTP server on port 80
   * - Registers URI handlers
   */
  esp_err_t Initialize(config::DeviceConfig* config,
                       wifi_subsystem::WiFiSubsystem* wifi,
                       config::Storage* storage,
                       config::ParameterRegistry* param_registry,
                       app::ApplicationController* app_controller);

  /**
   * @brief Stop HTTP server and release resources
   *
   * Safe to call multiple times (idempotent).
   */
  void Deinitialize();

  /**
   * @brief Stop HTTP server temporarily (for port 80 handoff to captive portal)
   *
   * Stops the httpd server but retains context for quick restart.
   * Use Start() to resume server with same configuration.
   *
   * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
   */
  esp_err_t Stop();

  /**
   * @brief Restart HTTP server (after Stop() call)
   *
   * Restarts httpd server using retained context from previous Initialize().
   *
   * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not previously initialized
   */
  esp_err_t Start();

  /**
   * @brief Check if server is running
   *
   * @return true if Initialize() succeeded and server is active
   */
  bool IsRunning() const { return server_ != nullptr; }

  /**
   * @brief Send JSON response (helper for handlers)
   *
   * @param req HTTP request handle
   * @param json JSON string (null-terminated)
   * @return ESP_OK on success
   */
  static esp_err_t SendJson(httpd_req_t* req, const char* json);
  static esp_err_t SendAsset(httpd_req_t* req, std::string_view asset_path);

  /**
   * @brief Send error JSON response (helper for handlers)
   *
   * @param req HTTP request handle
   * @param status_code HTTP status code
   * @param message Error message
   * @return ESP_OK on success
   */
  static esp_err_t SendError(httpd_req_t* req, int status_code, const char* message);

 private:
  /**
   * @brief Handler context passed to HTTP request handlers
   */
  static constexpr const char* kLogTag = "HttpServer";

  struct HandlerContext {
    config::DeviceConfig* config;
    wifi_subsystem::WiFiSubsystem* wifi;
    config::Storage* storage;
    config::ParameterRegistry* param_registry;
    app::ApplicationController* app_controller;
  };

  // URI handlers (static methods for C compatibility)
  static esp_err_t HandleRoot(httpd_req_t* req);
  static esp_err_t HandleGetConfigPage(httpd_req_t* req);
  static esp_err_t HandleGetStatus(httpd_req_t* req);
  static esp_err_t HandleGetSchema(httpd_req_t* req);
  static esp_err_t HandleGetConfig(httpd_req_t* req);
  static esp_err_t HandleGetTimeline(httpd_req_t* req);
  static esp_err_t HandleGetRemote(httpd_req_t* req);
  static esp_err_t HandleGetDecoder(httpd_req_t* req);
  static esp_err_t HandleGetSystem(httpd_req_t* req);
  static esp_err_t HandleGetAsset(httpd_req_t* req);
  static esp_err_t HandlePostParameter(httpd_req_t* req);
  static esp_err_t HandlePostSave(httpd_req_t* req);

  // Remote keying API endpoints
  static esp_err_t HandleGetRemoteStatus(httpd_req_t* req);
  static esp_err_t HandlePostClientStart(httpd_req_t* req);
  static esp_err_t HandlePostClientStop(httpd_req_t* req);
  static esp_err_t HandlePostServerStart(httpd_req_t* req);
  static esp_err_t HandlePostServerStop(httpd_req_t* req);

  // Decoder API endpoints
  static esp_err_t HandleGetDecoderStatus(httpd_req_t* req);
  static esp_err_t HandlePostDecoderEnable(httpd_req_t* req);

  // Timeline API endpoints
  static esp_err_t HandleGetTimelineEvents(httpd_req_t* req);
  static esp_err_t HandleGetTimelineConfig(httpd_req_t* req);

  // System Monitor API endpoints
  static esp_err_t HandleGetSystemStats(httpd_req_t* req);

  // Firmware update API endpoints
  static esp_err_t HandleGetFirmwarePage(httpd_req_t* req);
  static esp_err_t HandlePostEnterBootloader(httpd_req_t* req);

  // Text Keyer API endpoints
  static esp_err_t HandleGetKeyerPage(httpd_req_t* req);
  static esp_err_t HandleGetKeyerStatus(httpd_req_t* req);
  static esp_err_t HandlePostKeyerSend(httpd_req_t* req);
  static esp_err_t HandlePostKeyerMessage(httpd_req_t* req);
  static esp_err_t HandlePostKeyerAbort(httpd_req_t* req);

  // Helper: Extract query parameter from URI
  static bool GetQueryParam(httpd_req_t* req, const char* param_name,
                           char* out_value, size_t max_len);

  httpd_handle_t server_ = nullptr;
  HandlerContext context_{};
  std::vector<httpd_uri_t> asset_routes_;
  bool context_valid_ = false;  // True if Initialize() was called at least once
};

}  // namespace ui
