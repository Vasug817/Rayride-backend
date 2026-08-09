"""
Unit tests for aws_packets.py. No network or AWS credentials required -
these are pure functions over plain dicts (the same dicts
rayglides_protocol.decode_frame() already produces).

Run with:  python3 -m unittest test_aws_packets.py -v
"""

import unittest

from rayglides_protocol import (
    encode_frame, decode_single_frame, decode_frame,
    MSG_STATUS_UPDATE, MSG_FAULT_REPORT,
)
from aws_packets import (
    build_telemetry_packet, build_fault_packet, build_gateway_heartbeat,
    build_ota_status_packet, build_shadow_reported,
    FAULT_NAMES, CHARGE_STATE_NAMES, SEVERITY_NAMES,
)


class TelemetryPacketTests(unittest.TestCase):
    def test_full_payload_maps_every_field(self):
        # [SOC=78, ChargeState=1(CHARGING), BattV=54, BattI=-6(0xFA), BattT=31, SolarV=21, SolarPower=118, SOH=96]
        payload = bytes([78, 1, 54, 0xFA, 31, 21, 118, 96])
        raw = encode_frame(MSG_STATUS_UPDATE, payload)
        decoded = decode_frame(decode_single_frame(raw))

        packet = build_telemetry_packet("dev-1", decoded, firmware_seq=42)

        self.assertEqual(packet["device_id"], "dev-1")
        self.assertEqual(packet["packet_type"], "telemetry")
        self.assertEqual(packet["protocol_msg_id"], "0x01")
        self.assertEqual(packet["firmware_seq"], 42)
        self.assertEqual(packet["battery"]["soc_pct"], 78)
        self.assertEqual(packet["battery"]["voltage_v"], 54)
        self.assertEqual(packet["battery"]["current_a"], -6)
        self.assertEqual(packet["battery"]["temperature_c"], 31)
        self.assertEqual(packet["battery"]["soh_pct"], 96)
        self.assertEqual(packet["solar"], {"voltage_v": 21, "power_w": 118})
        self.assertEqual(packet["charge_state"], "STATE_CHARGING")
        self.assertTrue(packet["checksum_valid"])

    def test_short_payload_omits_optional_fields(self):
        payload = bytes([50, 0])  # SOC=50, ChargeState=0 (IDLE) only
        raw = encode_frame(MSG_STATUS_UPDATE, payload)
        decoded = decode_frame(decode_single_frame(raw))

        packet = build_telemetry_packet("dev-1", decoded)

        self.assertEqual(packet["battery"], {"soc_pct": 50})
        self.assertNotIn("solar", packet)
        self.assertEqual(packet["charge_state"], "STATE_IDLE")

    def test_unknown_charge_state_falls_back(self):
        payload = bytes([50, 99])  # 99 isn't a real ChargeState
        raw = encode_frame(MSG_STATUS_UPDATE, payload)
        decoded = decode_frame(decode_single_frame(raw))
        packet = build_telemetry_packet("dev-1", decoded)
        self.assertEqual(packet["charge_state"], "UNKNOWN")


class FaultPacketTests(unittest.TestCase):
    def test_maps_fault_name_and_severity(self):
        payload = bytes([11, 1])  # F011_OVER_CURRENT, CRITICAL
        raw = encode_frame(MSG_FAULT_REPORT, payload)
        decoded = decode_frame(decode_single_frame(raw))

        packet = build_fault_packet("dev-1", decoded)

        self.assertEqual(packet["fault_code"], 11)
        self.assertEqual(packet["fault_name"], "F011_OVER_CURRENT")
        self.assertEqual(packet["severity"], "CRITICAL")
        self.assertTrue(packet["active"])

    def test_unknown_fault_code_falls_back_to_hex(self):
        packet = build_fault_packet("dev-1", {"fault_code": 200, "severity": 0})
        self.assertEqual(packet["fault_name"], "UNKNOWN(0xC8)")
        self.assertEqual(packet["severity"], "WARNING")

    def test_can_mark_fault_cleared(self):
        packet = build_fault_packet("dev-1", {"fault_code": 0, "severity": 0}, active=False)
        self.assertEqual(packet["fault_name"], "FAULT_NONE")
        self.assertFalse(packet["active"])


class HeartbeatPacketTests(unittest.TestCase):
    def test_fields_pass_through(self):
        hb = build_gateway_heartbeat(
            device_id="dev-1", serial_port="/dev/ttyACM0",
            last_frame_age_ms=480, checksum_errors_total=2,
            malformed_frames_total=0, comm_timeout_flag=False,
        )
        self.assertEqual(hb["packet_type"], "gateway_heartbeat")
        self.assertEqual(hb["serial_port"], "/dev/ttyACM0")
        self.assertEqual(hb["last_frame_age_ms"], 480)
        self.assertFalse(hb["comm_timeout_flag"])


class OTAStatusPacketTests(unittest.TestCase):
    def test_progress_pct_computed_from_chunks(self):
        packet = build_ota_status_packet(
            "dev-1", "job-1", "OTA_DATA", chunk_seq=203, chunk_total=812,
            crc32_running=0x8F3A21C0, last_ack_status=0,
        )
        self.assertEqual(packet["progress_pct"], round(100 * 203 / 812))
        self.assertEqual(packet["crc32_running"], "0x8F3A21C0")
        self.assertEqual(packet["last_ack_status"], "OK")

    def test_begin_stage_has_no_progress_without_chunk_seq(self):
        packet = build_ota_status_packet("dev-1", "job-1", "OTA_BEGIN", chunk_total=812)
        self.assertNotIn("progress_pct", packet)
        self.assertEqual(packet["chunk_total"], 812)

    def test_error_ack_status_maps_to_error(self):
        packet = build_ota_status_packet("dev-1", "job-1", "ABORTED", last_ack_status=1)
        self.assertEqual(packet["last_ack_status"], "ERROR")


class ShadowDocTests(unittest.TestCase):
    def test_reported_state_shape(self):
        doc = build_shadow_reported(
            connected=True, firmware_version="1.4.1", soc_pct=78, soh_pct=96,
            charge_state=1, active_fault=None, last_telemetry_at="2026-08-05T09:14:22.531Z",
        )
        reported = doc["state"]["reported"]
        self.assertTrue(reported["connected"])
        self.assertEqual(reported["charge_state"], "STATE_CHARGING")
        self.assertEqual(reported["battery"], {"soc_pct": 78, "soh_pct": 96})
        self.assertIsNone(reported["active_fault"])

    def test_none_charge_state_stays_none(self):
        doc = build_shadow_reported(
            connected=False, firmware_version="1.4.1", soc_pct=None, soh_pct=None,
            charge_state=None, active_fault="F011_OVER_CURRENT", last_telemetry_at=None,
        )
        self.assertIsNone(doc["state"]["reported"]["charge_state"])
        self.assertEqual(doc["state"]["reported"]["active_fault"], "F011_OVER_CURRENT")


if __name__ == "__main__":
    unittest.main()
