#pragma once

#include <cstdint>
#include <deque>

#include "hal/paddle_hal.hpp"
#include "timeline/timeline_hooks.hpp"

namespace keying {

enum class PaddleElement : uint8_t {
  kDit = 0,
  kDah = 1,
};

// Iambic keyer mode (Mode A vs Mode B)
enum class IambicMode : uint8_t {
  kA = 0,  // Standard iambic: alternates dit/dah on each element
  kB = 1,  // Mode B: adds bonus opposite element when squeeze detected
};

// Memory mode configuration (which paddles can be memorized)
enum class MemoryMode : uint8_t {
  kNone = 0,        // No memory - only current paddle state matters
  kDotOnly = 1,     // Only dit paddle can be memorized
  kDahOnly = 2,     // Only dah paddle can be memorized
  kDotAndDah = 3,   // Both paddles can be memorized
};

// Squeeze detection mode (when to snapshot paddle state)
enum class SqueezeMode : uint8_t {
  kSnapshot = 0,  // Capture paddle state at element start, ignore mid-element changes
  kLive = 1,      // Continuously update paddle state during element
};

struct PaddleEngineConfig {
  uint32_t speed_wpm = 20;              // Keying speed in words per minute (default: 20 WPM)
  IambicMode iambic_mode = IambicMode::kB;  // Mode A or Mode B operation (default: Mode B)
  MemoryMode memory_mode = MemoryMode::kDotAndDah;  // Which paddles can be memorized (default: both)
  SqueezeMode squeeze_mode = SqueezeMode::kSnapshot;  // When to capture paddle state (default: snapshot at element start)

  // Memory window timing (both are positions from element start, NOT dead zones):
  // Window is active when progress ∈ [mem_block_start_pct, mem_block_end_pct]
  // Examples:
  //   start=0, end=100   → Memory active 0%-100% (always active)
  //   start=15, end=85   → Memory active 15%-85% (dead zones at start/end)
  //   start=60, end=99   → Memory active 60%-99% (late release window)
  float mem_block_start_pct = 0.0f;     // Memory window opens at this % (0 = opens immediately, default: 0%)
  float mem_block_end_pct = 100.0f;     // Memory window closes at this % (100 = closes at element end, default: 100%)

  // L-S-P timing system (Linea-Spazio-Punto) - Industry-standard HST/QRQ timing parameters
  // Runtime-only (not persisted to NVS), reset to 30-50-50 on device reboot
  // Reference: Standard L-S-P is 30-50-50 for 3:1 dash ratio, 1:1 gap, 100% dit
  uint8_t timing_l = 30;  // L (Dash length): 10-90, default 30 → dash = (L/10.0) * dit_effective (L=30 → 3:1 ratio)
  uint8_t timing_s = 50;  // S (Gap space): 0-99, default 50 → gap = (S/50.0) * dit_effective (S=50 → 1:1 ratio)
  uint8_t timing_p = 50;  // P (Dit duration): 10-99, default 50 → dit_effective = dit_theoretical * (P/50.0) (P=50 → 100%)
};

struct PaddleEngineCallbacks {
  void (*on_element_started)(PaddleElement element, int64_t start_time_us,
                             void* context) = nullptr;
  void (*on_element_finished)(PaddleElement element, int64_t end_time_us,
                              void* context) = nullptr;
  void (*on_key_state_changed)(bool key_active, int64_t timestamp_us,
                               void* context) = nullptr;
  void* context = nullptr;

  // Timeline hooks for real-time visualization (memory window, late release, latch, squeeze)
  timeline::TimelineHooks timeline_hooks{};
};

class PaddleEngine {
 public:
  PaddleEngine() = default;

  bool Initialize(const PaddleEngineConfig& config,
                  const PaddleEngineCallbacks& callbacks);

  void Reset();

  void OnPaddleEvent(const hal::PaddleEvent& event);

  void Tick(int64_t now_us);

  uint32_t speed_wpm() const { return config_.speed_wpm; }

  // L-S-P timing system helpers
  float CalculateEffectiveWpm() const;  // Calculate effective WPM based on PARIS timing with current L-S-P values
  float GetDashRatio() const;           // Get dash ratio (e.g., 3.0 for L=30, 4.0 for L=40)

  // Debug: Dump complete state machine status
  void DumpState(int64_t now_us) const;

 private:
  // FSM states for the paddle engine
  enum class State : uint8_t {
    kIdle = 0,           // Waiting for paddle input
    kSendDit = 1,        // Actively sending a dit element
    kSendDah = 2,        // Actively sending a dah element
    kIntraElementGap = 3,  // Gap between elements
  };

  // Paddle combination state (what paddles are currently pressed)
  enum class PaddleCombo : uint8_t {
    kNone = 0,      // No paddles pressed
    kDitOnly = 1,   // Only dit paddle pressed
    kDahOnly = 2,   // Only dah paddle pressed
    kBoth = 3,      // Both paddles pressed (squeeze)
  };

  struct ElementInfo {
    PaddleElement type = PaddleElement::kDit;
    int64_t start_time_us = 0;
    int64_t end_time_us = 0;
  };

  struct PaddleTracker {
    bool active = false;
    int64_t press_time_us = 0;
    int64_t release_time_us = 0;
  };

  void StartElement(PaddleElement element, int64_t start_time_us);
  void FinishElement(int64_t timestamp_us);
  void EnterGap(int64_t now_us);

  bool MemoryWindowAllows(int64_t timestamp_us) const;

  // FSM helper methods
  PaddleCombo GetComboNow() const;  // Get current paddle combination state
  float GetElementProgressPct(int64_t now_us) const;  // Get % of current element elapsed
  void CheckMemoryAndSqueezeDuringElement(int64_t now_us);  // Check and arm memory flags
  void UpdatePaddles(bool dit, bool dah);  // Update paddle state based on squeeze_mode

  int64_t DitDurationUs() const;
  int64_t DahDurationUs() const;
  int64_t GapDurationUs() const;

  // Configuration and callbacks
  PaddleEngineConfig config_{};
  PaddleEngineCallbacks callbacks_{};

  // FSM state tracking (follows Python prototype structure)
  State state_ = State::kIdle;                    // Current FSM state
  PaddleElement current_element_ = PaddleElement::kDit;  // Element being sent
  PaddleElement last_element_ = PaddleElement::kDah;     // Last sent element (for iambic alternation)

  // Element timing (microseconds)
  int64_t element_start_us_ = 0;  // When current element started
  int64_t element_end_us_ = 0;    // When current element should end

  // Gap timing (microseconds)
  int64_t gap_start_us_ = 0;      // When gap started
  int64_t gap_end_us_ = 0;        // When gap should end

  // Paddle state (current physical state of paddles)
  bool dit_pressed_ = false;      // True if dit paddle currently pressed
  bool dah_pressed_ = false;      // True if dah paddle currently pressed

  // Element queue for memory system (max 4 elements: 2 memory + 1 Mode B bonus + 1 current)
  std::deque<PaddleElement> queue_;

  // Memory flags (armed when paddle pressed during memory window)
  bool dot_requested_ = false;    // True if dit memory armed
  bool dah_requested_ = false;    // True if dah memory armed

  // Squeeze tracking for Mode B bonus element
  bool squeeze_seen_this_element_ = false;  // True if squeeze detected during current element
  PaddleCombo last_valid_combo_ = PaddleCombo::kNone;  // Last non-None combo (for snapshot mode)
};

}  // namespace keying
