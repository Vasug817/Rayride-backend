# RayGlides Protocol - Raspberry Pi Host Library

Talks to the RayGlides EMS over USB Serial (`/dev/ttyACM0` @ 115200 baud
by default), using the exact same binary framing the firmware's
`RayGlidesProtocol.cpp` sends and parses:

```
[FRAME_START 0xAA][msgId][len][payload...][checksum][FRAME_END 0x55]
checksum = msgId ^ len ^ payload[0] ^ payload[1] ^ ...
```

This only works against the current firmware. If your board is running
an older build where `sendFrame()` printed ASCII HEX text instead of
real binary bytes, this library will see nothing but noise on receive -
reflash first.

## Files
- `rayglides_protocol.py` - the library: encode/decode, a resilient
  streaming `FrameParser`, message-specific decoders, and `RayGlidesLink`
  (a pyserial wrapper for the real port).
- `test_rayglides_protocol.py` - 27 unit tests, no hardware required
  (uses a fake serial port). Run these first on any machine.
- `monitor.py` - a small live demo that connects to real hardware and
  prints every decoded frame it receives.
- `aws_packets.py` - pure functions that turn decoded frames into the
  JSON packets used on the AWS side (telemetry, fault, gateway
  heartbeat, OTA status, device shadow). No AWS SDK dependency - see
  `test_aws_packets.py`.
- `aws_gateway.py` - the AWS IoT Core gateway: polls the EMS over serial,
  publishes the JSON packets from `aws_packets.py` over MQTT/TLS, keeps
  the device shadow current, and handles incoming AWS IoT Jobs by
  downloading a firmware image from S3 and driving the existing
  `OTA_BEGIN/DATA/END` handshake against the EMS. Requires `paho-mqtt`
  and `boto3` (see `requirements-aws.txt`) - only to actually run it
  against AWS; it imports fine without them for testing.
- `aws_config.example.json` - copy to `aws_config.json` and fill in your
  AWS IoT endpoint, device certs, and serial port.
- `test_aws_packets.py` - unit tests for `aws_packets.py`, no network or
  AWS credentials required.

See `RayGlides_AWS_Cloud_Architecture.md` (project root, or wherever it
was delivered alongside this code) for the full architecture this
module implements: MQTT topic layout, AWS service responsibilities,
and the JSON packet formats in detail.

## Setup (on the Raspberry Pi)
```bash
pip install pyserial
# If you get a permission error opening the port:
sudo usermod -aG dialout $USER   # then log out and back in
```

## Run the tests (no hardware needed)
```bash
python3 -m unittest test_rayglides_protocol.py -v
```

## Talk to real hardware
```bash
python3 monitor.py                      # /dev/ttyACM0 @ 115200
python3 monitor.py --port /dev/ttyUSB0  # different port
```

## Run the AWS IoT gateway (bridges the EMS to AWS)
```bash
pip install pyserial -r requirements-aws.txt
cp aws_config.example.json aws_config.json   # fill in endpoint + cert paths
python3 -m unittest test_aws_packets.py -v   # sanity-check the packet builders first
python3 aws_gateway.py --config aws_config.json
```
This publishes telemetry/fault/heartbeat JSON to AWS IoT Core, keeps the
device shadow current, and listens for AWS IoT Jobs to drive OTA updates
against the EMS. It's a separate process from `monitor.py` - run one or
the other against a given serial port, not both at once.

## Using it in your own code
```python
from rayglides_protocol import RayGlidesLink, decode_frame, MSG_FAULT_REPORT

with RayGlidesLink(port="/dev/ttyACM0", baudrate=115200) as link:
    # Receive + decode whatever the EMS has sent since the last poll()
    for frame in link.poll():
        print(decode_frame(frame))

    # Send a frame - e.g. echoing a fault report back for a loopback test
    link.send(MSG_FAULT_REPORT, bytes([13, 1]))  # [fault_code=13, severity=1]

    # Or block for exactly one reply, e.g. waiting on an OTA_ACK
    reply = link.read_frame(timeout=2.0)
```

## Message types currently decoded
| msg_id | Name | Decoder |
|---|---|---|
| 0x01 | STATUS_UPDATE | `decode_status_update()` - SOC, state, battery V/I/T, solar V/P, SOH |
| 0x02 | FAULT_REPORT | `decode_fault_report()` - fault code, severity |
| 0x14 | OTA_ACK | `decode_ota_ack()` - status, sequence number |

`0x10`-`0x13` (`OTA_BEGIN`/`DATA`/`END`/`ABORT`) are host-to-device
messages the Pi would *send*, not receive - `encode_frame()` builds
those directly; there's no decoder for them since the device never
sends them back. Unknown message IDs decode to
`{"type": "unknown", "msg_id": ..., "payload": ...}` rather than
raising, so a firmware update that adds a new message type doesn't
break existing host tooling.

## Scope note
This library covers general protocol send/receive/validate/decode, not
a full OTA flashing tool. Driving a firmware upload (chunking a `.bin`
into `MSG_OTA_DATA` frames with a running CRC32, per
`src/ota/PSEUDOCODE.md` in the firmware) would be a good next piece to
build on top of this, using `RayGlidesLink.send()` / `read_frame()`
directly.
