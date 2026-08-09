# Pseudocode — CAN Communication

Corresponds to: `CANComm.h/.cpp`

Uses the ESP32's built-in TWAI controller (CAN 2.0B compatible) at
500 kbit/s. Unlike the RS485/Serial protocol, CAN frames are
self-delimiting (identifier + data length + up to 8 data bytes), so no
START/END framing bytes or checksum are needed - the CAN controller
hardware handles bit-stuffing and CRC error detection itself.

```
MODULE CANCommunication

CONSTANTS
    CAN_TX_PIN, CAN_RX_PIN
    CAN_ID_STATUS = 0x100
    CAN_ID_FAULT  = 0x101
    BITRATE = 500 kbit/s


FUNCTION InitCAN():
    generalConfig = DefaultConfig(CAN_TX_PIN, CAN_RX_PIN, MODE_NORMAL)
    timingConfig = TimingConfig(500 kbit/s)
    filterConfig = AcceptAllFilter()

    IF InstallDriver(generalConfig, timingConfig, filterConfig) FAILED:
        LogError("CAN driver install failed")
        RETURN false

    IF StartDriver() FAILED:
        LogError("CAN failed to start")
        RETURN false

    RETURN true


FUNCTION SendCANStatus(battery, solar, chargeState):
    payload = [
        battery.soc,
        chargeState,
        Round(battery.voltage),
        RoundSigned(battery.current),
        Round(battery.temperature),
        Round(solar.voltage),
        Round(solar.power)
    ]                                    // 7 bytes - fits CAN's 8-byte limit
    TransmitFrame(CAN_ID_STATUS, payload, length=7)


FUNCTION SendCANFault(faultCode, severity):
    payload = [faultCode, severity]
    TransmitFrame(CAN_ID_FAULT, payload, length=2)


FUNCTION TransmitFrame(id, data, length):
    message.identifier = id
    message.data_length_code = length
    message.data = data
    result = Transmit(message, timeout=100ms)
    IF result FAILED:
        LogError("CAN TX failed for ID " + id)


FUNCTION ReceiveCAN():
    // Non-blocking - a 0ms timeout means "return immediately if nothing waiting"
    IF Receive(message, timeout=0) == OK:
        RETURN (valid=true, id=message.identifier, data=message.data,
                length=message.data_length_code)
    RETURN (valid=false)
```

## EMS integration

`main.cpp` calls `InitCAN()` once at startup. Each loop cycle,
`SendCANStatus()` reports the same battery/solar telemetry that goes out
over the USB Serial protocol and RS485, and `SendCANFault()` fires
whenever a new fault latches - so all three communication buses carry
consistent data, just over different physical transports. `ReceiveCAN()`
is polled every cycle (non-blocking) to check for incoming traffic from
other CAN nodes on the bus, feeding the same communication timeout
watchdog used by the other buses.
