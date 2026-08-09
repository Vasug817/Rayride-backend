# Pseudocode — Communication Module

Corresponds to: `RayGlidesProtocol.h/.cpp`

```
MODULE CommunicationModule

CONSTANTS
    FRAME_START = 0xAA
    FRAME_END   = 0x55
    MSG_STATUS_UPDATE = 0x01
    MSG_FAULT_REPORT  = 0x02
    MSG_ACK           = 0x03   // generic transport ACK, non-OTA frames only
    MSG_NACK          = 0x04   // generic transport NACK, non-OTA frames only

    NACK_CHECKSUM_ERROR  = 1
    NACK_MALFORMED_FRAME = 2
    NACK_UNKNOWN_MSG_ID  = 3

// --- Sending ---

FUNCTION ComputeChecksum(msgId, length, payload[]):
    checksum = msgId XOR length
    FOR EACH byte IN payload:
        checksum = checksum XOR byte
    RETURN checksum

FUNCTION SendFrame(msgId, payload[], length):
    checksum = ComputeChecksum(msgId, length, payload)
    TransmitByte(FRAME_START)
    TransmitByte(msgId)
    TransmitByte(length)
    FOR EACH byte IN payload:
        TransmitByte(byte)
    TransmitByte(checksum)
    TransmitByte(FRAME_END)

FUNCTION SendStatusUpdate(soc, chargeState):
    payload = [soc, chargeState]
    SendFrame(MSG_STATUS_UPDATE, payload, 2)

FUNCTION SendFaultReport(faultCode, severity):
    payload = [faultCode, severity]
    SendFrame(MSG_FAULT_REPORT, payload, 2)


// --- Receiving ---

FUNCTION SendAck(ackedMsgId):
    SendFrame(MSG_ACK, [ackedMsgId], 1)

FUNCTION SendNack(reason, ackedMsgId):
    SendFrame(MSG_NACK, [reason, ackedMsgId], 2)


FUNCTION ReceiveFrame():
    WAIT UNTIL byte received == FRAME_START
    msgId = ReadByte()
    length = ReadByte()

    IF length > MAX_PAYLOAD:
        LogWarn("Malformed frame - length exceeds MAX_PAYLOAD")
        IF NOT OTAInProgress(): SendNack(NACK_MALFORMED_FRAME, msgId)
        RETURN ERROR_MALFORMED_FRAME

    payload = ReadBytes(length)
    receivedChecksum = ReadByte()
    endByte = ReadByte()

    IF endByte != FRAME_END:
        LogWarn("Malformed frame - missing FRAME_END")
        IF NOT OTAInProgress(): SendNack(NACK_MALFORMED_FRAME, msgId)
        RETURN ERROR_MALFORMED_FRAME

    expectedChecksum = ComputeChecksum(msgId, length, payload)
    IF receivedChecksum != expectedChecksum:
        RaiseFault(F009_CHECKSUM_ERROR, WARNING)     // unchanged - fault log still fires
        LogWarn("Checksum mismatch on received frame")
        IF NOT OTAInProgress(): SendNack(NACK_CHECKSUM_ERROR, msgId)
        RETURN ERROR_CHECKSUM_MISMATCH

    RETURN (msgId, payload)      // Valid frame - hand off to message handler

// NACKs are suppressed while an OTA transfer is in progress so they
// can't interleave with in-flight OTA_DATA chunks on the same wire -
// OTA failures are already fully covered by OTAUpdate's own MSG_OTA_ACK
// error statuses (see ota/PSEUDOCODE.md), which this module doesn't
// duplicate.


FUNCTION HandleIncomingFrame(msgId, payload):
    // Only called for non-OTA frames - main.cpp routes MSG_OTA_* to the
    // OTA module's own handler before this function ever sees them.
    IF msgId == MSG_STATUS_UPDATE:
        ProcessStatusUpdate(payload)
        SendAck(msgId)
    ELSE IF msgId == MSG_FAULT_REPORT:
        ProcessFaultReport(payload)
        SendAck(msgId)
    ELSE IF msgId == MSG_ACK OR msgId == MSG_NACK:
        LogAckOrNack(msgId, payload)    // never re-acknowledged, avoids ping-pong
    ELSE:
        LogUnknownMessageType(msgId)
        SendNack(NACK_UNKNOWN_MSG_ID, msgId)


// --- Timeout watchdog ---

FUNCTION CheckCommunicationTimeout(lastMessageTimestamp, currentTime, timeoutLimit):
    IF (currentTime - lastMessageTimestamp) > timeoutLimit:
        RaiseFault(F008_COMM_TIMEOUT, WARNING)
```
