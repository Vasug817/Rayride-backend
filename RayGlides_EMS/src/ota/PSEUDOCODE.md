# OTA Update - Design Notes

## Transport decision
No WiFi exists anywhere in this codebase - it's a bus-connected EMS, not
a WiFi IoT device. Rather than bolting on a WiFi stack, OTA reuses the
existing RayGlidesProtocol framing (FRAME_START/END, checksum,
ReceivedFrame) that USB Serial and RS485 already share.

CAN was deliberately left out: classic CAN frames cap at 8 data bytes,
which would need thousands of frames per MB of firmware. Serial and
RS485 both already carry MAX_PAYLOAD=32-byte frames, which is workable.

## Message flow (host = flashing tool, device = this firmware)
```
host -> MSG_OTA_BEGIN  [size:4][crc32:4]        device -> MSG_OTA_ACK
host -> MSG_OTA_DATA    [seq:2][up to 30 bytes]  device -> MSG_OTA_ACK   (repeated per chunk)
host -> MSG_OTA_END     (no payload)             device -> MSG_OTA_ACK, then reboots
host -> MSG_OTA_ABORT   (no payload)             device -> MSG_OTA_ACK   (any time)
```
Chunks must arrive in order (seq 0, 1, 2, ...) - any gap or duplicate
aborts the whole transfer rather than trying to patch around it. This
project doesn't need chunk-level retry logic; if a transfer fails, the
host just starts a fresh MSG_OTA_BEGIN. Keeping it "abort and restart"
rather than "resume" is a deliberate scope decision, not an oversight.

## Safety interlocks
1. **Relay forced open during transfer.** `RelayControl::updateRelay()`
   checks `isOTAInProgress()` first, ahead of even the watchdog-lockout
   check. Rewriting flash and driving the charging relay should never
   happen at the same time.
2. **Debug logging silenced during transfer.** `DebugLog` writes to the
   same `Serial` UART that OTA frames travel over on that bus - plain
   text mid-transfer would corrupt the binary stream. Silenced at
   MSG_OTA_BEGIN, restored on any exit that isn't a reboot.
3. **CRC32 + size check before committing.** The running CRC32 is
   accumulated chunk-by-chunk and checked against the value sent at
   MSG_OTA_BEGIN before `Update.end(true)` is ever called. A mismatch
   aborts and leaves the OLD image as the one that boots next - nothing
   is committed on a bad transfer.
4. **Post-flash validation with automatic rollback.** `Update.end(true)`
   flips the ESP32 bootloader's target partition, but that alone doesn't
   prove the new image actually works. Before rebooting, a
   `pendingValidation` flag + the *previous* running partition are
   persisted to EEPROM (separate region from `WatchdogState`, same
   magic+checksum pattern). `confirmOTAHealthy()` clears that flag once
   the new image reaches `WATCHDOG_HEALTHY_UPTIME_MS` of stable uptime -
   the exact same checkpoint `confirmWatchdogHealthy()` already uses.
   If the flag is still set after `OTA_MAX_BOOT_ATTEMPTS` boots (i.e. the
   new image keeps crashing/resetting before ever confirming healthy),
   `initOTARecovery()` calls `esp_ota_set_boot_partition()` back to the
   previous partition and reboots into known-good firmware, automatically.

## Why this composes with WatchdogRecovery instead of duplicating it
A bad OTA image that hangs will trip the hardware watchdog like any
other hang - `WatchdogRecovery`'s own crash counter and lockout already
handle that case. `OTAUpdate` doesn't re-implement that; it adds one
extra layer on top: even an image that *doesn't* hang (boots fine, runs,
but is subtly broken) still won't get treated as good until it proves
`WATCHDOG_HEALTHY_UPTIME_MS` of real uptime. Both mechanisms share that
checkpoint but track independent EEPROM state, so they can't mask each
other's fault reporting.

## Prerequisites already satisfied
- ESP32's default Arduino partition table (unmodified `platformio.ini`)
  already provides two OTA app slots (`app0`/`app1` via `ota_0`/`ota_1`) -
  no partition table changes were needed.
- `Update.h`, `esp_ota_ops.h`, and `esp_partition.h` all ship with the
  arduino-esp32 core already in this project - no new `lib_deps`.

## Known follow-up
- No chunk retransmission/resume - a failed transfer restarts from
  MSG_OTA_BEGIN. Fine for a bench/host-tool workflow; would need
  revisiting for a flow where dropped frames are common.
- No image signing/authentication - anything sending well-formed OTA
  frames on Serial/RS485 can flash the device. Acceptable for a
  physically-wired bench setup; would need addressing before this
  firmware is reachable from anything less trusted than a direct cable.
