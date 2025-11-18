#pragma once

/**
 * @file init_phase.hpp
 * @brief Initialization Pipeline Infrastructure - Base classes for modular boot sequence
 *
 * ARCHITECTURE RATIONALE:
 * =======================
 * Refactors ApplicationController::Initialize() from a 199-line "God Method" into a
 * composable pipeline of testable, independent initialization phases using the
 * Builder + Strategy pattern.
 *
 * PATTERN: Builder + Strategy
 * - InitPhase (Strategy): Abstract interface for each initialization step
 * - InitializationPipeline (Builder): Composes and executes phases in sequence
 *
 * BENEFITS:
 * - Testability: Each phase can be unit tested with mock dependencies
 * - Explicit dependencies: Hardware init order enforced by phase registration order
 * - Extensibility: Add new subsystems by creating new phase classes
 * - SRP compliance: Each phase has single responsibility
 * - Zero functional changes: Preserves existing boot sequence, error handling, LED signals
 *
 * USAGE PATTERN:
 * ```cpp
 * InitializationPipeline pipeline;
 * pipeline.AddPhase(std::make_unique<UartDebugPhase>());
 * pipeline.AddPhase(std::make_unique<NvsFlashPhase>());
 * // ... register all phases in dependency order ...
 * bool success = pipeline.Execute();  // Returns false if any critical phase failed
 * ```
 *
 * CRITICAL vs NON-CRITICAL PHASES:
 * - Critical phases: Hardware infrastructure (UART, NVS, Clock, Paddle HAL, Keying)
 *   → Failure aborts boot via FatalInitError()
 * - Non-critical phases: Optional features (Audio, WiFi, HTTP server)
 *   → Failure logs ESP_LOGE and continues boot
 */

#include "esp_err.h"
#include <memory>
#include <vector>

namespace app {

/**
 * @brief Abstract base class for initialization phases.
 *
 * Each phase represents a discrete step in the boot sequence (e.g., NVS init,
 * WiFi subsystem init, etc.). Phases are executed sequentially by InitializationPipeline.
 *
 * CONTRACT:
 * - Execute() called exactly once during boot
 * - GetName() used for logging and error messages
 * - IsCritical() determines error handling strategy:
 *   - true: Failure aborts boot (calls FatalInitError)
 *   - false: Failure logs error and continues
 *
 * THREAD SAFETY:
 * - All methods called from main thread context only
 * - No concurrent access (sequential execution guaranteed by pipeline)
 */
class InitPhase {
 public:
  virtual ~InitPhase() = default;

  /**
   * @brief Execute this initialization phase.
   *
   * Implementations should:
   * - Perform initialization logic (call subsystem Init(), configure hardware, etc.)
   * - Return ESP_OK on success
   * - Return appropriate ESP_ERR_* code on failure
   * - Avoid blocking indefinitely (watchdog may trigger)
   *
   * @return ESP_OK on success, error code on failure
   */
  virtual esp_err_t Execute() = 0;

  /**
   * @brief Get human-readable phase name for logging.
   *
   * Used in log messages like "Executing init phase: NVS Flash" and
   * error messages like "FATAL INIT ERROR: NVS Flash (0x101)".
   *
   * @return Phase name (null-terminated string, must outlive phase instance)
   */
  virtual const char* GetName() const = 0;

  /**
   * @brief Check if this phase is critical to boot success.
   *
   * Critical phases (UART, NVS, Clock, Config, Paddle HAL, Keying):
   * - Hardware infrastructure required for basic operation
   * - Failure aborts boot via FatalInitError() → device reboot
   *
   * Non-critical phases (Audio, WiFi, HTTP, Watchdog):
   * - Optional features, graceful degradation acceptable
   * - Failure logs ESP_LOGE and continues boot
   *
   * @return true if failure should abort boot, false if boot can continue
   */
  virtual bool IsCritical() const = 0;
};

/**
 * @brief Initialization pipeline that executes phases sequentially.
 *
 * Composes multiple InitPhase instances and executes them in registration order.
 * Handles error propagation, logging, and critical vs non-critical failure logic.
 *
 * EXECUTION FLOW:
 * 1. For each registered phase:
 *    a. Log "Executing init phase: [name]"
 *    b. Call phase->Execute()
 *    c. If failure, call HandlePhaseError()
 *       - Critical phase: FatalInitError() → abort
 *       - Non-critical: ESP_LOGE, continue
 * 2. Return true if all critical phases succeeded
 *
 * OWNERSHIP:
 * - Pipeline owns all phases (std::unique_ptr)
 * - Phases destroyed when pipeline destroyed
 *
 * THREAD SAFETY:
 * - Single-threaded (called from ApplicationController::Initialize())
 * - No concurrent AddPhase/Execute calls
 */
class InitializationPipeline {
 public:
  InitializationPipeline() = default;
  ~InitializationPipeline() = default;

  // Non-copyable (owns unique_ptr resources)
  InitializationPipeline(const InitializationPipeline&) = delete;
  InitializationPipeline& operator=(const InitializationPipeline&) = delete;

  /**
   * @brief Register an initialization phase for execution.
   *
   * Phases execute in the order they are registered. Hardware dependencies must be
   * respected (e.g., register UartDebugPhase before any phase that uses ESP_LOGI).
   *
   * DEPENDENCY ORDERING EXAMPLE:
   * 1. UartDebugPhase - Must run before any ESP_LOGI calls
   * 2. NvsFlashPhase - Must run before ConfigStoragePhase (config stored in NVS)
   * 3. ConfigStoragePhase - Must run before subsystems that read config
   * 4. DiagnosticsSubsystemPhase - Must run before LED signaling phases
   * 5. PaddleHalPhase - Must run before KeyingSubsystemPhase (keying uses paddle HAL)
   *
   * @param phase Ownership transferred to pipeline
   */
  void AddPhase(std::unique_ptr<InitPhase> phase);

  /**
   * @brief Execute all registered phases sequentially.
   *
   * Logs each phase start, handles errors via HandlePhaseError(), and returns
   * overall success status.
   *
   * BEHAVIOR:
   * - Critical phase failure: Aborts boot (FatalInitError → device reboot)
   * - Non-critical phase failure: Logs error, continues to next phase
   * - Empty pipeline (no phases): Returns true (no failures)
   *
   * @return true if all critical phases succeeded, false if any critical phase failed
   *         (Note: false return only possible if FatalInitError doesn't abort)
   */
  bool Execute();

 private:
  /**
   * @brief Handle phase execution error.
   *
   * Determines error handling strategy based on phase.IsCritical():
   * - Critical: Call FatalInitError(phase.GetName(), error) → abort, reboot
   * - Non-critical: Log ESP_LOGE with error code, allow boot to continue
   *
   * @param phase Phase that failed (used for name, criticality)
   * @param error Error code returned by phase.Execute()
   */
  void HandlePhaseError(const InitPhase& phase, esp_err_t error);

  /// Registered phases (executed in registration order)
  std::vector<std::unique_ptr<InitPhase>> phases_;
};

}  // namespace app
