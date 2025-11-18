#pragma once

/**
 * @file system_monitor.hpp
 * @brief System Monitor - CPU usage, task statistics, and memory monitoring
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Provides comprehensive runtime statistics for CPU usage, task states,
 * and memory consumption. Encapsulates FreeRTOS trace facility APIs
 * and ESP-IDF system information for diagnostics and performance analysis.
 *
 * RESPONSIBILITIES:
 * - Collect FreeRTOS runtime statistics (CPU usage per task)
 * - Collect task snapshot information (state, priority, stack)
 * - Monitor heap memory usage (free, minimum free, fragmentation)
 * - Format statistics as human-readable text or JSON
 *
 * THREAD SAFETY:
 * - All methods are safe to call from main loop context
 * - Do NOT call from ISR context (uses FreeRTOS trace APIs)
 *
 * USAGE PATTERN:
 * ```
 * SystemMonitor monitor;
 *
 * // Get CPU statistics as formatted text
 * std::string cpu_stats = monitor.GetCpuStatsFormatted();
 *
 * // Get task list as formatted text
 * std::string task_list = monitor.GetTaskListFormatted();
 *
 * // Get all stats as JSON
 * std::string json = monitor.GetSystemStatsJson();
 *
 * // Get individual metrics
 * auto heap_info = monitor.GetHeapInfo();
 * auto uptime = monitor.GetUptimeSeconds();
 * ```
 */

#include <cstdint>
#include <string>

namespace system_monitor {

/**
 * @brief Heap memory information
 */
struct HeapInfo {
  uint32_t free_bytes;           // Current free heap
  uint32_t minimum_free_bytes;   // Minimum free heap since boot (water mark)
  uint32_t total_bytes;          // Total heap size
  uint32_t largest_free_block;   // Largest contiguous free block
};

/**
 * @brief System uptime information
 */
struct UptimeInfo {
  uint64_t uptime_us;       // Uptime in microseconds
  uint32_t uptime_seconds;  // Uptime in seconds
  uint32_t uptime_minutes;  // Uptime in minutes
  uint32_t uptime_hours;    // Uptime in hours
};

/**
 * @brief System monitor for CPU, task, and memory statistics
 */
class SystemMonitor {
 public:
  SystemMonitor() = default;
  ~SystemMonitor() = default;

  // Disable copy/move (stateless singleton pattern)
  SystemMonitor(const SystemMonitor&) = delete;
  SystemMonitor& operator=(const SystemMonitor&) = delete;

  /**
   * @brief Get CPU runtime statistics as formatted text
   *
   * Output format (example):
   * ```
   * Task Name         Runtime (us)    CPU %
   * ──────────────────────────────────────────
   * main              12345678        45.2%
   * wifi_task         5678901         20.8%
   * IDLE0             4567890         16.7%
   * IDLE1             4567890         16.7%
   * Tmr Svc           150000          0.6%
   * ──────────────────────────────────────────
   * Total CPU Time: 27310359 us
   * ```
   *
   * @return Formatted CPU statistics string (empty if stats unavailable)
   */
  std::string GetCpuStatsFormatted() const;

  /**
   * @brief Get task list snapshot as formatted text
   *
   * Output format (example):
   * ```
   * Task Name         State   Prio   Stack HWM   Core
   * ─────────────────────────────────────────────────
   * main              R       1      512         0
   * wifi_task         B       5      1024        1
   * IDLE0             R       0      256         0
   * IDLE1             R       0      256         1
   * Tmr Svc           B       1      384         -
   *
   * States: R=Running, B=Blocked, S=Suspended, D=Deleted
   * Stack HWM = High Water Mark (bytes free)
   * Core: -1 = No affinity
   * ```
   *
   * @return Formatted task list string (empty if trace facility unavailable)
   */
  std::string GetTaskListFormatted() const;

  /**
   * @brief Get heap memory information
   *
   * @return HeapInfo structure with current heap statistics
   */
  HeapInfo GetHeapInfo() const;

  /**
   * @brief Get system uptime
   *
   * @return UptimeInfo structure with uptime in various units
   */
  UptimeInfo GetUptimeInfo() const;

  /**
   * @brief Get complete system statistics as JSON
   *
   * JSON format:
   * ```json
   * {
   *   "uptime": {
   *     "seconds": 12345,
   *     "minutes": 205,
   *     "hours": 3
   *   },
   *   "heap": {
   *     "free_bytes": 123456,
   *     "minimum_free_bytes": 100000,
   *     "total_bytes": 327680,
   *     "largest_free_block": 65536
   *   },
   *   "tasks": [
   *     {
   *       "name": "main",
   *       "state": "R",
   *       "priority": 1,
   *       "stack_hwm": 512,
   *       "core": 0,
   *       "runtime_us": 12345678,
   *       "cpu_percent": 45.2
   *     },
   *     ...
   *   ],
   *   "cpu_cores": 2
   * }
   * ```
   *
   * @return JSON string with complete system statistics
   */
  std::string GetSystemStatsJson() const;

  /**
   * @brief Get uptime in seconds (convenience method)
   *
   * @return System uptime in seconds
   */
  uint32_t GetUptimeSeconds() const;

 private:
  /**
   * @brief Get total runtime across all tasks (for percentage calculation)
   *
   * @return Total runtime in microseconds, or 0 if unavailable
   */
  uint64_t GetTotalRuntime() const;

  /**
   * @brief Convert task state character to human-readable string
   *
   * @param state Task state character (R/B/S/D)
   * @return State description string
   */
  static const char* TaskStateToString(char state);

  /**
   * @brief Escape JSON string (handle quotes, backslashes, etc.)
   *
   * @param input Input string
   * @return JSON-escaped string
   */
  static std::string JsonEscape(const std::string& input);
};

}  // namespace system_monitor
