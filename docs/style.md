# C++ Style Guide

## Only use english for code comments.

## Language Standard
- Target ISO C++17 for all firmware components.
- Stick to a C-like subset of the language: plain structs/classes, raw arrays, and POD types.
- Avoid the C++ standard library in firmware (no STL containers/algorithms); rely on ESP-IDF or handcrafted helpers instead.

## Naming
- Use `CamelCase` for types and `kCamelCase` for constant variables.
- Use `snake_case` for functions, methods, namespaces, variables, and file names.
- Reserve `ALL_CAPS` for macros that originate from ESP-IDF or hardware definitions.

## Structure
- Keep translation units focused on one responsibility; split code into components when logic grows.
- Place forward declarations and interfaces in headers under `include/` mirroring the source layout.
- Prefer anonymous namespaces over `static` for internal linkage in source files.

## Formatting
- Two-space indentation, spaces around binary operators, and trailing commas in multi-line initializers.
- Wrap lines at ~100 characters.
- Keep `#include` blocks ordered: related header first, then other project headers, then ESP-IDF/standard headers.

## Error Handling and Logging
- Check and log return values from ESP-IDF APIs; convert status codes to actionable logs.
- Use the `ESP_LOG*` macros with concise tags; avoid logging inside tight loops unless rate-limited.

## Concurrency
- Favor `freertos::` wrappers where available; otherwise, document mutex ownership in code comments.
- Avoid blocking calls in `app_main` before peripherals are initialized.


## Commenting

You must comment properly files, and code.

# 🧭 C++ Commenting Guide

## 1. Introduction
Comments are not decorations — they are a critical part of communicating *why* code exists and *how* it should be maintained.  
Good comments make the intent of the code clear, provide useful context, and reduce cognitive load for future developers (including your future self).  

C++ offers two styles of comments:
- `// single-line comments`
- `/* multi-line comments */`

For structured projects, the **Doxygen** style is recommended because it can generate professional documentation automatically from the source.

---

## 2. File Header
Every C++ file should start with a brief header explaining what it contains and who maintains it.

```cpp
/**
 * @file motor_controller.cpp
 * @brief Implementation of the motor control logic.
 * @details
 *  This file defines all the functions related to motor speed regulation,
 *  current sensing, and protection routines. It is part of the low-level
 *  hardware abstraction layer.
 * @author Simone Fabris
 * @date 2025-10-26
 * @version 1.0
 */
```

The file header should describe:
- The **purpose** of the file, not just its name.
- The **scope** or subsystem it belongs to.
- Optional: author, date, and version information (useful for small teams).

---

## 3. Section Comments
Separate major logical sections using clear headings.  
They improve navigation and readability, especially in large files.

```cpp
// ======================================================
// === Initialization and Configuration Routines ========
// ======================================================
```

Avoid decorative ASCII art unless it adds real clarity — the goal is structure, not aesthetics.

---

## 4. Function and Method Comments
Use **Doxygen comment blocks** immediately before each function definition.  
Explain what the function *does*, not how it does it (the code itself should make that clear).

```cpp
/**
 * @brief  Initializes the ADC module and prepares it for conversion.
 * @param  channel  ADC channel to configure.
 * @return True if initialization succeeded, false otherwise.
 * @note   Must be called before any call to `adc_read()`.
 */
bool adc_init(int channel);
```

If the function is complex, include a short “Details” section:

```cpp
/**
 * @brief  Performs a median filter on sensor data.
 * @param  samples  Vector containing raw input values.
 * @return Filtered average value.
 * @details
 *  The function sorts the input values and returns the median,
 *  which provides better noise immunity compared to a simple mean.
 *  It assumes the vector is non-empty.
 */
float median_filter(const std::vector<float>& samples);
```

---

## 5. Inline Comments
Use inline comments **sparingly** to clarify non-obvious logic or assumptions.

```cpp
current = read_sensor();   // Raw ADC reading in millivolts
power = current * voltage; // Convert to milliwatts
```

Avoid obvious remarks such as:
```cpp
i++; // increment i   <-- unnecessary
```

Instead, focus on *why* something is done:
```cpp
i++; // Advance pointer to skip header byte
```

---

## 6. Class and Struct Documentation
When documenting classes, describe their **purpose**, **responsibilities**, and **main interactions**.

```cpp
/**
 * @class MotorController
 * @brief Handles closed-loop control of a DC motor.
 * @details
 *  This class reads the motor speed, computes the control effort using
 *  a PID algorithm, and sends the command to the PWM driver.
 */
class MotorController {
public:
    MotorController(float kp, float ki, float kd);
    void setSpeed(float rpm);
    void update();
};
```

---

## 7. Constants, Enums, and Macros
Comment constants or macros that have special meaning or constraints:

```cpp
/// Maximum allowed motor current (in Amperes)
constexpr float MAX_MOTOR_CURRENT = 2.5f;

/// Error codes used by the controller
enum class ErrorCode {
    OK = 0,          ///< Normal operation
    Overcurrent,     ///< Exceeded MAX_MOTOR_CURRENT
    SensorFailure    ///< ADC or sensor not responding
};
```

---

## 8. What to Avoid
- ❌ Commenting every line (“noise comments”).  
- ❌ Outdated comments — they are worse than none.  
- ❌ Repeating what the code already says.  
- ❌ Using comments to excuse poor code.  
- ❌ Mixing languages — stay consistent (English for international projects).

---

## 9. Example: Well-Commented File

```cpp
/**
 * @file temperature_sensor.cpp
 * @brief Read and filter temperature data from an analog sensor.
 */

#include "temperature_sensor.h"

/**
 * @brief Initialize the ADC and prepare the temperature sensor.
 * @return true if the initialization succeeded.
 */
bool temp_sensor_init() {
    // Configure ADC channel for temperature input
    adc_attach_channel(TEMP_SENSOR_CHANNEL);

    // Wait a short time to allow sensor to stabilize
    delay(50);

    return true;
}

/**
 * @brief Read and convert temperature to degrees Celsius.
 * @return Temperature value in °C.
 */
float temp_sensor_read() {
    int raw = adc_read(TEMP_SENSOR_CHANNEL);
    float voltage = raw * ADC_TO_VOLT;
    return (voltage - 0.5f) * 100.0f; // LM35 formula
}
```

---
