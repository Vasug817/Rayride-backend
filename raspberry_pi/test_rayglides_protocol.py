"""
Unit tests for rayglides_protocol.py. No hardware required - encode/decode
tests exercise pure functions, and RayGlidesLink tests use a FakeSerial
that mimics pyserial's Serial API well enough to drive it in-process.

Run with:  python3 -m unittest test_rayglides_protocol.py -v
"""

import unittest
from unittest.mock import patch

from rayglides_protocol import (
    FRAME_START, FRAME_END, MAX_PAYLOAD,
    MSG_STATUS_UPDATE, MSG_FAULT_REPORT, MSG_OTA_ACK, MSG_ACK, MSG_NACK,
    NACK_CHECKSUM_ERROR, NACK_MALFORMED_FRAME, NACK_UNKNOWN_MSG_ID,
    compute_checksum, encode_frame, decode_single_frame, decode_frame,
    decode_status_update, decode_fault_report, decode_ota_ack,
    decode_ack, decode_nack,
    Frame, FrameParser, FrameParseError, ChecksumError,
    RayGlidesLink, AckTimeoutError, NackReceived,
)


class FakeSerial:
    """Minimal stand-in for serial.Serial, enough to drive RayGlidesLink
    without a real port. Two independent buffers: writes go to `_out`
    (what the test can assert was transmitted), and `inject()` fills
    `_in` (what the test pretends arrived from the device)."""

    def __init__(self, port=None, baudrate=None, timeout=None):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self._out = bytearray()
        self._in = bytearray()
        self.closed = False

    @property
    def in_waiting(self):
        return len(self._in)

    def write(self, data):
        self._out.extend(data)
        return len(data)

    def read(self, n):
        chunk = bytes(self._in[:n])
        del self._in[:n]
        return chunk

    def close(self):
        self.closed = True

    def inject(self, data: bytes):
        self._in.extend(data)


class ChecksumTests(unittest.TestCase):
    def test_matches_firmware_xor_logic(self):
        # Hand-computed: msgId=0x01, len=2, payload=[0x10, 0x20]
        # cs = 0x01 ^ 0x02 = 0x03; 0x03 ^ 0x10 = 0x13; 0x13 ^ 0x20 = 0x33
        self.assertEqual(compute_checksum(0x01, 2, b"\x10\x20"), 0x33)

    def test_empty_payload(self):
        self.assertEqual(compute_checksum(0x02, 0, b""), 0x02)


class EncodeDecodeRoundtripTests(unittest.TestCase):
    def test_status_update_roundtrip(self):
        payload = bytes([80, 2, 55, 253, 30, 40, 100, 95])  # includes a negative current byte
        raw = encode_frame(MSG_STATUS_UPDATE, payload)

        self.assertEqual(raw[0], FRAME_START)
        self.assertEqual(raw[-1], FRAME_END)

        frame = decode_single_frame(raw)
        self.assertEqual(frame.msg_id, MSG_STATUS_UPDATE)
        self.assertEqual(frame.payload, payload)

        decoded = decode_status_update(frame.payload)
        self.assertEqual(decoded["soc"], 80)
        self.assertEqual(decoded["state"], 2)
        self.assertEqual(decoded["battery_current"], 253 - 256)  # signed int8 -> -3

    def test_fault_report_roundtrip(self):
        raw = encode_frame(MSG_FAULT_REPORT, bytes([13, 1]))
        frame = decode_single_frame(raw)
        decoded = decode_fault_report(frame.payload)
        self.assertEqual(decoded, {"fault_code": 13, "severity": 1})

    def test_ota_ack_roundtrip(self):
        seq = 300  # exercises the two-byte little-endian seq encoding
        raw = encode_frame(MSG_OTA_ACK, bytes([8, seq & 0xFF, (seq >> 8) & 0xFF]))
        frame = decode_single_frame(raw)
        decoded = decode_ota_ack(frame.payload)
        self.assertEqual(decoded, {"status": 8, "seq": 300})

    def test_decode_frame_dispatch(self):
        raw = encode_frame(MSG_STATUS_UPDATE, bytes([50, 1]))
        frame = decode_single_frame(raw)
        result = decode_frame(frame)
        self.assertEqual(result["type"], "status_update")
        self.assertEqual(result["soc"], 50)

    def test_decode_frame_unknown_msg_id_passthrough(self):
        raw = encode_frame(0x99, bytes([1, 2, 3]))
        frame = decode_single_frame(raw)
        result = decode_frame(frame)
        self.assertEqual(result["type"], "unknown")
        self.assertEqual(result["payload"], b"\x01\x02\x03")

    def test_payload_too_long_rejected(self):
        with self.assertRaises(ValueError):
            encode_frame(MSG_STATUS_UPDATE, bytes(MAX_PAYLOAD + 1))


class AckNackTests(unittest.TestCase):
    """MSG_ACK / MSG_NACK - the generic transport-level acknowledgment
    added alongside RayGlidesProtocol.h's NackReason enum."""

    def test_ack_roundtrip(self):
        raw = encode_frame(MSG_ACK, bytes([MSG_STATUS_UPDATE]))
        frame = decode_single_frame(raw)
        result = decode_frame(frame)
        self.assertEqual(result, {"type": "ack", "acked_msg_id": MSG_STATUS_UPDATE})

    def test_nack_roundtrip_checksum_error(self):
        # Mirrors what the firmware sends from receiveFrame() when a
        # frame's checksum doesn't match: NackReason=NACK_CHECKSUM_ERROR.
        raw = encode_frame(MSG_NACK, bytes([NACK_CHECKSUM_ERROR, MSG_STATUS_UPDATE]))
        frame = decode_single_frame(raw)
        result = decode_frame(frame)
        self.assertEqual(result["type"], "nack")
        self.assertEqual(result["reason"], NACK_CHECKSUM_ERROR)
        self.assertEqual(result["reason_name"], "CHECKSUM_ERROR")
        self.assertEqual(result["acked_msg_id"], MSG_STATUS_UPDATE)

    def test_nack_roundtrip_malformed_frame(self):
        # Mirrors receiveFrame() rejecting a bad length or missing FRAME_END.
        raw = encode_frame(MSG_NACK, bytes([NACK_MALFORMED_FRAME, 0x77]))
        decoded = decode_nack(decode_single_frame(raw).payload)
        self.assertEqual(decoded["reason_name"], "MALFORMED_FRAME")

    def test_nack_roundtrip_unknown_msg_id(self):
        raw = encode_frame(MSG_NACK, bytes([NACK_UNKNOWN_MSG_ID, 0x77]))
        decoded = decode_nack(decode_single_frame(raw).payload)
        self.assertEqual(decoded["reason_name"], "UNKNOWN_MSG_ID")

    def test_nack_unknown_reason_code_falls_back(self):
        # A NackReason value the host doesn't recognize yet (e.g. a newer
        # firmware) shouldn't raise - it should degrade to a labeled unknown.
        decoded = decode_nack(bytes([250, 0x01]))
        self.assertEqual(decoded["reason_name"], "UNKNOWN(250)")

    def test_nack_without_acked_msg_id_byte(self):
        # decode_nack() should tolerate the minimal 1-byte form gracefully
        # rather than raising, in case a reason is ever sent alone.
        decoded = decode_nack(bytes([NACK_CHECKSUM_ERROR]))
        self.assertIsNone(decoded["acked_msg_id"])

    def test_ack_empty_payload_raises(self):
        with self.assertRaises(ValueError):
            decode_ack(b"")

    def test_nack_empty_payload_raises(self):
        with self.assertRaises(ValueError):
            decode_nack(b"")


class DecodeSingleFrameValidationTests(unittest.TestCase):
    def test_too_short(self):
        with self.assertRaises(FrameParseError):
            decode_single_frame(b"\xAA\x01")

    def test_missing_start(self):
        raw = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        raw[0] = 0x00
        with self.assertRaises(FrameParseError):
            decode_single_frame(bytes(raw))

    def test_missing_end(self):
        raw = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        raw[-1] = 0x00
        with self.assertRaises(FrameParseError):
            decode_single_frame(bytes(raw))

    def test_checksum_mismatch_raises_specific_error(self):
        raw = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        raw[-2] ^= 0xFF  # corrupt the checksum byte
        with self.assertRaises(ChecksumError):
            decode_single_frame(bytes(raw))

    def test_length_mismatch(self):
        raw = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        raw[2] = 5  # claim 5 payload bytes when only 2 are present
        with self.assertRaises(FrameParseError):
            decode_single_frame(bytes(raw))


class FrameParserStreamingTests(unittest.TestCase):
    def test_single_frame(self):
        parser = FrameParser()
        raw = encode_frame(MSG_STATUS_UPDATE, bytes([42, 1]))
        frames = list(parser.feed(raw))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].msg_id, MSG_STATUS_UPDATE)
        self.assertEqual(frames[0].payload, bytes([42, 1]))

    def test_resyncs_past_leading_garbage(self):
        # Mirrors receiveFrame()'s own behavior: any stray byte before a
        # real FRAME_START (e.g. leftover text, line noise) is discarded
        # and parsing picks the valid frame right back up.
        parser = FrameParser()
        garbage = b"\x00\x01\xFF\x10"
        raw = encode_frame(MSG_FAULT_REPORT, bytes([9, 0]))
        frames = list(parser.feed(garbage + raw))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].msg_id, MSG_FAULT_REPORT)
        self.assertEqual(parser.malformed_frames, 0)  # garbage before START isn't "malformed", just noise

    def test_split_across_multiple_feeds(self):
        # Simulates a real serial read arriving in several chunks instead
        # of all at once - the parser must carry state between feed() calls.
        parser = FrameParser()
        raw = encode_frame(MSG_STATUS_UPDATE, bytes([77, 3, 10, 20, 30, 40, 50, 60]))

        frames = []
        for i in range(0, len(raw), 3):  # feed 3 bytes at a time
            frames.extend(parser.feed(raw[i:i + 3]))

        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].msg_id, MSG_STATUS_UPDATE)
        decoded = decode_status_update(frames[0].payload)
        self.assertEqual(decoded["soc"], 77)

    def test_checksum_error_counted_not_raised(self):
        # A corrupted frame in a live stream must not kill the parser -
        # it should be dropped and counted, and the stream keeps flowing.
        parser = FrameParser()
        bad = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        bad[-2] ^= 0xFF
        good = encode_frame(MSG_FAULT_REPORT, b"\x03\x04")

        frames = list(parser.feed(bytes(bad) + good))

        self.assertEqual(len(frames), 1)  # only the good one comes through
        self.assertEqual(frames[0].payload, b"\x03\x04")
        self.assertEqual(parser.checksum_errors, 1)

    def test_oversized_length_byte_resyncs(self):
        parser = FrameParser()
        malformed = bytes([FRAME_START, MSG_STATUS_UPDATE, MAX_PAYLOAD + 5])
        good = encode_frame(MSG_FAULT_REPORT, b"\x01\x02")

        frames = list(parser.feed(malformed + good))

        self.assertEqual(len(frames), 1)
        self.assertEqual(parser.malformed_frames, 1)

    def test_multiple_frames_in_one_feed(self):
        parser = FrameParser()
        raw = encode_frame(MSG_STATUS_UPDATE, b"\x01\x02") + encode_frame(MSG_FAULT_REPORT, b"\x03\x04")
        frames = list(parser.feed(raw))
        self.assertEqual([f.msg_id for f in frames], [MSG_STATUS_UPDATE, MSG_FAULT_REPORT])


class RayGlidesLinkTests(unittest.TestCase):
    def _make_link(self, fake):
        with patch("rayglides_protocol.serial.Serial", return_value=fake):
            return RayGlidesLink(port="/dev/ttyACM0", baudrate=115200)

    def test_send_writes_correct_binary_frame(self):
        fake = FakeSerial()
        link = self._make_link(fake)

        link.send(MSG_FAULT_REPORT, bytes([9, 1]))

        expected = encode_frame(MSG_FAULT_REPORT, bytes([9, 1]))
        self.assertEqual(bytes(fake._out), expected)

    def test_poll_returns_decoded_frames(self):
        fake = FakeSerial()
        link = self._make_link(fake)

        fake.inject(encode_frame(MSG_STATUS_UPDATE, bytes([60, 1])))
        frames = link.poll()

        self.assertEqual(len(frames), 1)
        self.assertEqual(decode_status_update(frames[0].payload)["soc"], 60)

    def test_poll_returns_empty_when_nothing_available(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        self.assertEqual(link.poll(), [])

    def test_read_frame_returns_none_on_timeout(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        result = link.read_frame(timeout=0.05)
        self.assertIsNone(result)

    def test_read_frame_returns_frame_when_available(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        fake.inject(encode_frame(MSG_OTA_ACK, bytes([0, 5, 0])))
        frame = link.read_frame(timeout=1.0)
        self.assertIsNotNone(frame)
        self.assertEqual(decode_ota_ack(frame.payload), {"status": 0, "seq": 5})

    def test_close_closes_underlying_serial(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        link.close()
        self.assertTrue(fake.closed)

    def test_context_manager_closes_on_exit(self):
        fake = FakeSerial()
        with patch("rayglides_protocol.serial.Serial", return_value=fake):
            with RayGlidesLink(port="/dev/ttyACM0") as link:
                link.send(MSG_FAULT_REPORT, b"\x01\x02")
        self.assertTrue(fake.closed)

    def test_error_counters_exposed(self):
        fake = FakeSerial()
        link = self._make_link(fake)

        bad = bytearray(encode_frame(MSG_FAULT_REPORT, b"\x01\x02"))
        bad[-2] ^= 0xFF
        fake.inject(bytes(bad))
        link.poll()

        self.assertEqual(link.checksum_errors, 1)

    def test_read_frame_does_not_drop_extra_frames_from_one_burst(self):
        # poll() can decode more than one frame out of a single serial
        # read - read_frame() must queue the leftovers instead of
        # discarding them, or a fast burst of replies loses data.
        fake = FakeSerial()
        link = self._make_link(fake)
        fake.inject(encode_frame(MSG_STATUS_UPDATE, bytes([10, 0]))
                    + encode_frame(MSG_FAULT_REPORT, bytes([1, 0])))

        first = link.read_frame(timeout=0.5)
        second = link.read_frame(timeout=0.5)

        self.assertEqual(first.msg_id, MSG_STATUS_UPDATE)
        self.assertEqual(second.msg_id, MSG_FAULT_REPORT)


class RayGlidesLinkSendAndAwaitTests(unittest.TestCase):
    """send_and_await() - the generic ACK/NACK request/response helper.
    Same FakeSerial harness as RayGlidesLinkTests above."""

    def _make_link(self, fake):
        with patch("rayglides_protocol.serial.Serial", return_value=fake):
            return RayGlidesLink(port="/dev/ttyACM0", baudrate=115200)

    def test_returns_ack_frame_on_success(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        fake.inject(encode_frame(MSG_ACK, bytes([MSG_STATUS_UPDATE])))

        frame = link.send_and_await(MSG_STATUS_UPDATE, b"\x01\x02", timeout=0.5)

        self.assertEqual(frame.msg_id, MSG_ACK)
        # Exactly one frame was actually sent - no retry needed on success.
        sent = list(FrameParser().feed(bytes(fake._out)))
        self.assertEqual(len(sent), 1)
        self.assertEqual(sent[0].msg_id, MSG_STATUS_UPDATE)

    def test_raises_nack_received_without_retrying(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        fake.inject(encode_frame(MSG_NACK, bytes([NACK_UNKNOWN_MSG_ID, 0x99])))

        with self.assertRaises(NackReceived) as ctx:
            link.send_and_await(0x99, b"", timeout=0.5, retries=3)

        self.assertEqual(ctx.exception.reason, NACK_UNKNOWN_MSG_ID)
        self.assertEqual(ctx.exception.reason_name, "UNKNOWN_MSG_ID")
        self.assertEqual(ctx.exception.acked_msg_id, 0x99)
        # A NACK means the device already understood and rejected the
        # frame - resending identical bytes wouldn't help, so there
        # should be exactly one send, not four.
        sent = list(FrameParser().feed(bytes(fake._out)))
        self.assertEqual(len(sent), 1)

    def test_raises_ack_timeout_after_exhausting_retries(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        # Nothing injected at all - every attempt times out.

        with self.assertRaises(AckTimeoutError):
            link.send_and_await(0x77, b"", timeout=0.05, retries=2)

        # Initial attempt + 2 retries = 3 sends total.
        sent = list(FrameParser().feed(bytes(fake._out)))
        self.assertEqual(len(sent), 3)

    def test_retries_after_timeout_then_succeeds(self):
        fake = FakeSerial()
        link = self._make_link(fake)
        # No reply for the first attempt; inject the ACK only once
        # send_and_await is already past its first (timed-out) attempt.

        real_read = fake.read
        state = {"sends": 0}

        def write_hook(data):
            state["sends"] += 1
            if state["sends"] == 2:  # the retry - now let the ACK arrive
                fake.inject(encode_frame(MSG_ACK, bytes([0x55])))
            fake._out.extend(data)
            return len(data)

        fake.write = write_hook

        frame = link.send_and_await(0x55, b"", timeout=0.1, retries=2)

        self.assertEqual(frame.msg_id, MSG_ACK)
        self.assertEqual(state["sends"], 2)  # first attempt timed out, second succeeded

    def test_unrelated_frame_does_not_satisfy_the_wait(self):
        # A telemetry frame arriving while waiting for an ACK must not be
        # mistaken for one - the wait should continue until the real
        # ACK/NACK (or the timeout) arrives.
        fake = FakeSerial()
        link = self._make_link(fake)
        fake.inject(encode_frame(MSG_STATUS_UPDATE, bytes([50, 0])))
        fake.inject(encode_frame(MSG_ACK, bytes([0x99])))

        frame = link.send_and_await(0x99, b"", timeout=0.5)

        self.assertEqual(frame.msg_id, MSG_ACK)


if __name__ == "__main__":
    unittest.main()
