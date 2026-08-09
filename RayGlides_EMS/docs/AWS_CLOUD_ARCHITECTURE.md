# RayGlides EMS — AWS Cloud Communication Architecture

## 1. Starting point: what the firmware actually does today

The ESP32 EMS firmware (`RayGlidesProtocol.cpp`) has **no WiFi or cloud
connectivity of its own**. It speaks a compact binary frame protocol over
USB Serial (and RS485) only:

```
[0xAA][msgId][len][payload...][checksum][0x55]
checksum = msgId XOR len XOR payload bytes (XOR)
```

Message types it already implements: `STATUS_UPDATE (0x01)`,
`FAULT_REPORT (0x02)`, and the OTA sequence
`OTA_BEGIN/DATA/END/ABORT/ACK (0x10–0x14)`. The Raspberry Pi is the only
node with a network interface (`rayglides_protocol.py` / `monitor.py`
already decode these frames over `/dev/ttyACM0`).

That makes the **Raspberry Pi the IoT edge gateway** in this
architecture — it owns the AWS IoT device identity, terminates TLS, and
is the translation point between RayGlides binary frames and the JSON
packets defined below. The EMS board itself never talks to AWS directly.

## 2. End-to-end data flow

```
┌────────────┐   binary serial    ┌───────────────────┐   MQTT/TLS 1.2    ┌──────────────┐
│  RayGlides │  (0xAA…0x55 frame) │   Raspberry Pi      │  (X.509 cert)      │  AWS IoT     │
│  EMS (ESP32│ ─────────────────► │   Gateway            │ ─────────────────►│  Core        │
│  no WiFi)  │ ◄───────────────── │  (rayglides_protocol │ ◄─────────────────│  (MQTT broker│
└────────────┘   OTA frames only  │   .py + AWS IoT SDK) │   shadow/jobs      │  + Device    │
                                   └───────────────────┘                     │  Shadow +    │
                                                                              │  Jobs)       │
                                                                              └──────┬───────┘
                                                                                     │ IoT Rules Engine
                                                        ┌────────────────────────────┼─────────────────────────┐
                                                        ▼                            ▼                         ▼
                                                ┌──────────────┐            ┌──────────────┐          ┌──────────────┐
                                                │   Lambda      │            │   Lambda      │          │   Lambda      │
                                                │ (telemetry)   │            │  (fault)      │          │ (OTA status)  │
                                                └──────┬───────┘            └──────┬───────┘          └──────┬───────┘
                                                       ▼                           ▼                         ▼
                                                ┌──────────────┐            ┌──────────────┐          ┌──────────────┐
                                                │  Timestream   │            │  DynamoDB     │          │  DynamoDB     │
                                                │ (SOC/V/I/T    │            │ (fault log) + │          │ (job/OTA      │
                                                │  time series) │            │  SNS push     │          │  execution)   │
                                                └──────┬───────┘            └──────┬───────┘          └──────┬───────┘
                                                       └───────────────┬───────────┘                         │
                                                                       ▼                                     │
                                                              ┌─────────────────┐                            │
                                                              │  AppSync (GraphQL │◄───────────────────────────┘
                                                              │  + subscriptions) │
                                                              │  or API Gateway   │
                                                              └────────┬─────────┘
                                                                       ▼
                                                              ┌─────────────────┐
                                                              │  Mobile App      │
                                                              │  (Cognito auth,  │
                                                              │  live dashboard, │
                                                              │  push alerts)    │
                                                              └─────────────────┘
```

Firmware binary (`.bin`) for OTA is stored in **S3**; an **AWS IoT Job**
tells the Pi gateway which S3 object to pull, and the Pi then drives the
existing `OTA_BEGIN/DATA/END` serial sequence against the EMS itself —
AWS never talks OTA bytes to the board directly, it only orchestrates
the job and hosts the image.

## 3. JSON communication packets

These are the packets the **Pi gateway publishes to / receives from AWS
IoT Core**. Each one is a JSON-ified, human-readable mirror of a binary
frame the Pi already decodes with `decode_frame()` — field names map
1:1 to the payload layout documented in `RayGlidesProtocol.h` and
`rayglides_protocol.py`.

### 3.1 Telemetry packet (from `STATUS_UPDATE`, msgId `0x01`)

Published every loop cycle the EMS sends one (nominally every 500 ms on
the firmware side; the gateway should throttle/batch before publishing
to MQTT — see §5).

```json
{
  "device_id": "rayglides-ems-0F3A9C",
  "packet_type": "telemetry",
  "protocol_msg_id": "0x01",
  "firmware_seq": 18452,
  "timestamp": "2026-08-05T09:14:22.531Z",
  "battery": {
    "soc_pct": 78,
    "soh_pct": 96,
    "voltage_v": 54,
    "current_a": -6,
    "temperature_c": 31
  },
  "solar": {
    "voltage_v": 21,
    "power_w": 118
  },
  "charge_state": "STATE_CHARGING",
  "charging_mode": "MODE_HYBRID",
  "checksum_valid": true
}
```

Field mapping to the firmware payload `[SOC, ChargeState, BattV,
BattI(signed), BattT, SolarV, SolarPower, BattSOH]`: `soc_pct` =
payload[0], `charge_state` = payload[1] decoded via `stateName()`,
`voltage_v` = payload[2], `current_a` = payload[3] (signed int8, negative
= discharging), `temperature_c` = payload[4], `solar.voltage_v` =
payload[5], `solar.power_w` = payload[6], `soh_pct` = payload[7].
`charging_mode` is not in this frame — it comes from the Pi's local
knowledge of the last `ChargingDecision` state if that's also exposed,
or is omitted if not yet wired up on the firmware side.

### 3.2 Fault packet (from `FAULT_REPORT`, msgId `0x02`)

Published immediately on receipt — never batched, since this drives
push alerts.

```json
{
  "device_id": "rayglides-ems-0F3A9C",
  "packet_type": "fault",
  "protocol_msg_id": "0x02",
  "timestamp": "2026-08-05T09:15:03.118Z",
  "fault_code": 11,
  "fault_name": "F011_OVER_CURRENT",
  "severity": "CRITICAL",
  "active": true
}
```

`fault_code` / `fault_name` map to the `FaultCode` enum
(`F001_NOT_DETECTED` … `F013_WATCHDOG_LOCKOUT`); `severity` maps
`SEV_WARNING`/`SEV_CRITICAL` from the 2-byte payload
`[FaultCode, Severity]`.

### 3.3 Gateway heartbeat / link-health packet (Pi-originated, no firmware equivalent)

Not a RayGlides protocol frame — this is metadata the gateway generates
itself about the serial link, useful for distinguishing "device is fine
but quiet" from "gateway lost the board."

```json
{
  "device_id": "rayglides-ems-0F3A9C",
  "packet_type": "gateway_heartbeat",
  "timestamp": "2026-08-05T09:15:30.000Z",
  "serial_port": "/dev/ttyACM0",
  "last_frame_age_ms": 480,
  "checksum_errors_total": 2,
  "malformed_frames_total": 0,
  "comm_timeout_flag": false
}
```

### 3.4 OTA job status packet (Pi → AWS, tracks `OTA_BEGIN/DATA/END/ACK`)

Published at each stage transition while the gateway drives the OTA
sequence against the EMS, so the AWS IoT Job execution and the mobile
app's progress bar can follow along.

```json
{
  "device_id": "rayglides-ems-0F3A9C",
  "packet_type": "ota_status",
  "job_id": "AWSIoT-fw-2026-08-05-v1.4.2",
  "timestamp": "2026-08-05T09:20:11.900Z",
  "stage": "OTA_DATA",
  "chunk_seq": 214,
  "chunk_total": 812,
  "progress_pct": 26,
  "crc32_running": "0x8F3A21C0",
  "last_ack_status": "OK"
}
```

`stage` cycles through `OTA_BEGIN → OTA_DATA → OTA_END → REBOOTING →
VALIDATING → CONFIRMED` (or `ROLLED_BACK` / `ABORTED`), reflecting the
firmware's chunk-in-order + CRC32 + post-boot validation design
described in `src/ota/PSEUDOCODE.md`.

### 3.5 OTA job command (AWS → Pi, delivered via AWS IoT Jobs document)

This is the payload AWS IoT Jobs delivers to the gateway to kick off an
update — it is **not** sent over the RayGlides serial link; the gateway
consumes it, downloads the image from the referenced S3 object, then
performs the binary `OTA_BEGIN/DATA/END` handshake with the EMS itself.

```json
{
  "job_id": "AWSIoT-fw-2026-08-05-v1.4.2",
  "packet_type": "ota_command",
  "firmware_version": "1.4.2",
  "s3_bucket": "rayglides-firmware-images",
  "s3_key": "ems/1.4.2/rayglides_ems_1.4.2.bin",
  "size_bytes": 812000,
  "crc32": "0x8F3A21C0",
  "min_current_version": "1.3.0",
  "issued_at": "2026-08-05T09:19:40.000Z"
}
```

### 3.6 Device shadow document (reported state, AWS IoT Device Shadow)

Standard AWS IoT `reported` shadow section, kept current by the gateway
on every telemetry/fault packet so the mobile app can read "last known
state" instantly on load without waiting on a live MQTT message.

```json
{
  "state": {
    "reported": {
      "connected": true,
      "firmware_version": "1.4.1",
      "battery": { "soc_pct": 78, "soh_pct": 96 },
      "charge_state": "STATE_CHARGING",
      "active_fault": null,
      "last_telemetry_at": "2026-08-05T09:14:22.531Z"
    }
  }
}
```

## 4. AWS services and their role

| Service | Role in this architecture |
|---|---|
| **AWS IoT Core** | MQTT broker the Pi gateway connects to over TLS with an X.509 device certificate; hosts the Device Shadow and the Rules Engine that fans telemetry/fault packets out to processing. |
| **IoT Rules Engine** | SQL-like rules on incoming topics that route `telemetry` → Timestream Lambda, `fault` → alerting Lambda, `ota_status` → Jobs-tracking Lambda, without the gateway needing to know downstream consumers. |
| **AWS IoT Jobs** | Delivers OTA commands (§3.5) to the gateway and tracks execution status per job, reusing the firmware's existing chunked-transfer design instead of a custom push channel. |
| **AWS Lambda** | Stateless handlers per rule: validate/normalize packets, write to storage, trigger alerts, update the shadow. |
| **Amazon Timestream** | Time-series store for telemetry (SOC, voltage, current, temperature, solar power) — built for the high-cardinality, append-only writes this data pattern produces. |
| **Amazon DynamoDB** | Fault event log and OTA job execution history — point-lookup, low-latency reads for the mobile app's "recent faults" / "update status" screens. |
| **Amazon S3** | Firmware binaries (`.bin`) referenced by OTA jobs; optionally a long-term data lake (Timestream export or Kinesis Firehose) for historical analytics via Athena. |
| **Amazon SNS** | Push notifications (APNs/FCM platform endpoints) triggered on `CRITICAL` fault packets, and topic-based fan-out for ops alerting (email/SMS) on repeated warnings. |
| **AWS AppSync** (or API Gateway + Lambda) | GraphQL API with real-time subscriptions for the mobile app — subscribe to shadow updates and fault events without polling; falls back to REST via API Gateway if GraphQL isn't wanted. |
| **Amazon Cognito** | Mobile app user authentication (user pool) and scoped, temporary AWS credentials (identity pool) for calling AppSync/API Gateway. |
| **AWS IoT Device Defender** | Optional: flags anomalous connection patterns or MQTT publish rates from a gateway, useful given the OTA channel is a flashing vector per the firmware's own "no image signing yet" caveat. |
| **Amazon CloudWatch** | Logs/metrics for all Lambdas, IoT Core connection logs, and alarms on Rules Engine error rates. |

## 5. MQTT topic design

```
rayglides/{device_id}/telemetry          Pi → IoT Core   (STATUS_UPDATE mirror)
rayglides/{device_id}/fault              Pi → IoT Core   (FAULT_REPORT mirror)
rayglides/{device_id}/gateway/heartbeat  Pi → IoT Core   (link health)
rayglides/{device_id}/ota/status         Pi → IoT Core   (OTA progress)
$aws/things/{device_id}/shadow/update    Pi → IoT Core   (reserved shadow topic)
$aws/things/{device_id}/jobs/notify      IoT Core → Pi   (reserved Jobs topic, delivers §3.5)
```

Each device's X.509 certificate policy is scoped to publish only on its
own `rayglides/{device_id}/*` topics and its own reserved shadow/Jobs
topics — a compromised gateway cannot publish as, or read data from,
another unit.

Given the firmware's own 500 ms loop cadence, the gateway should
**debounce telemetry publishes to roughly 1–5 s** (last-value-wins) to
control MQTT/Lambda/Timestream cost; faults and OTA status remain
un-batched since they're low-frequency and latency-sensitive.

## 6. Fault-to-notification flow (concrete example)

1. EMS firmware detects `F011_OVER_CURRENT`, sends binary `FAULT_REPORT`
   frame.
2. Pi gateway decodes it via `decode_fault_report()`, builds the JSON
   packet in §3.2, publishes to `rayglides/{device_id}/fault`.
3. IoT Rule matches `packet_type = 'fault' AND severity = 'CRITICAL'`,
   invokes the fault Lambda.
4. Lambda writes the event to DynamoDB, updates the shadow's
   `active_fault`, and publishes to an SNS topic with platform endpoints
   registered for the owner's mobile device.
5. Mobile app receives the push notification and, if open, also gets a
   live AppSync subscription update reflecting the new shadow state —
   both paths fire from the same Lambda invocation.

## 7. Notes and open items

- The firmware currently has **no remote-command path** beyond OTA
  (no way to remotely toggle the relay or override charging mode) — the
  architecture above only carries data AWS could plausibly send today
  (OTA jobs). Adding a `MSG_CMD` message type on the RayGlides wire
  protocol would be the natural extension point if remote control is
  wanted later.
- Per the firmware's own OTA design notes, there's currently no image
  signing — before this pipeline is used outside a bench setup, the S3
  firmware object and the IoT Job document should be signed (AWS
  Signer / IoT code-signing for AWS IoT Jobs) so the gateway can verify
  authenticity before flashing.
