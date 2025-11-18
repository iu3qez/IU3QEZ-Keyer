# Keyer QRS2HST - Development Guide

**Archived**: Previous documentation moved to [DEVELOPMENT.md.1](DEVELOPMENT.md.1)

---

## Quick Start

```bash
# Build firmware
idf.py build

# Generate UF2 firmware for USB updates (optional)
ninja -C build firmware-uf2

# Flash to device
idf.py -p /dev/ttyUSB0 flash

# Run host tests
./run_tests.sh
```

## Firmware Build and Distribution

### Standard ESP-IDF Build

The project uses ESP-IDF v5.4.3 build system:

```bash
idf.py build                    # Build firmware (output: build/keyer_qrs2hst.bin)
idf.py -p /dev/ttyUSB0 flash   # Flash via serial (UART or USB-JTAG)
idf.py monitor                  # Open serial monitor
```

### UF2 Firmware Generation

For USB-based firmware updates via tinyuf2 bootloader, the project can generate UF2 format files:

```bash
# Manual generation after build
idf.py build
ninja -C build firmware-uf2     # Generates build/firmware.uf2

# Or use script directly
./scripts/build/generate_uf2.sh build/keyer_qrs2hst.bin build/firmware.uf2
```

**UF2 Configuration:**
- **Family ID:** 0xc47e5767 (ESP32-S3)
- **Base Address:** 0x134000 (ota_0 partition with factory bootloader)
- **Output:** `build/firmware.uf2`

**Usage:**
1. Enter bootloader mode: console `upgrade` command or web UI button
2. Device appears as USB drive "KEYERBOOT"
3. Drag `firmware.uf2` onto the drive
4. Device auto-resets with new firmware

**Automatic UF2 Generation (Optional):**

Edit `CMakeLists.txt` and uncomment the `POST_BUILD` custom command to automatically generate UF2 after every build.

## Project Structure

- `components/` - Modular firmware components
- `main/` - Entry point
- `tests_host/` - Host-side unit tests
- `docs/` - Documentation

## Application Initialization Pipeline

**Modular, testable initialization system using Builder + Strategy pattern.**

### Architecture Overview

The application initialization is orchestrated by [`InitializationPipeline`](../components/app/init_pipeline.cpp) which executes 16 distinct phases in sequence. Each phase implements the [`InitPhase`](../components/app/include/app/init_phase.hpp) interface:

```cpp
class InitPhase {
 public:
  virtual esp_err_t Execute() = 0;        // Perform initialization
  virtual const char* GetName() const = 0; // Phase name for logging
  virtual bool IsCritical() const = 0;     // true = abort on failure
};
```

### Boot Sequence (16 Phases)

**Phase execution order** ([application_controller.cpp:47-77](../components/app/application_controller.cpp#L47-L77)):

```
┌─────────────────────────────────────────┐
│ Phase 1-3: Critical Infrastructure     │
├─────────────────────────────────────────┤
│ 1. UartDebugPhase         [CRITICAL]   │  Enable debug logging
│ 2. NvsFlashPhase          [CRITICAL]   │  Initialize NVS storage (LED: Magenta)
│ 3. HighPrecisionClockPhase [CRITICAL]  │  Configure ESP32-S3 clock
├─────────────────────────────────────────┤
│ Phase 4-5: Configuration                │
├─────────────────────────────────────────┤
│ 4. ConfigStoragePhase     [CRITICAL]   │  Load device configuration (LED: Cyan)
│ 5. ParameterRegistryPhase [CRITICAL]   │  Register 40 parameters
├─────────────────────────────────────────┤
│ Phase 6-7: Diagnostics + USB            │
├─────────────────────────────────────────┤
│ 6. DiagnosticsSubsystemPhase [NON-CRIT]│  LED controller + timeline
│ 7. UsbEarlyInitPhase      [NON-CRIT]   │  USB device initialization
├─────────────────────────────────────────┤
│ Phase 8-12: Subsystem Creation + Init   │
├─────────────────────────────────────────┤
│ 8. SubsystemCreationPhase [CRITICAL]   │  Create all subsystem objects
│ 9. PaddleHalPhase         [CRITICAL]   │  Initialize paddle hardware (LED: Orange)
│10. TxHalPhase             [CRITICAL]   │  Initialize TX hardware
│11. KeyingSubsystemPhase   [CRITICAL]   │  Configure keying engine
│12. AudioSubsystemPhase    [CRITICAL]   │  Configure audio (sidetone/CW-over-USB)
├─────────────────────────────────────────┤
│ Phase 13: Dependency Wiring             │
├─────────────────────────────────────────┤
│13. SubsystemWiringPhase   [CRITICAL]   │  Wire subsystem dependencies
├─────────────────────────────────────────┤
│ Phase 14-15: Network Services           │
├─────────────────────────────────────────┤
│14. WiFiSubsystemPhase     [NON-CRIT]   │  Connect to WiFi (LED: Yellow)
│15. HttpServerPhase        [NON-CRIT]   │  Start web UI server
├─────────────────────────────────────────┤
│ Phase 16: Watchdog                      │
├─────────────────────────────────────────┤
│16. WatchdogPhase          [CRITICAL]   │  Enable watchdog timer
└─────────────────────────────────────────┘
         ↓
   ✅ Boot Complete (LED: Green)
```

### Error Handling

**Critical phases** (`IsCritical() == true`):
- Failure calls `ApplicationController::FatalInitError()` → LED blinking + serial log + infinite loop
- Example: NVS flash corruption, GPIO initialization failure

**Non-critical phases** (`IsCritical() == false`):
- Failure logged with `ESP_LOGE()` priority
- Initialization continues to next phase
- Example: WiFi connection timeout, diagnostics subsystem unavailable

### LED Boot Phase Signaling

The [`DiagnosticsSubsystem`](../components/diagnostics_subsystem/include/diagnostics_subsystem/diagnostics_subsystem.hpp) provides visual feedback during boot:

| Phase | LED Color | Milestone                  |
|-------|-----------|----------------------------|
| 0     | Magenta   | NVS flash initialized      |
| 1     | Cyan      | Configuration loaded       |
| 2     | Orange    | Subsystems created + wired |
| 3     | Yellow    | WiFi connected             |
| 4     | Green     | Boot complete              |

**Implementation:** `diagnostics_subsystem_->SignalBootPhase(n)` called at specific checkpoints.

### Adding a New Initialization Phase

**1. Declare phase class in [`init_phases.hpp`](../components/app/include/app/init_phases.hpp):**

```cpp
class MyNewPhase : public InitPhase {
 public:
  explicit MyNewPhase(DependencyA* dep_a, DependencyB* dep_b);
  esp_err_t Execute() override;
  const char* GetName() const override { return "MyNewPhase"; }
  bool IsCritical() const override { return true; }  // or false

 private:
  DependencyA* dep_a_;
  DependencyB* dep_b_;
};
```

**2. Implement in [`init_phases.cpp`](../components/app/init_phases.cpp):**

```cpp
MyNewPhase::MyNewPhase(DependencyA* dep_a, DependencyB* dep_b)
    : dep_a_(dep_a), dep_b_(dep_b) {}

esp_err_t MyNewPhase::Execute() {
  ESP_LOGI("MyNewPhase", "Initializing...");

  // Your initialization logic here
  esp_err_t err = dep_a_->Init();
  if (err != ESP_OK) {
    ESP_LOGE("MyNewPhase", "Failed to initialize DependencyA: %s", esp_err_to_name(err));
    return err;
  }

  return ESP_OK;
}
```

**3. Register in [`ApplicationController::Initialize()`](../components/app/application_controller.cpp):**

```cpp
bool ApplicationController::Initialize() {
  InitializationPipeline pipeline;

  // ... existing phases ...

  pipeline.AddPhase(std::make_unique<MyNewPhase>(&dep_a_, &dep_b_));  // Add at correct position

  // ... remaining phases ...

  return pipeline.Execute();
}
```

**Important:** Respect hardware dependency order. Example: NVS must initialize before ConfigStorage, GPIO before peripherals.

### Benefits of Pipeline Architecture

- **Testability:** Each phase can be unit-tested in isolation with mock dependencies
- **Single Responsibility:** Each phase has one clear initialization task (15-40 LOC typical)
- **Explicit Dependencies:** Constructor injection makes dependencies visible
- **Clear Error Handling:** Critical vs non-critical failures explicitly declared
- **Extensibility:** Adding new phases doesn't require modifying existing code
- **Readability:** `ApplicationController::Initialize()` reduced from 199 LOC → 48 LOC

### Code References

- **Pipeline orchestrator:** [init_pipeline.cpp](../components/app/init_pipeline.cpp)
- **Phase interface:** [init_phase.hpp](../components/app/include/app/init_phase.hpp)
- **Phase implementations:** [init_phases.cpp](../components/app/init_phases.cpp) (16 classes, 355 LOC)
- **Integration point:** [application_controller.cpp:47-85](../components/app/application_controller.cpp#L47-L85)

## Parameter System

**YAML-driven code generation for device configuration parameters (Feature 4).**

### Architecture

Single source of truth: [`components/config/parameters.yaml`](../components/config/parameters.yaml)

**Auto-generates:**
- `parameter_table.hpp` - Type system + PARAMETER_TABLE macro for NVS storage
- `parameter_registry_generated.cpp` - RegisterAllParameters() with getter/setter lambdas

**Build integration:**
- CMake custom command runs Python generator on YAML changes
- Generated files in `.gitignore` (not committed to Git)

### Adding a New Parameter

**1. Add parameter to `parameters.yaml`:**

```yaml
parameters:
  - subsystem: audio         # Subsystem name (audio|keying|hardware|wifi|general)
    name: bass_boost         # Parameter short name
    nvs_key: audio_bass      # NVS storage key (max 15 chars, unique)
    field: audio.bass_boost  # DeviceConfig field path
    type: UINT8              # NVS type (INT32|UINT32|UINT16|UINT8|INT8|BOOL|STRING|FLOAT)
    min: 0                   # Minimum value (for numeric types)
    max: 10                  # Maximum value (for numeric types)
    reset_required: false    # true = hardware param (needs reboot)
    description: "Bass boost level"  # Short description
    unit: "dB"               # Unit string (or "" for none)
    validator: RangeValidatorTag     # Validator tag
    help:
      short: "Set bass boost level (0-10 dB)"
      long: |
        Controls the bass frequency boost applied to sidetone audio.

        0 dB = no boost (flat response)
        10 dB = maximum boost

        Default: 0 dB
      examples:
        - command: "audio bass_boost 0"
          description: "Disable bass boost"
        - command: "audio bass_boost 5"
          description: "Moderate bass boost"
```

**2. Add field to `DeviceConfig` struct:**

```cpp
// In components/config/include/config/device_config.hpp
struct AudioConfig {
  uint8_t bass_boost = 0;  // NEW: Bass boost level (0-10 dB)
  // ... existing fields
};
```

**3. Build:**

```bash
idf.py build
# Generated files automatically updated:
# - components/config/include/config/parameter_table.hpp
# - components/config/parameter_registry_generated.cpp
```

**Done!** Parameter is now:
- ✅ Stored in NVS (`audio_bass` key)
- ✅ Registered in ParameterRegistry (`audio.bass_boost`)
- ✅ Accessible via console (`audio bass_boost 5`)
- ✅ Visible in Web UI config page
- ✅ Documented with help text

### CMake Regeneration Workflow

**Automatic regeneration:**
- Editing `parameters.yaml` triggers rebuild
- CMake detects file change via `DEPENDS` clause
- Python script runs before compilation
- Generated files overwrite previous versions

**Manual regeneration (if needed):**

```bash
python3 components/config/scripts/generate_parameters.py \
  --input components/config/parameters.yaml \
  --output-table components/config/include/config/parameter_table.hpp \
  --output-registry components/config/parameter_registry_generated.cpp \
  --schema components/config/parameters_schema.json
```

### Python Script Usage

**Generator script:** `components/config/scripts/generate_parameters.py`

**Arguments:**
- `--input` - Path to parameters.yaml (required)
- `--output-table` - Path for parameter_table.hpp output (required)
- `--output-registry` - Path for parameter_registry_generated.cpp output (required)
- `--schema` - Path to parameters_schema.json for validation (optional)

**Validation:**
- JSON Schema validation (if `jsonschema` package available)
- Manual field presence checks (required fields)
- Type-specific validation (e.g., min/max for numeric types)
- Clear error messages with parameter index and field name

**Error example:**

```
Error in parameters[5]: Missing required field 'nvs_key'
  Parameter: audio.bass_boost
```

### Validator Tags

Available validators for parameter validation:

- `RangeValidatorTag` - Numeric range check (min ≤ value ≤ max)
- `GpioUniqueValidatorTag` - GPIO range check (0-48 or -1=disabled)
- `StringCallsignValidatorTag` - ITU callsign format (A-Z, 0-9, /, -)
- `StringPrintableValidatorTag` - Printable ASCII (space through tilde)

### Generated File Structure

**`parameter_table.hpp`** (218 lines):
```cpp
// Type system
enum class NvsType { INT32, UINT32, ... };
struct ParameterDescriptor { ... };

// Validators
template <typename T> struct RangeValidator { ... };
struct GpioUniqueValidator { ... };

// Macro
#define PARAMETER_TABLE(X) \
  X(audio, freq, "audio_freq", audio.sidetone_frequency_hz, UINT16, ...) \
  X(audio, volume, "audio_vol", audio.sidetone_volume_percent, UINT8, ...) \
  // ... 40 parameters total
```

**`parameter_registry_generated.cpp`** (335 lines):
```cpp
void RegisterAllParameters(ParameterRegistry& registry) {
  // audio.freq
  registry.Register(std::make_unique<IntParameter<100, 2000>>(
      "audio.freq", "Sidetone frequency", "Hz",
      [](const DeviceConfig& c) -> int32_t { return static_cast<int32_t>(c.audio.sidetone_frequency_hz); },
      [](DeviceConfig& c, int32_t v) { c.audio.sidetone_frequency_hz = static_cast<decltype(...)>(v); }
  ));
  // ... 40 parameters total
}
```

## Key Resources

- [Web UI API Specification](WEB_UI_API_SPECIFICATION.md)
- [Archived Development Guide](DEVELOPMENT.md.1)
- [Task List](.project-management/current-prd/tasks-prd-feature2.md)


### Web UI Asset Pipeline
- Source files live under `webui/` (`html/`, `js/`, `css/`). Edit these instead of modifying C++ strings.
- Assets are converted to gzip-compressed blobs during the CMake configure step via `scripts/webui/embed_assets.py`. The generated table lands in `components/ui/generated/web_assets_data.inc` (ignored from git).
- `idf.py build` or `cmake --build` automatically regenerates the blobs whenever files under `webui/` change. No manual commands are required, but you can run the script directly while iterating:
  ```bash
  python scripts/webui/embed_assets.py --source webui --output components/ui/generated/web_assets_data.inc --gzip
  ```
- Runtime access goes through `ui::assets::Find(path)` (see `components/ui/web_assets.cpp`). HTTP handlers call `HttpServer::SendAsset()` which sets MIME type and `Content-Encoding: gzip` when relevant.
- To add a new page: place the HTML/JS/CSS asset under `webui/`; the build regenerates embedded blobs and `HttpServer::Initialize()` auto-registers each asset path. Extend `_TEXT_EXTENSIONS` in `scripts/webui/embed_assets.py` if you need additional MIME types.
