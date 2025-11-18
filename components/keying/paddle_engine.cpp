#include "keying/paddle_engine.hpp"

#include <algorithm>
#include <cinttypes>
#include "esp_log.h"
#include "hal/high_precision_clock.hpp"

namespace keying {
namespace {

constexpr char kLogTag[] = "paddle_engine";
constexpr int64_t kMicrosPerSecond = 1'000'000;
constexpr float kDitSecondsPerWpm = 1.2f;

inline size_t IndexForElement(PaddleElement element) {
  return (element == PaddleElement::kDit) ? 0 : 1;
}

inline PaddleElement ElementForLine(hal::PaddleLine line) {
  return (line == hal::PaddleLine::kDah) ? PaddleElement::kDah
                                         : PaddleElement::kDit;
}

}  // namespace

bool PaddleEngine::Initialize(const PaddleEngineConfig& config,
                              const PaddleEngineCallbacks& callbacks) {
  // Task 5.4: Validate config
  if (config.speed_wpm == 0) {
    ESP_LOGE(kLogTag, "Initialize failed: speed_wpm must be > 0");
    return false;
  }

  // Validate memory window percentages
  if (config.mem_block_start_pct < 0.0f || config.mem_block_start_pct > 100.0f) {
    ESP_LOGE(kLogTag, "Initialize failed: mem_block_start_pct must be 0-100 (got %.1f)",
             config.mem_block_start_pct);
    return false;
  }
  if (config.mem_block_end_pct < 0.0f || config.mem_block_end_pct > 100.0f) {
    ESP_LOGE(kLogTag, "Initialize failed: mem_block_end_pct must be 0-100 (got %.1f)",
             config.mem_block_end_pct);
    return false;
  }
  // Ensure window is valid: start must be <= end (both are positions from element start)
  // Examples: start=0, end=100 ✓  |  start=60, end=99 ✓  |  start=90, end=10 ✗
  if (config.mem_block_start_pct > config.mem_block_end_pct) {
    ESP_LOGE(kLogTag, "Initialize failed: memory window invalid (start=%.1f%% > end=%.1f%%)",
             config.mem_block_start_pct, config.mem_block_end_pct);
    return false;
  }

  // Validate L-S-P timing parameters
  if (config.timing_l < 10 || config.timing_l > 90) {
    ESP_LOGE(kLogTag, "Initialize failed: timing_l must be 10-90 (got %u)", config.timing_l);
    return false;
  }
  if (config.timing_s > 99) {
    ESP_LOGE(kLogTag, "Initialize failed: timing_s must be 0-99 (got %u)", config.timing_s);
    return false;
  }
  if (config.timing_p < 10 || config.timing_p > 99) {
    ESP_LOGE(kLogTag, "Initialize failed: timing_p must be 10-99 (got %u)", config.timing_p);
    return false;
  }

  config_ = config;
  callbacks_ = callbacks;

  Reset();
  return true;
}

void PaddleEngine::Reset() {
  // Task 5.5: Reset all FSM state variables
  state_ = State::kIdle;
  current_element_ = PaddleElement::kDit;
  last_element_ = PaddleElement::kDah;
  element_start_us_ = 0;
  element_end_us_ = 0;
  gap_start_us_ = 0;
  gap_end_us_ = 0;
  dit_pressed_ = false;
  dah_pressed_ = false;

  // Clear queue and memory flags (Task 3.1-3.2)
  queue_.clear();
  dot_requested_ = false;
  dah_requested_ = false;

  // Clear squeeze flags (Task 4.1)
  squeeze_seen_this_element_ = false;
  last_valid_combo_ = PaddleCombo::kNone;
}

void PaddleEngine::OnPaddleEvent(const hal::PaddleEvent& event) {
  // Task 2.10 & 4.4: Update paddle state and squeeze tracking

  // Ignore straight key events (we only handle iambic paddles)
  if (event.line == hal::PaddleLine::kKey) {
    return;
  }

  // Update paddle pressed state
  if (event.line == hal::PaddleLine::kDit) {
    dit_pressed_ = event.active;
    ESP_LOGI(kLogTag, "OnPaddleEvent: DIT %s @ %lld us (state=%d, dah=%d)",
             event.active ? "PRESSED" : "RELEASED",
             event.timestamp_us,
             (int)state_,
             dah_pressed_);
  } else if (event.line == hal::PaddleLine::kDah) {
    dah_pressed_ = event.active;
    ESP_LOGI(kLogTag, "OnPaddleEvent: DAH %s @ %lld us (state=%d, dit=%d)",
             event.active ? "PRESSED" : "RELEASED",
             event.timestamp_us,
             (int)state_,
             dit_pressed_);
  }

  // Update paddle state with squeeze mode logic
  UpdatePaddles(dit_pressed_, dah_pressed_);
}

void PaddleEngine::Tick(int64_t now_us) {
  switch (state_) {
    case State::kIdle: {
      // Task 2.7: IDLE state handling
      // FIRST: Check if queue has elements (memory/bonus elements take priority)
      if (!queue_.empty()) {
        PaddleElement next = queue_.front();
        queue_.pop_front();
        ESP_LOGD(kLogTag, "IDLE: Sending queued %s (remaining=%zu)",
                 (next == PaddleElement::kDit) ? "DIT" : "DAH",
                 queue_.size());
        StartElement(next, now_us);
        break;
      }

      // SECOND: Check current paddle combination and decide what to send
      PaddleCombo combo = GetComboNow();

      // If no paddles pressed and queue empty, nothing to do
      if (combo == PaddleCombo::kNone) {
        break;
      }

      // Determine next element based on combo and iambic alternation
      PaddleElement next = PaddleElement::kDit;
      if (combo == PaddleCombo::kBoth) {
        // Both paddles: alternate from last element (iambic)
        next = (last_element_ == PaddleElement::kDit) ? PaddleElement::kDah
                                                      : PaddleElement::kDit;
        ESP_LOGD(kLogTag, "IDLE: Both paddles pressed, alternating to %s",
                 (next == PaddleElement::kDit) ? "DIT" : "DAH");
      } else if (combo == PaddleCombo::kDahOnly) {
        next = PaddleElement::kDah;
        ESP_LOGD(kLogTag, "IDLE: DAH paddle pressed");
      } else {
        next = PaddleElement::kDit;
        ESP_LOGD(kLogTag, "IDLE: DIT paddle pressed");
      }

      StartElement(next, now_us);
      break;
    }

    case State::kSendDit:
    case State::kSendDah: {
      // Task 2.8 & 3.6: Element state handling
      // Check for memory arming and squeeze detection during element
      CheckMemoryAndSqueezeDuringElement(now_us);

      // Check if element duration has elapsed
      if (now_us >= element_end_us_) {
        FinishElement(element_end_us_);
        // Always enter gap - required for proper Morse timing (1 dit duration)
        // Gap cannot be skipped even when queue has elements, otherwise decoder
        // cannot distinguish individual elements within a character
        EnterGap(element_end_us_);
      }
      break;
    }

    case State::kIntraElementGap: {
      // Task 2.9: GAP state handling
      // Check if gap has elapsed
      if (now_us >= gap_end_us_) {
        // Gap complete - return to IDLE and check for next element
        ESP_LOGD(kLogTag, "GAP complete, transitioning to IDLE (elapsed=%lld us)",
                 (long long)(now_us - gap_start_us_));
        state_ = State::kIdle;
        Tick(now_us);  // Immediately check if we should start next element
      }
      break;
    }
  }
}

void PaddleEngine::StartElement(PaddleElement element, int64_t start_time_us) {
  // Calculate element duration based on type
  const int64_t duration = (element == PaddleElement::kDit) ? DitDurationUs()
                                                            : DahDurationUs();

  // Set FSM state to appropriate sending state
  state_ = (element == PaddleElement::kDit) ? State::kSendDit : State::kSendDah;

  // Record element info and timing
  current_element_ = element;
  element_start_us_ = start_time_us;
  element_end_us_ = start_time_us + duration;

  ESP_LOGI(kLogTag, "→ START %s @ %lld us | planned_dur=%lld us | target_end=%lld us | queue=%zu",
           (element == PaddleElement::kDit) ? "DIT" : "DAH",
           start_time_us,
           duration,
           element_end_us_,
           queue_.size());

  // Update last element for iambic alternation
  last_element_ = element;

  // Task 4.7: Reset squeeze flag at start of new element
  squeeze_seen_this_element_ = false;

  // Task 5.1: Invoke callbacks - CRITICAL: Call IMMEDIATELY without logging first
  // Reason: ESP_LOGI can take 20-60ms, introducing unacceptable audio timing latency
  if (callbacks_.on_element_started != nullptr) {
    callbacks_.on_element_started(element, start_time_us, callbacks_.context);
  }
  if (callbacks_.on_key_state_changed != nullptr) {
    callbacks_.on_key_state_changed(true, start_time_us, callbacks_.context);
  }
}

void PaddleEngine::FinishElement(int64_t timestamp_us) {
  const int64_t actual_duration_us = timestamp_us - element_start_us_;
  const int64_t planned_duration_us = element_end_us_ - element_start_us_;
  const int64_t timing_error_us = actual_duration_us - planned_duration_us;

  ESP_LOGI(kLogTag, "← FINISH %s @ %lld us | actual_dur=%lld us | planned_dur=%lld us | ERROR=%+lld us (%.1f%%)",
           (current_element_ == PaddleElement::kDit) ? "DIT" : "DAH",
           timestamp_us,
           actual_duration_us,
           planned_duration_us,
           timing_error_us,
           (planned_duration_us > 0 ? (100.0 * timing_error_us / planned_duration_us) : 0.0));

  // Save memory flags state BEFORE consuming (needed for Mode B bonus check)
  const bool memory_armed_dit = dot_requested_;
  const bool memory_armed_dah = dah_requested_;

  // Task 3.7: Consume memory flags by pushing to queue
  // Push memory elements to queue FIRST (FR-13, priority order)
  if (dot_requested_) {
    queue_.push_back(PaddleElement::kDit);
    ESP_LOGD(kLogTag, "  Memory: Added DIT to queue (current was %s, size=%zu)",
             (current_element_ == PaddleElement::kDit) ? "DIT" : "DAH",
             queue_.size());
    dot_requested_ = false;
  }
  if (dah_requested_) {
    queue_.push_back(PaddleElement::kDah);
    ESP_LOGD(kLogTag, "  Memory: Added DAH to queue (current was %s, size=%zu)",
             (current_element_ == PaddleElement::kDit) ? "DIT" : "DAH",
             queue_.size());
    dah_requested_ = false;
  }

  // Task 4.5 & 4.6: Mode B bonus logic
  // Add bonus opposite element if:
  // 1. iambic_mode == kB
  // 2. squeeze was seen this element
  // 3. Reference combo (last_valid_combo_) is NOT both paddles
  // 4. Memory didn't already queue the opposite element (avoid duplicates)
  if (config_.iambic_mode == IambicMode::kB &&
      squeeze_seen_this_element_ &&
      last_valid_combo_ != PaddleCombo::kBoth) {

    // Determine bonus element (opposite of current)
    PaddleElement bonus = (current_element_ == PaddleElement::kDit)
                          ? PaddleElement::kDah
                          : PaddleElement::kDit;

    // Check if memory already queued this element (avoid duplicates)
    const bool already_queued_by_memory =
        (bonus == PaddleElement::kDit && memory_armed_dit) ||
        (bonus == PaddleElement::kDah && memory_armed_dah);

    if (!already_queued_by_memory) {
      // Push bonus element AFTER memory elements (FR-16)
      queue_.push_back(bonus);
      ESP_LOGD(kLogTag, "  Mode B bonus: Added %s (opposite of current %s) to queue (size=%zu)",
               (bonus == PaddleElement::kDit) ? "DIT" : "DAH",
               (current_element_ == PaddleElement::kDit) ? "DIT" : "DAH",
               queue_.size());
    } else {
      ESP_LOGD(kLogTag, "  Mode B bonus SKIPPED: opposite element already queued by memory");
    }
  } else if (config_.iambic_mode == IambicMode::kB) {
    // Debug: why was bonus NOT added?
    if (!squeeze_seen_this_element_) {
      ESP_LOGD(kLogTag, "  Mode B bonus SKIPPED: no squeeze detected");
    } else if (last_valid_combo_ == PaddleCombo::kBoth) {
      ESP_LOGD(kLogTag, "  Mode B bonus SKIPPED: last_valid_combo is BOTH");
    }
  }

  // Task 5.2: Invoke callbacks - CRITICAL: Call IMMEDIATELY without logging first
  // Reason: ESP_LOGI can take 20-60ms, introducing unacceptable audio timing latency
  if (callbacks_.on_element_finished != nullptr) {
    callbacks_.on_element_finished(current_element_, timestamp_us, callbacks_.context);
  }
  if (callbacks_.on_key_state_changed != nullptr) {
    callbacks_.on_key_state_changed(false, timestamp_us, callbacks_.context);
  }
}

void PaddleEngine::EnterGap(int64_t now_us) {
  // Transition to gap state
  state_ = State::kIntraElementGap;

  const int64_t gap_duration = GapDurationUs();
  ESP_LOGD(kLogTag, "→ Enter GAP (duration=%lld us, queue_size=%zu)",
           (long long)gap_duration, queue_.size());

  // Record gap timing
  gap_start_us_ = now_us;
  gap_end_us_ = now_us + gap_duration;
}

// Task 3.4: Check if memory window allows arming memory
bool PaddleEngine::MemoryWindowAllows(int64_t now_us) const {
  // Memory window is only active during element sending states
  if (state_ != State::kSendDit && state_ != State::kSendDah) {
    return false;
  }

  // Calculate progress percentage
  const float progress = GetElementProgressPct(now_us);

  // Window is open when progress is within [mem_block_start_pct, mem_block_end_pct]
  // Both parameters are positions from element start (not dead zones):
  // - mem_block_start_pct: window opens at this % (0 = opens immediately)
  // - mem_block_end_pct: window closes at this % (100 = closes at element end)
  // Examples:
  //   start=0, end=100 → window 0%-100% (memory always active)
  //   start=15, end=85 → window 15%-85% (dead zones: 0-15%, 85-100%)
  //   start=60, end=99 → window 60%-99% (late release window)
  const float window_start = config_.mem_block_start_pct;
  const float window_end = config_.mem_block_end_pct;

  return (progress >= window_start) && (progress <= window_end);
}

// Task 3.5: Check memory and squeeze during element
void PaddleEngine::CheckMemoryAndSqueezeDuringElement(int64_t now_us) {
  // Only check during element sending states
  if (state_ != State::kSendDit && state_ != State::kSendDah) {
    return;
  }

  // Check if we're in the memory window
  if (!MemoryWindowAllows(now_us)) {
    return;
  }

  // Get current paddle combination
  PaddleCombo combo = GetComboNow();

  // Determine which paddles can be memorized based on memory_mode
  bool can_memory_dit = false;
  bool can_memory_dah = false;

  switch (config_.memory_mode) {
    case MemoryMode::kNone:
      // No memory allowed
      break;
    case MemoryMode::kDotOnly:
      can_memory_dit = true;
      break;
    case MemoryMode::kDahOnly:
      can_memory_dah = true;
      break;
    case MemoryMode::kDotAndDah:
      can_memory_dit = true;
      can_memory_dah = true;
      break;
  }

  // Arm memory flags based on paddle combo and memory mode
  // IAMBIC RULE: Only arm the OPPOSITE ELEMENT to the one currently sending
  // (e.g., if sending DAH, only arm DIT; if sending DIT, only arm DAH)
  // This ensures automatic alternation in iambic keying
  if (combo == PaddleCombo::kDitOnly && can_memory_dit && current_element_ != PaddleElement::kDit) {
    if (!dot_requested_) {
      dot_requested_ = true;
      ESP_LOGD(kLogTag, "  Memory armed: DIT (opposite of current element, progress=%.1f%%)", GetElementProgressPct(now_us));
      // Task 5.3: Call timeline hook for memory window (only when newly armed)
      if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
        callbacks_.timeline_hooks.OnMemoryWindowChanged(false, true, now_us, callbacks_.timeline_hooks.context);
      }
    }
  } else if (combo == PaddleCombo::kDahOnly && can_memory_dah && current_element_ != PaddleElement::kDah) {
    if (!dah_requested_) {
      dah_requested_ = true;
      ESP_LOGD(kLogTag, "  Memory armed: DAH (opposite of current element, progress=%.1f%%)", GetElementProgressPct(now_us));
      // Task 5.3: Call timeline hook for memory window (only when newly armed)
      if (callbacks_.timeline_hooks.OnMemoryWindowChanged != nullptr) {
        callbacks_.timeline_hooks.OnMemoryWindowChanged(true, true, now_us, callbacks_.timeline_hooks.context);
      }
    }
  } else if (combo == PaddleCombo::kBoth) {
    // Squeeze: Both paddles pressed - arm the opposite element
    // If sending DAH → arm DIT only (iambic alternation)
    // If sending DIT → arm DAH only (iambic alternation)
    bool newly_armed = false;
    if (can_memory_dit && current_element_ != PaddleElement::kDit && !dot_requested_) {
      dot_requested_ = true;
      newly_armed = true;
    }
    if (can_memory_dah && current_element_ != PaddleElement::kDah && !dah_requested_) {
      dah_requested_ = true;
      newly_armed = true;
    }
    if (newly_armed) {
      ESP_LOGD(kLogTag, "  Memory armed: BOTH (progress=%.1f%%)", GetElementProgressPct(now_us));
    }

    // Task 4.2: Set squeeze flag for Mode B bonus detection
    if (!squeeze_seen_this_element_) {
      squeeze_seen_this_element_ = true;
      ESP_LOGD(kLogTag, "  Squeeze detected (progress=%.1f%%)", GetElementProgressPct(now_us));
      // Task 5.3: Call timeline hook for squeeze detection (only when newly detected)
      if (callbacks_.timeline_hooks.OnSqueezeDetected != nullptr) {
        callbacks_.timeline_hooks.OnSqueezeDetected(now_us, callbacks_.timeline_hooks.context);
      }
    }
  }
}

// FSM Helper: Get current paddle combination state
PaddleEngine::PaddleCombo PaddleEngine::GetComboNow() const {
  if (dit_pressed_ && dah_pressed_) {
    return PaddleCombo::kBoth;
  } else if (dit_pressed_) {
    return PaddleCombo::kDitOnly;
  } else if (dah_pressed_) {
    return PaddleCombo::kDahOnly;
  } else {
    return PaddleCombo::kNone;
  }
}

// Task 3.3: Get percentage of current element elapsed
float PaddleEngine::GetElementProgressPct(int64_t now_us) const {
  const int64_t duration = element_end_us_ - element_start_us_;
  if (duration <= 0) {
    return 0.0f;
  }
  const int64_t elapsed = now_us - element_start_us_;
  if (elapsed < 0) {
    return 0.0f;
  }
  return (static_cast<float>(elapsed) / static_cast<float>(duration)) * 100.0f;
}

// Task 4.3: Update paddle state based on squeeze_mode
void PaddleEngine::UpdatePaddles(bool dit, bool dah) {
  // Always update physical paddle state
  dit_pressed_ = dit;
  dah_pressed_ = dah;

  // Get current combo
  PaddleCombo combo = GetComboNow();
  PaddleCombo old_combo = last_valid_combo_;

  // Update last_valid_combo based on squeeze_mode
  if (config_.squeeze_mode == SqueezeMode::kSnapshot) {
    // Snapshot mode: Only update last_valid_combo when combo changes to non-None
    // This "freezes" the combo at element start
    if (combo != PaddleCombo::kNone) {
      last_valid_combo_ = combo;
    }
  } else {
    // Live mode: Always update last_valid_combo to current combo
    // This tracks paddle state in real-time
    last_valid_combo_ = combo;
  }

  // Log combo changes for debugging
  if (old_combo != last_valid_combo_) {
    const char* combo_names[] = {"NONE", "DIT", "DAH", "BOTH"};
    ESP_LOGD(kLogTag, "UpdatePaddles: last_valid_combo %s → %s (squeeze_mode=%s)",
             combo_names[(int)old_combo],
             combo_names[(int)last_valid_combo_],
             (config_.squeeze_mode == SqueezeMode::kSnapshot) ? "SNAPSHOT" : "LIVE");
  }
}

int64_t PaddleEngine::DitDurationUs() const {
  // Timing chain: WPM → dit_theoretical → P → dit_effective
  // 1. Calculate theoretical dit duration from WPM (standard: 1.2s / wpm)
  const float wpm = static_cast<float>(config_.speed_wpm);
  const float dit_theoretical_sec = kDitSecondsPerWpm / wpm;
  const int64_t dit_theoretical_us = static_cast<int64_t>(dit_theoretical_sec * static_cast<float>(kMicrosPerSecond));

  // 2. Apply P parameter (dit weight): P=50 is 100% (standard), P=25 is 50%, P=75 is 150%
  // dit_effective = dit_theoretical * (P / 50.0)
  const float p_multiplier = static_cast<float>(config_.timing_p) / 50.0f;
  const int64_t dit_effective_us = static_cast<int64_t>(static_cast<float>(dit_theoretical_us) * p_multiplier);

  return dit_effective_us;
}

int64_t PaddleEngine::DahDurationUs() const {
  // Apply L parameter (dash length): L=30 is 3:1 (standard), L=40 is 4:1, L=10 is 1:1
  // dash = dit_effective * (L / 10.0)
  const int64_t dit_eff = DitDurationUs();
  const float l_multiplier = static_cast<float>(config_.timing_l) / 10.0f;
  return static_cast<int64_t>(static_cast<float>(dit_eff) * l_multiplier);
}

int64_t PaddleEngine::GapDurationUs() const {
  // Apply S parameter (gap space): S=50 is 1:1 (standard), S=25 is 0.5:1, S=75 is 1.5:1
  // gap = dit_effective * (S / 50.0)
  const int64_t dit_eff = DitDurationUs();
  const float s_multiplier = static_cast<float>(config_.timing_s) / 50.0f;
  return static_cast<int64_t>(static_cast<float>(dit_eff) * s_multiplier);
}

float PaddleEngine::CalculateEffectiveWpm() const {
  // Calculate effective WPM based on PARIS standard word timing
  // PARIS composition: 10 dits + 4 dahs + 9 intra-gaps + 4 inter-char gaps (3 dit each) + 1 word gap (7 dit)
  // With L-S-P adjustments:
  //   - dit_effective (affected by P)
  //   - dash = dit_effective * (L/10)
  //   - intra_gap = dit_effective * (S/50)
  //   - inter_char_gap = 3 * dit_effective (fixed)
  //   - word_gap = 7 * dit_effective (fixed)

  const int64_t dit_us = DitDurationUs();
  const int64_t dash_us = DahDurationUs();
  const int64_t gap_us = GapDurationUs();

  // PARIS time in microseconds
  const int64_t paris_time_us = 10 * dit_us +        // 10 dits
                                 4 * dash_us +         // 4 dahs
                                 9 * gap_us +          // 9 intra-element gaps
                                 4 * 3 * dit_us +      // 4 inter-character gaps (3 dit each)
                                 7 * dit_us;           // 1 word gap (7 dit)

  // Effective WPM = 60 seconds / (PARIS time in seconds)
  const float paris_time_sec = static_cast<float>(paris_time_us) / static_cast<float>(kMicrosPerSecond);
  const float effective_wpm = 60.0f / paris_time_sec;

  return effective_wpm;
}

float PaddleEngine::GetDashRatio() const {
  // Dash ratio = L / 10.0 (e.g., L=30 → 3.0:1, L=40 → 4.0:1)
  return static_cast<float>(config_.timing_l) / 10.0f;
}

void PaddleEngine::DumpState(int64_t now_us) const {
  // Task 5.6: Implement debug dump method
  const char* state_str = "";
  switch (state_) {
    case State::kIdle: state_str = "IDLE"; break;
    case State::kSendDit: state_str = "SEND_DIT"; break;
    case State::kSendDah: state_str = "SEND_DAH"; break;
    case State::kIntraElementGap: state_str = "INTRA_GAP"; break;
  }

  const char* current_elem = (current_element_ == PaddleElement::kDit) ? "DIT" : "DAH";
  const char* last_elem = (last_element_ == PaddleElement::kDit) ? "DIT" : "DAH";

  ESP_LOGI(kLogTag, "=== PADDLE ENGINE STATE DUMP ===");
  ESP_LOGI(kLogTag, "  State: %s", state_str);
  ESP_LOGI(kLogTag, "  Current element: %s (start=%lld, end=%lld)",
           current_elem,
           (long long)element_start_us_,
           (long long)element_end_us_);
  ESP_LOGI(kLogTag, "  Last element: %s", last_elem);
  ESP_LOGI(kLogTag, "  Now: %lld us, Gap end: %lld us",
           (long long)now_us, (long long)gap_end_us_);
  ESP_LOGI(kLogTag, "  Paddle state: dit=%d dah=%d", dit_pressed_, dah_pressed_);
  ESP_LOGI(kLogTag, "  Config: WPM=%lu, mode=%d, mem=%d, squeeze=%d",
           (unsigned long)config_.speed_wpm,
           (int)config_.iambic_mode,
           (int)config_.memory_mode,
           (int)config_.squeeze_mode);
  ESP_LOGI(kLogTag, "  Config: mem_block_start=%.1f%%, mem_block_end=%.1f%%",
           config_.mem_block_start_pct,
           config_.mem_block_end_pct);

  // Task 5.6: Add queue size and memory flags
  ESP_LOGI(kLogTag, "  Queue size: %zu", queue_.size());
  ESP_LOGI(kLogTag, "  Memory flags: dot_requested=%d dah_requested=%d",
           dot_requested_, dah_requested_);

  // Task 5.6: Add squeeze flags
  ESP_LOGI(kLogTag, "  Squeeze: seen_this_element=%d last_valid_combo=%d",
           squeeze_seen_this_element_, (int)last_valid_combo_);

  ESP_LOGI(kLogTag, "================================");
}

}  // namespace keying
