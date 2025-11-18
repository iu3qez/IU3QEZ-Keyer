#pragma once
// Host-side stub for freertos/portmacro.h providing minimal spinlock and critical section macros.

// Stub portMUX_TYPE for host testing (use POD type to avoid C++ linkage issues).
// Real ESP32 portMUX_TYPE is a spinlock struct; for host tests we use a simple int placeholder.
typedef struct {
  int owner;      // Owner ID (unused in stubs).
  int count;      // Lock count (unused in stubs).
} portMUX_TYPE;

// Initializer macro for host tests (zero-initialized struct).
#define portMUX_INITIALIZER_UNLOCKED {0, 0}

// Critical section stubs (no-op for single-threaded host tests).
#define portENTER_CRITICAL(mux) (void)0
#define portEXIT_CRITICAL(mux) (void)0
#define portENTER_CRITICAL_ISR(mux) (void)0
#define portEXIT_CRITICAL_ISR(mux) (void)0
