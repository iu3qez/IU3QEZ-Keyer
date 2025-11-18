# Console Command Reference

## Overview

The keyer provides a USB CDC console interface for configuration, diagnostics, and debugging. The console is accessible via a dedicated USB port (separate from any programming/debug interface) and provides real-time interaction with the keyer's subsystems.

The console features a **Text User Interface (TUI)** with a bordered layout, status bar, scrollable output area, and dedicated input area for an enhanced user experience.

## Console Interface Layout

### TUI Design

The console uses a bordered ASCII art interface that automatically refreshes on connection and command execution:

```
┌─ IU3QEZ Keyer | Uptime: 00:05:32 | WPM: 20 | Freq: 650Hz ───────┐
│                                                                   │
│  OUTPUT AREA (scrollable)                                        │
│  > keying status                                                 │
│  KEYING preset=V3 wpm=20 window=60.0-99.0 late=consider         │
│  > audio status                                                  │
│  AUDIO freq=650Hz volume=65% fade_in=8ms fade_out=8ms           │
│                                                                   │
│                                                                   │
│                                                                   │
├───────────────────────────────────────────────────────────────────┤
│ Input area:                                                       │
│ > command_here_                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### Interface Features

#### Status Bar (Top)
Real-time device status displayed at the top border:
- **Device callsign**: IU3QEZ Keyer
- **Uptime**: System uptime in HH:MM:SS format
- **WPM**: Current keying speed
- **Freq**: Current sidetone frequency
- Additional indicators as needed (paddle state, audio state, etc.)

#### Output Area (Upper Panel)
Scrollable message history showing:
- Command echo (prefixed with `>`)
- Command responses (OK/ERR/status messages)
- System notifications
- Color-coded messages (ANSI colors):
  - **Green**: Success (OK messages)
  - **Red**: Errors (ERR messages)
  - **Yellow**: Warnings
  - **Cyan**: Info messages
  - **White**: Normal output

**Scrolling Controls:**
- **Page Up**: Scroll output history up
- **Page Down**: Scroll output history down
- **Home**: Jump to top of history
- **End**: Jump to bottom (latest messages)

#### Input Area (Lower Panel)
Dedicated command input with visual separator:
- Clear "Input area:" label
- Command prompt (`>`)
- Cursor position indicator
- Real-time character echo

**Input Controls:**
- **Enter**: Execute command and refresh display
- **Up Arrow**: Previous command from history
- **Down Arrow**: Next command from history
- **Tab**: Auto-complete command (if available)
- **Ctrl+L**: Manual full screen refresh
- **Ctrl+C**: Cancel current input line
- **Backspace**: Delete character

#### Auto-Refresh Behavior

The console automatically redraws the entire interface when:
1. **Initial connection** (DTR/RTS signal change detected)
   - Clears screen
   - Draws full TUI layout
   - Shows last N messages from history
   - Positions cursor in input area
2. **Command execution** (Enter pressed)
   - Updates output area with command + response
   - Redraws borders and status bar
   - Returns cursor to input area
3. **Navigation** (arrow keys, Page Up/Down)
   - Updates output area scroll position
   - Maintains input area state
4. **Manual refresh** (Ctrl+L)
   - Full redraw of entire interface
   - Useful if terminal gets corrupted

This ensures that users always see a clean, properly formatted interface even after disconnect/reconnect cycles.

### Terminal Compatibility

The TUI uses standard **VT100/ANSI escape sequences** compatible with:
- Linux: screen, minicom, picocom
- macOS: Terminal.app, iTerm2, screen
- Windows: PuTTY, TeraTerm, Windows Terminal (modern), Arduino Serial Monitor

**Minimum terminal requirements:**
- 80 columns × 24 rows (recommended: 100×30 for comfortable viewing)
- ANSI color support (optional but recommended)
- VT100 cursor positioning support

## Accessing the Console

### Hardware Connection
Connect to the keyer's USB CDC port using a serial terminal application:

**Linux/macOS:**
```bash
screen /dev/ttyACM0 115200
# or
minicom -D /dev/ttyACM0 -b 115200
# or
picocom /dev/ttyACM0 -b 115200
```

**Windows:**
Use PuTTY, TeraTerm, or the Arduino Serial Monitor:
- Port: COMx (check Device Manager)
- Baud rate: 115200
- Data bits: 8
- Parity: None
- Stop bits: 1

### Console Startup

Upon connection, the TUI automatically draws and displays:

```
┌─ IU3QEZ Keyer | Uptime: 00:00:15 | WPM: 20 | Freq: 600Hz ───────┐
│                                                                   │
│  ===== Keyer QRS2HST Firmware v1.0.0 =====                       │
│  USB Console Interface - Type 'help' for commands                │
│                                                                   │
│                                                                   │
│                                                                   │
├───────────────────────────────────────────────────────────────────┤
│ Input area:                                                       │
│ > _                                                               │
└───────────────────────────────────────────────────────────────────┘
```

The interface redraws automatically whenever you reconnect, ensuring a clean display.

## Command Structure

Commands follow this general syntax:
```
<subsystem> <action> [parameters...]
```

- **subsystem**: audio, keying
- **action**: status, start, stop, set parameters, etc.
- **parameters**: values specific to the action

Commands are **case-insensitive**. Lines starting with `#` are treated as comments and ignored.

## Audio Commands

Control sidetone generation and audio subsystem.

### `audio status`
Display current audio configuration and state.

**Example:**
```
> audio status
AUDIO freq=600Hz volume=50% fade_in=8ms fade_out=8ms state=running
```

### `audio volume <0-100>`
Set sidetone volume percentage.

**Parameters:**
- `0-100`: Volume percentage (0 = mute, 100 = maximum)

**Example:**
```
> audio volume 75
OK volume=75
```

### `audio freq <100-2000>`
Set sidetone frequency in Hertz.

**Parameters:**
- `100-2000`: Frequency in Hz

**Example:**
```
> audio freq 700
OK freq=700
```

### `audio fade <fade_in_ms> <fade_out_ms>`
Set attack/release envelope timing to avoid clicks.

**Parameters:**
- `fade_in_ms`: Attack time in milliseconds (0-500)
- `fade_out_ms`: Release time in milliseconds (0-500)

**Example:**
```
> audio fade 10 15
OK fade_in=10 fade_out=15
```

### `audio start`
Start sidetone generation (unmute).

**Example:**
```
> audio start
OK sidetone started
```

### `audio stop`
Stop sidetone generation (mute).

**Example:**
```
> audio stop
OK sidetone stopped
```

### `audio save`
Save current audio configuration to non-volatile storage.

**Example:**
```
> audio save
OK config saved
```

### `audio load`
Reload audio configuration from storage and apply.

**Example:**
```
> audio load
OK config reloaded
```

### `audio help`
Display auto-generated help text for all audio parameters from the Parameter Metadata System.

**Output Format:**
- Parameter name (short form, without subsystem prefix)
- Type (int, float, bool, enum)
- Description
- Valid range in brackets (type-specific format)
- Unit (if applicable)
- Current value

**Example:**
```
> audio help
Audio Parameters:
  freq (int): Sidetone frequency [100-2000] Hz (current: 700)
  volume (int): Sidetone volume [0-100] % (current: 50)
  fade_in (int): Sidetone fade-in time [0-100] ms (current: 8)
  fade_out (int): Sidetone fade-out time [0-100] ms (current: 8)
  enabled (bool): Sidetone enabled [on/off] (current: off)
```

**Notes:**
- Help text is generated from parameter metadata at runtime
- Only visible parameters for current configuration are shown
- Range format varies by type:
  - `int`: "MIN-MAX" (e.g., "100-2000")
  - `float`: "min-max" with decimal precision (e.g., "0.0-100.0")
  - `bool`: "true_name/false_name" (e.g., "on/off", "consider/forget")
  - `enum`: "value1/value2/..." (e.g., "V0/V1/.../MANUAL")

---

## Keying Commands

Configure paddle behavior, iambic modes, and timing parameters.

### `keying status` (or just `keying`)
Display current keying configuration.

**Example:**
```
> keying status
KEYING preset=V3 wpm=20 window=60.0-99.0 late=consider latch=state dit_mem=on dah_mem=on
```

**Fields:**
- `preset`: Active preset (V0-V9, MANUAL)
- `wpm`: Keying speed in words per minute
- `window`: Memory latch window (percentage of element duration)
- `late`: Late release behavior (consider/forget)
- `latch`: Trigger mode (state/edge)
- `dit_mem`: Dit paddle memory (on/off)
- `dah_mem`: Dah paddle memory (on/off)

### `keying help`
Display auto-generated help text for all keying parameters from the Parameter Metadata System.

**Output Format:**
- Parameter name (short form, without subsystem prefix)
- Type (int, float, bool, enum)
- Description
- Valid range in brackets (type-specific format)
- Unit (if applicable)
- Current value

**Example:**
```
> keying help
Keying Parameters:
  wpm (int): Keying speed [5-80] WPM (current: 20)
  preset (enum): Iambic keying preset [V0/V1/V2/V3/V4/V5/V6/V7/V8/V9/MANUAL] (current: V3)
  window_open (float): Memory window open threshold [0.0-100.0] % (current: 60.0)
  window_close (float): Memory window close threshold [0.0-100.0] % (current: 99.0)
  late (bool): Consider late paddle release [consider/forget] (current: consider)
  dit_memory (bool): Manual mode dit memory enable [on/off] (current: on)
  dah_memory (bool): Manual mode dah memory enable [on/off] (current: on)
  latch (bool): Manual mode latch type [state/edge] (current: state)
```

**Notes:**
- Help text is generated from parameter metadata at runtime
- Window parameters (window_open, window_close) are only visible when preset is set to MANUAL (Task 5.7.6)
- Range format varies by type (see `audio help` for details)

### `keying preset <V0-V9|manual>`
Select an iambic keying preset.

**Presets:**
- **V0**: Super Keyer II/III - Both memories, state-latch, 55% window
- **V1**: Super Keyer II/III - Dit memory only
- **V2**: Super Keyer II/III - Dah memory only
- **V3**: Accukeyer - Both memories (DEFAULT)
- **V4**: Accukeyer - Dit memory only
- **V5**: Accukeyer - Dah memory only
- **V6**: Curtis Mode A - Both memories, edge-trigger
- **V7**: Curtis Mode A - Dit memory only
- **V8**: Curtis Mode A - Dah memory only
- **V9**: No memory (straight key emulation)
- **manual**: Custom configuration (use individual parameter commands)

**Example:**
```
> keying preset V6
OK preset set
KEYING preset=V6 wpm=20 window=60.0-99.0 late=consider latch=edge dit_mem=on dah_mem=on
```

### `keying wpm <5-80>`
Set keying speed in words per minute (PARIS standard).

**Parameters:**
- `5-80`: Speed in WPM

**Example:**
```
> keying wpm 25
OK wpm updated
KEYING preset=V3 wpm=25 window=60.0-99.0 late=consider latch=state dit_mem=on dah_mem=on
```

### `keying window <open_percent> <close_percent>`
Set memory latch window (advanced - switches to MANUAL preset).

**Parameters:**
- `open_percent`: Window opens at this % of element duration (0.0-100.0)
- `close_percent`: Window closes at this % of element duration (0.0-100.0)

**Example:**
```
> keying window 50 95
OK window updated
KEYING preset=MANUAL wpm=20 window=50.0-95.0 late=consider latch=state dit_mem=on dah_mem=on
```

### `keying late <consider|forget>`
Set late release behavior.

**Modes:**
- `consider`: Emit queued element even if paddle releases during gap (forgiving)
- `forget`: Cancel queued element if paddle releases before gap finishes (strict)

**Example:**
```
> keying late forget
OK late release updated
```

### `keying memory <dit|dah> <on|off>`
Enable/disable memory for individual paddles (switches to MANUAL preset).

**Example:**
```
> keying memory dit off
OK memory flag updated
KEYING preset=MANUAL wpm=20 window=60.0-99.0 late=consider latch=state dit_mem=off dah_mem=on
```

### `keying latch <edge|state>`
Set memory trigger mechanism (switches to MANUAL preset).

**Modes:**
- `edge`: Edge-triggered (Curtis Mode A) - memory arms on press event
- `state`: State-latched (Accukeyer) - paddle must remain pressed during window

**Example:**
```
> keying latch edge
OK latch mode updated
KEYING preset=MANUAL wpm=20 window=60.0-99.0 late=consider latch=edge dit_mem=on dah_mem=on
```

### `keying save`
Save current keying configuration to non-volatile storage.

**Example:**
```
> keying save
OK keying config saved
```

### `keying reload`
Reload keying configuration from storage and apply to engine.

**Example:**
```
> keying reload
OK keying config reloaded
KEYING preset=V3 wpm=20 window=60.0-99.0 late=consider latch=state dit_mem=on dah_mem=on
```

---

## Global Commands

### `help`
Display available commands and subsystems.

**Example:**
```
> help
Available subsystems:
  audio   - Sidetone and audio configuration
  keying  - Paddle and iambic mode configuration

Global commands:
  help    - Show this help message
  reboot  - Reboot device into bootloader (USB DFU mode)

Type '<subsystem> help' for subsystem-specific commands
Examples:
  audio status
  keying preset V6
  keying wpm 25
```

### `reboot`
Reboot the device into bootloader mode (USB DFU) for firmware updates.

**Usage:**
```
> reboot
Rebooting into bootloader (USB DFU mode)...
Please wait 3 seconds before reconnecting.
```

**Notes:**
- Device will restart and enter USB DFU mode
- Used for firmware updates via USB (no external programmer needed)
- Console connection will be dropped
- Wait ~3 seconds before attempting to reconnect or flash new firmware
- Use `esptool.py` or ESP-IDF's `idf.py dfu-flash` to upload firmware in DFU mode

**Warning:** Only use this command when you intend to update firmware. Normal operation does not require reboots.

---

## Common Workflows

### Initial Setup (First Power-On)

1. **Connect to console**
   ```
   screen /dev/ttyACM0 115200
   ```

2. **Check default configuration**
   ```
   > keying status
   > audio status
   ```

3. **Set your preferred speed**
   ```
   > keying wpm 25
   ```

4. **Select keying mode**
   ```
   > keying preset V6    # Curtis Mode A
   ```

5. **Configure sidetone**
   ```
   > audio freq 650
   > audio volume 60
   > audio start
   ```

6. **Save configuration**
   ```
   > keying save
   > audio save
   ```

### Switching Between Keyer Modes

**Try Accukeyer mode (state-latch, forgiving):**
```
> keying preset V3
```

**Try Curtis Mode A (edge-trigger, responsive):**
```
> keying preset V6
```

**Try straight key mode (no memory):**
```
> keying preset V9
```

### Speed Practice Session

**Start slow:**
```
> keying wpm 15
```

**Incrementally increase:**
```
> keying wpm 18
> keying wpm 20
> keying wpm 25
```

### Advanced: Custom Timing

**Tighten squeeze window for cleaner iambic:**
```
> keying window 65 95
> keying latch state
> keying save
```

### Troubleshooting: Restore Defaults

**Reload from storage:**
```
> keying reload
> audio reload
```

**Or manually reset to Accukeyer defaults:**
```
> keying preset V3
> keying wpm 20
> audio freq 600
> audio volume 50
> keying save
> audio save
```

---

## Configuration Management Commands

Task 5.5 provides configuration versioning, backup/restore, and profile cloning functionality.

### `config status`
Show configuration version and backup status.

**Example:**
```
> config status
CONFIG version=1 callsign=IU3QEZ
  backup=exists
```

### `config backup [namespace]`
Backup current configuration to alternate NVS namespace (default: `keyer_backup`).

**Example:**
```
> config backup
Backing up configuration to 'keyer_backup'...
OK configuration backed up

> config backup my_backup
Backing up configuration to 'my_backup'...
OK configuration backed up
```

**Use case:** Create a snapshot before experimenting with paddle settings.

### `config restore [namespace]`
Restore configuration from backup namespace and apply to all subsystems.

**Example:**
```
> config restore
Restoring configuration from 'keyer_backup'...
OK configuration restored and applied

> config restore my_backup
Restoring configuration from 'my_backup'...
OK configuration restored and applied
```

**Notes:**
- Automatically reloads config into memory and applies to audio/keying subsystems
- Displays `ERR no backup found` if namespace doesn't exist
- Displays `WARN keying config apply failed` if keying engine fails to reconfigure

### `config clone <source> <dest>`
Clone configuration between NVS namespaces (advanced usage).

**Example:**
```
> config clone keyer keyer_v3_fast
Cloning configuration 'keyer' -> 'keyer_v3_fast'...
OK configuration cloned

> config clone keyer_backup experiment1
Cloning configuration 'keyer_backup' -> 'experiment1'...
OK configuration cloned
```

**Use cases:**
- Create named profiles for different operating styles (contest, ragchew, CW practice)
- Experiment with settings without losing working config
- Share configurations between devices (export namespace, restore on another device)

### Configuration Versioning

The firmware tracks configuration version for automatic migration:
- **Version 0**: Pre-versioning configs (legacy)
- **Version 1**: Initial versioned config (current)

When loading older configs, migration logic automatically updates structure to current version.

**Workflow example:**
```
# 1. Backup current working config
> config backup

# 2. Experiment with aggressive memory settings
> keying preset manual
> keying window 40 95
> keying late forget
> keying save

# 3. Test paddle behavior... if unsatisfactory:
> config restore
OK configuration restored and applied

# 4. Your original settings are back!
```

---

## Response Format

All commands return structured responses:

### Success Response
```
OK <confirmation message>
```

### Error Response
```
ERR <error description>
```

### Status Response
```
<SUBSYSTEM> <field1>=<value1> <field2>=<value2> ...
```

---

## Tips & Best Practices

1. **Use `status` commands frequently** - Always check current state before making changes
2. **Save after configuration** - Use `save` commands to persist settings across reboots
3. **Start with presets** - Use V0-V9 presets before tweaking individual parameters
4. **Test incrementally** - Change one parameter at a time and test paddle response
5. **Comments are your friend** - Use `# comment` lines to document your console sessions
6. **Sidetone helps learning** - Keep sidetone enabled during practice (`audio start`)

---

## Console Session Example

```
> # Initial setup for Curtis Mode A keying
> keying preset V6
OK preset set
KEYING preset=V6 wpm=20 window=60.0-99.0 late=consider latch=edge dit_mem=on dah_mem=on

> # Set speed for comfortable sending
> keying wpm 22
OK wpm updated

> # Configure pleasant sidetone
> audio freq 650
OK freq=650
> audio volume 65
OK volume=65
> audio start
OK sidetone started

> # Save configuration
> keying save
OK keying config saved
> audio save
OK config saved

> # Verify final state
> keying status
KEYING preset=V6 wpm=22 window=60.0-99.0 late=consider latch=edge dit_mem=on dah_mem=on
> audio status
AUDIO freq=650Hz volume=65% fade_in=8ms fade_out=8ms state=running
```

---

## Diagnostic Commands

### `debug`
Display current global log level and show available commands.

**Example:**
```
> debug
Current log level: info

Available levels: none, error, warn, info, debug, verbose
Subcommands:
  debug tags     - List all available logging tags
  debug timeline - Dump timeline hooks status
Usage:
  debug <level>       - Set global log level
  debug <tag> <level> - Set log level for specific tag
  debug tags          - Show all available tags
  debug timeline      - Dump timeline hooks status

Examples:
  debug debug                - Set global level to debug
  debug paddle_engine debug  - Set paddle_engine to debug
  debug tags                 - List all logging tags
```

### `debug <level>`
Set global ESP-IDF log level for all components.

**Parameters:**
- `level`: Log level (none, error, warn, info, debug, verbose)

**Example:**
```
> debug debug
Global log level set to: debug
D (12345) test: Test DEBUG log (global)
I (12346) test: Test INFO log (global)
```

### `debug <tag> <level>`
Set log level for a specific component tag.

**Parameters:**
- `tag`: Component tag name (see `debug tags` for available tags)
- `level`: Log level (none, error, warn, info, debug, verbose)

**Example:**
```
> debug paddle_engine debug
Log level for 'paddle_engine' set to: debug
D (23456) paddle_engine: Test DEBUG log for paddle_engine
I (23457) paddle_engine: Test INFO log for paddle_engine
```

**Use Cases:**
- `debug paddle_engine verbose` - Detailed paddle state debugging
- `debug morse_decoder debug` - See decoded patterns and timing
- `debug remote_server none` - Silence remote server logs
- `debug WiFiSubsystem error` - Only show WiFi errors

### `debug tags`
List all available logging tags in the codebase.

**Example:**
```
> debug tags

Known Logging Tags:
-------------------
  ConfigStorage
  HttpServer
  LedDriver
  PaddleHal
  RemoteCwClient
  SerialConsole
  SidetoneService
  TimelineEmitter
  TxHal
  WiFiSubsystem
  app
  audio_subsystem
  console_param_bridge
  console_system
  diagnostics_subsystem
  init_phases
  init_pipeline
  keying_subsystem
  morse_decoder
  morse_table
  paddle_engine
  param_registry
  remote_server
  usb_early

Total: 24 tags

Usage:
  debug <tag> <level>  - Set log level for specific tag
  debug <level>        - Set global log level

Example:
  debug paddle_engine debug  - Enable debug logs for paddle_engine
  debug paddle_engine none   - Disable all logs for paddle_engine
```

**Notes:**
- Tags correspond to component names in the codebase
- Use this command to discover available tags before filtering
- Tag filtering is useful for isolating issues in specific subsystems

### `debug timeline`
Dump timeline hooks status for LOGIC overlay diagnostics.

**Example:**
```
> debug timeline
Dumping timeline hooks status...
Timeline hooks dump complete (check log output)
```

**Output (in log):**
```
I (34567) keying_subsystem: === Timeline Hooks Status ===
I (34568) keying_subsystem: Timeline Emitter: connected (0x3fc9a123)
I (34569) keying_subsystem:
I (34570) keying_subsystem: Paddle Callbacks (in paddle_callbacks_ struct):
I (34571) keying_subsystem:   OnMemoryWindowChanged: connected (0x42001234)
I (34572) keying_subsystem:   OnLatchStateChanged:   connected (0x42001256)
I (34573) keying_subsystem:   OnSqueezeDetected:     connected (0x42001278)
I (34574) keying_subsystem:   Context pointer:       0x3fc9a456
I (34575) keying_subsystem:
I (34576) keying_subsystem: Note: Hooks are copied to PaddleEngine during Initialize().
I (34577) keying_subsystem: If hooks show NULL, check:
I (34578) keying_subsystem:   1. SetTimelineEmitter() was called before Initialize()
I (34579) keying_subsystem:   2. TimelineEventEmitter::GetHooks() returns valid pointers
I (34580) keying_subsystem:   3. PaddleEngine::Initialize() received and copied callbacks
I (34581) keying_subsystem: ===========================
```

### `keying-debug`
Dump complete paddle engine state for troubleshooting lockups.

**Example:**
```
> keying-debug
Dumping paddle engine state...
State dump complete (check log output)
```

**Output (in log):**
Shows detailed state including:
- Current state machine state
- Paddle states (dit/dah pressed)
- Memory flags (dit_requested, dah_requested)
- Latch status (seen_this_element, last_valid_combo)
- Current element timing (start, end, duration)
- Configuration (WPM, mode, memory window, squeeze mode)

**Use Cases:**
- Paddle not responding
- Stuck in keying state
- Memory mode not working
- Squeeze detection issues

### `reboot`
Restart the device immediately.

**Example:**
```
> reboot

Rebooting device...
```

**Notes:**
- Device will restart within 100ms
- All unsaved configuration will be lost
- Serial console will disconnect

### `factory-reset confirm`
Erase all NVS flash data and restore factory defaults.

**Example:**
```
> factory-reset
WARNING: This will erase ALL configuration and reboot!

This action will:
  - Erase all NVS flash data
  - Reset to factory defaults
  - Reboot the device

To proceed, type: factory-reset confirm

> factory-reset confirm

Erasing NVS flash...
NVS erased successfully!
Rebooting to factory defaults...
```

**Safety:**
- Requires "confirm" argument to prevent accidental resets
- Shows warning message if confirm is missing
- Irreversible operation

---

## See Also

- [DEVELOPMENT.md](DEVELOPMENT.md) - Hardware wiring and development setup
- [docs/RemoteCwNetProtocol.md](RemoteCwNetProtocol.md) - Remote keying protocol
- Iambic mode specifications - See PRD section on keying modes

---

*Document version: 1.2*
*Last updated: 2025-11-08*
*Changes: Added comprehensive diagnostic commands documentation (debug, debug tags, debug timeline, keying-debug, reboot, factory-reset) with examples and use cases for log filtering by component tag*
