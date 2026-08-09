#!/usr/bin/env python3
"""
AWS IoT Core gateway - Raspberry Pi host-side.

Bridges the RayGlides EMS's binary serial protocol (rayglides_protocol.py)
to the AWS IoT Core MQTT architecture and JSON packet formats described in
the cloud communication architecture doc. This is the only node in the
system with a network interface - the EMS itself has no WiFi.

Responsibilities:
  1. Poll the EMS over serial, decode STATUS_UPDATE / FAULT_REPORT frames,
     and publish the matching JSON packets (aws_packets.py) to AWS IoT Core.
  2. Keep the AWS IoT Device Shadow's `reported` state current.
  3. Publish a periodic gateway heartbeat (link health has no firmware
     equivalent - it's purely a property of this serial connection).
  4. Subscribe to AWS IoT Jobs and, on a new OTA job, download the
     firmware image from S3 and drive the existing binary
     OTA_BEGIN/DATA/END/ACK handshake against the EMS (see
     src/ota/PSEUDOCODE.md in the firmware), reporting progress back as
     ota_status packets at each stage.

Note: the firmware also exposes a generic MSG_ACK/MSG_NACK handshake
for any non-OTA frame sent to it (RayGlidesProtocol.h's NackReason) via
RayGlidesLink.send_and_await() - handle_ota_job() below doesn't use it
today (OTA has its own richer ACK vocabulary), but it's there for a
future host->device command channel.

Usage:
    python3 aws_gateway.py --config aws_config.json

See aws_config.example.json for the expected config shape.
"""

import argparse
import json
import logging
import struct
import time
import zlib
from typing import Optional

try:
    import paho.mqtt.client as mqtt  # pip install paho-mqtt
except ImportError:  # pragma: no cover - only required to actually run the gateway
    mqtt = None

try:
    import boto3  # pip install boto3 - only needed to pull OTA images from S3
except ImportError:  # pragma: no cover
    boto3 = None

from rayglides_protocol import (
    RayGlidesLink, decode_frame,
    MSG_OTA_BEGIN, MSG_OTA_DATA, MSG_OTA_END, MSG_OTA_ABORT,
)
from aws_packets import (
    build_telemetry_packet, build_fault_packet, build_gateway_heartbeat,
    build_ota_status_packet, build_shadow_reported, FAULT_NAMES,
)

# = MAX_PAYLOAD(32) - 2 bytes reserved for the sequence number, matches
# OTA_CHUNK_MAX_DATA in config.h.
OTA_CHUNK_MAX_DATA = 30

logger = logging.getLogger("aws_gateway")


def build_topics(device_id: str) -> dict:
    """MQTT topic layout from the architecture doc, section 5."""
    return {
        "telemetry": f"rayglides/{device_id}/telemetry",
        "fault": f"rayglides/{device_id}/fault",
        "heartbeat": f"rayglides/{device_id}/gateway/heartbeat",
        "ota_status": f"rayglides/{device_id}/ota/status",
        "shadow_update": f"$aws/things/{device_id}/shadow/update",
        "jobs_notify_next": f"$aws/things/{device_id}/jobs/notify-next",
    }


class AWSIoTGateway:
    def __init__(self, device_id: str, endpoint: str, cert_path: str,
                 key_path: str, ca_path: str, serial_port: str = "/dev/ttyACM0",
                 serial_baud: int = 115200,
                 telemetry_publish_interval_s: float = 2.0,
                 heartbeat_interval_s: float = 10.0,
                 firmware_version: str = "unknown"):
        if mqtt is None:
            raise RuntimeError("paho-mqtt is required - pip install paho-mqtt")

        self.device_id = device_id
        self.firmware_version = firmware_version
        self.topics = build_topics(device_id)
        self.telemetry_publish_interval_s = telemetry_publish_interval_s
        self.heartbeat_interval_s = heartbeat_interval_s

        self.link = RayGlidesLink(port=serial_port, baudrate=serial_baud)

        self._client = mqtt.Client(client_id=device_id)
        self._client.tls_set(ca_certs=ca_path, certfile=cert_path, keyfile=key_path)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._endpoint = endpoint

        self._last_telemetry_publish = 0.0
        self._last_heartbeat_publish = 0.0
        self._last_frame_time = time.monotonic()
        self._active_fault_name: Optional[str] = None
        self._latest_soc: Optional[int] = None
        self._latest_soh: Optional[int] = None
        self._latest_charge_state: Optional[int] = None

    # --- MQTT lifecycle ---

    def connect(self):
        self._client.connect(self._endpoint, 8883, keepalive=30)
        self._client.loop_start()

    def disconnect(self):
        self._client.loop_stop()
        self._client.disconnect()
        self.link.close()

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            logger.info("Connected to AWS IoT Core as %s", self.device_id)
            client.subscribe(self.topics["jobs_notify_next"])
        else:
            logger.error("AWS IoT Core connect failed, rc=%s", rc)

    def _on_message(self, client, userdata, msg):
        if msg.topic != self.topics["jobs_notify_next"]:
            return
        try:
            doc = json.loads(msg.payload)
            job = doc.get("execution", {}).get("jobDocument", doc)
            self.handle_ota_job(job)
        except Exception:
            logger.exception("Failed to handle incoming OTA job")

    def _publish(self, topic: str, packet: dict, qos: int = 1):
        self._client.publish(topic, json.dumps(packet), qos=qos)

    # --- Telemetry / fault / heartbeat loop ---

    def poll_once(self):
        """Call in a loop (see run_forever). Reads whatever the EMS has
        sent since the last call, publishes the matching JSON packet(s),
        keeps the shadow current, and publishes a heartbeat on interval."""
        for frame in self.link.poll():
            self._last_frame_time = time.monotonic()
            decoded = decode_frame(frame)

            if decoded["type"] == "status_update":
                self._latest_soc = decoded.get("soc")
                self._latest_soh = decoded.get("battery_soh")
                self._latest_charge_state = decoded.get("state")
                now = time.monotonic()
                if now - self._last_telemetry_publish >= self.telemetry_publish_interval_s:
                    self._publish(self.topics["telemetry"],
                                  build_telemetry_packet(self.device_id, decoded), qos=0)
                    self._publish(self.topics["shadow_update"], self._shadow_doc())
                    self._last_telemetry_publish = now

            elif decoded["type"] == "fault_report":
                self._active_fault_name = FAULT_NAMES.get(decoded["fault_code"])
                self._publish(self.topics["fault"],
                              build_fault_packet(self.device_id, decoded))
                self._publish(self.topics["shadow_update"], self._shadow_doc())

        now = time.monotonic()
        if now - self._last_heartbeat_publish >= self.heartbeat_interval_s:
            age_ms = int((now - self._last_frame_time) * 1000)
            self._publish(self.topics["heartbeat"], build_gateway_heartbeat(
                device_id=self.device_id,
                serial_port=self.link._serial.port,
                last_frame_age_ms=age_ms,
                checksum_errors_total=self.link.checksum_errors,
                malformed_frames_total=self.link.malformed_frames,
                comm_timeout_flag=age_ms > 5000,  # matches firmware's COMM_TIMEOUT_MS
            ), qos=0)
            self._last_heartbeat_publish = now

    def run_forever(self, poll_interval_s: float = 0.05):
        logger.info("Gateway running on %s, device_id=%s", self.link._serial.port, self.device_id)
        while True:
            self.poll_once()
            time.sleep(poll_interval_s)

    def _shadow_doc(self) -> dict:
        return build_shadow_reported(
            connected=True,
            firmware_version=self.firmware_version,
            soc_pct=self._latest_soc,
            soh_pct=self._latest_soh,
            charge_state=self._latest_charge_state,
            active_fault=self._active_fault_name,
            last_telemetry_at=None,
        )

    # --- OTA job handling ---
    # Drives the firmware's existing binary OTA_BEGIN/DATA/END handshake
    # (RayGlidesProtocol.h, src/ota/PSEUDOCODE.md) using an image pulled
    # from S3 per the incoming AWS IoT Job document (architecture doc,
    # section 3.5). Chunks must arrive in order with no gaps, matching
    # the firmware's "abort and restart, don't resume" design - a failed
    # ACK here sends MSG_OTA_ABORT rather than trying to patch around it.

    def handle_ota_job(self, job: dict):
        job_id = job.get("job_id", "unknown")
        logger.info("Starting OTA job %s -> firmware %s", job_id, job.get("firmware_version"))

        if boto3 is None:
            logger.error("boto3 is required to fetch firmware from S3 - pip install boto3")
            self._report_ota_aborted(job_id)
            return

        try:
            s3 = boto3.client("s3")
            obj = s3.get_object(Bucket=job["s3_bucket"], Key=job["s3_key"])
            image = obj["Body"].read()
        except Exception:
            logger.exception("Failed to fetch firmware image from S3")
            self._report_ota_aborted(job_id)
            return

        expected_crc32 = job["crc32"]
        if isinstance(expected_crc32, str):
            expected_crc32 = int(expected_crc32, 16)
        actual_crc32 = zlib.crc32(image) & 0xFFFFFFFF
        if actual_crc32 != expected_crc32:
            logger.error("Downloaded image CRC32 mismatch: got 0x%08X, expected 0x%08X",
                          actual_crc32, expected_crc32)
            self._report_ota_aborted(job_id)
            return

        chunk_total = (len(image) + OTA_CHUNK_MAX_DATA - 1) // OTA_CHUNK_MAX_DATA

        # OTA_BEGIN payload: [size:4 LE][crc32:4 LE]
        self.link.send(MSG_OTA_BEGIN, struct.pack("<II", len(image), actual_crc32))
        self._publish(self.topics["ota_status"], build_ota_status_packet(
            self.device_id, job_id, "OTA_BEGIN", chunk_total=chunk_total))
        if not self._await_ack(job_id, "OTA_BEGIN"):
            return

        for seq in range(chunk_total):
            start = seq * OTA_CHUNK_MAX_DATA
            chunk = image[start:start + OTA_CHUNK_MAX_DATA]
            # OTA_DATA payload: [seq:2 LE][up to 30 bytes]
            self.link.send(MSG_OTA_DATA, struct.pack("<H", seq) + chunk)
            if not self._await_ack(job_id, "OTA_DATA", seq=seq):
                self.link.send(MSG_OTA_ABORT)
                self._publish(self.topics["ota_status"], build_ota_status_packet(
                    self.device_id, job_id, "ABORTED", chunk_seq=seq, chunk_total=chunk_total))
                return
            if seq % 20 == 0 or seq == chunk_total - 1:
                self._publish(self.topics["ota_status"], build_ota_status_packet(
                    self.device_id, job_id, "OTA_DATA", chunk_seq=seq + 1,
                    chunk_total=chunk_total, crc32_running=actual_crc32))

        self.link.send(MSG_OTA_END)
        self._publish(self.topics["ota_status"], build_ota_status_packet(
            self.device_id, job_id, "OTA_END", chunk_seq=chunk_total, chunk_total=chunk_total))
        if self._await_ack(job_id, "REBOOTING"):
            logger.info("OTA job %s: device rebooting into new image", job_id)
        # VALIDATING / CONFIRMED / ROLLED_BACK happen on-device after
        # reboot per OTA_MAX_BOOT_ATTEMPTS and surface as ordinary
        # STATUS_UPDATE/FAULT_REPORT traffic once comms re-establish -
        # poll_once() picks that up like any other telemetry.

    def _await_ack(self, job_id: str, stage: str, seq: Optional[int] = None,
                    timeout: float = 5.0) -> bool:
        frame = self.link.read_frame(timeout=timeout)
        if frame is None:
            logger.error("OTA %s: no ACK received within %.1fs, aborting", stage, timeout)
            self._publish(self.topics["ota_status"], build_ota_status_packet(
                self.device_id, job_id, "ABORTED", chunk_seq=seq))
            return False
        decoded = decode_frame(frame)
        if decoded.get("type") != "ota_ack" or decoded.get("status") != 0:
            logger.error("OTA %s: ACK reported error: %s", stage, decoded)
            self._publish(self.topics["ota_status"], build_ota_status_packet(
                self.device_id, job_id, "ABORTED", chunk_seq=seq,
                last_ack_status=decoded.get("status")))
            return False
        return True

    def _report_ota_aborted(self, job_id: str):
        self._publish(self.topics["ota_status"],
                      build_ota_status_packet(self.device_id, job_id, "ABORTED"))


def main():
    parser = argparse.ArgumentParser(description="RayGlides AWS IoT gateway")
    parser.add_argument("--config", default="aws_config.json",
                        help="Path to config JSON (see aws_config.example.json)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    with open(args.config) as f:
        cfg = json.load(f)

    gateway = AWSIoTGateway(
        device_id=cfg["device_id"],
        endpoint=cfg["endpoint"],
        cert_path=cfg["cert_path"],
        key_path=cfg["key_path"],
        ca_path=cfg["ca_path"],
        serial_port=cfg.get("serial_port", "/dev/ttyACM0"),
        serial_baud=cfg.get("serial_baud", 115200),
        telemetry_publish_interval_s=cfg.get("telemetry_publish_interval_s", 2.0),
        heartbeat_interval_s=cfg.get("heartbeat_interval_s", 10.0),
        firmware_version=cfg.get("firmware_version", "unknown"),
    )
    gateway.connect()
    try:
        gateway.run_forever()
    except KeyboardInterrupt:
        logger.info("Stopping.")
    finally:
        gateway.disconnect()


if __name__ == "__main__":
    main()
