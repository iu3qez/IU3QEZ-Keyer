/**
 * @file init_pipeline.cpp
 * @brief Implementation of InitializationPipeline (orchestrator/framework)
 *
 * Provides sequential execution of initialization phases with error handling
 * and logging. The concrete phase implementations are in init_phases.cpp.
 * See init_phase.hpp for architecture rationale.
 */

#include "app/init_phase.hpp"
#include "app/application_controller.hpp"  // For ApplicationController::FatalInitError
#include "esp_log.h"

namespace app {

namespace {
constexpr const char* kLogTag = "init_pipeline";
}

void InitializationPipeline::AddPhase(std::unique_ptr<InitPhase> phase) {
  phases_.push_back(std::move(phase));
}

bool InitializationPipeline::Execute() {
  ESP_LOGI(kLogTag, "Starting initialization pipeline (%zu phases registered)",
           phases_.size());

  for (size_t i = 0; i < phases_.size(); ++i) {
    const auto& phase = phases_[i];
    const char* phase_name = phase->GetName();

    ESP_LOGI(kLogTag, "Executing phase %zu/%zu: %s", i + 1, phases_.size(), phase_name);

    esp_err_t err = phase->Execute();
    if (err != ESP_OK) {
      HandlePhaseError(*phase, err);
      // If phase was critical, HandlePhaseError() calls FatalInitError() which aborts
      // If we reach here, phase was non-critical and boot continues
    }
  }

  ESP_LOGI(kLogTag, "Initialization pipeline completed successfully");
  return true;
}

void InitializationPipeline::HandlePhaseError(const InitPhase& phase, esp_err_t error) {
  const char* phase_name = phase.GetName();

  if (phase.IsCritical()) {
    // Critical phase failure: Log error and abort boot
    ESP_LOGE(kLogTag, "CRITICAL phase '%s' failed: %s (0x%x)",
             phase_name, esp_err_to_name(error), error);
    ApplicationController::FatalInitError(phase_name, error);  // Does not return (calls abort())
  } else {
    // Non-critical phase failure: Log error and continue
    ESP_LOGE(kLogTag, "Non-critical phase '%s' failed: %s (continuing)",
             phase_name, esp_err_to_name(error));
  }
}

}  // namespace app
