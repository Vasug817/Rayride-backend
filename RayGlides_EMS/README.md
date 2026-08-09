# RayGlides Smart EMS — Firmware

ESP32-S3 (N16R8: 16MB quad flash, 8MB octal PSRAM) Energy Management System
firmware for the RayGlides solar-assisted electric rickshaw. Built with
PlatformIO / Arduino framework.

## Folder Structure

```
RayGlides_EMS/
├── platformio.ini              Project + build configuration
├── README.md                   This file
│
├── include/
│   └── config.h                 Shared pins, thresholds, and constants
│
├── src/
│   ├── main.cpp                 Entry point: setup()/loop(), wires all modules together
│   │
│   ├── battery/
│   │   ├── BatteryStateMachine.h/.cpp
│   │   ├── PSEUDOCODE.md         Battery monitoring pseudocode
│   │                             Battery monitoring: reads SOC, runs the
│   │                             IDLE -> CHARGING -> FULLY_CHARGED state machine
│   │
│   ├── fault/
│   │   ├── FaultDetection.h/.cpp
│   │                             Debounced fault codes (F001-F003, F008-F009),
│   │                             severity classification
│   │
│   ├── relay/
│   │   ├── RelayControl.h/.cpp
│   │   ├── PSEUDOCODE.md         Relay control pseudocode
│   │                             Physical relay + indicator LED control, with
│   │                             a hard safety interlock on critical faults
│   │
│   ├── charging/
│   │   ├── ChargingDecision.h/.cpp
│   │                             Solar / Grid / Hybrid / No-Charge mode selection
│   │
│   └── protocol/
│       ├── RayGlidesProtocol.h/.cpp
│       ├── PSEUDOCODE.md         Communication module pseudocode
│                                 RayGlides framed byte protocol: send + receive +
│                                 checksum validation + timeout watchdog
│
├── lib/                         Reserved for third-party libraries
├── test/                        Reserved for PlatformIO unit tests
└── docs/                        Design documents, diagrams, reference material
```

## Module Responsibilities

| Module | Responsibility |
|---|---|
| `config.h` | Single source of truth for pins and tunable thresholds |
| `BatteryMonitor` | Reads voltage (via divider), current (ACS712-style), temperature (LM35-style) - SOC comes from `SOCEstimator`, not a raw ADC percentage |
| `SOCEstimator` | Hybrid battery SOC estimation: OCV lookup table (voltage-based) + Coulomb counting (current integration), blended |
| `SOHEstimator` | Battery State of Health: internal resistance drift + equivalent cycle-count fade, combined |
| `DebugLog` | Structured UART debug logging: timestamp + level + module tag, with runtime enable/disable and level filtering |
| `FaultLog` | Two-tier fault logging: RAM ring buffer (all faults) + NVS-persisted history (critical faults only, survives reboot) |
| `EEPROMStorage` | Low-level EEPROM (flash-emulated) read/write with magic number + checksum validation |
| `DataLogger` | Periodic full-system RAM history buffer + EEPROM checkpointing of SOC/SOH estimator state, so estimates survive a reboot |
| `SolarMonitor` | Reads panel voltage and current, derives power - bundled into one `SolarData` struct |
| `MPPTAlgorithm` | Perturb & Observe and Incremental Conductance MPPT, driving the DC-DC converter's PWM duty cycle |
| `CANComm` | CAN bus communication (ESP32 TWAI controller, 500 kbit/s) - status and fault frames |
| `RS485Comm` | RS485 half-duplex communication (UART2 + DE/RE direction pin) - reuses the RayGlidesProtocol frame format |
| `BatteryStateMachine` | Pure state logic: decides IDLE/CHARGING/FULLY_CHARGED from SOC + fault criticality |
| `FaultDetection` | Debounced fault detection across battery (voltage/current/temp), solar, and comm faults |
| `RelayControl` | Translates state + fault into a physical relay position; enforces the fault-open safety interlock |
| `ChargingDecision` | Decides Solar Only / Grid Only / Hybrid / No Charge from solar power + grid availability |
| `RayGlidesProtocol` | Builds/sends full battery + solar telemetry & fault frames; parses/validates incoming frames; tracks comm timeout |
| `main.cpp` | Reads sensors, runs modules in priority order each cycle, applies outputs |

## Build & Run

```
pio run                 # build (env:esp32-s3-n16r8, see platformio.ini)
pio run -t upload       # flash to a connected ESP32-S3 N16R8 board
pio device monitor       # open serial monitor (115200 baud)
```

Pin assignments in `include/config.h` are deliberately confined to
GPIO1-18/21 - the N16R8's octal PSRAM reserves GPIO26-37, and GPIO19/20
carry the native USB Serial link this firmware's protocol and debug
logging both run over. See the comment block at the top of `config.h`
before repurposing any pin.

For simulation without hardware, load into Wokwi with a potentiometer + pushbutton
+ LEDs standing in for the battery/solar sensors, grid-connect line, and relay/fault
indicators.
