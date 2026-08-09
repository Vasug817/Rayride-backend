"""
RayGlides Protocol - Raspberry Pi host-side library.

Mirrors the on-wire format defined in the ESP32 firmware
(src/protocol/RayGlidesProtocol.cpp / .h) byte-for-byte:

    [FRAME_START][msgId][len][payload...][checksum][FRAME_END]

    FRAME_START = 0xAA          FRAME_END = 0x55
    checksum    = msgId ^ len ^ payload[0] ^ payload[1] ^ ... (XOR)

This library is transport-agnostic at its core (encode/decode are pure
functions of bytes) with a thin pyserial wrapper (RayGlidesLink) for the
real use case: talking to the EMS over USB Serial at /dev/ttyACM0.

Three layers, matching how the firmware itself is structured:
  1. encode_frame() / compute_checksum()      - build a valid frame
  2. FrameParser                              - resilient streaming decode,
                                                 tolerant of noise/resync,
                                                 mirrors receiveFrame()'s
                                                 byte-at-a-time behavior
  3. decode_single_frame()                    - strict one-shot decode of
                                                 an exact frame, raises on
                                                 any malformation - used
                                                 for validation/tests
  4. decode_frame() + decode_status_update() / decode_fault_report() /
     decode_ota_ack()                         - turn a Frame's raw
                                                 payload into a plain dict
"""

from dataclasses import dataclass
from typing import Iterator, List, Optional
import time
import threading

try:
    import serial  # pyserial - only required if you actually use RayGlidesLink
except ImportError:  # pragma: no cover - protocol-only usage doesn't need it
    serial = None


# --- Wire-format constants (must match config.h / RayGlidesProtocol.h) ---

FRAME_START = 0xAA
FRAME_END = 0x55
MAX_PAYLOAD = 32

MSG_STATUS_UPDATE = 0x01
MSG_FAULT_REPORT = 0x02
MSG_ACK = 0x03   # Generic transport ACK - non-OTA frames only, see NackReason below
MSG_NACK = 0x04  # Generic transport NACK - non-OTA frames only
MSG_OTA_BEGIN = 0x10
MSG_OTA_DATA = 0x11
MSG_OTA_END = 0x12
MSG_OTA_ABORT = 0x13
MSG_OTA_ACK = 0x14

MSG_NAMES = {
    MSG_STATUS_UPDATE: "STATUS_UPDATE",
    MSG_FAULT_REPORT: "FAULT_REPORT",
    MSG_ACK: "ACK",
    MSG_NACK: "NACK",
    MSG_OTA_BEGIN: "OTA_BEGIN",
    MSG_OTA_DATA: "OTA_DATA",
    MSG_OTA_END: "OTA_END",
    MSG_OTA_ABORT: "OTA_ABORT",
    MSG_OTA_ACK: "OTA_ACK",
}

# Reasons a generic MSG_NACK was sent, carried in payload[0] - must match
# the NackReason enum in RayGlidesProtocol.h exactly. Distinct from
# OTAAckStatus, which covers the separate OTA_BEGIN/DATA/END handshake.
NACK_CHECKSUM_ERROR = 1
NACK_MALFORMED_FRAME = 2
NACK_UNKNOWN_MSG_ID = 3

NACK_REASON_NAMES = {
    NACK_CHECKSUM_ERROR: "CHECKSUM_ERROR",
    NACK_MALFORMED_FRAME: "MALFORMED_FRAME",
    NACK_UNKNOWN_MSG_ID: "UNKNOWN_MSG_ID",
}


# --- Errors ---

class FrameParseError(Exception):
    """A frame was malformed (bad length, missing FRAME_END, too short)."""


class ChecksumError(FrameParseError):
    """A structurally valid frame failed its checksum."""


class AckTimeoutError(Exception):
    """RayGlidesLink.send_and_await() got no ACK/NACK within the timeout,
    across all retries."""


class NackReceived(Exception):
    """RayGlidesLink.send_and_await() got an explicit MSG_NACK back - the
    device understood the frame and rejected it. Not retried automatically:
    resending identical bytes after a checksum/malformed/unknown-msgId
    rejection would just fail the same way again."""

    def __init__(self, reason: int, reason_name: str, acked_msg_id: Optional[int]):
        self.reason = reason
        self.reason_name = reason_name
        self.acked_msg_id = acked_msg_id
        super().__init__(
            f"Device NACKed msg_id={acked_msg_id}: {reason_name} ({reason})")


# --- Encoding ---

def compute_checksum(msg_id: int, length: int, payload: bytes) -> int:
    """Same XOR checksum as computeChecksum() in RayGlidesProtocol.cpp."""
    cs = (msg_id ^ length) & 0xFF
    for b in payload:
        cs ^= b
    return cs & 0xFF


def encode_frame(msg_id: int, payload: bytes = b"") -> bytes:
    """Builds one complete, ready-to-transmit binary frame."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too long: {len(payload)} > MAX_PAYLOAD={MAX_PAYLOAD}")
    length = len(payload)
    checksum = compute_checksum(msg_id, length, payload)
    return bytes([FRAME_START, msg_id, length]) + bytes(payload) + bytes([checksum, FRAME_END])


# --- Decoding: data model ---

@dataclass
class Frame:
    msg_id: int
    payload: bytes

    @property
    def msg_name(self) -> str:
        return MSG_NAMES.get(self.msg_id, f"UNKNOWN(0x{self.msg_id:02X})")


# --- Decoding: strict, single-frame (validation / tests) ---

def decode_single_frame(raw: bytes) -> Frame:
    """
    Decodes exactly one complete frame from `raw`, which must contain
    nothing but that frame (no leading/trailing bytes). Raises
    FrameParseError / ChecksumError on any malformation. Use this for
    validating a byte sequence you already believe is one whole frame -
    for streaming serial data, use FrameParser instead.
    """
    if len(raw) < 5:
        raise FrameParseError(f"frame too short: {len(raw)} bytes (minimum 5)")
    if raw[0] != FRAME_START:
        raise FrameParseError(f"missing FRAME_START: got 0x{raw[0]:02X}")

    msg_id = raw[1]
    length = raw[2]
    if length > MAX_PAYLOAD:
        raise FrameParseError(f"length {length} exceeds MAX_PAYLOAD={MAX_PAYLOAD}")

    expected_total = 3 + length + 2
    if len(raw) != expected_total:
        raise FrameParseError(
            f"length mismatch: header says {length} payload bytes, "
            f"frame is {len(raw)} bytes (expected {expected_total})")

    payload = bytes(raw[3:3 + length])
    checksum = raw[3 + length]
    end_byte = raw[3 + length + 1]

    if end_byte != FRAME_END:
        raise FrameParseError(f"missing FRAME_END: got 0x{end_byte:02X}")

    expected_checksum = compute_checksum(msg_id, length, payload)
    if checksum != expected_checksum:
        raise ChecksumError(f"checksum mismatch: got 0x{checksum:02X}, expected 0x{expected_checksum:02X}")

    return Frame(msg_id=msg_id, payload=payload)


# --- Decoding: resilient streaming parser (live serial data) ---

class FrameParser:
    """
    Incremental, byte-at-a-time parser for feeding directly from a live
    serial read. Mirrors receiveFrame()'s own tolerance: any byte that
    isn't a valid FRAME_START while idle is silently discarded and
    parsing resyncs on the next byte, exactly like the firmware. A
    structurally valid frame with a bad checksum is dropped (counted,
    not raised) so one corrupted frame never stops the stream - use
    decode_single_frame() instead if you want a hard failure on bad data.
    """

    _WAIT_START, _WAIT_ID, _WAIT_LEN, _WAIT_PAYLOAD, _WAIT_CHECKSUM, _WAIT_END = range(6)

    def __init__(self):
        self.checksum_errors = 0
        self.malformed_frames = 0
        self._log_buffer = bytearray()
        self._reset()

    def _reset(self):
        self._state = self._WAIT_START
        self._msg_id = 0
        self._length = 0
        self._payload = bytearray()
        self._checksum_byte = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        """Feed raw bytes; yields a Frame for each complete, valid frame found."""
        for byte in data:
            frame = self._feed_byte(byte)
            if frame is not None:
                yield frame

    def _feed_byte(self, byte: int) -> Optional[Frame]:
        if self._state == self._WAIT_START:
            if byte == FRAME_START:
                self._state = self._WAIT_ID
            else:
                if 32 <= byte <= 126 or byte in (10, 13, 9):
                    if byte == 10:  # '\n'
                        try:
                            text = self._log_buffer.decode('utf-8', errors='ignore').strip()
                            if text:
                                print(f"[ESP32 Console] {text}")
                        except Exception:
                            pass
                        self._log_buffer.clear()
                    elif byte != 13:  # skip '\r'
                        self._log_buffer.append(byte)
                else:
                    self._log_buffer.clear()
            return None

        if self._state == self._WAIT_ID:
            self._msg_id = byte
            self._state = self._WAIT_LEN
            return None

        if self._state == self._WAIT_LEN:
            if byte > MAX_PAYLOAD:
                self.malformed_frames += 1
                self._reset()  # Reject and resync, matches firmware
                return None
            self._length = byte
            self._payload = bytearray()
            self._state = self._WAIT_PAYLOAD if self._length > 0 else self._WAIT_CHECKSUM
            return None

        if self._state == self._WAIT_PAYLOAD:
            self._payload.append(byte)
            if len(self._payload) >= self._length:
                self._state = self._WAIT_CHECKSUM
            return None

        if self._state == self._WAIT_CHECKSUM:
            self._checksum_byte = byte
            self._state = self._WAIT_END
            return None

        if self._state == self._WAIT_END:
            msg_id, length, payload, checksum = self._msg_id, self._length, bytes(self._payload), self._checksum_byte
            end_byte = byte
            self._reset()

            if end_byte != FRAME_END:
                self.malformed_frames += 1
                return None

            expected = compute_checksum(msg_id, length, payload)
            if checksum != expected:
                self.checksum_errors += 1
                return None

            return Frame(msg_id=msg_id, payload=payload)

        return None  # pragma: no cover - unreachable


# --- Decoding: known message types -> plain dicts ---

def decode_status_update(payload: bytes) -> dict:
    """Payload: [SOC, ChargeState, BattV, BattI(signed), BattT, SolarV, SolarPower, BattSOH, PrimaryFaultCode]"""
    if len(payload) < 2:
        raise ValueError(f"StatusUpdate payload too short: {len(payload)} bytes")
    result = {"soc": payload[0], "state": payload[1]}
    if len(payload) >= 8:
        current = payload[3]
        result.update({
            "battery_voltage": payload[2],
            "battery_current": current - 256 if current > 127 else current,  # int8_t
            "battery_temp": payload[4],
            "solar_voltage": payload[5],
            "solar_power": payload[6],
            "battery_soh": payload[7],
        })
    if len(payload) >= 9:
        result["active_fault_code"] = payload[8]
    return result


def decode_fault_report(payload: bytes) -> dict:
    """Payload: [FaultCode, Severity]"""
    if len(payload) < 2:
        raise ValueError(f"FaultReport payload too short: {len(payload)} bytes")
    return {"fault_code": payload[0], "severity": payload[1]}


def decode_ota_ack(payload: bytes) -> dict:
    """Payload: [OTAAckStatus, seqLow, seqHigh]"""
    if len(payload) < 3:
        raise ValueError(f"OTA ACK payload too short: {len(payload)} bytes")
    seq = payload[1] | (payload[2] << 8)
    return {"status": payload[0], "seq": seq}


def decode_ack(payload: bytes) -> dict:
    """Payload: [ackedMsgId]"""
    if len(payload) < 1:
        raise ValueError(f"ACK payload too short: {len(payload)} bytes")
    return {"acked_msg_id": payload[0]}


def decode_nack(payload: bytes) -> dict:
    """Payload: [reason, ackedMsgId]"""
    if len(payload) < 1:
        raise ValueError(f"NACK payload too short: {len(payload)} bytes")
    reason = payload[0]
    acked_msg_id = payload[1] if len(payload) >= 2 else None
    return {
        "reason": reason,
        "reason_name": NACK_REASON_NAMES.get(reason, f"UNKNOWN({reason})"),
        "acked_msg_id": acked_msg_id,
    }


def decode_frame(frame: Frame) -> dict:
    """Dispatches a Frame to the right decoder by msg_id. Unknown message
    IDs pass the raw payload through untouched rather than raising, so a
    firmware update that adds a new message type doesn't break older
    host tooling parsing everything else."""
    if frame.msg_id == MSG_STATUS_UPDATE:
        return {"type": "status_update", **decode_status_update(frame.payload)}
    if frame.msg_id == MSG_FAULT_REPORT:
        return {"type": "fault_report", **decode_fault_report(frame.payload)}
    if frame.msg_id == MSG_ACK:
        return {"type": "ack", **decode_ack(frame.payload)}
    if frame.msg_id == MSG_NACK:
        return {"type": "nack", **decode_nack(frame.payload)}
    if frame.msg_id == MSG_OTA_ACK:
        return {"type": "ota_ack", **decode_ota_ack(frame.payload)}
    return {"type": "unknown", "msg_id": frame.msg_id, "payload": bytes(frame.payload)}


# --- Serial link wrapper ---

class RayGlidesLink:
    """
    Thin pyserial wrapper for talking to the RayGlides EMS over USB
    Serial. Defaults match the firmware: /dev/ttyACM0 at 115200 baud.
    Sends real binary frames (encode_frame()) and decodes incoming bytes
    through a FrameParser - this only works correctly against the
    binary-framing version of sendFrame() (see RayGlidesProtocol.cpp);
    it will not parse the older HEX-text debug output.
    """

    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200, timeout: float = 0.1):
        if serial is None:
            raise RuntimeError("pyserial is required for RayGlidesLink - pip install pyserial")
        self._serial = serial.Serial(port, baudrate, timeout=timeout)
        self._serial_lock = threading.Lock()
        self._parser = FrameParser()
        # Frames read by read_frame() beyond the one it returns (poll()
        # can decode several frames from a single burst of serial data) -
        # queued here instead of dropped, so a fast run of replies
        # (e.g. noise immediately followed by the ACK it's waiting on,
        # both arriving in the same read()) isn't silently lost.
        self._read_queue: List[Frame] = []
        self._read_queue_lock = threading.Lock()
        
        # Thread-safe synchronization dictionary for serial command responses
        self._pending_lock = threading.Lock()
        self._pending_commands = {}  # msg_id -> {"event": threading.Event(), "response": None}

    def close(self):
        with self._serial_lock:
            self._serial.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()

    def send(self, msg_id: int, payload: bytes = b"") -> None:
        with self._serial_lock:
            self._serial.write(encode_frame(msg_id, payload))

    def poll(self) -> List[Frame]:
        """Reads whatever is currently buffered and returns any complete,
        valid frames found (usually 0 or 1). Non-blocking.
        Intercepts ACK and NACK frames to coordinate with waiting send_and_await calls."""
        with self._serial_lock:
            n = self._serial.in_waiting
            if n == 0:
                return []
            data = self._serial.read(n)
            frames = list(self._parser.feed(data))
        
        non_ack_frames = []
        for frame in frames:
            if frame.msg_id in (MSG_ACK, MSG_NACK):
                # ACK payload[0] is the acknowledged msg_id
                # NACK payload[1] is the acknowledged msg_id
                acked_msg_id = None
                if frame.msg_id == MSG_ACK and len(frame.payload) >= 1:
                    acked_msg_id = frame.payload[0]
                elif frame.msg_id == MSG_NACK and len(frame.payload) >= 2:
                    acked_msg_id = frame.payload[1]
                
                if acked_msg_id is not None:
                    with self._pending_lock:
                        if acked_msg_id in self._pending_commands:
                            self._pending_commands[acked_msg_id]["response"] = frame
                            self._pending_commands[acked_msg_id]["event"].set()
                            continue  # Intercepted!
            non_ack_frames.append(frame)
            
        return non_ack_frames

    def read_frame(self, timeout: float = 2.0) -> Optional[Frame]:
        """Blocks up to `timeout` seconds for exactly one frame - handy
        for request/response flows like waiting on an OTA_ACK. Frames
        beyond the one returned (poll() can decode several from a single
        burst) are queued and returned first-in-first-out by later
        read_frame() calls, rather than dropped."""
        with self._read_queue_lock:
            if self._read_queue:
                return self._read_queue.pop(0)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frames = self.poll()
            with self._read_queue_lock:
                if frames:
                    self._read_queue.extend(frames[1:])
                    return frames[0]
            time.sleep(0.01)
        return None

    def send_and_await(self, msg_id: int, payload: bytes = b"",
                        timeout: float = 2.0, retries: int = 2) -> Frame:
        """Sends a frame and blocks for the device's MSG_ACK/MSG_NACK reply
        using a thread-safe event listener."""
        event = threading.Event()
        wait_entry = {"event": event, "response": None}
        
        with self._pending_lock:
            self._pending_commands[msg_id] = wait_entry
            
        try:
            for attempt in range(retries + 1):
                event.clear()
                wait_entry["response"] = None
                
                self.send(msg_id, payload)
                
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if event.wait(timeout=0.01):
                        break
                    self.poll()
                
                if event.is_set():
                    resp_frame = wait_entry["response"]
                    if resp_frame is not None:
                        if resp_frame.msg_id == MSG_NACK:
                            decoded = decode_nack(resp_frame.payload)
                            raise NackReceived(decoded["reason"], decoded["reason_name"],
                                                decoded["acked_msg_id"])
                        if resp_frame.msg_id == MSG_ACK:
                            return resp_frame
            raise AckTimeoutError(
                f"No ACK/NACK for msg_id=0x{msg_id:02X} after {retries + 1} attempt(s)")
        finally:
            with self._pending_lock:
                self._pending_commands.pop(msg_id, None)

    @property
    def checksum_errors(self) -> int:
        return self._parser.checksum_errors

    @property
    def malformed_frames(self) -> int:
        return self._parser.malformed_frames
