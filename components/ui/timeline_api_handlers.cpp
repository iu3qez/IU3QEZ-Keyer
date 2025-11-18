/**
 * @file timeline_api_handlers.cpp
 * @brief REST API handlers for real-time timeline visualization
 *
 * Provides HTTP endpoints for timeline event streaming and configuration:
 * - GET /api/timeline/events?since=<timestamp>&limit=<max_events>
 * - GET /api/timeline/config
 *
 * Separated from http_server.cpp to avoid bloating the main file.
 * Included directly in http_server.cpp (not compiled separately).
 */

#include "ui/http_server.hpp"
#include "timeline/event_logger.hpp"
#include "app/application_controller.hpp"
#include "keying_subsystem/keying_subsystem.hpp"

extern "C" {
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
}

#include <cinttypes>
#include <cstdlib>
#include <cstring>

// Timeline API log tag (macro to avoid namespace scope issues when included)
#define TIMELINE_TAG "TimelineAPI"

// NOTE: This file is included in http_server.cpp inside namespace ui,
// so we don't need to declare namespace ui here.

namespace {

/**
 * @brief Map EventType enum to JSON string representation
 *
 * @param type EventType enum value
 * @return const char* JSON-compatible string name
 */
const char* EventTypeToString(timeline::EventType type) {
  using timeline::EventType;

  switch (type) {
    case EventType::kPaddleEdge:
      return "paddle_edge";
    case EventType::kRemoteEvent:
      return "remote_event";
    case EventType::kDiagnostics:
      return "diagnostics";
    case EventType::kAudio:
      return "audio";
    case EventType::kKeying:
      return "keying";
    case EventType::kMemoryWindow:
      return "memory_window";
    case EventType::kLatch:
      return "latch";
    case EventType::kSqueeze:
      return "squeeze";
    case EventType::kGapMarker:
      return "gap_marker";
    case EventType::kDecodedChar:
      return "decoded_char";
    default:
      return "unknown";
  }
}

}  // namespace

/**
 * @brief Handle GET /api/timeline/events
 *
 * Returns JSON array of timeline events with optional filtering.
 *
 * Query parameters:
 * - since: (optional) Microsecond timestamp - only return events after this time
 * - limit: (optional) Maximum events to return (default: 1000, max: 5000)
 *
 * Response JSON:
 * {
 *   "events": [
 *     {
 *       "timestamp_us": 1234567890,
 *       "type": "paddle_edge",
 *       "arg0": 0,
 *       "arg1": 1
 *     },
 *     ...
 *   ],
 *   "server_time_us": 1234567890,
 *   "dropped_count": 0
 * }
 */
esp_err_t HttpServer::HandleGetTimelineEvents(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Parse query parameters
  int64_t since_timestamp = 0;  // Default: return all events
  size_t limit = 200;           // Default limit (reduced to prevent OOM)

  char param_buf[32];
  if (GetQueryParam(req, "since", param_buf, sizeof(param_buf))) {
    // Parse as int64_t microseconds
    since_timestamp = strtoll(param_buf, nullptr, 10);
    ESP_LOGD(TIMELINE_TAG, "Filter since: %lld us", static_cast<long long>(since_timestamp));
  }

  if (GetQueryParam(req, "limit", param_buf, sizeof(param_buf))) {
    limit = static_cast<size_t>(atoi(param_buf));
    // Clamp to max 5000 events
    if (limit > 5000) {
      limit = 5000;
    }
    ESP_LOGD(TIMELINE_TAG, "Limit: %zu events", limit);
  }

  // Get KeyingSubsystem from ApplicationController
  keying_subsystem::KeyingSubsystem* keying = nullptr;
  if (ctx->app_controller != nullptr) {
    keying = ctx->app_controller->GetKeyingSubsystem();
  }

  if (keying == nullptr) {
    ESP_LOGW(TIMELINE_TAG, "KeyingSubsystem not initialized");
    return SendError(req, 500, "Keying subsystem not initialized");
  }

  // Get EventLogger from KeyingSubsystem
  auto& timeline = keying->GetTimeline();

  // Create JSON response
  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    ESP_LOGE(TIMELINE_TAG, "Failed to allocate root JSON object");
    return SendError(req, 500, "Failed to allocate JSON");
  }

  // Create events array
  cJSON* events_array = cJSON_CreateArray();
  if (events_array == nullptr) {
    cJSON_Delete(root);
    ESP_LOGE(TIMELINE_TAG, "Failed to allocate events array");
    return SendError(req, 500, "Failed to allocate JSON");
  }

  // Add events array to root
  cJSON_AddItemToObject(root, "events", events_array);

  // Iterate timeline events and add to JSON
  size_t event_count = 0;
  timeline.for_each([&](const timeline::TimelineEvent& evt) {
    // Filter by timestamp
    if (evt.timestamp_us <= since_timestamp) {
      return;  // Skip this event
    }

    // Check limit
    if (event_count >= limit) {
      return;  // Stop adding events
    }

    // Create event object
    cJSON* event_obj = cJSON_CreateObject();
    if (event_obj == nullptr) {
      ESP_LOGW(TIMELINE_TAG, "Failed to allocate event object (event %zu)", event_count);
      return;
    }

    // Add fields to event object
    cJSON_AddNumberToObject(event_obj, "timestamp_us",
                           static_cast<double>(evt.timestamp_us));
    cJSON_AddStringToObject(event_obj, "type",
                           EventTypeToString(evt.type));
    cJSON_AddNumberToObject(event_obj, "arg0",
                           static_cast<double>(evt.arg0));
    cJSON_AddNumberToObject(event_obj, "arg1",
                           static_cast<double>(evt.arg1));

    // Add event to array
    cJSON_AddItemToArray(events_array, event_obj);
    event_count++;
  });

  // Add server timestamp (current time)
  int64_t server_time = esp_timer_get_time();
  cJSON_AddNumberToObject(root, "server_time_us",
                         static_cast<double>(server_time));

  // Add dropped event count
  size_t dropped = timeline.dropped_count();
  cJSON_AddNumberToObject(root, "dropped_count",
                         static_cast<double>(dropped));

  ESP_LOGD(TIMELINE_TAG, "Returning %zu events (dropped: %zu)", event_count, dropped);

  // Send JSON response
  return SendJsonDocument(req, root);
}

/**
 * @brief Handle GET /api/timeline/config
 *
 * Returns configuration needed for timeline visualization.
 *
 * Response JSON:
 * {
 *   "wpm": 20,
 *   "wpm_source": "keying_config",  // or "decoder_adaptive"
 *   "decoder_enabled": false,
 *   "keying_speed_wpm": 20
 * }
 */
esp_err_t HttpServer::HandleGetTimelineConfig(httpd_req_t* req) {
  auto* ctx = static_cast<HandlerContext*>(req->user_ctx);

  // Create JSON response
  cJSON* root = cJSON_CreateObject();
  if (root == nullptr) {
    ESP_LOGE(TIMELINE_TAG, "Failed to allocate root JSON object");
    return SendError(req, 500, "Failed to allocate JSON");
  }

  // Get KeyingSubsystem from ApplicationController
  keying_subsystem::KeyingSubsystem* keying = nullptr;
  morse_decoder::MorseDecoder* decoder = nullptr;
  uint32_t keying_wpm = 20;  // Default
  uint32_t effective_wpm = 20;
  const char* wpm_source = "keying_config";
  bool decoder_enabled = false;

  if (ctx->app_controller != nullptr) {
    keying = ctx->app_controller->GetKeyingSubsystem();
    decoder = ctx->app_controller->GetMorseDecoder();
  }

  // Get keying speed from engine config
  if (keying != nullptr) {
    keying_wpm = keying->GetEngine().speed_wpm();
    effective_wpm = keying_wpm;
  }

  // Check if decoder is available and active
  if (decoder != nullptr && decoder->IsEnabled()) {
    decoder_enabled = true;
    uint32_t decoder_wpm = decoder->GetDetectedWPM();
    if (decoder_wpm > 0) {
      // Use decoder WPM if available (adaptive)
      effective_wpm = decoder_wpm;
      wpm_source = "decoder_adaptive";
    }
  }

  // Add fields to JSON
  cJSON_AddNumberToObject(root, "wpm", static_cast<double>(effective_wpm));
  cJSON_AddStringToObject(root, "wpm_source", wpm_source);
  cJSON_AddBoolToObject(root, "decoder_enabled", decoder_enabled);
  cJSON_AddNumberToObject(root, "keying_speed_wpm", static_cast<double>(keying_wpm));

  ESP_LOGD(TIMELINE_TAG, "Timeline config: wpm=%" PRIu32 ", source=%s", effective_wpm, wpm_source);

  // Send JSON response
  return SendJsonDocument(req, root);
}

// End of timeline_api_handlers.cpp (included in http_server.cpp)
