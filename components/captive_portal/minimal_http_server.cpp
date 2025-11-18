/**
 * @file minimal_http_server.cpp
 * @brief Implementation of minimal HTTP server for captive portal.
 */

#include "captive_portal/minimal_http_server.hpp"

#include "config/device_config.hpp"
#include "wifi_subsystem/wifi_subsystem.hpp"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include <cstring>

namespace captive_portal {

// Forward declaration for GetSetupPageHtml (from setup_page.cpp)
extern const char* GetSetupPageHtml();

namespace {
constexpr const char* kTag = "MinimalHttpServer";

// Reboot delay after saving WiFi configuration (2000ms = 2 seconds)
constexpr uint32_t kRebootDelayMs = 2000;

// FreeRTOS timer for scheduled reboot
TimerHandle_t g_reboot_timer = nullptr;

/**
 * @brief FreeRTOS timer callback to reboot the device.
 */
void RebootTimerCallback(TimerHandle_t timer) {
  ESP_LOGI(kTag, "Reboot timer expired, restarting device...");
  esp_restart();
}

}  // namespace

MinimalHttpServer::MinimalHttpServer(wifi_subsystem::WiFiSubsystem* wifi_subsystem,
                                     config::DeviceConfig* device_config,
                                     config::Storage* config_storage)
    : wifi_subsystem_(wifi_subsystem),
      device_config_(device_config),
      config_storage_(config_storage),
      server_handle_(nullptr) {
  ESP_LOGI(kTag, "MinimalHttpServer constructed");
}

MinimalHttpServer::~MinimalHttpServer() {
  Stop();
  ESP_LOGI(kTag, "MinimalHttpServer destroyed");
}

bool MinimalHttpServer::Start() {
  if (server_handle_ != nullptr) {
    ESP_LOGW(kTag, "HTTP server already running");
    return true;
  }

  // Configure HTTP server
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kHttpPort;
  config.ctrl_port = 32768;  // Control port for internal use
  config.max_open_sockets = 4;  // Limit to 4 concurrent connections (AP mode)
  config.max_uri_handlers = 16;  // Increase from default 8 to support 11 handlers
  config.lru_purge_enable = true;  // Enable LRU purge for old connections

  // Start HTTP server
  esp_err_t err = httpd_start(&server_handle_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Failed to start HTTP server: %s", esp_err_to_name(err));
    return false;
  }

  // Register all handlers
  if (!RegisterHandlers()) {
    ESP_LOGE(kTag, "Failed to register HTTP handlers");
    httpd_stop(server_handle_);
    server_handle_ = nullptr;
    return false;
  }

  ESP_LOGI(kTag, "HTTP server started on port %d", kHttpPort);
  return true;
}

bool MinimalHttpServer::Stop() {
  if (server_handle_ == nullptr) {
    return true;
  }

  esp_err_t err = httpd_stop(server_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Failed to stop HTTP server: %s", esp_err_to_name(err));
    return false;
  }

  server_handle_ = nullptr;
  ESP_LOGI(kTag, "HTTP server stopped");
  return true;
}

bool MinimalHttpServer::IsRunning() const {
  return server_handle_ != nullptr;
}

bool MinimalHttpServer::RegisterHandlers() {
  // Captive portal detection endpoints
  httpd_uri_t apple_detect = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = HandleAppleDetection,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &apple_detect);

  httpd_uri_t apple_alt = {
      .uri = "/library/test/success.html",
      .method = HTTP_GET,
      .handler = HandleAppleAlt,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &apple_alt);

  httpd_uri_t android_detect = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = HandleAndroidDetection,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &android_detect);

  httpd_uri_t android_alt = {
      .uri = "/gen_204",
      .method = HTTP_GET,
      .handler = HandleAndroidAlt,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &android_alt);

  httpd_uri_t windows_detect = {
      .uri = "/connecttest.txt",
      .method = HTTP_GET,
      .handler = HandleWindowsDetection,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &windows_detect);

  httpd_uri_t windows_ncsi = {
      .uri = "/ncsi.txt",
      .method = HTTP_GET,
      .handler = HandleWindowsNcsi,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &windows_ncsi);

  httpd_uri_t firefox_detect = {
      .uri = "/success.txt",
      .method = HTTP_GET,
      .handler = HandleFirefoxDetection,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &firefox_detect);

  // Root (/) serves setup page directly
  httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = HandleSetupPage,  // Serve setup page directly
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &root);

  // Setup page and WiFi APIs
  httpd_uri_t setup_page = {
      .uri = "/setup",
      .method = HTTP_GET,
      .handler = HandleSetupPage,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &setup_page);

  httpd_uri_t wifi_scan = {
      .uri = "/api/wifi/scan",
      .method = HTTP_GET,
      .handler = HandleWifiScan,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &wifi_scan);

  httpd_uri_t wifi_configure = {
      .uri = "/api/wifi/configure",
      .method = HTTP_POST,
      .handler = HandleWifiConfigure,
      .user_ctx = this
  };
  httpd_register_uri_handler(server_handle_, &wifi_configure);

  ESP_LOGI(kTag, "Registered 11 HTTP handlers (7 detection + 1 root + 1 setup + 2 API)");
  return true;
}

MinimalHttpServer* MinimalHttpServer::GetInstance(httpd_req_t* req) {
  return static_cast<MinimalHttpServer*>(req->user_ctx);
}

// ===== Captive Portal Detection Endpoints =====

esp_err_t MinimalHttpServer::HandleAppleDetection(httpd_req_t* req) {
  ESP_LOGI(kTag, "Apple detection hit: %s", req->uri);
  const char* response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleAppleAlt(httpd_req_t* req) {
  ESP_LOGI(kTag, "Apple alt detection hit: %s", req->uri);
  const char* response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleAndroidDetection(httpd_req_t* req) {
  ESP_LOGI(kTag, "Android detection hit: %s", req->uri);
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, nullptr, 0);  // Empty body
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleAndroidAlt(httpd_req_t* req) {
  ESP_LOGI(kTag, "Android alt detection hit: %s", req->uri);
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, nullptr, 0);  // Empty body
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleWindowsDetection(httpd_req_t* req) {
  ESP_LOGI(kTag, "Windows detection hit: %s", req->uri);
  const char* response = "Microsoft Connect Test";
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleWindowsNcsi(httpd_req_t* req) {
  ESP_LOGI(kTag, "Windows NCSI hit: %s", req->uri);
  const char* response = "Microsoft NCSI";
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleFirefoxDetection(httpd_req_t* req) {
  ESP_LOGI(kTag, "Firefox detection hit: %s", req->uri);
  const char* response = "success";
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, response, strlen(response));
  return ESP_OK;
}

// ===== Setup Page and WiFi APIs =====

esp_err_t MinimalHttpServer::HandleSetupPage(httpd_req_t* req) {
  ESP_LOGI(kTag, "Serving setup page");
  const char* html = GetSetupPageHtml();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html, strlen(html));
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleWifiScan(httpd_req_t* req) {
  ESP_LOGI(kTag, "WiFi scan API called");

  MinimalHttpServer* server = GetInstance(req);
  if (server == nullptr || server->wifi_subsystem_ == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server error");
    return ESP_FAIL;
  }

  // Perform WiFi scan
  uint64_t scan_start_us = esp_timer_get_time();
  std::vector<wifi_subsystem::WifiScanResult> results;
  esp_err_t err = server->wifi_subsystem_->ScanNetworks(results, 20);  // Max 20 networks

  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WiFi scan failed: %s", esp_err_to_name(err));
    const char* error_response = "{\"status\": \"error\", \"message\": \"Scan failed\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;  // Return OK to HTTP server (error is in JSON)
  }

  uint64_t scan_time_ms = (esp_timer_get_time() - scan_start_us) / 1000;

  // Build JSON response
  cJSON* root = cJSON_CreateObject();
  cJSON* networks_array = cJSON_CreateArray();

  for (const auto& result : results) {
    cJSON* network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "ssid", result.ssid);
    cJSON_AddNumberToObject(network, "rssi", result.rssi);
    cJSON_AddNumberToObject(network, "channel", result.channel);

    // Convert authmode enum to string
    const char* authmode_str = "UNKNOWN";
    switch (result.authmode) {
      case WIFI_AUTH_OPEN:
        authmode_str = "OPEN";
        break;
      case WIFI_AUTH_WEP:
        authmode_str = "WEP";
        break;
      case WIFI_AUTH_WPA_PSK:
        authmode_str = "WPA-PSK";
        break;
      case WIFI_AUTH_WPA2_PSK:
        authmode_str = "WPA2-PSK";
        break;
      case WIFI_AUTH_WPA_WPA2_PSK:
        authmode_str = "WPA/WPA2-PSK";
        break;
      case WIFI_AUTH_WPA3_PSK:
        authmode_str = "WPA3-PSK";
        break;
      case WIFI_AUTH_WPA2_WPA3_PSK:
        authmode_str = "WPA2/WPA3-PSK";
        break;
      default:
        authmode_str = "UNKNOWN";
        break;
    }
    cJSON_AddStringToObject(network, "authmode", authmode_str);

    cJSON_AddItemToArray(networks_array, network);
  }

  cJSON_AddItemToObject(root, "networks", networks_array);
  cJSON_AddNumberToObject(root, "scan_time_ms", scan_time_ms);

  char* json_string = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (json_string == nullptr) {
    ESP_LOGE(kTag, "Failed to serialize JSON");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "WiFi scan complete: %d networks, %llu ms", results.size(), scan_time_ms);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_string, strlen(json_string));

  cJSON_free(json_string);
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleWifiConfigure(httpd_req_t* req) {
  ESP_LOGI(kTag, "WiFi configure API called");

  MinimalHttpServer* server = GetInstance(req);
  if (server == nullptr || server->device_config_ == nullptr || server->config_storage_ == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server error");
    return ESP_FAIL;
  }

  // Read POST body (max 512 bytes)
  char content[512];
  int received = httpd_req_recv(req, content, sizeof(content) - 1);
  if (received <= 0) {
    const char* error_response = "{\"status\": \"error\", \"message\": \"No data received\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }
  content[received] = '\0';

  ESP_LOGI(kTag, "Received configuration JSON (%d bytes)", received);

  // Parse JSON body
  cJSON* root = cJSON_Parse(content);
  if (root == nullptr) {
    const char* error_response = "{\"status\": \"error\", \"message\": \"Invalid JSON\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  // Extract SSID and password
  cJSON* ssid_item = cJSON_GetObjectItem(root, "sta_ssid");
  cJSON* password_item = cJSON_GetObjectItem(root, "sta_password");

  if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == nullptr) {
    cJSON_Delete(root);
    const char* error_response = "{\"status\": \"error\", \"message\": \"Missing or invalid sta_ssid\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  const char* ssid = ssid_item->valuestring;
  const char* password = cJSON_IsString(password_item) ? password_item->valuestring : "";

  // Validate SSID length (1-32 characters)
  size_t ssid_len = strlen(ssid);
  if (ssid_len == 0 || ssid_len > 32) {
    cJSON_Delete(root);
    const char* error_response = "{\"status\": \"error\", \"message\": \"SSID must be 1-32 characters\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  // Validate password length (0-63 characters)
  size_t password_len = strlen(password);
  if (password_len > 63) {
    cJSON_Delete(root);
    const char* error_response = "{\"status\": \"error\", \"message\": \"Password must be 0-63 characters\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  ESP_LOGI(kTag, "Configuration validated: SSID='%s', password_len=%d", ssid, password_len);

  // Update device configuration
  strncpy(server->device_config_->wifi.sta_ssid, ssid, sizeof(server->device_config_->wifi.sta_ssid) - 1);
  server->device_config_->wifi.sta_ssid[sizeof(server->device_config_->wifi.sta_ssid) - 1] = '\0';

  strncpy(server->device_config_->wifi.sta_password, password, sizeof(server->device_config_->wifi.sta_password) - 1);
  server->device_config_->wifi.sta_password[sizeof(server->device_config_->wifi.sta_password) - 1] = '\0';

  cJSON_Delete(root);

  // Save configuration to NVS
  esp_err_t err = server->config_storage_->Save(*server->device_config_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Failed to save configuration to NVS: %s", esp_err_to_name(err));
    const char* error_response = "{\"status\": \"error\", \"message\": \"Failed to save configuration\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  ESP_LOGI(kTag, "Configuration saved to NVS successfully");

  // Schedule reboot using FreeRTOS timer
  if (g_reboot_timer == nullptr) {
    g_reboot_timer = xTimerCreate(
        "RebootTimer",                          // Timer name
        pdMS_TO_TICKS(kRebootDelayMs),          // Period (2000ms)
        pdFALSE,                                // One-shot timer (not auto-reload)
        nullptr,                                // Timer ID (not used)
        RebootTimerCallback                     // Callback function
    );
  }

  if (g_reboot_timer == nullptr) {
    ESP_LOGE(kTag, "Failed to create reboot timer");
    const char* error_response = "{\"status\": \"error\", \"message\": \"Failed to schedule reboot\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  // Start the timer
  if (xTimerStart(g_reboot_timer, 0) != pdPASS) {
    ESP_LOGE(kTag, "Failed to start reboot timer");
    const char* error_response = "{\"status\": \"error\", \"message\": \"Failed to schedule reboot\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, error_response, strlen(error_response));
    return ESP_OK;
  }

  ESP_LOGI(kTag, "Reboot scheduled in %u ms", kRebootDelayMs);

  // Build success response
  cJSON* response = cJSON_CreateObject();
  cJSON_AddStringToObject(response, "status", "success");
  cJSON_AddStringToObject(response, "message", "Configuration saved. Rebooting...");
  cJSON_AddNumberToObject(response, "reboot_delay_ms", kRebootDelayMs);

  char* json_string = cJSON_PrintUnformatted(response);
  cJSON_Delete(response);

  if (json_string == nullptr) {
    const char* fallback_response = "{\"status\": \"success\", \"message\": \"Configuration saved. Rebooting...\", \"reboot_delay_ms\": 2000}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, fallback_response, strlen(fallback_response));
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_string, strlen(json_string));

  cJSON_free(json_string);
  return ESP_OK;
}

esp_err_t MinimalHttpServer::HandleWildcardRedirect(httpd_req_t* req) {
  // Skip redirect for known endpoints (already handled above)
  const char* uri = req->uri;
  if (strstr(uri, "/hotspot-detect") || strstr(uri, "/generate_204") ||
      strstr(uri, "/connecttest") || strstr(uri, "/success.txt") ||
      strstr(uri, "/setup") || strstr(uri, "/api/")) {
    // Already handled by specific handlers, no redirect needed
    return ESP_OK;
  }

  ESP_LOGI(kTag, "Wildcard redirect: %s -> /setup", uri);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
  httpd_resp_send(req, nullptr, 0);  // Empty body for redirect
  return ESP_OK;
}

}  // namespace captive_portal
