# Hardware-in-the-Loop Tester Interface Requirements

## Goal
- Provide a Python API that external hardware testers can implement to drive paddle inputs and measure response timing for the keyer firmware.

## Scope
- The interface must support both command-and-measure (Codex prompts hardware jig to toggle paddles) and passive capture (hardware jig reports observed events and timestamps).
- Focus on deterministic timing recording (microsecond precision) and reproducible sequences.
- Interface should not assume a specific communication transport; allow serial (PySerial), TCP, or other mechanisms.
- The hardware fixture exposes an interactive console (text-based) that accepts simple commands and streams structured responses; commands map 1:1 to the Python API methods below.

## Functional Requirements
- Expose abstract base classes for:
  - `PaddleStimulusProvider`: schedules toggle events (`press_dit`, `release_dit`, `press_dah`, `release_dah`, optional `press_key`) with intended timing offsets.
  - `MeasurementSink`: reports observed firmware reactions (timestamped events, e.g., `paddle_edge_detected`, `sidetone_started`).
  - `LatencyProbe`: convenience helpers to compute latency between commanded edge and observed response.
- Provide a reference `TestScenario` descriptor describing sequences (e.g., `[{"action": "press_dit", "delay_us": 0}, ...]`).
- Include serialization schema (JSON) for exchanging scenario definitions and measurement results.
- Define timebase expectations: the jig should synchronize to wall-clock or provide monotonic timestamps (microseconds) with known offset.
- Supply utility functions:
  - Scenario validation (ensure timings non-negative, sequences feasible).
  - Result comparison vs thresholds (latency <= 300 microseconds jitter, etc.).
- Document initialization handshake: the harness negotiates capabilities (max toggle rate, supported paddles, measurement channels).
- Console command protocol:
  - `HELLO` → fixture replies with JSON capabilities payload (`{"protocol":"paddle-test","version":"1.0","supports":["dit","dah","key","latency","capture"],"timebase":"us"}`).
  - `SCENARIO <json>` → load scenario definition; reply `OK` or `ERROR <message>`.
  - `RUN` → execute loaded scenario, streaming measurement JSON lines (`{"event":"stimulus","id":...}`, `{"event":"measurement","timestamp_us":...}`) and ending with `DONE`.
  - `ABORT` → halt active scenario, reply `ABORTED`.
  - `MEASURE <channel>` → single measurement for a specific commutation sensor (e.g., `dit_edge`, `dah_edge`, `key_edge`), reply with `{ "event":"measurement","channel":..., "timestamp_us":... }`.
  - `STATUS` → fixture health/arming status.
  - `RESET` → clear state; respond `RESET`.
  - Console must tolerate `#`-prefixed comment lines and ignore blank input.
- Responses must be newline-delimited UTF-8 text; measurement payloads use microsecond timestamps and identify source (`hardware`, `firmware`, `logic_analyzer`). The fixture does **not** evaluate sidetone—only electrical commutations (DIT, DAH, KEY) and any auxiliary digital channels are required.
- Include support for bidirectional metadata: fixture can emit `NOTICE <text>` informational lines; Python harness should surface these via logging.

### Example Console Exchanges

**Capability negotiation**
```
> HELLO
{"protocol":"paddle-test","version":"1.0","supports":["dit","dah","key","latency","capture"],"timebase":"us","max_toggle_rate_hz":1200}
```

**Load and run a scenario**
```
> SCENARIO {"name":"dit_hold","steps":[{"action":"press_dit","delay_us":0},{"action":"release_dit","delay_us":50000}]}
OK
> RUN
{"event":"stimulus","action":"press_dit","scheduled_timestamp_us":173000000}
{"event":"measurement","channel":"dit_edge","timestamp_us":173000050,"source":"hardware"}
{"event":"stimulus","action":"release_dit","scheduled_timestamp_us":173050000}
{"event":"measurement","channel":"dit_edge","timestamp_us":173050120,"source":"hardware"}
{"event":"latency","stimulus_id":0,"channel":"dit_edge","latency_us":50}
{"event":"latency","stimulus_id":1,"channel":"dit_edge","latency_us":120}
DONE
```

**On-demand measurement**
```
> MEASURE dah_edge
{"event":"measurement","channel":"dah_edge","timestamp_us":173200345,"source":"logic_analyzer"}
```

## Non-Functional Requirements
- Pure Python 3.10 compatible; avoid platform-specific dependencies.
- Keep dependencies minimal (std library + optional PySerial).
- Provide thorough docstrings and type hints for third-party implementers.
- Support logging hooks (structlog or std logging) to trace sequences.

## Deliverables
- `scripts/hil/paddle_tester.py` module defining ABCs and helper classes.
- Example JSON scenario file (`scripts/hil/examples/basic_dit_sequence.json`).
- Documentation snippet in `DEVELOPMENT.md` referencing the interface and explaining integration steps.
- Update `CHANGELOG.md` and `journal.md` when interface is finalized.
