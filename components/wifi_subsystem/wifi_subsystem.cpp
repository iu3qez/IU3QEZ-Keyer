#include "wifi_subsystem/wifi_subsystem.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

extern "C" {
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
}

#include "diagnostics_subsystem/diagnostics_subsystem.hpp"

namespace wifi_subsystem {

namespace {
constexpr char kLogTag[] = "WiFiSubsystem";

// STA connection timeout and retry configuration
constexpr uint8_t kMaxStaRetries = 3;  // Retry 3 times before AP fallback
constexpr uint32_t kStaRetryDelayMs = 5000;  // Wait 5s between retries

}  // namespace

WiFiSubsystem::~WiFiSubsystem() {
  Deinitialize();
}

esp_err_t WiFiSubsystem::Initialize(const config::WiFiConfig& config,
                                    diagnostics_subsystem::DiagnosticsSubsystem* diagnostics) {
  if (initialized_) {
    ESP_LOGW(kLogTag, "WiFi subsystem already initialized");
    return ESP_ERR_INVALID_STATE;
  }

  config_ = config;
  diagnostics_ = diagnostics;

  // DEBUG: Signal WiFi init phase 1 - Netif init (purple pulse)
  if (diagnostics_) {
    diagnostics_->SignalCheckpoint(128, 0, 128, 200);  // Purple
  }

  // Initialize ESP-IDF network interface
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kLogTag, "Failed to initialize netif: %s", esp_err_to_name(err));
    if (diagnostics_) {
      diagnostics_->SignalWifiError();  // Red flash
    }
    return err;
  }

  // Create default event loop (if not already created)
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kLogTag, "Failed to create event loop: %s", esp_err_to_name(err));
    return err;
  }

  // Create network interfaces for STA and AP
  netif_sta_ = esp_netif_create_default_wifi_sta();
  netif_ap_ = esp_netif_create_default_wifi_ap();

  // Initialize WiFi driver with default config
  wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&wifi_init_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to initialize WiFi driver: %s", esp_err_to_name(err));
    return err;
  }

  // Register event handlers
  err = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiSubsystem::EventHandler, this,
      &wifi_event_handler_);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to register WiFi event handler: %s",
             esp_err_to_name(err));
    esp_wifi_deinit();
    return err;
  }

  err = esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiSubsystem::EventHandler, this,
      &ip_event_handler_);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to register IP event handler: %s", esp_err_to_name(err));
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_event_handler_);
    esp_wifi_deinit();
    return err;
  }

  // Set storage mode to RAM only (config persisted via NVS separately)
  err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) {
    ESP_LOGW(kLogTag, "Failed to set WiFi storage mode: %s", esp_err_to_name(err));
  }

  initialized_ = true;
  ESP_LOGI(kLogTag, "WiFi subsystem initialized");

  // Start in STA mode if SSID configured, otherwise AP mode
  if (config_.sta_ssid[0] != '\0') {
    return StartStaMode();
  } else if (config_.enable_ap_fallback) {
    return StartApMode();
  }

  return ESP_OK;
}

void WiFiSubsystem::Deinitialize() {
  if (!initialized_) {
    return;
  }

  StopWiFi();

  if (wifi_event_handler_ != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_event_handler_);
    wifi_event_handler_ = nullptr;
  }
  if (ip_event_handler_ != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_event_handler_);
    ip_event_handler_ = nullptr;
  }

  esp_wifi_deinit();

  if (netif_sta_ != nullptr) {
    esp_netif_destroy(netif_sta_);
    netif_sta_ = nullptr;
  }
  if (netif_ap_ != nullptr) {
    esp_netif_destroy(netif_ap_);
    netif_ap_ = nullptr;
  }

  initialized_ = false;
  mode_.store(WiFiMode::kIdle, std::memory_order_release);
  ready_.store(false, std::memory_order_release);
  ESP_LOGI(kLogTag, "WiFi subsystem deinitialized");
}

esp_err_t WiFiSubsystem::StartStaMode() {
  ESP_LOGI(kLogTag, "Starting STA mode (SSID: %s)", config_.sta_ssid);

  // Configure WiFi mode
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to set STA mode: %s", esp_err_to_name(err));
    return err;
  }

  // Configure STA credentials
  wifi_config_t wifi_cfg = {};
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), config_.sta_ssid,
               sizeof(wifi_cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.sta.password), config_.sta_password,
               sizeof(wifi_cfg.sta.password) - 1);
  wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_cfg.sta.pmf_cfg.capable = true;
  wifi_cfg.sta.pmf_cfg.required = false;

  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to set STA config: %s", esp_err_to_name(err));
    return err;
  }

  // Start WiFi
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to start WiFi: %s", esp_err_to_name(err));
    return err;
  }

  mode_.store(WiFiMode::kStaConnecting, std::memory_order_release);
  sta_connect_start_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
  sta_retry_count_ = 0;

  ESP_LOGI(kLogTag, "STA mode started, connecting...");
  return ESP_OK;
}

esp_err_t WiFiSubsystem::StartApMode() {
  ESP_LOGI(kLogTag, "Starting AP mode (SSID: %s)", config_.ap_ssid);

  // Configure WiFi mode to AP only (ScanNetworks will temporarily switch to APSTA when needed)
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to set AP mode: %s", esp_err_to_name(err));
    return err;
  }

  // Configure AP settings
  wifi_config_t wifi_cfg = {};
  std::strncpy(reinterpret_cast<char*>(wifi_cfg.ap.ssid), config_.ap_ssid,
               sizeof(wifi_cfg.ap.ssid) - 1);
  wifi_cfg.ap.ssid_len = std::strlen(config_.ap_ssid);
  wifi_cfg.ap.channel = 1;
  wifi_cfg.ap.max_connection = 4;
  wifi_cfg.ap.beacon_interval = 100;

  // Set password if configured (open network if empty)
  if (config_.ap_password[0] != '\0') {
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.ap.password), config_.ap_password,
                 sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
  }

  err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to set AP config: %s", esp_err_to_name(err));
    return err;
  }

  // Start WiFi
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(kLogTag, "Failed to start WiFi: %s", esp_err_to_name(err));
    return err;
  }

  mode_.store(WiFiMode::kApActive, std::memory_order_release);
  ready_.store(true, std::memory_order_release);

  ESP_LOGI(kLogTag, "AP mode active, IP: 192.168.4.1");
  return ESP_OK;
}

void WiFiSubsystem::StopWiFi() {
  if (!initialized_) {
    return;
  }

  esp_wifi_disconnect();
  esp_wifi_stop();
  mode_.store(WiFiMode::kIdle, std::memory_order_release);
  ready_.store(false, std::memory_order_release);
}

void WiFiSubsystem::Tick(uint32_t now_ms) {
  if (!initialized_) {
    return;
  }

  const WiFiMode current_mode = mode_.load(std::memory_order_acquire);

  // Check STA connection timeout
  if (current_mode == WiFiMode::kStaConnecting) {
    const uint32_t elapsed_ms = now_ms - sta_connect_start_ms_;
    const uint32_t timeout_ms = config_.sta_timeout_sec * 1000;

    if (elapsed_ms >= timeout_ms) {
      ESP_LOGW(kLogTag, "STA connection timeout (%u sec), retry %u/%u",
               static_cast<unsigned>(config_.sta_timeout_sec),
               static_cast<unsigned>(sta_retry_count_ + 1),
               static_cast<unsigned>(kMaxStaRetries));

      sta_retry_count_++;
      if (sta_retry_count_ >= kMaxStaRetries) {
        // Max retries reached, fallback to AP
        ESP_LOGE(kLogTag, "STA connection failed after %u retries", static_cast<unsigned>(kMaxStaRetries));
        FallbackToAp();
      } else {
        // Retry connection
        sta_connect_start_ms_ = now_ms + kStaRetryDelayMs;
        esp_wifi_disconnect();
        esp_wifi_connect();
      }
    }
  }

  // Periodic client count monitoring in AP mode (every 5 seconds)
  if (current_mode == WiFiMode::kApActive) {
    constexpr uint32_t kClientCheckIntervalMs = 5000;  // 5 seconds

    if (now_ms - last_client_check_ms_ >= kClientCheckIntervalMs) {
      wifi_sta_list_t sta_list;
      memset(&sta_list, 0, sizeof(sta_list));

      esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
      if (err == ESP_OK) {
        uint8_t client_count = sta_list.num;
        connected_clients_.store(client_count, std::memory_order_release);

        if (client_count != connected_clients_.load(std::memory_order_relaxed)) {
          ESP_LOGI(kLogTag, "AP client count updated: %u", static_cast<unsigned>(client_count));
        }
      } else {
        ESP_LOGW(kLogTag, "Failed to get AP client list: %s", esp_err_to_name(err));
      }

      last_client_check_ms_ = now_ms;
    }
  }
}

WiFiStatus WiFiSubsystem::GetStatus() const {
  WiFiStatus status;
  status.mode = mode_.load(std::memory_order_acquire);
  status.ready = ready_.load(std::memory_order_acquire);

  if (status.mode == WiFiMode::kStaConnected && netif_sta_ != nullptr) {
    // Get IP address
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif_sta_, &ip_info) == ESP_OK) {
      snprintf(status.ip_address, sizeof(status.ip_address), IPSTR,
               IP2STR(&ip_info.ip));
    }

    // Get RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
      status.rssi_dbm = ap_info.rssi;
    }
  } else if (status.mode == WiFiMode::kApActive) {
    // AP mode always 192.168.4.1
    std::strncpy(status.ip_address, "192.168.4.1", sizeof(status.ip_address) - 1);

    // Get connected clients count
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
      status.clients = sta_list.num;
    }
  }

  return status;
}

void WiFiSubsystem::SetConfig(const config::WiFiConfig& config) {
  config_ = config;
}

esp_err_t WiFiSubsystem::Reconnect() {
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }

  StopWiFi();

  // Delay to allow WiFi stack to fully stop
  vTaskDelay(pdMS_TO_TICKS(500));

  // Restart in STA mode if SSID configured
  if (config_.sta_ssid[0] != '\0') {
    sta_retry_count_ = 0;
    return StartStaMode();
  } else if (config_.enable_ap_fallback) {
    return StartApMode();
  }

  return ESP_OK;
}

void WiFiSubsystem::EventHandler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
  auto* self = static_cast<WiFiSubsystem*>(arg);

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        ESP_LOGI(kLogTag, "STA started, attempting connection...");
        esp_wifi_connect();
        break;

      case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(kLogTag, "STA connected to AP");
        break;

      case WIFI_EVENT_STA_DISCONNECTED: {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        ESP_LOGW(kLogTag, "STA disconnected (reason: %d)", event->reason);

        const WiFiMode current_mode = self->mode_.load(std::memory_order_acquire);
        if (current_mode == WiFiMode::kStaConnected) {
          // Was connected, try to reconnect
          self->mode_.store(WiFiMode::kStaConnecting, std::memory_order_release);
          self->ready_.store(false, std::memory_order_release);
          self->sta_connect_start_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
          esp_wifi_connect();
        }
        break;
      }

      case WIFI_EVENT_AP_START:
        ESP_LOGI(kLogTag, "AP started");
        break;

      case WIFI_EVENT_AP_STACONNECTED: {
        auto* event = static_cast<wifi_event_ap_staconnected_t*>(event_data);
        ESP_LOGI(kLogTag, "Client connected to AP (MAC: " MACSTR ")",
                 MAC2STR(event->mac));
        break;
      }

      case WIFI_EVENT_AP_STADISCONNECTED: {
        auto* event = static_cast<wifi_event_ap_stadisconnected_t*>(event_data);
        ESP_LOGI(kLogTag, "Client disconnected from AP (MAC: " MACSTR ")",
                 MAC2STR(event->mac));
        break;
      }

      default:
        break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(kLogTag, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    self->mode_.store(WiFiMode::kStaConnected, std::memory_order_release);
    self->ready_.store(true, std::memory_order_release);
    self->sta_retry_count_ = 0;

    // Trigger NeoPixel animation (Task 1.7 requirement)
    if (self->diagnostics_ != nullptr) {
      self->diagnostics_->SignalWifiConnected();
    }
  }
}

void WiFiSubsystem::FallbackToAp() {
  if (!config_.enable_ap_fallback) {
    ESP_LOGW(kLogTag, "AP fallback disabled in config, staying disconnected");
    return;
  }

  ESP_LOGI(kLogTag, "Falling back to AP mode");
  StopWiFi();
  vTaskDelay(pdMS_TO_TICKS(500));
  StartApMode();
}

esp_err_t WiFiSubsystem::ScanNetworks(std::vector<WifiScanResult>& results, uint16_t max_results) {
  // WiFi scan disabled - captive portal uses manual SSID entry only
  results.clear();
  ESP_LOGI(kLogTag, "WiFi scan disabled (manual entry only)");
  return ESP_OK;
}

uint8_t WiFiSubsystem::GetConnectedClientCount() const {
  // Only valid in AP mode
  if (mode_.load(std::memory_order_acquire) != WiFiMode::kApActive) {
    return 0;
  }

  return connected_clients_.load(std::memory_order_acquire);
}

}  // namespace wifi_subsystem
