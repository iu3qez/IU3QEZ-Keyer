/**
 * @file system_monitor.cpp
 * @brief System Monitor implementation - CPU, task, and memory statistics
 */

#include "system_monitor/system_monitor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace system_monitor {

namespace {

// Buffer sizes for FreeRTOS statistics
// Use heap allocation to avoid stack overflow in calling tasks
constexpr size_t kTaskStatsBufferSize = 2048;
constexpr size_t kTaskListBufferSize = 2048;
constexpr size_t kMaxTasks = 24;  // Reduced from 32 to limit heap usage

}  // namespace

std::string SystemMonitor::GetCpuStatsFormatted() const {
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  // Allocate buffers on HEAP to avoid stack overflow
  char* buffer = static_cast<char*>(malloc(kTaskStatsBufferSize));
  if (buffer == nullptr) {
    return "Error: Failed to allocate memory for CPU stats\n";
  }

  vTaskGetRunTimeStats(buffer);

  // Check if buffer is valid
  if (buffer[0] == '\0') {
    free(buffer);
    return "No task statistics available\n";
  }

  // Make a copy BEFORE any strtok calls
  char* buffer_copy = static_cast<char*>(malloc(kTaskStatsBufferSize));
  if (buffer_copy == nullptr) {
    free(buffer);
    return "Error: Failed to allocate memory for CPU stats\n";
  }

  strncpy(buffer_copy, buffer, kTaskStatsBufferSize - 1);
  buffer_copy[kTaskStatsBufferSize - 1] = '\0';

  std::ostringstream oss;
  oss << "Task Name              Runtime (us)      CPU %\n";
  oss << "──────────────────────────────────────────────────\n";

  // First pass: calculate total runtime
  uint64_t total_runtime = 0;
  char* temp_line = strtok(buffer_copy, "\n");

  while (temp_line != nullptr) {
    char task_name[configMAX_TASK_NAME_LEN + 1];
    uint32_t runtime;
    uint32_t percentage;

    // Limit string length to prevent buffer overflow
    int parsed = sscanf(temp_line, "%15s\t%lu\t%lu%%", task_name, &runtime, &percentage);
    if (parsed == 3) {
      total_runtime += runtime;
    }
    temp_line = strtok(nullptr, "\n");
  }

  free(buffer_copy);  // Done with copy

  // Second pass: format output with recalculated percentages
  // Use original buffer (now we can tokenize it)
  char* line = strtok(buffer, "\n");

  while (line != nullptr) {
    char task_name[configMAX_TASK_NAME_LEN + 1];
    uint32_t runtime;
    uint32_t percentage;

    int parsed = sscanf(line, "%15s\t%lu\t%lu%%", task_name, &runtime, &percentage);
    if (parsed == 3) {
      // Calculate percentage as (task_runtime / sum_of_all_runtimes) * 100 * num_cores
      // On multi-core systems, scale by number of cores so each core can show up to 100%
      double cpu_percent = total_runtime > 0 ? (runtime * 100.0 * portNUM_PROCESSORS / total_runtime) : 0.0;

      oss << std::left << std::setw(22) << task_name
          << std::right << std::setw(12) << runtime
          << std::setw(12) << std::fixed << std::setprecision(1) << cpu_percent << "%\n";
    }
    line = strtok(nullptr, "\n");
  }

  free(buffer);  // Done with buffer

  oss << "──────────────────────────────────────────────────\n";
  oss << "Total CPU Time: " << total_runtime << " us\n";

  return oss.str();
#else
  return "CPU statistics not available (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS not enabled)\n";
#endif
}

std::string SystemMonitor::GetTaskListFormatted() const {
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
  // Allocate buffer on HEAP to avoid stack overflow
  char* buffer = static_cast<char*>(malloc(kTaskListBufferSize));
  if (buffer == nullptr) {
    return "Error: Failed to allocate memory for task list\n";
  }

  vTaskList(buffer);

  // Check if buffer is valid
  if (buffer[0] == '\0') {
    free(buffer);
    return "No task list available\n";
  }

  std::ostringstream oss;
  oss << "Task Name              State   Prio   Stack HWM   Core\n";
  oss << "───────────────────────────────────────────────────────\n";

  // Parse and reformat the output
  // Format from vTaskList: "TaskName\tState\tPrio\tStackHWM\tTaskNum\n"
  char* line = strtok(buffer, "\n");

  while (line != nullptr) {
    char task_name[configMAX_TASK_NAME_LEN + 1];
    char state;
    uint32_t priority;
    uint32_t stack_hwm;
    uint32_t task_num;

    // Limit string length to prevent buffer overflow
    int parsed = sscanf(line, "%15s\t%c\t%lu\t%lu\t%lu", task_name, &state, &priority, &stack_hwm, &task_num);
    if (parsed == 5) {
      // Try to get core affinity (ESP-IDF specific)
      // For now, show "-" as we don't have easy access to core affinity from vTaskList
      oss << std::left << std::setw(22) << task_name
          << std::setw(8) << state
          << std::right << std::setw(6) << priority
          << std::setw(12) << stack_hwm
          << std::setw(8) << "-" << "\n";
    }
    line = strtok(nullptr, "\n");
  }

  free(buffer);  // Done with buffer

  oss << "\nStates: R=Running, B=Blocked, S=Suspended, D=Deleted\n";
  oss << "Stack HWM = High Water Mark (bytes free)\n";
  oss << "Core: - = Not available from vTaskList\n";

  return oss.str();
#else
  return "Task list not available (CONFIG_FREERTOS_USE_TRACE_FACILITY not enabled)\n";
#endif
}

HeapInfo SystemMonitor::GetHeapInfo() const {
  HeapInfo info;
  info.free_bytes = esp_get_free_heap_size();
  info.minimum_free_bytes = esp_get_minimum_free_heap_size();

  // Get total heap size (sum of all regions)
  multi_heap_info_t heap_info;
  heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
  info.total_bytes = heap_info.total_free_bytes + heap_info.total_allocated_bytes;
  info.largest_free_block = heap_info.largest_free_block;

  return info;
}

UptimeInfo SystemMonitor::GetUptimeInfo() const {
  UptimeInfo info;
  info.uptime_us = esp_timer_get_time();
  info.uptime_seconds = info.uptime_us / 1000000;
  info.uptime_minutes = info.uptime_seconds / 60;
  info.uptime_hours = info.uptime_minutes / 60;
  return info;
}

std::string SystemMonitor::GetSystemStatsJson() const {
  std::ostringstream json;
  json << "{\n";

  // Uptime
  auto uptime = GetUptimeInfo();
  json << "  \"uptime\": {\n";
  json << "    \"microseconds\": " << uptime.uptime_us << ",\n";
  json << "    \"seconds\": " << uptime.uptime_seconds << ",\n";
  json << "    \"minutes\": " << uptime.uptime_minutes << ",\n";
  json << "    \"hours\": " << uptime.uptime_hours << "\n";
  json << "  },\n";

  // Heap
  auto heap = GetHeapInfo();
  json << "  \"heap\": {\n";
  json << "    \"free_bytes\": " << heap.free_bytes << ",\n";
  json << "    \"minimum_free_bytes\": " << heap.minimum_free_bytes << ",\n";
  json << "    \"total_bytes\": " << heap.total_bytes << ",\n";
  json << "    \"largest_free_block\": " << heap.largest_free_block << ",\n";
  json << "    \"fragmentation_percent\": " << std::fixed << std::setprecision(1)
       << (heap.free_bytes > 0 ? (100.0 * (1.0 - (double)heap.largest_free_block / heap.free_bytes)) : 0.0) << "\n";
  json << "  },\n";

  // Tasks with CPU stats
  json << "  \"tasks\": [\n";

#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
  // Allocate task status array on HEAP to avoid stack overflow
  TaskStatus_t* task_status = static_cast<TaskStatus_t*>(malloc(kMaxTasks * sizeof(TaskStatus_t)));
  if (task_status == nullptr) {
    json << "  ],\n";
    json << "  \"cpu_cores\": " << portNUM_PROCESSORS << ",\n";
    json << "  \"error\": \"Failed to allocate memory for task status\"\n";
    json << "}\n";
    return json.str();
  }

  uint32_t total_runtime = 0;
  UBaseType_t num_tasks = uxTaskGetSystemState(task_status, kMaxTasks, &total_runtime);

  // Clamp to max tasks to prevent array overflow
  if (num_tasks > kMaxTasks) {
    num_tasks = kMaxTasks;
  }

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  // Calculate sum of all task runtimes for percentage calculation
  // NOTE: total_runtime from uxTaskGetSystemState() is NOT the sum of task runtimes!
  // It's the total system time, which doesn't account for multiple cores running in parallel.
  // We must sum all task runtimes manually to get correct percentages.
  uint64_t sum_of_task_runtimes = 0;
  for (UBaseType_t i = 0; i < num_tasks; i++) {
    sum_of_task_runtimes += task_status[i].ulRunTimeCounter;
  }
#endif

  bool first_task = true;
  for (UBaseType_t i = 0; i < num_tasks; i++) {
    // Skip if task name is NULL (safety check)
    if (task_status[i].pcTaskName == nullptr) {
      continue;
    }

    // Add comma before all tasks except the first
    if (!first_task) {
      json << ",\n";
    }
    first_task = false;

    json << "    {\n";
    json << "      \"name\": \"" << JsonEscape(task_status[i].pcTaskName) << "\",\n";

    // State
    const char* state_str = TaskStateToString(task_status[i].eCurrentState);
    json << "      \"state\": \"" << state_str << "\",\n";

    json << "      \"priority\": " << task_status[i].uxCurrentPriority << ",\n";
    json << "      \"stack_hwm\": " << task_status[i].usStackHighWaterMark << ",\n";
    json << "      \"task_number\": " << task_status[i].xTaskNumber << ",\n";

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    json << "      \"runtime_us\": " << task_status[i].ulRunTimeCounter << ",\n";
    // Calculate percentage as (task_runtime / sum_of_all_task_runtimes) * 100 * num_cores
    // On multi-core systems, scale by number of cores so each core can show up to 100%
    // Example: On dual-core, IDLE0 at ~50% of sum becomes ~100% (fully idle on core 0)
    // Total of all tasks can reach (num_cores * 100%) when all cores are fully utilized
    double cpu_percent = sum_of_task_runtimes > 0 ? (task_status[i].ulRunTimeCounter * 100.0 * portNUM_PROCESSORS / sum_of_task_runtimes) : 0.0;
    json << "      \"cpu_percent\": " << std::fixed << std::setprecision(2) << cpu_percent << "\n";
#else
    json << "      \"runtime_us\": null,\n";
    json << "      \"cpu_percent\": null\n";
#endif

    json << "    }";
  }
  json << "\n";

  free(task_status);  // Done with task status
#endif

  json << "  ],\n";

  // CPU cores
  json << "  \"cpu_cores\": " << portNUM_PROCESSORS << "\n";

  json << "}\n";

  return json.str();
}

uint32_t SystemMonitor::GetUptimeSeconds() const {
  return GetUptimeInfo().uptime_seconds;
}

uint64_t SystemMonitor::GetTotalRuntime() const {
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  uint32_t total_runtime = 0;
  TaskStatus_t task_status[kMaxTasks];
  uxTaskGetSystemState(task_status, kMaxTasks, &total_runtime);
  return total_runtime;
#else
  return 0;
#endif
}

const char* SystemMonitor::TaskStateToString(char state) {
  switch (state) {
    case 'R': return "Running";
    case 'B': return "Blocked";
    case 'S': return "Suspended";
    case 'D': return "Deleted";
    default: return "Unknown";
  }
}

std::string SystemMonitor::JsonEscape(const std::string& input) {
  std::ostringstream oss;
  for (char c : input) {
    switch (c) {
      case '"':  oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (c < 0x20) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
        } else {
          oss << c;
        }
        break;
    }
  }
  return oss.str();
}

}  // namespace system_monitor
