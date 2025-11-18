#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../support/fake_esp_idf.hpp"

struct i2c_master_bus_t {
  i2c_master_bus_config_t config{};
};

struct i2s_channel {
  i2s_chan_config_t chan_cfg{};
  i2s_std_config_t std_cfg{};
  bool std_initialized = false;
  bool enabled = false;
};

struct esp_io_expander {
  uint32_t address = 0;
  uint32_t direction_mask = 0;
  uint32_t level_mask = 0;
};

namespace {

int64_t g_fake_time_us = 0;

struct GpioState {
  bool configured = false;
  gpio_config_t config{};
  gpio_int_type_t intr_type = GPIO_INTR_DISABLE;
  int level = 0;
  gpio_isr_t handler = nullptr;
  void* arg = nullptr;
};

esp_err_t g_gpio_install_result = ESP_OK;
bool g_gpio_service_installed = false;
std::unordered_map<gpio_num_t, GpioState> g_gpio_states;

struct FakeNvsNamespace {
  struct Value {
    enum class Kind { kI32, kU8, kU16, kU32, kString };
    Kind kind = Kind::kI32;
    int64_t number = 0;
    std::string string_value;
  };
  std::unordered_map<std::string, Value> values;
};

struct FakeNvsHandle {
  std::shared_ptr<FakeNvsNamespace> ns;
};

std::unordered_map<std::string, std::shared_ptr<FakeNvsNamespace>> g_nvs_namespaces;

struct FakeLedStrip {
  led_strip_config_t config{};
  std::vector<std::array<uint8_t, 3>> pixels;
  int refresh_count = 0;
};

std::unordered_map<led_strip_handle_t, std::unique_ptr<FakeLedStrip>> g_led_strips;
int g_next_strip_id = 1;

std::unordered_map<i2c_master_bus_handle_t, std::unique_ptr<i2c_master_bus_t>> g_i2c_buses;
std::unordered_map<i2s_chan_handle_t, std::unique_ptr<i2s_channel>> g_i2s_channels;

struct FakeTask {
  TaskFunction_t func = nullptr;
  void* param = nullptr;
};

std::unordered_map<TaskHandle_t, std::unique_ptr<FakeTask>> g_fake_tasks;

std::unordered_map<esp_io_expander_handle_t, std::unique_ptr<esp_io_expander>> g_io_expanders;

FakeLedStrip* GetStrip(led_strip_handle_t handle) {
  auto it = g_led_strips.find(handle);
  if (it == g_led_strips.end()) {
    return nullptr;
  }
  return it->second.get();
}

}  // namespace

extern "C" {

const char* esp_err_to_name(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return "ESP_OK";
    case ESP_FAIL:
      return "ESP_FAIL";
    case ESP_ERR_INVALID_ARG:
      return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
      return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NO_MEM:
      return "ESP_ERR_NO_MEM";
    case ESP_ERR_NVS_NOT_FOUND:
      return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_INVALID_LENGTH:
      return "ESP_ERR_NVS_INVALID_LENGTH";
    default:
      return "ESP_ERR_UNKNOWN";
  }
}

int64_t esp_timer_get_time(void) {
  return g_fake_time_us;
}

esp_err_t gpio_config(const gpio_config_t* config) {
  if (config == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  const uint64_t mask = config->pin_bit_mask;
  if (mask == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  for (gpio_num_t gpio = 0; gpio < 64; ++gpio) {
    if (((mask >> static_cast<uint64_t>(gpio)) & 0x1ULL) == 0) {
      continue;
    }
    GpioState& state = g_gpio_states[gpio];
    state.configured = true;
    state.config = *config;
  }
  return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type) {
  GpioState& state = g_gpio_states[gpio_num];
  state.intr_type = intr_type;
  return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num) {
  auto it = g_gpio_states.find(gpio_num);
  if (it == g_gpio_states.end()) {
    return 0;
  }
  return it->second.level;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, int level) {
  GpioState& state = g_gpio_states[gpio_num];
  state.level = level;
  return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args) {
  GpioState& state = g_gpio_states[gpio_num];
  state.handler = isr_handler;
  state.arg = args;
  return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num) {
  auto it = g_gpio_states.find(gpio_num);
  if (it == g_gpio_states.end()) {
    return ESP_OK;
  }
  it->second.handler = nullptr;
  it->second.arg = nullptr;
  return ESP_OK;
}

esp_err_t gpio_install_isr_service(int) {
  if (g_gpio_service_installed) {
    return ESP_ERR_INVALID_STATE;
  }
  if (g_gpio_install_result == ESP_OK) {
    g_gpio_service_installed = true;
  }
  return g_gpio_install_result;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task_func,
                                   const char*,
                                   uint32_t,
                                   void* params,
                                   UBaseType_t,
                                   TaskHandle_t* out_handle,
                                   BaseType_t) {
  if (task_func == nullptr) {
    return pdFAIL;
  }
  auto task = std::make_unique<FakeTask>();
  task->func = task_func;
  task->param = params;
  TaskHandle_t handle = reinterpret_cast<TaskHandle_t>(task.get());
  if (out_handle != nullptr) {
    *out_handle = handle;
  }
  g_fake_tasks[handle] = std::move(task);
  return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
  if (task == nullptr) {
    return;
  }
  auto it = g_fake_tasks.find(task);
  if (it != g_fake_tasks.end()) {
    g_fake_tasks.erase(it);
  }
}

void vTaskDelay(uint32_t) {}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t* config, i2c_master_bus_handle_t* handle) {
  if (config == nullptr || handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto bus = std::make_unique<i2c_master_bus_t>();
  bus->config = *config;
  i2c_master_bus_handle_t raw = reinterpret_cast<i2c_master_bus_handle_t>(bus.get());
  g_i2c_buses[raw] = std::move(bus);
  *handle = raw;
  return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_i2c_buses.find(handle);
  if (it != g_i2c_buses.end()) {
    g_i2c_buses.erase(it);
  }
  return ESP_OK;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t* config,
                          i2s_chan_handle_t* tx_out,
                          i2s_chan_handle_t* rx_out) {
  if (config == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (tx_out != nullptr) {
    auto channel = std::make_unique<i2s_channel>();
    channel->chan_cfg = *config;
    i2s_chan_handle_t raw = reinterpret_cast<i2s_chan_handle_t>(channel.get());
    g_i2s_channels[raw] = std::move(channel);
    *tx_out = raw;
  }
  if (rx_out != nullptr) {
    *rx_out = nullptr;
  }
  return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t* std_cfg) {
  if (handle == nullptr || std_cfg == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_i2s_channels.find(handle);
  if (it == g_i2s_channels.end()) {
    return ESP_ERR_INVALID_STATE;
  }
  it->second->std_cfg = *std_cfg;
  it->second->std_initialized = true;
  return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_i2s_channels.find(handle);
  if (it == g_i2s_channels.end()) {
    return ESP_ERR_INVALID_STATE;
  }
  it->second->enabled = true;
  return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_i2s_channels.find(handle);
  if (it == g_i2s_channels.end()) {
    return ESP_ERR_INVALID_STATE;
  }
  it->second->enabled = false;
  return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_i2s_channels.find(handle);
  if (it != g_i2s_channels.end()) {
    g_i2s_channels.erase(it);
  }
  return ESP_OK;
}

esp_err_t esp_io_expander_new_i2c_tca95xx_16bit(void*, uint32_t address, esp_io_expander_handle_t* handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto expander = std::make_unique<esp_io_expander>();
  expander->address = address;
  esp_io_expander_handle_t raw = reinterpret_cast<esp_io_expander_handle_t>(expander.get());
  g_io_expanders[raw] = std::move(expander);
  *handle = raw;
  return ESP_OK;
}

esp_err_t esp_io_expander_del(esp_io_expander_handle_t handle) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_io_expanders.find(handle);
  if (it != g_io_expanders.end()) {
    g_io_expanders.erase(it);
  }
  return ESP_OK;
}

esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t handle, uint32_t mask, esp_io_expander_direction_t) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_io_expanders.find(handle);
  if (it == g_io_expanders.end()) {
    return ESP_ERR_INVALID_STATE;
  }
  it->second->direction_mask = mask;
  return ESP_OK;
}

esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle, uint32_t mask, uint32_t level) {
  if (handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto it = g_io_expanders.find(handle);
  if (it == g_io_expanders.end()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (level == 0) {
    it->second->level_mask &= ~mask;
  } else {
    it->second->level_mask |= mask;
  }
  return ESP_OK;
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t, nvs_handle_t* out_handle) {
  if (name == nullptr || out_handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto& entry = g_nvs_namespaces[name];
  if (!entry) {
    entry = std::make_shared<FakeNvsNamespace>();
  }
  auto* handle = new FakeNvsHandle();
  handle->ns = entry;
  *out_handle = reinterpret_cast<nvs_handle_t>(handle);
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  delete h;
}

esp_err_t nvs_set_i32(nvs_handle_t handle, const char* key, int32_t value) {
  if (handle == nullptr || key == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto& entry = h->ns->values[key];
  entry.kind = FakeNvsNamespace::Value::Kind::kI32;
  entry.number = value;
  entry.string_value.clear();
  return ESP_OK;
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char* key, int32_t* out_value) {
  if (handle == nullptr || key == nullptr || out_value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto it = h->ns->values.find(key);
  if (it == h->ns->values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (it->second.kind != FakeNvsNamespace::Value::Kind::kI32) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_value = static_cast<int32_t>(it->second.number);
  return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
  if (handle == nullptr || key == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto& entry = h->ns->values[key];
  entry.kind = FakeNvsNamespace::Value::Kind::kU8;
  entry.number = value;
  entry.string_value.clear();
  return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out_value) {
  if (handle == nullptr || key == nullptr || out_value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto it = h->ns->values.find(key);
  if (it == h->ns->values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (it->second.kind != FakeNvsNamespace::Value::Kind::kU8) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_value = static_cast<uint8_t>(it->second.number);
  return ESP_OK;
}

esp_err_t nvs_set_u16(nvs_handle_t handle, const char* key, uint16_t value) {
  if (handle == nullptr || key == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto& entry = h->ns->values[key];
  entry.kind = FakeNvsNamespace::Value::Kind::kU16;
  entry.number = value;
  entry.string_value.clear();
  return ESP_OK;
}

esp_err_t nvs_get_u16(nvs_handle_t handle, const char* key, uint16_t* out_value) {
  if (handle == nullptr || key == nullptr || out_value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto it = h->ns->values.find(key);
  if (it == h->ns->values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (it->second.kind != FakeNvsNamespace::Value::Kind::kU16) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_value = static_cast<uint16_t>(it->second.number);
  return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char* key, uint32_t value) {
  if (handle == nullptr || key == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto& entry = h->ns->values[key];
  entry.kind = FakeNvsNamespace::Value::Kind::kU32;
  entry.number = value;
  entry.string_value.clear();
  return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char* key, uint32_t* out_value) {
  if (handle == nullptr || key == nullptr || out_value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto it = h->ns->values.find(key);
  if (it == h->ns->values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (it->second.kind != FakeNvsNamespace::Value::Kind::kU32) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_value = static_cast<uint32_t>(it->second.number);
  return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value) {
  if (handle == nullptr || key == nullptr || value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto& entry = h->ns->values[key];
  entry.kind = FakeNvsNamespace::Value::Kind::kString;
  entry.string_value = value;
  entry.number = static_cast<int64_t>(entry.string_value.size());
  return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out_value, size_t* length) {
  if (handle == nullptr || key == nullptr || length == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* h = reinterpret_cast<FakeNvsHandle*>(handle);
  auto it = h->ns->values.find(key);
  if (it == h->ns->values.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (it->second.kind != FakeNvsNamespace::Value::Kind::kString) {
    return ESP_ERR_INVALID_ARG;
  }

  const std::string& stored = it->second.string_value;
  const size_t required = stored.size() + 1;  // Include null terminator.

  if (out_value == nullptr || *length < required) {
    *length = required;
    if (out_value == nullptr) {
      return ESP_OK;
    }
    return ESP_ERR_NVS_INVALID_LENGTH;
  }

  std::snprintf(out_value, *length, "%s", stored.c_str());
  *length = required;
  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  (void)handle;
  return ESP_OK;
}

esp_err_t led_strip_new_rmt_device(const led_strip_config_t* config,
                                   const led_strip_rmt_config_t*,
                                   led_strip_handle_t* out_handle) {
  if (config == nullptr || out_handle == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  auto strip = std::make_unique<FakeLedStrip>();
  strip->config = *config;
  strip->pixels.assign(config->max_leds, {0, 0, 0});
  auto* raw_ptr = reinterpret_cast<led_strip_handle_t>(static_cast<intptr_t>(g_next_strip_id++));
  g_led_strips.emplace(raw_ptr, std::move(strip));
  *out_handle = raw_ptr;
  return ESP_OK;
}

esp_err_t led_strip_del(led_strip_handle_t handle) {
  g_led_strips.erase(handle);
  return ESP_OK;
}

esp_err_t led_strip_clear(led_strip_handle_t handle) {
  FakeLedStrip* strip = GetStrip(handle);
  if (strip == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  for (auto& px : strip->pixels) {
    px = {0, 0, 0};
  }
  return ESP_OK;
}

esp_err_t led_strip_set_pixel(led_strip_handle_t handle,
                              uint32_t index,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue) {
  FakeLedStrip* strip = GetStrip(handle);
  if (strip == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (index >= strip->pixels.size()) {
    return ESP_ERR_INVALID_ARG;
  }
  strip->pixels[index] = {red, green, blue};
  return ESP_OK;
}

esp_err_t led_strip_refresh(led_strip_handle_t handle) {
  FakeLedStrip* strip = GetStrip(handle);
  if (strip == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  ++strip->refresh_count;
  return ESP_OK;
}

}  // extern "C"

// Test control helpers -------------------------------------------------------

void fake_esp_reset_time() {
  g_fake_time_us = 0;
}

void fake_esp_timer_set_time(int64_t time_us) {
  g_fake_time_us = time_us;
}

void fake_esp_timer_advance(int64_t delta_us) {
  g_fake_time_us += delta_us;
}

void fake_gpio_reset() {
  g_gpio_states.clear();
  g_gpio_service_installed = false;
  g_gpio_install_result = ESP_OK;
}

void fake_gpio_set_level(gpio_num_t gpio, int level) {
  g_gpio_states[gpio].level = level;
}

FakeGpioStateSnapshot fake_gpio_snapshot(gpio_num_t gpio) {
  FakeGpioStateSnapshot snapshot{};
  auto it = g_gpio_states.find(gpio);
  if (it != g_gpio_states.end()) {
    const GpioState& state = it->second;
    snapshot.configured = state.configured;
    snapshot.config = state.config;
    snapshot.intr_type = state.intr_type;
    snapshot.level = state.level;
    snapshot.isr_installed = state.handler != nullptr;
  }
  return snapshot;
}

void fake_gpio_trigger(gpio_num_t gpio) {
  auto it = g_gpio_states.find(gpio);
  if (it == g_gpio_states.end()) {
    return;
  }
  const GpioState& state = it->second;
  if (state.handler != nullptr) {
    state.handler(state.arg);
  }
}

void fake_gpio_set_install_result(esp_err_t result) {
  g_gpio_install_result = result;
  g_gpio_service_installed = false;
}

void fake_nvs_reset() {
  g_nvs_namespaces.clear();
}

std::vector<FakeNvsSnapshotEntry> fake_nvs_snapshot(const std::string& ns_name) {
  std::vector<FakeNvsSnapshotEntry> snapshot;
  auto it = g_nvs_namespaces.find(ns_name);
  if (it == g_nvs_namespaces.end()) {
    return snapshot;
  }
  for (const auto& [key, value] : it->second->values) {
    FakeNvsSnapshotEntry entry{};
    entry.key = key;
    entry.value = value.number;
    entry.is_u8 = (value.kind == FakeNvsNamespace::Value::Kind::kU8);
    entry.is_string = (value.kind == FakeNvsNamespace::Value::Kind::kString);
    if (entry.is_string) {
      entry.string_value = value.string_value;
    }
    snapshot.push_back(std::move(entry));
  }
  return snapshot;
}

void fake_led_strip_reset() {
  g_led_strips.clear();
  g_next_strip_id = 1;
}

FakeLedStripSnapshot fake_led_strip_snapshot(led_strip_handle_t handle) {
  FakeLedStripSnapshot snapshot{};
  FakeLedStrip* strip = GetStrip(handle);
  if (strip == nullptr) {
    return snapshot;
  }
  snapshot.led_count = strip->pixels.size();
  snapshot.pixels = strip->pixels;
  snapshot.refresh_count = strip->refresh_count;
  return snapshot;
}

void fake_esp_idf_reset() {
  fake_esp_reset_time();
  fake_gpio_reset();
  fake_nvs_reset();
  fake_led_strip_reset();
  g_i2c_buses.clear();
  g_i2s_channels.clear();
  g_fake_tasks.clear();
  g_io_expanders.clear();
}

std::vector<led_strip_handle_t> fake_led_strip_handles() {
  std::vector<led_strip_handle_t> handles;
  handles.reserve(g_led_strips.size());
  for (const auto& entry : g_led_strips) {
    handles.push_back(entry.first);
  }
  return handles;
}

// ============================================================================
// cJSON: Using real ESP-IDF implementation (included in CMakeLists.txt)
// ============================================================================
// NOTE: cJSON stubs removed - conflicted with ESP-IDF's cJSON.c
// Real implementation provided by: /opt/esp/idf/components/json/cJSON/cJSON.c

// ============================================================================
// ApplicationController Stubs (for init_pipeline.cpp testing)
// ============================================================================

#include <cstdlib>
#include "esp_err.h"

namespace app {

// Forward declaration to avoid including full application_controller.hpp
class ApplicationController {
 public:
  [[noreturn]] static void FatalInitError(const char* subsystem, esp_err_t error_code);
};

// Stub implementation
[[noreturn]] void ApplicationController::FatalInitError(const char* subsystem, esp_err_t error_code) {
  fprintf(stderr, "[FATAL] Initialization error in %s: 0x%x\n", subsystem, error_code);
  std::exit(1);
}

}  // namespace app

// ============================================================================
// InitializationPipeline Stubs (for init_pipeline_test.cpp)
// ============================================================================

#include <memory>

namespace app {

// Forward declarations
class InitPhase {
 public:
  virtual ~InitPhase() = default;
};

class InitializationPipeline {
 public:
  void AddPhase(std::unique_ptr<InitPhase> phase) {
    // Stub: do nothing
    (void)phase;
  }

  bool Execute() {
    // Stub: always return true
    return true;
  }
};

}  // namespace app

// ============================================================================
// lwip Socket Stubs (for DNS server testing)
// ============================================================================

extern "C" {

int lwip_socket(int domain, int type, int protocol) {
  // Stub: return fake socket fd
  (void)domain;
  (void)type;
  (void)protocol;
  return 3;  // Fake socket file descriptor
}

int lwip_bind(int s, const struct sockaddr* name, socklen_t namelen) {
  // Stub: always succeed
  (void)s;
  (void)name;
  (void)namelen;
  return 0;
}

ssize_t lwip_recvfrom(int s, void* mem, size_t len, int flags,
                      struct sockaddr* from, socklen_t* fromlen) {
  // Stub: return -1 (no data available)
  (void)s;
  (void)mem;
  (void)len;
  (void)flags;
  (void)from;
  (void)fromlen;
  return -1;
}

ssize_t lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                    const struct sockaddr* to, socklen_t tolen) {
  // Stub: always succeed
  (void)s;
  (void)dataptr;
  (void)flags;
  (void)to;
  (void)tolen;
  return static_cast<ssize_t>(size);
}

int lwip_close(int s) {
  // Stub: always succeed
  (void)s;
  return 0;
}

int lwip_fcntl(int s, int cmd, int val) {
  // Stub: always succeed
  (void)s;
  (void)cmd;
  (void)val;
  return 0;
}

}  // extern "C"

// ============================================================================
// esp_http_server Stubs (for minimal HTTP server testing)
// ============================================================================

#include "esp_http_server.h"

extern "C" {

esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
  // Stub: always fail (not implemented for host tests)
  (void)handle;
  (void)config;
  return ESP_FAIL;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
  // Stub: always succeed
  (void)handle;
  return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t* uri_handler) {
  // Stub: always succeed
  (void)handle;
  (void)uri_handler;
  return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t* r, const char* type) {
  // Stub: always succeed
  (void)r;
  (void)type;
  return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t* r, const char* status) {
  // Stub: always succeed
  (void)r;
  (void)status;
  return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t* r, const char* field, const char* value) {
  // Stub: always succeed
  (void)r;
  (void)field;
  (void)value;
  return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t* r, const char* buf, ssize_t buf_len) {
  // Stub: always succeed
  (void)r;
  (void)buf;
  (void)buf_len;
  return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t* req, httpd_err_code_t error, const char* msg) {
  // Stub: always succeed
  (void)req;
  (void)error;
  (void)msg;
  return ESP_OK;
}

int httpd_req_recv(httpd_req_t* r, char* buf, size_t buf_len) {
  // Stub: return -1 (no data available)
  (void)r;
  (void)buf;
  (void)buf_len;
  return -1;
}

}  // extern "C"

// ============================================================================
// esp_restart Stub (for reboot testing)
// ============================================================================

extern "C" {

[[noreturn]] void esp_restart(void) {
  // Stub: print message and exit
  fprintf(stderr, "[STUB] esp_restart() called - would reboot device\n");
  std::exit(0);
}

}  // extern "C"

// ============================================================================
// WiFi Scan API Stubs (for WiFi subsystem testing)
// ============================================================================

extern "C" {

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t* config, bool block) {
  // Stub: simulate successful scan start
  (void)config;
  (void)block;
  fprintf(stderr, "[STUB] esp_wifi_scan_start() called\n");
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t* number) {
  // Stub: return 3 mock networks
  if (number == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  *number = 3;
  fprintf(stderr, "[STUB] esp_wifi_scan_get_ap_num() -> 3 networks\n");
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t* number, wifi_ap_record_t* ap_records) {
  // Stub: return 3 mock WiFi networks
  if (number == nullptr || ap_records == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  const uint16_t mock_count = 3;
  if (*number < mock_count) {
    return ESP_ERR_INVALID_SIZE;
  }

  // Mock network 1: "TestNetwork1" (WPA2, -40 dBm, channel 6)
  memset(&ap_records[0], 0, sizeof(wifi_ap_record_t));
  strcpy(reinterpret_cast<char*>(ap_records[0].ssid), "TestNetwork1");
  ap_records[0].primary = 6;
  ap_records[0].rssi = -40;
  ap_records[0].authmode = WIFI_AUTH_WPA2_PSK;

  // Mock network 2: "TestNetwork2" (Open, -60 dBm, channel 1)
  memset(&ap_records[1], 0, sizeof(wifi_ap_record_t));
  strcpy(reinterpret_cast<char*>(ap_records[1].ssid), "TestNetwork2");
  ap_records[1].primary = 1;
  ap_records[1].rssi = -60;
  ap_records[1].authmode = WIFI_AUTH_OPEN;

  // Mock network 3: "TestNetwork3" (WPA3, -50 dBm, channel 11)
  memset(&ap_records[2], 0, sizeof(wifi_ap_record_t));
  strcpy(reinterpret_cast<char*>(ap_records[2].ssid), "TestNetwork3");
  ap_records[2].primary = 11;
  ap_records[2].rssi = -50;
  ap_records[2].authmode = WIFI_AUTH_WPA3_PSK;

  *number = mock_count;
  fprintf(stderr, "[STUB] esp_wifi_scan_get_ap_records() -> 3 mock networks\n");
  return ESP_OK;
}

esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* sta) {
  // Stub: return 2 connected clients
  if (sta == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(sta, 0, sizeof(wifi_sta_list_t));
  sta->num = 2;  // 2 mock connected clients

  fprintf(stderr, "[STUB] esp_wifi_ap_get_sta_list() -> 2 clients\n");
  return ESP_OK;
}

}  // extern "C"
