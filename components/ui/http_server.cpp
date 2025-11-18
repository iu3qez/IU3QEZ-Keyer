#include "ui/http_server.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

extern "C" {
#include "cJSON.h"
}

#include "app/application_controller.hpp"
#include "app/bootloader_entry.hpp"
#include "remote/remote_cw_client.hpp"
#include "remote/remote_cw_server.hpp"
#include "system_monitor/system_monitor.hpp"
#include "ui/web_assets.hpp"
#include "morse_decoder/morse_decoder.hpp"
#include "text_keyer/text_keyer.hpp"
#include "system_monitor/system_monitor.hpp"

extern "C" {
#include "esp_log.h"
}

namespace ui {

namespace {
// Static asset delivery helpers configured via generated web asset table.

bool EqualsIgnoreCase(const std::string& value, const char* token) {
  return strcasecmp(value.c_str(), token) == 0;
}

bool InterpretBooleanValue(const std::string& value,
                           const config::BooleanParameter* bool_param) {
  if (bool_param != nullptr) {
    return EqualsIgnoreCase(value, bool_param->GetTrueName());
  }

  // Recognize legacy textual representations produced by BooleanParameter instances.
  return EqualsIgnoreCase(value, "on") || EqualsIgnoreCase(value, "true") ||
         EqualsIgnoreCase(value, "consider") || EqualsIgnoreCase(value, "state");
}

static esp_err_t SendJsonDocument(httpd_req_t* req, cJSON* document) {
  if (document == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  char* payload = cJSON_PrintUnformatted(document);
  if (payload == nullptr) {
    cJSON_Delete(document);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = HttpServer::SendJson(req, payload);
  cJSON_free(payload);
  cJSON_Delete(document);
  return result;
}

}  // namespace

esp_err_t HttpServer::HandleRoot(httpd_req_t* req) {
  // Serve Svelte SPA for home page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Home page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetConfigPage(httpd_req_t* req) {
  // Serve Svelte SPA for config page (same SPA, different route)
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Configuration page asset missing");
  }
  return ESP_OK;
}

HttpServer::~HttpServer() {
  Deinitialize();
}

esp_err_t HttpServer::Initialize(config::DeviceConfig* config,
                                  wifi_subsystem::WiFiSubsystem* wifi,
                                  config::Storage* storage,
                                  config::ParameterRegistry* param_registry,
                                  app::ApplicationController* app_controller) {
  if (server_ != nullptr) {
    ESP_LOGW(HttpServer::kLogTag, "HTTP server already running");
    return ESP_ERR_INVALID_STATE;
  }

  if (config == nullptr || wifi == nullptr || storage == nullptr || param_registry == nullptr) {
    ESP_LOGE(HttpServer::kLogTag, "Invalid arguments (null pointers)");
    return ESP_ERR_INVALID_ARG;
  }

  // Store context for handlers
  context_.config = config;
  context_.wifi = wifi;
  context_.storage = storage;
  context_.param_registry = param_registry;
  context_.app_controller = app_controller;
  context_valid_ = true;

  // Start the server (can be called again via Start() after Stop())
  return Start();
}

esp_err_t HttpServer::Stop() {
  if (server_ == nullptr) {
    return ESP_OK;  // Already stopped, idempotent
  }

  httpd_stop(server_);
  server_ = nullptr;
  asset_routes_.clear();
  ESP_LOGI(HttpServer::kLogTag, "HTTP server stopped (temporary, context retained for restart)");
  return ESP_OK;
}

esp_err_t HttpServer::Start() {
  if (!context_valid_) {
    ESP_LOGE(HttpServer::kLogTag, "Cannot start: server was never initialized (call Initialize() first)");
    return ESP_ERR_INVALID_STATE;
  }

  if (server_ != nullptr) {
    ESP_LOGW(HttpServer::kLogTag, "HTTP server already running");
    return ESP_ERR_INVALID_STATE;
  }

  // Configure HTTP server
  httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
  httpd_config.server_port = 80;
  httpd_config.max_uri_handlers = 50;  // 19 API endpoints + asset routes + margin for future growth
  httpd_config.stack_size = 4096;      // Sufficient for JSON parsing
  // Note: max header length configured via CONFIG_HTTPD_MAX_REQ_HDR_LEN in sdkconfig.defaults

  // Start server
  esp_err_t err = httpd_start(&server_, &httpd_config);
  if (err != ESP_OK) {
    ESP_LOGE(HttpServer::kLogTag, "Failed to start HTTP server: %s", esp_err_to_name(err));
    return err;
  }

  // Register URI handlers
  httpd_uri_t uri_root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = HandleRoot,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_root);

  httpd_uri_t uri_config_page = {
      .uri = "/config",
      .method = HTTP_GET,
      .handler = HandleGetConfigPage,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_config_page);

  httpd_uri_t uri_timeline = {
      .uri = "/timeline",
      .method = HTTP_GET,
      .handler = HandleGetTimeline,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_timeline);

  httpd_uri_t uri_remote = {
      .uri = "/remote",
      .method = HTTP_GET,
      .handler = HandleGetRemote,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_remote);

  httpd_uri_t uri_decoder = {
      .uri = "/decoder",
      .method = HTTP_GET,
      .handler = HandleGetDecoder,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_decoder);

  httpd_uri_t uri_system = {
      .uri = "/system",
      .method = HTTP_GET,
      .handler = HandleGetSystem,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_system);

  httpd_uri_t uri_firmware = {
      .uri = "/firmware",
      .method = HTTP_GET,
      .handler = HandleGetFirmwarePage,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_firmware);

  httpd_uri_t uri_status = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = HandleGetStatus,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_status);

  // New endpoints (Task 5.4.2)
  httpd_uri_t uri_schema = {
      .uri = "/api/config/schema",
      .method = HTTP_GET,
      .handler = HandleGetSchema,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_schema);

  httpd_uri_t uri_get_config = {
      .uri = "/api/config",
      .method = HTTP_GET,
      .handler = HandleGetConfig,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_get_config);

  // Parameter update endpoint (no wildcard needed)
  httpd_uri_t uri_post_param = {
      .uri = "/api/parameter",
      .method = HTTP_POST,
      .handler = HandlePostParameter,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_post_param);

  httpd_uri_t uri_save = {
      .uri = "/api/config/save",
      .method = HTTP_POST,
      .handler = HandlePostSave,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_save);

  // Remote keying endpoints (Task 6)
  httpd_uri_t uri_remote_status = {
      .uri = "/api/remote/status",
      .method = HTTP_GET,
      .handler = HandleGetRemoteStatus,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_remote_status);

  httpd_uri_t uri_client_start = {
      .uri = "/api/remote/client/start",
      .method = HTTP_POST,
      .handler = HandlePostClientStart,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_client_start);

  httpd_uri_t uri_client_stop = {
      .uri = "/api/remote/client/stop",
      .method = HTTP_POST,
      .handler = HandlePostClientStop,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_client_stop);

  httpd_uri_t uri_server_start = {
      .uri = "/api/remote/server/start",
      .method = HTTP_POST,
      .handler = HandlePostServerStart,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_server_start);

  httpd_uri_t uri_server_stop = {
      .uri = "/api/remote/server/stop",
      .method = HTTP_POST,
      .handler = HandlePostServerStop,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_server_stop);

  httpd_uri_t uri_decoder_status = {
      .uri = "/api/decoder/status",
      .method = HTTP_GET,
      .handler = HandleGetDecoderStatus,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_decoder_status);

  httpd_uri_t uri_decoder_enable = {
      .uri = "/api/decoder/enable",
      .method = HTTP_POST,
      .handler = HandlePostDecoderEnable,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_decoder_enable);

  httpd_uri_t uri_timeline_events = {
      .uri = "/api/timeline/events",
      .method = HTTP_GET,
      .handler = HandleGetTimelineEvents,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_timeline_events);

  httpd_uri_t uri_timeline_config = {
      .uri = "/api/timeline/config",
      .method = HTTP_GET,
      .handler = HandleGetTimelineConfig,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_timeline_config);

  // Text keyer endpoints
  httpd_uri_t uri_keyer_page = {
      .uri = "/keyer",
      .method = HTTP_GET,
      .handler = HandleGetKeyerPage,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_keyer_page);

  httpd_uri_t uri_keyer_status = {
      .uri = "/api/keyer/status",
      .method = HTTP_GET,
      .handler = HandleGetKeyerStatus,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_keyer_status);

  httpd_uri_t uri_keyer_send = {
      .uri = "/api/keyer/send",
      .method = HTTP_POST,
      .handler = HandlePostKeyerSend,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_keyer_send);

  httpd_uri_t uri_keyer_message = {
      .uri = "/api/keyer/message",
      .method = HTTP_POST,
      .handler = HandlePostKeyerMessage,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_keyer_message);

  httpd_uri_t uri_keyer_abort = {
      .uri = "/api/keyer/abort",
      .method = HTTP_POST,
      .handler = HandlePostKeyerAbort,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_keyer_abort);

  // System monitoring endpoints
  httpd_uri_t uri_system_stats = {
      .uri = "/api/system/stats",
      .method = HTTP_GET,
      .handler = HandleGetSystemStats,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_system_stats);

  // Bootloader management endpoints
  httpd_uri_t uri_enter_bootloader = {
      .uri = "/api/enter-bootloader",
      .method = HTTP_POST,
      .handler = HandlePostEnterBootloader,
      .user_ctx = &context_,
  };
  httpd_register_uri_handler(server_, &uri_enter_bootloader);

  asset_routes_.clear();
  const std::size_t asset_count = assets::Count();
  ESP_LOGI(HttpServer::kLogTag, "Registering %d web assets", static_cast<int>(asset_count));
  asset_routes_.reserve(asset_count);
  const assets::Asset* embedded_assets = assets::List();
  for (std::size_t i = 0; i < asset_count; ++i) {
    const assets::Asset& asset = embedded_assets[i];
    ESP_LOGI(HttpServer::kLogTag, "  Asset %d: %s (%d bytes, %s)",
             static_cast<int>(i), asset.path, static_cast<int>(asset.size), asset.content_type);
    httpd_uri_t uri = {
        .uri = asset.path,
        .method = HTTP_GET,
        .handler = HandleGetAsset,
        .user_ctx = &context_,
    };
    asset_routes_.push_back(uri);
    esp_err_t reg_err = httpd_register_uri_handler(server_, &asset_routes_.back());
    if (reg_err != ESP_OK) {
      ESP_LOGE(HttpServer::kLogTag, "  Failed to register %s: %s", asset.path, esp_err_to_name(reg_err));
    }
  }

  ESP_LOGI(HttpServer::kLogTag, "HTTP server started on port 80 with %d endpoints", 21 + static_cast<int>(asset_routes_.size()));
  return ESP_OK;
}

void HttpServer::Deinitialize() {
  if (server_ != nullptr) {
    httpd_stop(server_);
    server_ = nullptr;
    asset_routes_.clear();
    ESP_LOGI(HttpServer::kLogTag, "HTTP server stopped");
  }
  context_valid_ = false;  // Invalidate context, requires Initialize() to restart
}

esp_err_t HttpServer::HandleGetStatus(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  wifi_subsystem::WiFiStatus status = ctx->wifi->GetStatus();

  const char* mode_str = "Unknown";
  switch (status.mode) {
    case wifi_subsystem::WiFiMode::kIdle:
      mode_str = "Idle";
      break;
    case wifi_subsystem::WiFiMode::kStaConnecting:
      mode_str = "Connecting";
      break;
    case wifi_subsystem::WiFiMode::kStaConnected:
      mode_str = "Connected (STA)";
      break;
    case wifi_subsystem::WiFiMode::kApActive:
      mode_str = "Access Point";
      break;
  }

  cJSON* body = cJSON_CreateObject();
  if (body == nullptr) {
    return SendError(req, 500, "Failed to allocate status JSON");
  }

  if (cJSON_AddStringToObject(body, "mode", mode_str) == nullptr ||
      cJSON_AddStringToObject(body, "ip", status.ip_address) == nullptr ||
      cJSON_AddNumberToObject(body, "rssi", static_cast<double>(status.rssi_dbm)) == nullptr ||
      cJSON_AddNumberToObject(body, "clients", static_cast<double>(status.clients)) == nullptr ||
      cJSON_AddBoolToObject(body, "ready", status.ready ? cJSON_True : cJSON_False) == nullptr) {
    cJSON_Delete(body);
    return SendError(req, 500, "Failed to build status JSON");
  }

  return SendJsonDocument(req, body);
}

esp_err_t HttpServer::HandleGetTimeline(httpd_req_t* req) {
  // Serve Svelte SPA for timeline page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Timeline page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetRemote(httpd_req_t* req) {
  // Serve Svelte SPA for remote page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Remote page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetDecoder(httpd_req_t* req) {
  // Serve Svelte SPA for decoder page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Decoder page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetSystem(httpd_req_t* req) {
  // Serve Svelte SPA for system monitor page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "System monitor asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetFirmwarePage(httpd_req_t* req) {
  // Serve Svelte SPA for firmware update page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Firmware update page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetAsset(httpd_req_t* req) {
  std::string_view path(req->uri);
  const std::size_t query_pos = path.find('?');
  if (query_pos != std::string_view::npos) {
    path = path.substr(0, query_pos);
  }

  if (path == "/") {
    return SendAsset(req, "/index.html");
  }

  const esp_err_t result = SendAsset(req, path);
  if (result == ESP_OK) {
    return ESP_OK;
  }
  return SendError(req, 404, "Requested asset not found");
}

esp_err_t HttpServer::SendJson(httpd_req_t* req, const char* json) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");  // CORS for development
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HttpServer::SendAsset(httpd_req_t* req, std::string_view asset_path) {
  const assets::Asset* asset = assets::Find(asset_path);
  if (asset == nullptr) {
    ESP_LOGW(HttpServer::kLogTag, "Asset not found: %.*s", static_cast<int>(asset_path.size()), asset_path.data());
    return ESP_FAIL;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, asset->content_type);
  if (asset->compressed) {
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  }

  return httpd_resp_send(req, reinterpret_cast<const char*>(asset->data), asset->size);
}

esp_err_t HttpServer::SendError(httpd_req_t* req, int status_code, const char* message) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_status(req, status_code == 404 ? "404 Not Found" :
                             status_code == 400 ? "400 Bad Request" :
                             status_code == 500 ? "500 Internal Server Error" :
                             "200 OK");

  cJSON* body = cJSON_CreateObject();
  if (body != nullptr) {
    if (cJSON_AddBoolToObject(body, "success", cJSON_False) != nullptr &&
        cJSON_AddStringToObject(body, "message", message) != nullptr) {
      const esp_err_t result = SendJsonDocument(req, body);
      if (result != ESP_ERR_NO_MEM) {
        return result;
      }
      // fall through to stack-based fallback if serialization failed
    } else {
      cJSON_Delete(body);
    }
  }

  char fallback[256];
  snprintf(fallback, sizeof(fallback),
           "{\"success\":false,\"message\":\"%s\"}", message);
  return SendJson(req, fallback);
}

bool HttpServer::GetQueryParam(httpd_req_t* req, const char* param_name,
                               char* out_value, size_t max_len) {
  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len == 0) {
    return false;
  }

  char* query_buf = static_cast<char*>(malloc(query_len + 1));
  if (query_buf == nullptr) {
    return false;
  }

  esp_err_t err = httpd_req_get_url_query_str(req, query_buf, query_len + 1);
  if (err != ESP_OK) {
    free(query_buf);
    return false;
  }

  // Use ESP-IDF helper to extract parameter
  err = httpd_query_key_value(query_buf, param_name, out_value, max_len);
  free(query_buf);

  return (err == ESP_OK);
}

//
// New handlers (Task 5.4.2)
//

esp_err_t HttpServer::HandleGetSchema(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Export JSON schema from ParameterRegistry
  std::string schema = ctx->param_registry->ExportJsonSchema(*ctx->config);

  return SendJson(req, schema.c_str());
}

esp_err_t HttpServer::HandleGetConfig(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Check for subsystem filter query parameter
  char subsystem_filter[32] = "";
  GetQueryParam(req, "subsystem", subsystem_filter, sizeof(subsystem_filter));

  const char* subsystems[] = {"general", "audio", "keying", "wifi", "hardware", "remote", "server"};

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    return SendError(req, 500, "Failed to allocate configuration JSON");
  }

  for (const char* subsystem : subsystems) {
    // Skip if filter is set and doesn't match
    if (subsystem_filter[0] != '\0' && strcmp(subsystem_filter, subsystem) != 0) {
      continue;
    }

    // Get visible parameters for this subsystem
    auto params = ctx->param_registry->GetVisibleParameters(subsystem, *ctx->config);
    if (params.empty()) {
      continue;
    }

    cJSON* subsystem_obj = cJSON_CreateObject();
    if (subsystem_obj == nullptr) {
      cJSON_Delete(root);
      return SendError(req, 500, "Failed to allocate subsystem JSON");
    }

    bool has_parameters = false;
    for (config::Parameter* param : params) {
      // Extract short param name (after "subsystem.")
      const char* full_name = param->GetName();
      const char* short_name = strchr(full_name, '.');
      if (short_name == nullptr) {
        short_name = full_name;
      } else {
        short_name++;  // Skip '.'
      }

      // Get current value
      std::string value = param->GetCurrentValue(*ctx->config);

      // Format value based on type
      const char* type = param->GetTypeName();
      cJSON* added_field = nullptr;
      if (strcmp(type, "int") == 0) {
        char* endptr = nullptr;
        const long parsed = std::strtol(value.c_str(), &endptr, 10);
        if (endptr != value.c_str() && *endptr == '\0') {
          added_field = cJSON_AddNumberToObject(subsystem_obj, short_name,
                                                static_cast<double>(parsed));
        }
      } else if (strcmp(type, "float") == 0) {
        char* endptr = nullptr;
        const double parsed = std::strtod(value.c_str(), &endptr);
        if (endptr != value.c_str() && *endptr == '\0') {
          added_field = cJSON_AddNumberToObject(subsystem_obj, short_name, parsed);
        }
      } else if (strcmp(type, "bool") == 0) {
        // Safe cast: GetTypeName() already verified type is "bool"
        const auto* bool_param = static_cast<const config::BooleanParameter*>(param);
        const bool bool_value = InterpretBooleanValue(value, bool_param);
        added_field = cJSON_AddBoolToObject(subsystem_obj, short_name, bool_value);
      }

      if (added_field == nullptr) {
        if (cJSON_AddStringToObject(subsystem_obj, short_name, value.c_str()) == nullptr) {
          cJSON_Delete(subsystem_obj);
          cJSON_Delete(root);
          return SendError(req, 500, "Failed to build configuration JSON");
        }
      }

      has_parameters = true;
    }

    if (has_parameters) {
      cJSON_AddItemToObject(root, subsystem, subsystem_obj);
    } else {
      cJSON_Delete(subsystem_obj);
    }
  }

  const esp_err_t result = SendJsonDocument(req, root);
  if (result == ESP_ERR_NO_MEM) {
    return SendError(req, 500, "Failed to serialize configuration JSON");
  }
  return result;
}

esp_err_t HttpServer::HandlePostParameter(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Read POST body (JSON: {"param": "subsystem.name", "value": "..."})
  char content[512];
  size_t recv_size = std::min(req->content_len, sizeof(content) - 1);
  int ret = httpd_req_recv(req, content, recv_size);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  content[ret] = '\0';

  ESP_LOGI(HttpServer::kLogTag, "POST /api/parameter body: %s", content);

  cJSON* body = cJSON_ParseWithLength(content, ret);
  if (body == nullptr) {
    return SendError(req, 400, "Invalid JSON payload");
  }

  const cJSON* param_item = cJSON_GetObjectItemCaseSensitive(body, "param");
  if (!cJSON_IsString(param_item) || param_item->valuestring == nullptr) {
    cJSON_Delete(body);
    return SendError(req, 400, "Missing 'param' field in JSON");
  }

  const char* full_param_name = param_item->valuestring;

  // Find parameter in registry
  config::Parameter* p = ctx->param_registry->Find(full_param_name);
  if (p == nullptr || !p->IsVisible(*ctx->config)) {
    cJSON_Delete(body);
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "Parameter not found: %s", full_param_name);
    return SendError(req, 404, error_msg);
  }

  const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(body, "value");
  if (value_item == nullptr) {
    cJSON_Delete(body);
    return SendError(req, 400, "Missing 'value' field in JSON");
  }

  std::string value_str;
  if (cJSON_IsString(value_item) && value_item->valuestring != nullptr) {
    value_str = value_item->valuestring;
  } else if (cJSON_IsBool(value_item)) {
    // Check if parameter is boolean type (safe cast if true)
    if (strcmp(p->GetTypeName(), "bool") == 0) {
      auto* bool_param = static_cast<config::BooleanParameter*>(p);
      value_str = value_item->valueint ? bool_param->GetTrueName() : bool_param->GetFalseName();
    } else {
      value_str = value_item->valueint ? "true" : "false";
    }
  } else if (cJSON_IsNumber(value_item)) {
    char buffer[64];
    if (strcmp(p->GetTypeName(), "int") == 0) {
      const long long rounded = std::llround(value_item->valuedouble);
      snprintf(buffer, sizeof(buffer), "%lld", rounded);
    } else {
      snprintf(buffer, sizeof(buffer), "%.10g", value_item->valuedouble);
    }
    value_str = buffer;
  } else {
    char* rendered = cJSON_PrintUnformatted(value_item);
    if (rendered == nullptr) {
      cJSON_Delete(body);
      return SendError(req, 500, "Failed to parse 'value' field");
    }
    value_str.assign(rendered);
    cJSON_free(rendered);
  }

  ESP_LOGI(HttpServer::kLogTag, "Setting %s = %s", full_param_name, value_str.c_str());

  // Execute parameter update
  std::string result_message;
  bool ok = p->Execute(value_str.c_str(), *ctx->config, &result_message);

  if (ok) {
    // Apply changes immediately to running subsystems (hot-reload)
    if (ctx->app_controller != nullptr) {
      ctx->app_controller->ApplyConfigChanges(*ctx->config);
    }

    // Check if parameter requires reset and format response accordingly
    bool requires_reset = p->GetRequiresReset();
    cJSON* response = cJSON_CreateObject();
    if (response == nullptr) {
      cJSON_Delete(body);
      return SendError(req, 500, "Failed to allocate response JSON");
    }

    if (cJSON_AddBoolToObject(response, "success", cJSON_True) == nullptr ||
        cJSON_AddStringToObject(response, "message", result_message.c_str()) == nullptr ||
        cJSON_AddBoolToObject(response, "requires_reset",
                              requires_reset ? cJSON_True : cJSON_False) == nullptr) {
      cJSON_Delete(response);
      cJSON_Delete(body);
      return SendError(req, 500, "Failed to build response JSON");
    }

    if (requires_reset) {
      if (cJSON_AddStringToObject(
              response, "warning",
              "This change requires device reset to take effect. Please save and restart.") ==
          nullptr) {
        cJSON_Delete(response);
        cJSON_Delete(body);
        return SendError(req, 500, "Failed to build response JSON");
      }
    }

    cJSON_Delete(body);

    const esp_err_t result = SendJsonDocument(req, response);
    if (result == ESP_ERR_NO_MEM) {
      return SendError(req, 500, "Failed to serialize response JSON");
    }
    return result;
  } else {
    cJSON_Delete(body);
    return SendError(req, 400, result_message.c_str());
  }
}

esp_err_t HttpServer::HandlePostSave(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Save config to NVS
  esp_err_t err = ctx->storage->Save(*ctx->config);
  if (err != ESP_OK) {
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "Failed to save: %s", esp_err_to_name(err));
    return SendError(req, 500, error_msg);
  }

  ESP_LOGI(HttpServer::kLogTag, "Configuration saved to NVS");

  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  if (cJSON_AddBoolToObject(response, "success", cJSON_True) == nullptr ||
      cJSON_AddStringToObject(response, "message", "Configuration saved to NVS") == nullptr) {
    cJSON_Delete(response);
    return SendError(req, 500, "Failed to build response JSON");
  }

  const esp_err_t result = SendJsonDocument(req, response);
  if (result == ESP_ERR_NO_MEM) {
    return SendError(req, 500, "Failed to serialize response JSON");
  }
  return result;
}

//
// Remote keying API handlers (Task 6)
//

esp_err_t HttpServer::HandleGetRemoteStatus(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    return SendError(req, 500, "Failed to allocate status JSON");
  }

  // Get remote client/server from ApplicationController
  remote::RemoteCwClient* client = nullptr;
  remote::RemoteCwServer* server = nullptr;
  if (ctx->app_controller != nullptr) {
    client = ctx->app_controller->GetRemoteClient();
    server = ctx->app_controller->GetRemoteServer();
  }

  // Build client status object
  cJSON* client_obj = cJSON_CreateObject();
  if (client_obj == nullptr) {
    cJSON_Delete(root);
    return SendError(req, 500, "Failed to allocate client JSON");
  }

  if (client != nullptr) {
    const int state = static_cast<int>(client->GetState());
    const uint32_t latency_ms = client->GetLatency();
    const char* server_host = ctx->config->remote.server_host;
    const uint16_t server_port = ctx->config->remote.server_port;
    const uint32_t ptt_tail_base_ms = ctx->config->remote.ptt_tail_ms;

    cJSON_AddNumberToObject(client_obj, "state", static_cast<double>(state));
    cJSON_AddStringToObject(client_obj, "server_host", server_host);
    cJSON_AddNumberToObject(client_obj, "server_port", static_cast<double>(server_port));
    cJSON_AddNumberToObject(client_obj, "latency_ms", static_cast<double>(latency_ms));
    cJSON_AddNumberToObject(client_obj, "ptt_tail_base_ms", static_cast<double>(ptt_tail_base_ms));
  } else {
    cJSON_AddNumberToObject(client_obj, "state", 0.0);  // Idle
    cJSON_AddStringToObject(client_obj, "server_host", "");
    cJSON_AddNumberToObject(client_obj, "server_port", 0.0);
    cJSON_AddNumberToObject(client_obj, "latency_ms", 0.0);
    cJSON_AddNumberToObject(client_obj, "ptt_tail_base_ms", 0.0);
  }
  cJSON_AddItemToObject(root, "client", client_obj);

  // Build server status object
  cJSON* server_obj = cJSON_CreateObject();
  if (server_obj == nullptr) {
    cJSON_Delete(root);
    return SendError(req, 500, "Failed to allocate server JSON");
  }

  if (server != nullptr) {
    const int state = static_cast<int>(server->state());
    const uint16_t listen_port = ctx->config->server.listen_port;
    const uint32_t ptt_tail_ms = ctx->config->server.ptt_tail_ms;
    const char* client_ip = server->client_ip();

    cJSON_AddNumberToObject(server_obj, "state", static_cast<double>(state));
    cJSON_AddNumberToObject(server_obj, "listen_port", static_cast<double>(listen_port));
    cJSON_AddStringToObject(server_obj, "client_ip", client_ip ? client_ip : "");
    cJSON_AddNumberToObject(server_obj, "ptt_tail_ms", static_cast<double>(ptt_tail_ms));
  } else {
    cJSON_AddNumberToObject(server_obj, "state", 0.0);  // Idle
    cJSON_AddNumberToObject(server_obj, "listen_port", 0.0);
    cJSON_AddStringToObject(server_obj, "client_ip", "");
    cJSON_AddNumberToObject(server_obj, "ptt_tail_ms", 0.0);
  }
  cJSON_AddItemToObject(root, "server", server_obj);

  // Build config object
  cJSON* config_obj = cJSON_CreateObject();
  if (config_obj == nullptr) {
    cJSON_Delete(root);
    return SendError(req, 500, "Failed to allocate config JSON");
  }

  cJSON_AddBoolToObject(config_obj, "client_enabled",
                        ctx->config->remote.enabled ? cJSON_True : cJSON_False);
  cJSON_AddStringToObject(config_obj, "client_server_host", ctx->config->remote.server_host);
  cJSON_AddNumberToObject(config_obj, "client_server_port",
                          static_cast<double>(ctx->config->remote.server_port));
  cJSON_AddBoolToObject(config_obj, "client_auto_reconnect",
                        ctx->config->remote.auto_reconnect ? cJSON_True : cJSON_False);
  cJSON_AddBoolToObject(config_obj, "server_enabled",
                        ctx->config->server.enabled ? cJSON_True : cJSON_False);
  cJSON_AddNumberToObject(config_obj, "server_listen_port",
                          static_cast<double>(ctx->config->server.listen_port));
  cJSON_AddItemToObject(root, "config", config_obj);

  return SendJsonDocument(req, root);
}

esp_err_t HttpServer::HandlePostClientStart(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  remote::RemoteCwClient* client = nullptr;
  if (ctx->app_controller != nullptr) {
    client = ctx->app_controller->GetRemoteClient();
  }

  if (client == nullptr) {
    return SendError(req, 400, "Remote client not initialized");
  }

  esp_err_t err = client->Start();
  if (err != ESP_OK) {
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "Failed to start client: %s", esp_err_to_name(err));
    return SendError(req, 500, error_msg);
  }

  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Client started");

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandlePostClientStop(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  remote::RemoteCwClient* client = nullptr;
  if (ctx->app_controller != nullptr) {
    client = ctx->app_controller->GetRemoteClient();
  }

  if (client == nullptr) {
    return SendError(req, 400, "Remote client not initialized");
  }

  client->Stop();

  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Client stopped");

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandlePostServerStart(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  remote::RemoteCwServer* server = nullptr;
  if (ctx->app_controller != nullptr) {
    server = ctx->app_controller->GetRemoteServer();
  }

  if (server == nullptr) {
    return SendError(req, 400, "Remote server not initialized");
  }

  esp_err_t err = server->Start();
  if (err != ESP_OK) {
    char error_msg[128];
    snprintf(error_msg, sizeof(error_msg), "Failed to start server: %s", esp_err_to_name(err));
    return SendError(req, 500, error_msg);
  }

  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Server started");

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandlePostServerStop(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  remote::RemoteCwServer* server = nullptr;
  if (ctx->app_controller != nullptr) {
    server = ctx->app_controller->GetRemoteServer();
  }

  if (server == nullptr) {
    return SendError(req, 400, "Remote server not initialized");
  }

  server->Stop();

  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Server stopped");

  return SendJsonDocument(req, response);
}

// Decoder API handlers

esp_err_t HttpServer::HandleGetDecoderStatus(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    return SendError(req, 500, "Failed to allocate decoder status JSON");
  }

  // Get MorseDecoder from ApplicationController
  morse_decoder::MorseDecoder* decoder = nullptr;
  if (ctx->app_controller != nullptr) {
    decoder = ctx->app_controller->GetMorseDecoder();
  }

  if (decoder != nullptr) {
    // Get decoder status
    bool enabled = decoder->IsEnabled();
    uint32_t wpm = decoder->GetDetectedWPM();
    std::string text = decoder->GetDecodedText();
    std::string pattern = decoder->GetCurrentPattern();

    // Add fields to JSON
    if (cJSON_AddBoolToObject(root, "enabled", enabled ? cJSON_True : cJSON_False) == nullptr ||
        cJSON_AddNumberToObject(root, "wpm", static_cast<double>(wpm)) == nullptr ||
        cJSON_AddStringToObject(root, "text", text.c_str()) == nullptr ||
        cJSON_AddStringToObject(root, "pattern", pattern.c_str()) == nullptr) {
      cJSON_Delete(root);
      return SendError(req, 500, "Failed to build decoder status JSON");
    }
  } else {
    // Decoder not initialized - return safe defaults
    if (cJSON_AddBoolToObject(root, "enabled", cJSON_False) == nullptr ||
        cJSON_AddNumberToObject(root, "wpm", 0.0) == nullptr ||
        cJSON_AddStringToObject(root, "text", "") == nullptr ||
        cJSON_AddStringToObject(root, "pattern", "") == nullptr) {
      cJSON_Delete(root);
      return SendError(req, 500, "Failed to build decoder status JSON");
    }
  }

  return SendJsonDocument(req, root);
}

esp_err_t HttpServer::HandlePostDecoderEnable(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Read POST body (JSON: {"enabled": true/false})
  char content[256];
  size_t recv_size = std::min(req->content_len, sizeof(content) - 1);
  int ret = httpd_req_recv(req, content, recv_size);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON* body = cJSON_ParseWithLength(content, ret);
  if (body == nullptr) {
    return SendError(req, 400, "Invalid JSON payload");
  }

  const cJSON* enabled_item = cJSON_GetObjectItemCaseSensitive(body, "enabled");
  if (!cJSON_IsBool(enabled_item)) {
    cJSON_Delete(body);
    return SendError(req, 400, "Missing or invalid 'enabled' field in JSON");
  }

  bool enabled = cJSON_IsTrue(enabled_item);
  cJSON_Delete(body);

  // Get MorseDecoder from ApplicationController
  morse_decoder::MorseDecoder* decoder = nullptr;
  if (ctx->app_controller != nullptr) {
    decoder = ctx->app_controller->GetMorseDecoder();
  }

  if (decoder == nullptr) {
    return SendError(req, 400, "Morse decoder not initialized");
  }

  // Set decoder enabled state
  decoder->SetEnabled(enabled);

  // Build success response
  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", enabled ? "Decoder enabled" : "Decoder disabled");
  cJSON_AddBoolToObject(response, "enabled", enabled ? cJSON_True : cJSON_False);

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandleGetSystemStats(httpd_req_t* req) {
  // Create system monitor instance
  system_monitor::SystemMonitor monitor;

  // Get complete system stats as JSON
  std::string json = monitor.GetSystemStatsJson();

  // Send JSON response
  return SendJson(req, json.c_str());
}

esp_err_t HttpServer::HandlePostEnterBootloader(httpd_req_t* req) {
  // No parameters needed - this just triggers bootloader entry
  // The request body can be empty or contain empty JSON {}

  ESP_LOGI(HttpServer::kLogTag, "Web UI bootloader entry request received");

  // Send success response BEFORE entering bootloader (connection will drop)
  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Entering bootloader mode - device will restart");

  esp_err_t result = SendJsonDocument(req, response);

  // Give HTTP response time to be sent before restarting
  // The device will disconnect from WiFi and reboot into UF2 bootloader
  vTaskDelay(pdMS_TO_TICKS(500));

  // Enter bootloader mode (sets RTC flag and restarts)
  app::EnterBootloaderMode();

  // Never returns (device restarts)
  return result;
}

// ============================================================================
// TEXT KEYER API HANDLERS
// ============================================================================

esp_err_t HttpServer::HandleGetKeyerPage(httpd_req_t* req) {
  // Serve Svelte SPA for keyer page
  const esp_err_t result = SendAsset(req, "/index.html");
  if (result != ESP_OK) {
    return SendError(req, 500, "Keyer page asset missing");
  }
  return ESP_OK;
}

esp_err_t HttpServer::HandleGetKeyerStatus(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    return SendError(req, 500, "Failed to allocate keyer status JSON");
  }

  // Get TextKeyer from ApplicationController
  text_keyer::TextKeyer* keyer = nullptr;
  if (ctx->app_controller != nullptr) {
    keyer = ctx->app_controller->GetTextKeyer();
  }

  if (keyer != nullptr) {
    // Get keyer status
    text_keyer::KeyerState state = keyer->GetState();
    uint32_t wpm = keyer->GetSpeed();
    size_t sent = 0, total = 0;
    keyer->GetProgress(sent, total);

    // State string
    const char* state_str = "idle";
    if (state == text_keyer::KeyerState::kSending) {
      state_str = "sending";
    } else if (state == text_keyer::KeyerState::kPaused) {
      state_str = "paused";
    }

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddNumberToObject(root, "wpm", wpm);
    cJSON_AddNumberToObject(root, "sent", static_cast<double>(sent));
    cJSON_AddNumberToObject(root, "total", static_cast<double>(total));
  } else {
    cJSON_AddStringToObject(root, "state", "unavailable");
    cJSON_AddNumberToObject(root, "wpm", 0);
    cJSON_AddNumberToObject(root, "sent", 0);
    cJSON_AddNumberToObject(root, "total", 0);
  }

  return SendJsonDocument(req, root);
}

esp_err_t HttpServer::HandlePostKeyerSend(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Read request body
  char buffer[512];
  int received = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
  if (received <= 0) {
    return SendError(req, 400, "Missing request body");
  }
  buffer[received] = '\0';

  // Parse JSON body
  cJSON* body = cJSON_Parse(buffer);
  if (body == nullptr) {
    return SendError(req, 400, "Invalid JSON in request body");
  }

  // Extract text and wpm fields
  cJSON* text_item = cJSON_GetObjectItem(body, "text");
  cJSON* wpm_item = cJSON_GetObjectItem(body, "wpm");

  if (!cJSON_IsString(text_item)) {
    cJSON_Delete(body);
    return SendError(req, 400, "Missing or invalid 'text' field in JSON");
  }

  const char* text = cJSON_GetStringValue(text_item);

  // Get TextKeyer from ApplicationController
  text_keyer::TextKeyer* keyer = nullptr;
  if (ctx->app_controller != nullptr) {
    keyer = ctx->app_controller->GetTextKeyer();
  }

  if (keyer == nullptr) {
    cJSON_Delete(body);
    return SendError(req, 500, "Text keyer not initialized");
  }

  // Set speed if provided
  if (cJSON_IsNumber(wpm_item)) {
    uint32_t wpm = static_cast<uint32_t>(cJSON_GetNumberValue(wpm_item));
    keyer->SetSpeed(wpm);
  }

  // Send text
  esp_err_t err = keyer->SendText(text);
  cJSON_Delete(body);

  if (err != ESP_OK) {
    return SendError(req, 400, "Failed to send text (already sending?)");
  }

  // Build success response
  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Text queued for sending");

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandlePostKeyerMessage(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Read request body
  char buffer[256];
  int received = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
  if (received <= 0) {
    return SendError(req, 400, "Missing request body");
  }
  buffer[received] = '\0';

  // Parse JSON body
  cJSON* body = cJSON_Parse(buffer);
  if (body == nullptr) {
    return SendError(req, 400, "Invalid JSON in request body");
  }

  // Extract message number field
  cJSON* message_item = cJSON_GetObjectItem(body, "message");

  if (!cJSON_IsNumber(message_item)) {
    cJSON_Delete(body);
    return SendError(req, 400, "Missing or invalid 'message' field in JSON");
  }

  int message_num = static_cast<int>(cJSON_GetNumberValue(message_item));
  cJSON_Delete(body);

  if (message_num < 1 || message_num > 10) {
    return SendError(req, 400, "Message number must be 1-10");
  }

  // Get message text from config
  const char* message_text = nullptr;
  switch (message_num) {
    case 1: message_text = ctx->config->stored_messages.message1; break;
    case 2: message_text = ctx->config->stored_messages.message2; break;
    case 3: message_text = ctx->config->stored_messages.message3; break;
    case 4: message_text = ctx->config->stored_messages.message4; break;
    case 5: message_text = ctx->config->stored_messages.message5; break;
    case 6: message_text = ctx->config->stored_messages.message6; break;
    case 7: message_text = ctx->config->stored_messages.message7; break;
    case 8: message_text = ctx->config->stored_messages.message8; break;
    case 9: message_text = ctx->config->stored_messages.message9; break;
    case 10: message_text = ctx->config->stored_messages.message10; break;
  }

  if (message_text == nullptr || message_text[0] == '\0') {
    return SendError(req, 400, "Message is empty");
  }

  // Get TextKeyer from ApplicationController
  text_keyer::TextKeyer* keyer = nullptr;
  if (ctx->app_controller != nullptr) {
    keyer = ctx->app_controller->GetTextKeyer();
  }

  if (keyer == nullptr) {
    return SendError(req, 500, "Text keyer not initialized");
  }

  // Set speed from global keying config
  keyer->SetSpeed(ctx->config->keying.speed_wpm);

  // Send message
  esp_err_t err = keyer->SendText(message_text);

  if (err != ESP_OK) {
    return SendError(req, 400, "Failed to send message (already sending?)");
  }

  // Build success response
  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Stored message queued for sending");

  return SendJsonDocument(req, response);
}

esp_err_t HttpServer::HandlePostKeyerAbort(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Get TextKeyer from ApplicationController
  text_keyer::TextKeyer* keyer = nullptr;
  if (ctx->app_controller != nullptr) {
    keyer = ctx->app_controller->GetTextKeyer();
  }

  if (keyer == nullptr) {
    return SendError(req, 500, "Text keyer not initialized");
  }

  // Abort transmission
  keyer->Abort();

  // Build success response
  cJSON* response = cJSON_CreateObject();
  if (response == nullptr) {
    return SendError(req, 500, "Failed to allocate response JSON");
  }

  cJSON_AddBoolToObject(response, "success", cJSON_True);
  cJSON_AddStringToObject(response, "message", "Transmission aborted");

  return SendJsonDocument(req, response);
}

// Include timeline API handlers (separate file to avoid bloating http_server.cpp)
#include "timeline_api_handlers.cpp"

}  // namespace ui
