# Pseudocode — RS485 Protocol

Corresponds to: `RS485Comm.h/.cpp`

RS485 is electrically half-duplex - only one device can drive the bus at
a time - so a transceiver chip (e.g. MAX485) needs a direction pin (DE/RE)
toggled HIGH to transmit and LOW to receive. This module reuses the exact
same frame format as `RayGlidesProtocol` (START/MSG_ID/LEN/PAYLOAD/
CHECKSUM/END), just over a second UART with that extra direction control
step wrapped around each transmission.

```
MODULE RS485Protocol

CONSTANTS
    RS485_TX_PIN, RS485_RX_PIN, RS485_DE_PIN
    BAUD = 9600
    FRAME_START = 0xAA
    FRAME_END = 0x55


FUNCTION InitRS485():
    SetPinMode(RS485_DE_PIN, OUTPUT)
    WriteDigitalPin(RS485_DE_PIN, LOW)     // start in receive mode
    OpenUART(BAUD, RS485_RX_PIN, RS485_TX_PIN)


FUNCTION SendFrame(msgId, payload, length):
    checksum = ComputeChecksum(msgId, length, payload)

    WriteDigitalPin(RS485_DE_PIN, HIGH)    // switch transceiver to transmit
    Wait(50 microseconds)                   // let the driver settle before data

    WriteByte(FRAME_START)
    WriteByte(msgId)
    WriteByte(length)
    FOR EACH byte IN payload:
        WriteByte(byte)
    WriteByte(checksum)
    WriteByte(FRAME_END)
    FlushUART()                             // block until fully clocked out

    WriteDigitalPin(RS485_DE_PIN, LOW)     // release the bus back to receive


FUNCTION ReceiveFrame():
    IF NothingAvailable():
        RETURN (valid=false)

    IF PeekByte() != FRAME_START:
        ReadByte()                          // discard stray byte, resync
        RETURN (valid=false)

    ReadByte()                              // consume START
    IF FewerThan(2) bytes available:
        RETURN (valid=false)                // not enough buffered yet

    msgId = ReadByte()
    length = ReadByte()
    IF length > MAX_PAYLOAD:
        RETURN (valid=false)                // malformed - reject

    WAIT UNTIL (length + 2) bytes are available   // payload + checksum + end

    payload = ReadBytes(length)
    receivedChecksum = ReadByte()
    endByte = ReadByte()

    IF endByte != FRAME_END:
        RETURN (valid=false)                // malformed frame

    expectedChecksum = ComputeChecksum(msgId, length, payload)
    IF receivedChecksum != expectedChecksum:
        LogError("RS485 checksum mismatch")
        RETURN (valid=false)

    RETURN (valid=true, msgId, payload, length)
```

## Why reuse the same frame format as the Serial protocol

RS485 is electrically different from the USB Serial link (differential
half-duplex over two wires vs. single-ended full-duplex), but at the byte
level it is still just a UART stream - so there is no reason to invent a
second framing scheme. `computeChecksum()`, `FRAME_START`, `FRAME_END`,
and the message ID constants are all reused directly from
`RayGlidesProtocol.h`, and only the transport (which UART, plus the DE
pin toggling) differs.

## EMS integration

`main.cpp` calls `InitRS485()` once at startup. Each loop cycle, the same
status and fault payloads sent over USB Serial and CAN are also sent over
RS485 via `rs485SendFrame()`, and `rs485ReceiveFrame()` is polled
non-blocking each cycle to check for incoming traffic - feeding the same
shared communication timeout watchdog as the other two buses.
