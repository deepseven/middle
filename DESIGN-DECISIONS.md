# Design Decisions

This document records the key design choices made in the Middle project, the
reasoning behind them, and alternatives that were considered and rejected.

---

## Product philosophy

Middle is a "record now, process later" device. The guiding constraint is that
the pendant itself should be as simple and power-efficient as possible — all
intelligence (transcription, task management, webhook delivery) lives on the
phone or server side. The pendant is a dumb recorder with a BLE radio.

---

## Hardware selection

### Why M5StickC Plus2 (current primary board)

The M5StickC Plus2 was chosen because it requires zero soldering: built-in
LiPo battery, USB-C charging, PDM microphone, TFT display, and three buttons.
It's a complete wearable package out of the box. The trade-off is an older
ESP32-PICO-V3-02 (not ESP32-S3), which uses the BlueDroid BLE stack instead
of NimBLE. BlueDroid is less reliable for high-throughput BLE notifications
(see "Notification pacing" below).

### Why ESP32 over nRF52 or other BLE SoCs

ESP32 was chosen for Arduino compatibility, large community, and the ability
to use PlatformIO. nRF52 would offer better BLE performance and lower power
consumption but has a steeper development curve and fewer off-the-shelf
prototyping boards with built-in microphones and batteries.

---

## Audio encoding: IMA ADPCM

### Why not raw PCM?

Raw 16-bit PCM at 16 kHz is 32 KB/s. LittleFS on the ESP32 has ~3 MB of
usable space, giving only ~90 seconds of recording. IMA ADPCM compresses 4:1
(~8 KB/s after ADPCM, packed two nibbles per byte = ~4 KB/s on flash),
allowing ~12 minutes of recording.

### Why not Opus, AAC, or MP3?

The ESP32 doesn't have hardware codec support, and software Opus/AAC encoding
at real-time rates would consume significant CPU and memory on a
microcontroller. IMA ADPCM is trivially simple (no floating point, no FFT,
just integer arithmetic), can encode sample-by-sample with no buffering, and
pairs well with the ring-buffer + FreeRTOS writer architecture.

### Why 16 kHz mono?

16 kHz is the sweet spot for speech. It captures all frequencies relevant to
human voice while keeping data rates low. Higher sample rates would waste
flash space on inaudible content. Mono because the pendant has one microphone.

---

## BLE protocol design

### Why custom GATT service, not Serial Port Profile (SPP)?

BLE doesn't support SPP natively (that's Bluetooth Classic). Custom GATT
service with discrete characteristics gives type-safe, structured
communication: separate characteristics for file count, file info, audio
data, commands, voltage, and pairing. This prevents framing issues and makes
the protocol self-documenting.

### Why notify-based streaming instead of write-with-response?

BLE notifications are unacknowledged at the GATT layer, enabling much higher
throughput (limited only by connection interval and MTU). With a 517-byte MTU,
each notification carries 514 bytes of audio data. Write-with-response would
halve throughput due to the mandatory ACK round-trip per write.

### Notification pacing

The firmware has different pacing strategies per BLE stack:

- **NimBLE** (ESP32-S3 boards): `delay(2)` every 4 chunks. NimBLE's TX queue
  is deeper and more predictable.
- **BlueDroid** (ESP32-PICO / M5StickC Plus2): `delay(8)` after every chunk.
  BlueDroid's `notify()` silently drops data when the TX queue is full — there
  is no back-pressure signal. Without pacing, phones drop 5–20% of chunks.

This was discovered empirically: the original code sent notifications as fast
as possible, resulting in ~70–80% data loss on the Android side. The root cause
was the Arduino BLE wrapper aborting on non-zero `ble_gatts_notify_custom()`
returns. The firmware now calls `ble_gatts_notify_custom()` directly and
retries up to 200 times on mbuf exhaustion.

### Why REQUEST_NEXT + START_STREAM two-step?

The phone sends `REQUEST_NEXT` (0x01) to tell the firmware to prepare a file,
then reads the file size from the `File Info` characteristic, then sends
`START_STREAM` (0x04) to begin the transfer. This two-step approach lets the
phone know the expected byte count before streaming begins, enabling progress
tracking and completion detection. Without it, the phone wouldn't know when
the transfer is done.

### Why delete-after-ACK instead of delete-all-after-sync?

Files are deleted one at a time, immediately after the phone confirms
successful receipt (`ACK_RECEIVED` = 0x02). This means a mid-sync
disconnection only loses the file currently being transferred — all
previously ACKed files are safely on the phone and off the pendant.
Batch-delete-after-sync would risk losing all files if the connection drops
before the final confirmation.

### SKIP_FILE command

If a file fails to transfer after 3 attempts (e.g., consistent notification
drops), the phone sends `SKIP_FILE` (0x06) to delete it from the pendant
and move on. This prevents one corrupt file from permanently blocking the
sync queue. The phone saves whatever partial data it received as a degraded
recording.

---

## Pairing: Trust-on-First-Use (TOFU)

### Why not BLE bonding?

BLE bonding (SMP pairing with LTK exchange) only stores encryption keys for
faster reconnection and encrypted links. It does not maintain a persistent
connection — the phone would still need to actively scan for advertisements.
Bonding also introduces complexity around bond database management, and is
not well-supported by all BLE library combinations (especially Nordic BLE
library + NimBLE firmware).

### The TOFU scheme

The pendant exposes a Pairing characteristic. When unclaimed (reads 0x00), any
phone can write a random 16-byte token to claim it. The token is stored in NVS
on the pendant and in EncryptedSharedPreferences on the phone. On subsequent
connections, the phone writes its stored token; the firmware verifies it and
disconnects on mismatch. This prevents casual hijacking while avoiding the
complexity of certificate-based auth.

**Known limitation**: this is not cryptographically secure. An attacker who
connects before the legitimate phone can steal the token. For the threat model
of a personal gadget, this is acceptable.

### UNPAIR command (0x07)

To re-pair with a different phone, the authenticated phone sends `UNPAIR`
(0x07), which erases the token from NVS. The pendant becomes unclaimed again.
Previously, only the phone-side token was cleared, leaving the pendant locked
to a phantom owner.

---

## Background BLE scanning strategy

### The problem

Android aggressively throttles BLE scanning when an app is not in the
foreground. Even with a foreground service, `SCAN_MODE_LOW_LATENCY`
callback-based scans are suppressed in Doze mode and App Standby. The
original implementation used the same scan mode for both foreground and
background, which meant recordings would not sync unless the user unlocked
the phone.

### The solution: PendingIntent-based scanning

When the app is in background (`AppVisibility.isForeground == false`), the
service switches to `BluetoothLeScanner.startScan(filters, settings,
PendingIntent)` with `SCAN_MODE_LOW_POWER`. This offloads filter matching
entirely to the Bluetooth controller hardware. The main CPU stays in deep
sleep; the BT chip autonomously watches for advertisements matching the
paired MAC address (or service UUID for discovery). When a match occurs,
Android delivers the `ScanResult` via the `PendingIntent`, which targets
the `SyncForegroundService` directly.

**Battery impact**: near-zero beyond having Bluetooth enabled. The BT
controller is already listening for BLE advertisements as part of its normal
operation; the scan filter just tells it which ones to report.

### Why not PendingIntent for foreground too?

`SCAN_MODE_LOW_LATENCY` with callbacks gives faster detection (~100 ms) when
the user is actively looking at the app. `LOW_POWER` PendingIntent scans can
have a detection delay of several seconds. The dual-mode approach gives the
best of both worlds.

### Wake lock during sync

Once a match is detected and `syncWithDevice()` begins, the service acquires
a `PARTIAL_WAKE_LOCK` to keep the CPU awake during the GATT connection and
data transfer. Without this, the CPU could go back to sleep mid-transfer in
Doze mode. The lock is released in the `finally` block after disconnect, and
has a 5-minute safety timeout to prevent leaks.

### Why not wake locks for scanning?

Wake locks during scanning would defeat the purpose — they'd keep the CPU
awake 24/7, which is exactly what Android's scan throttling was designed to
prevent. PendingIntent scanning is the correct architectural solution.

---

## Audio format: M4A/AAC on Android, MP3 on desktop

### Why M4A instead of MP3 on Android?

Android's `MediaCodec` has a built-in AAC encoder but no built-in MP3 encoder.
Using AAC/M4A avoids adding a native MP3 encoding library. OpenAI's
transcription API accepts both formats.

### Why MP3 on desktop (sync.py)?

Python has `lameenc` (a simple pip-installable MP3 encoder) which works
cross-platform. The desktop script predates the Android app and MP3 was the
natural choice.

---

## Firmware concurrency model

### Why dual-core with ring buffer?

The ESP32 has two cores. Core 1 runs the sampling loop (I2S read + ADPCM
encode), and Core 0 runs a FreeRTOS task that drains the ring buffer to
LittleFS. Flash writes, especially page erases, can stall for 10–100 ms.
Without this separation, flash stalls would cause sample drops. The 32 KB
ring buffer provides ~4 seconds of headroom at 8 KB/s ADPCM.

### Why not DMA-to-flash?

ESP32's DMA doesn't support direct I2S-to-flash transfers. The I2S peripheral
fills a DMA buffer, which the CPU then reads. The ADPCM encoding step also
requires CPU involvement (it's not just a memcpy).

---

## Deep sleep and power management

### Why deep sleep instead of light sleep?

Deep sleep draws ~7 µA (devkit) vs ~2 mA for light sleep. Since the pendant
spends most of its time idle, deep sleep is essential for multi-day battery
life. The trade-off is a full reboot on wake (~300 ms), which is acceptable
for a push-to-talk device.

### GPIO hold for power control (M5StickC Plus2)

The M5StickC Plus2 uses GPIO 4 as a power hold pin. If GPIO 4 goes LOW, the
device powers off completely. Before entering deep sleep, the firmware calls
`gpio_hold_en(GPIO_NUM_4)` followed by `gpio_deep_sleep_hold_en()` to keep
GPIO 4 HIGH during sleep. Without this, the device would power off instead of
sleeping, losing the ability to wake via button press.

### Recording discard threshold

Recordings shorter than 1000 ms are discarded. A quick button tap is used as
a "force sync" gesture — it wakes the device and starts BLE advertising
without saving a recording. This lets the user trigger a manual sync without
polluting the recordings list.

---

## Android app architecture

### Why foreground service, not WorkManager?

BLE operations require an active connection that can last tens of seconds (per
file transfer) and need to survive the full sync of multiple files. WorkManager
is designed for deferrable, guaranteed background work, but it runs in 10-minute
windows and can't maintain a BLE connection reliably. A foreground service with
a persistent notification is the correct primitive for long-running BLE
operations.

### Why Nordic BLE library instead of raw Android BLE APIs?

The Android BLE stack is notoriously difficult to use correctly. The Nordic
library (`no.nordicsemi.android:ble`) handles connection state machine
management, request queuing, automatic retries, and GATT operation timeouts.
Writing this from scratch would be error-prone and time-consuming.

### Why EncryptedSharedPreferences for settings?

The app stores API keys (OpenAI, ElevenLabs) which are sensitive. Using
`EncryptedSharedPreferences` with AES-256-GCM ensures keys are encrypted at
rest without requiring a custom encryption implementation.

### Webhook retry queue

Webhooks can fail due to transient network issues. The retry queue persists
pending deliveries as JSON files (survives app kill), uses binary exponential
backoff capped at 24 hours, and abandons after 10 attempts. 4xx responses are
treated as permanent failures (bad request, wrong URL) and deleted immediately;
only 5xx and network errors are retried.

---

## Stall detection (6-second timeout)

Instead of waiting the full 120-second transfer timeout on every stall, the
Android BLE manager polls the receive buffer every 3 seconds. If the buffer
size hasn't grown in two consecutive polls (6 seconds of silence), the
transfer is considered stalled and retried immediately. This reduces
worst-case recovery from 120 seconds to ~6 seconds per attempt, which
matters significantly for user experience when BlueDroid drops a run of
notifications.

---

## Decisions intentionally not made yet

- **End-to-end encryption**: the BLE link is unencrypted. TOFU token pairing
  prevents casual access but not a determined attacker with a BLE sniffer.
  Adding encryption would require a key exchange protocol and encrypt/decrypt
  overhead on the ESP32.
- **Over-the-air firmware updates (OTA)**: not implemented. Firmware updates
  require a USB cable. OTA would add complexity and a potential attack surface.
- **Cloud sync**: recordings stay on the phone. No cloud backup, no accounts,
  no servers. This is a deliberate privacy choice.
- **Multi-device support**: the pendant pairs with exactly one phone. Supporting
  multiple phones simultaneously would require a more complex pairing protocol.
