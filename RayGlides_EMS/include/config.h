#ifndef CONFIG_H
#define CONFIG_H

// --- Target board: ESP32-S3-WROOM-1 N16R8 (16MB quad SPI flash, 8MB octal
// SPI PSRAM) ---
// N16R8's octal PSRAM (on top of the flash pins every ESP32-S3 module
// needs) reserves GPIO26-37 entirely - those pins are wired to the
// in-package flash/PSRAM die and are NOT available as general-purpose
// I/O, unlike on a plain ESP32-S3 module without PSRAM. GPIO0/3/45/46
// are strapping pins (boot mode / VDD_SPI voltage) best left alone as
// outputs, and GPIO19/20 are the native USB D-/D+ lines this firmware's
// USB-Serial link runs over. Every pin below is chosen from the
// remaining GPIO1-18/21 range for exactly that reason - see
// platformio.ini for the matching board_build settings
// (board_build.arduino.memory_type = qio_opi).

// Set to true to simulate stable sensor readings when running standalone on a desk.
// Set to false to read raw analog pin values from the physical ADC.
#define SIMULATE_SENSORS true


// --- Pin assignments ---
#define BATTERY_VOLTAGE_PIN 1   // ADC1_CH0 - Voltage sense (via 200k/10k voltage divider)
#define BATTERY_CURRENT_PIN 2   // ADC1_CH1 - Current sense (simulated ACS712-style)
#define BATTERY_TEMP_PIN     4   // ADC1_CH3 - Temperature sense (simulated LM35-style); GPIO3 skipped (JTAG/strapping)
#define SOLAR_VOLTAGE_PIN    5   // ADC1_CH4 - Solar panel voltage sense (via divider)
#define SOLAR_CURRENT_PIN    6   // ADC1_CH5 - Solar panel current sense (simulated ACS712 5A-style)
#define GRID_BUTTON_PIN       7
#define RELAY_PIN            14  // Physical charging relay control pin
#define CHARGE_LED_PIN       15  // Charging indicator
#define FAULT_LED_PIN         16  // Fault indicator
#define SOLAR_LED_PIN         15  // Shared with charge LED in single-source demos
#define GRID_LED_PIN           16

// --- ADC ---
#define ADC_MAX 4095.0
#define ADC_VREF 3.3

// --- Voltage scaling (matches the 200k/10k voltage divider design) ---
#define VOLTAGE_DIVIDER_RATIO 21.0   // (R1+R2)/R2 = 210k/10k

// --- Current sensing (simulated ACS712 30A-style sensor - battery pack) ---
#define CURRENT_SENSOR_MIDPOINT 1.65  // Volts at 0A (ADC_VREF / 2)
#define CURRENT_SENSITIVITY_V_PER_A 0.066

// --- Solar sensing ---
#define SOLAR_VOLTAGE_DIVIDER_RATIO 8.0     // Scales sensed voltage up to ~26V range
#define SOLAR_CURRENT_SENSITIVITY_V_PER_A 0.185  // ACS712 5A-style (smaller panel currents)

// --- Temperature sensing (simulated LM35-style: 10mV per degree C) ---
#define TEMP_MV_PER_C 10.0

// --- Battery / charging thresholds ---
#define DETECT_MIN_RAW     50    // Below this raw ADC = battery not detected (F001)
#define UNDER_VOLT_MIN      42.0  // Volts - below this = under-voltage (F003)
#define OVER_VOLT_MAX       68.0  // Volts - above this = over-voltage (F002)
#define OVER_CURRENT_A      25.0  // Amps - over-current threshold (F011)
#define OVER_TEMP_C         60.0  // Celsius - over-temperature threshold (F004)
#define FULL_CHARGE_SOC     100
#define RESUME_CHARGE_SOC   95
#define DEBOUNCE_COUNT       3    // Consecutive readings needed to latch a fault

// --- Solar fault detection ---
#define SOLAR_FAULT_MIN_CURRENT 0.3  // Amps - current above this with ~0V is implausible (F005)
#define SOLAR_MAX_PLAUSIBLE_RAW 4090 // Raw ADC pinned near max = likely a wiring/short fault

// --- Charging mode thresholds (watts, derived from solar voltage x current) ---
#define SOLAR_SUFFICIENT_W   150.0  // Strong sun - solar alone can fully charge
#define SOLAR_USABLE_W         20.0  // Some usable solar, not enough alone

// --- Communication ---
#define COMM_TIMEOUT_MS    5000  // No valid frame within this window -> F008

// --- MPPT (Maximum Power Point Tracking) ---
#define MPPT_PWM_PIN               8    // PWM output to the DC-DC converter
#define MPPT_PWM_CHANNEL             0    // LEDC channel (0-15 available on ESP32)
#define MPPT_PWM_FREQ_HZ         5000
#define MPPT_PWM_RESOLUTION_BITS    8    // 0-255 duty steps
#define MPPT_DUTY_STEP_MIN         0.002 // Minimum adaptive perturbation size, used right at the MPP (finer than the old fixed 1% step)
#define MPPT_DUTY_STEP_MAX          0.03  // Maximum adaptive perturbation size, used far from the MPP / during fast irradiance swings
#define MPPT_ADAPTIVE_POWER_SCALE    5.0  // Watts of |deltaP| that maps to the max step; scales ~linearly below this
#define MPPT_DUTY_MIN              0.0
#define MPPT_DUTY_MAX              0.95 // Leave headroom, never command 100%
#define MPPT_POWER_EPSILON         0.05  // Watts - below this, treat power as "unchanged"
#define MPPT_VOLTAGE_EPSILON       0.02  // Volts - below this, treat voltage as "unchanged"
#define MPPT_CURRENT_EPSILON       0.01  // Amps - below this, treat current as "unchanged"
#define MPPT_COND_EPSILON          0.01  // Tolerance when comparing dI/dV to -I/V
#define MPPT_INTERVAL_MS            200  // MPPT algorithm re-evaluates duty this often, independent of loop() cadence
#define MPPT_SOFT_START_DURATION_MS 3000  // Ramp time (ms) from 0% duty up to the target duty on enable/recovery
#define MPPT_DUTY_SLEW_RATE_PER_S    0.5  // Max fractional duty change per second (e.g. 0.5 = 50%/s), applied to every write
#define MPPT_FILTER_ALPHA            0.2  // EMA weight on the newest solar V/I sample (0-1; lower = smoother/slower)
#define MPPT_STATS_LOG_INTERVAL_MS 30000  // How often updateMPPT() logs a cumulative performance summary

// --- CAN bus (TWAI controller, built into the ESP32) ---
#define CAN_TX_PIN          9
#define CAN_RX_PIN         10
#define CAN_ID_STATUS      0x100
#define CAN_ID_FAULT       0x101

// --- RS485 (over UART2, with a transceiver DE/RE direction pin) ---
#define RS485_TX_PIN       11
#define RS485_RX_PIN       12
#define RS485_DE_PIN       13
#define RS485_BAUD       9600

// --- Battery SOC estimation ---
#define BATTERY_CAPACITY_AH         20.0  // Pack capacity, amp-hours
#define SOC_REST_CURRENT_THRESHOLD_A 0.5  // |current| below this = "at rest" for OCV correction
#define SOC_BLEND_ALPHA               0.9  // Weight kept on Coulomb count when re-anchoring (0-1)

// --- Battery SOH estimation ---
#define BATTERY_NOMINAL_INTERNAL_RESISTANCE_OHMS 0.10  // "New battery" baseline
#define SOH_MIN_DELTA_I_FOR_R_ESTIMATE              1.0   // Amps - below this, dV/dI too noisy to trust
#define SOH_MAX_PLAUSIBLE_RESISTANCE_OHMS           5.0   // Reject outlier samples above this
#define SOH_R_EMA_ALPHA                              0.9   // Smoothing weight on the running resistance estimate
#define SOH_DEGRADATION_PER_CYCLE_PCT                0.02  // % health lost per equivalent full cycle

// --- UART debugging ---
#define DEBUG_ENABLED_DEFAULT true

// --- Fault logging ---
#define FAULT_LOG_MAX_ENTRIES    20  // RAM ring buffer size (all severities)
#define FAULT_LOG_PERSIST_MAX    10  // NVS-persisted ring size (CRITICAL faults only)

// --- EEPROM storage ---
#define EEPROM_SIZE               64          // Bytes reserved for the EEPROM emulation region
#define EEPROM_STATE_MAGIC 0xEA57C0DE          // Marks the region as containing valid saved state

// --- Data logger ---
#define DATALOGGER_MAX_RECORDS             30    // RAM history ring buffer size
#define DATALOGGER_RECORD_INTERVAL_MS   10000    // Snapshot to RAM every 10s (not every loop)
#define DATALOGGER_CHECKPOINT_INTERVAL_MS 60000    // Persist estimator state to EEPROM every 60s

// --- Loop timing ---
#define LOOP_DELAY_MS 500

// --- Watchdog recovery ---
#define WATCHDOG_TIMEOUT_S               8      // TWDT timeout (s) - must exceed worst-case loop() time
#define WATCHDOG_HEALTHY_UPTIME_MS    30000      // Stable runtime needed before the crash counter clears
#define WATCHDOG_MAX_CONSECUTIVE_RESETS  3      // Consecutive WDT resets >= this => lockout
#define WATCHDOG_STATE_EEPROM_OFFSET     32      // Byte offset in EEPROM region (past PersistedEstimatorState)
#define WATCHDOG_STATE_MAGIC     0xDEADBEEF      // Marks the watchdog-state EEPROM region as valid

// --- OTA update ---
#define OTA_CHUNK_MAX_DATA        30            // = MAX_PAYLOAD(32) - 2 bytes reserved for the sequence number
#define OTA_STATE_EEPROM_OFFSET   44                 // Byte offset in EEPROM region (past WatchdogState)
#define OTA_STATE_MAGIC           0xFEEDC0DE         // Marks the OTA-state EEPROM region as valid
#define OTA_MAX_BOOT_ATTEMPTS     2                  // Unconfirmed boots of a new image before auto-rollback

// --- Wi-Fi / MQTT Settings ---
#define WIFI_DEFAULT_SSID "iPhone"
#define WIFI_DEFAULT_PASS "Vasu@2005"
#define MQTT_DEFAULT_BROKER "172.20.10.7"
#define MQTT_DEFAULT_PORT 1883

#endif

