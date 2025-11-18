/**
 * @file captive_portal_manager.cpp
 * @brief Implementation of captive portal manager.
 * @note This is a stub implementation - full implementation in progress.
 */

#include "captive_portal/captive_portal_manager.hpp"
#include "captive_portal/dns_server.hpp"
#include "captive_portal/minimal_http_server.hpp"
#include "ui/http_server.hpp"

#include "esp_log.h"

namespace captive_portal {

namespace {
constexpr const char* kTag = "CaptivePortalManager";
}

CaptivePortalManager::CaptivePortalManager(wifi_subsystem::WiFiSubsystem* wifi_subsystem,
                                           config::DeviceConfig* device_config,
                                           config::Storage* config_storage,
                                           diagnostics_subsystem::StatusLED* status_led,
                                           ui::HttpServer* main_http_server)
    : wifi_subsystem_(wifi_subsystem),
      device_config_(device_config),
      config_storage_(config_storage),
      status_led_(status_led),
      main_http_server_(main_http_server),
      dns_server_(nullptr),
      http_server_(nullptr),
      initialized_(false),
      active_(false),
      main_server_was_running_(false) {
  ESP_LOGI(kTag, "CaptivePortalManager constructed");
}

CaptivePortalManager::~CaptivePortalManager() {
  Disable();

  if (http_server_ != nullptr) {
    delete http_server_;
    http_server_ = nullptr;
  }

  if (dns_server_ != nullptr) {
    delete dns_server_;
    dns_server_ = nullptr;
  }

  ESP_LOGI(kTag, "CaptivePortalManager destroyed");
}

bool CaptivePortalManager::Initialize() {
  if (initialized_) {
    ESP_LOGW(kTag, "Already initialized");
    return true;
  }

  // Create DNS server
  dns_server_ = new DnsServer();
  if (dns_server_ == nullptr) {
    ESP_LOGE(kTag, "Failed to allocate DNS server");
    return false;
  }

  // Create HTTP server
  http_server_ = new MinimalHttpServer(wifi_subsystem_, device_config_, config_storage_);
  if (http_server_ == nullptr) {
    ESP_LOGE(kTag, "Failed to allocate HTTP server");
    delete dns_server_;
    dns_server_ = nullptr;
    return false;
  }

  initialized_ = true;
  ESP_LOGI(kTag, "Initialized successfully");
  return true;
}

bool CaptivePortalManager::Enable() {
  if (!initialized_) {
    ESP_LOGE(kTag, "Cannot enable: not initialized");
    return false;
  }

  if (active_) {
    ESP_LOGW(kTag, "Already enabled");
    return true;
  }

  // Stop main HTTP server to free port 80
  main_server_was_running_ = false;
  if (main_http_server_ != nullptr && main_http_server_->IsRunning()) {
    ESP_LOGI(kTag, "Stopping main HTTP server to free port 80");
    main_http_server_->Stop();
    main_server_was_running_ = true;
  }

  // Start DNS server
  if (!dns_server_->Start()) {
    ESP_LOGE(kTag, "Failed to start DNS server");
    // Restart main HTTP server if it was running
    if (main_server_was_running_ && main_http_server_ != nullptr) {
      main_http_server_->Start();
    }
    return false;
  }

  // Start captive portal HTTP server on port 80
  if (!http_server_->Start()) {
    ESP_LOGE(kTag, "Failed to start captive portal HTTP server");
    dns_server_->Stop();
    // Restart main HTTP server if it was running
    if (main_server_was_running_ && main_http_server_ != nullptr) {
      main_http_server_->Start();
    }
    return false;
  }

  active_ = true;
  ESP_LOGI(kTag, "Captive portal enabled (DNS + HTTP servers running)");
  return true;
}

bool CaptivePortalManager::Disable() {
  if (!active_) {
    return true;
  }

  // Stop captive portal HTTP server
  if (http_server_ != nullptr) {
    http_server_->Stop();
  }

  // Stop DNS server
  if (dns_server_ != nullptr) {
    dns_server_->Stop();
  }

  // Restart main HTTP server if it was running before
  if (main_server_was_running_ && main_http_server_ != nullptr) {
    ESP_LOGI(kTag, "Restarting main HTTP server");
    main_http_server_->Start();
    main_server_was_running_ = false;
  }

  active_ = false;
  ESP_LOGI(kTag, "Captive portal disabled");
  return true;
}

bool CaptivePortalManager::IsActive() const {
  return active_;
}

}  // namespace captive_portal
