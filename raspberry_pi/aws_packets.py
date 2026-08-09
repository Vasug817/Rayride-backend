"""
AWS packet builders - Raspberry Pi gateway side.

Turns the plain dicts rayglides_protocol.decode_frame() already produces
(status_update / fault_report / ota_ack) into the JSON packets defined in
the AWS cloud communication architecture doc, ready to json.dumps() and
publish to AWS IoT Core.

These are pure functions - no MQTT, no boto3, nothing network-related -
so they're trivially unit-testable (see test_aws_packets.py) and reusable
from aws_gateway.py without pulling in any AWS SDK dependency.

Field-to-payload mapping mirrors RayGlidesProtocol.h / .cpp exactly:
  STATUS_UPDATE payload: [SOC, ChargeState, BattV, BattI, BattT, SolarV, SolarPower, BattSOH]
  FAULT_REPORT  payload: [FaultCode, Severity]
"""

from datetime import datetime, timezone
from typing import Optional


# --- Enum name tables (mirror FaultDetection.h / BatteryStateMachine.h) ---

FAULT_NAMES = {
    0: "FAULT_NONE",
    1: "F001_NOT_DETECTED",
    2: "F002_OVER_VOLTAGE",
    3: "F003_UNDER_VOLTAGE",
    4: "F004_OVER_TEMPERATURE",
    5: "F005_SOLAR_FAULT",
    8: "F008_COMM_TIMEOUT",
    9: "F009_CHECKSUM_ERROR",
    11: "F011_OVER_CURRENT",
    12: "F012_WATCHDOG_RESET",
    13: "F013_WATCHDOG_LOCKOUT",
}

# F012/F013 are watchdog-related and never clear themselves via a
# follow-up FAULT_NONE report the way sensor faults do, but they still
# carry a severity from the firmware - kept here only for the name
# lookup, not for any special-cased logic.
SEVERITY_NAMES = {0: "WARNING", 1: "CRITICAL"}

CHARGE_STATE_NAMES = {
    0: "STATE_IDLE",
    1: "STATE_CHARGING",
    2: "STATE_FULLY_CHARGED",
    3: "STATE_FAULT",
}

CHARGING_MODE_NAMES = {
    0: "MODE_SOLAR_ONLY",
    1: "MODE_GRID_ONLY",
    2: "MODE_HYBRID",
    3: "MODE_NO_CHARGE",
}

OTA_ACK_STATUS_NAMES = {
    0: "OK",
    1: "ERROR",
}


def _iso_now() -> str:
    """UTC timestamp, millisecond precision, 'Z' suffix - matches every
    example timestamp in the architecture doc."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + \
        f"{datetime.now(timezone.utc).microsecond // 1000:03d}Z"


# --- 3.1 Telemetry packet (from STATUS_UPDATE) ---

def build_telemetry_packet(device_id: str, decoded_status: dict,
                            firmware_seq: Optional[int] = None,
                            charging_mode: Optional[int] = None,
                            timestamp: Optional[str] = None) -> dict:
    """decoded_status is decode_status_update()'s output (or
    decode_frame()'s output for a STATUS_UPDATE frame, which is a
    superset with a 'type' key that's simply ignored here)."""
    packet = {
        "device_id": device_id,
        "packet_type": "telemetry",
        "protocol_msg_id": "0x01",
        "firmware_seq": firmware_seq,
        "timestamp": timestamp or _iso_now(),
        "battery": {
            "soc_pct": decoded_status["soc"],
        },
        "charge_state": CHARGE_STATE_NAMES.get(decoded_status["state"], "UNKNOWN"),
        "checksum_valid": True,
    }
    # Full telemetry (V/I/T/solar/SOH) is only present on 8-byte payloads -
    # decode_status_update() already guards this the same way.
    if "battery_voltage" in decoded_status:
        packet["battery"].update({
            "voltage_v": decoded_status["battery_voltage"],
            "current_a": decoded_status["battery_current"],
            "temperature_c": decoded_status["battery_temp"],
            "soh_pct": decoded_status["battery_soh"],
        })
        packet["solar"] = {
            "voltage_v": decoded_status["solar_voltage"],
            "power_w": decoded_status["solar_power"],
        }
    if charging_mode is not None:
        packet["charging_mode"] = CHARGING_MODE_NAMES.get(charging_mode, "UNKNOWN")
    return packet


# --- 3.2 Fault packet (from FAULT_REPORT) ---

def build_fault_packet(device_id: str, decoded_fault: dict,
                        active: bool = True,
                        timestamp: Optional[str] = None) -> dict:
    """decoded_fault is decode_fault_report()'s output (or
    decode_frame()'s output for a FAULT_REPORT frame)."""
    code = decoded_fault["fault_code"]
    return {
        "device_id": device_id,
        "packet_type": "fault",
        "protocol_msg_id": "0x02",
        "timestamp": timestamp or _iso_now(),
        "fault_code": code,
        "fault_name": FAULT_NAMES.get(code, f"UNKNOWN(0x{code:02X})"),
        "severity": SEVERITY_NAMES.get(decoded_fault["severity"], "UNKNOWN"),
        "active": active,
    }


# --- 3.3 Gateway heartbeat (Pi-originated, no firmware equivalent) ---

def build_gateway_heartbeat(device_id: str, serial_port: str,
                             last_frame_age_ms: int,
                             checksum_errors_total: int,
                             malformed_frames_total: int,
                             comm_timeout_flag: bool,
                             timestamp: Optional[str] = None) -> dict:
    return {
        "device_id": device_id,
        "packet_type": "gateway_heartbeat",
        "timestamp": timestamp or _iso_now(),
        "serial_port": serial_port,
        "last_frame_age_ms": last_frame_age_ms,
        "checksum_errors_total": checksum_errors_total,
        "malformed_frames_total": malformed_frames_total,
        "comm_timeout_flag": comm_timeout_flag,
    }


# --- 3.4 OTA job status (Pi -> AWS, tracks OTA_BEGIN/DATA/END/ACK) ---

def build_ota_status_packet(device_id: str, job_id: str, stage: str,
                             chunk_seq: Optional[int] = None,
                             chunk_total: Optional[int] = None,
                             crc32_running: Optional[int] = None,
                             last_ack_status: Optional[int] = None,
                             timestamp: Optional[str] = None) -> dict:
    """`stage` is one of: OTA_BEGIN, OTA_DATA, OTA_END, REBOOTING,
    VALIDATING, CONFIRMED, ROLLED_BACK, ABORTED - mirroring the flow in
    src/ota/PSEUDOCODE.md. `last_ack_status` is the raw OTA_ACK status
    byte (0=OK, 1=ERROR) if one was just received."""
    packet = {
        "device_id": device_id,
        "packet_type": "ota_status",
        "job_id": job_id,
        "timestamp": timestamp or _iso_now(),
        "stage": stage,
    }
    if chunk_seq is not None:
        packet["chunk_seq"] = chunk_seq
    if chunk_total is not None:
        packet["chunk_total"] = chunk_total
        if chunk_seq is not None and chunk_total > 0:
            packet["progress_pct"] = round(100 * chunk_seq / chunk_total)
    if crc32_running is not None:
        packet["crc32_running"] = f"0x{crc32_running:08X}"
    if last_ack_status is not None:
        packet["last_ack_status"] = OTA_ACK_STATUS_NAMES.get(last_ack_status, "UNKNOWN")
    return packet


# --- 3.6 Device shadow 'reported' section ---

def build_shadow_reported(connected: bool, firmware_version: str,
                           soc_pct: Optional[int], soh_pct: Optional[int],
                           charge_state: Optional[int],
                           active_fault: Optional[str],
                           last_telemetry_at: Optional[str]) -> dict:
    """Returns the full {"state": {"reported": {...}}} document expected
    by the AWS IoT Device Shadow update topic
    ($aws/things/{device_id}/shadow/update)."""
    return {
        "state": {
            "reported": {
                "connected": connected,
                "firmware_version": firmware_version,
                "battery": {"soc_pct": soc_pct, "soh_pct": soh_pct},
                "charge_state": CHARGE_STATE_NAMES.get(charge_state, None)
                if charge_state is not None else None,
                "active_fault": active_fault,
                "last_telemetry_at": last_telemetry_at,
            }
        }
    }
