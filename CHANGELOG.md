# Changelog

## 2026-04-22

### M5StickS3: deep sleep, battery caching, dev mode, ES8311 codec mic

Revamped the M5StickS3 port from always-on to deep sleep, fixed battery
display, added developer mode, and switched to the M5Unified-managed ES8311
codec microphone.

**Deep sleep (ext0 wakeup on GPIO 11):** The earlier "always-on" model was
caused by an incorrect button GPIO mapping (GPIO 35 is not an RTC GPIO on
ESP32-S3). BtnA is actually on GPIO 11, which is RTC-capable, so standard
ext0 deep sleep wakeup now works. Before sleeping, the firmware powers down
the ES8311 codec via `M5.In_I2C.bitOff(0x6E, 0x11, ...)` and calls
`M5.Power.powerOff()`. Deep sleep current is <100 µA.

**Battery percentage caching:** `M5.Power.getBatteryVoltage()` (PY32 PMIC
I2C read) succeeds on the first call after boot but returns 0 on subsequent
calls. A static `cached_battery_mv` variable preserves the last successful
reading and is used as a fallback, so battery % now appears on all display
screens.

**Dev mode:** Hold BtnB at boot to disable automatic sleep. The display shows
"DEV MODE / sleep off". Useful for iterative flashing during development
since the COM port disappears during deep sleep.

**ES8311 codec mic:** Switched from raw PDM I2S to M5Unified's
`Mic_Class` with `cfg.internal_mic = true`. M5Unified manages the ES8311
codec's I2C configuration and I2S bus. Speaker must be stopped
(`cfg.internal_spk = false` + `Speaker.end()`) before mic works due to
the mono shared codec.

**Files changed:**
- `src/main.cpp` — GPIO 11/12 button mapping, ext0 wakeup, battery caching,
  dev mode, ES8311 mic via M5Unified, PMIC power-off in enter_deep_sleep()
- `ARCHITECTURE.md` — updated M5StickS3 description (ES8311 codec, deep
  sleep model, sleep current)
- `HOW-TO-M5STICKS3.md` — rewritten power model section, updated GPIO table,
  board comparison table, usage instructions, and troubleshooting

### M5StickS3 board support + PlatformIO convenience script

Added full support for the M5StickS3 (K147) as a fourth board variant. This
is the first board to use the M5Unified library, which replaces TFT_eSPI
for display, provides a button API, and manages the PY32 PMIC for battery
reading.

**Key design decisions driven by M5Unified:**
- Display is drawn via `M5.Display.*` (M5GFX) instead of TFT_eSPI, because
  M5Unified owns the SPI bus and auto-configures the display pins.
- Buttons use `M5.BtnA`/`M5.BtnB` with `M5.update()` instead of
  `digitalRead()`, because M5Unified configures button GPIOs internally.
- The recording loop polls `M5.BtnA.isPressed()` with `M5.update()` each
  iteration instead of `digitalRead(pin_button) == LOW`.
- Battery voltage uses `M5.Power.getBatteryVoltage()` (PY32 PMIC) instead
  of ADC with voltage divider.
- I2S mic is managed directly with `cfg.internal_mic = false` and
  `cfg.internal_spk = false` to prevent M5Unified from claiming the I2S bus.
- Board auto-detection requires `cfg.fallback_board = board_M5StickS3` to
  avoid misidentification as AtomS3R.

**Always-on power model**: unlike all other boards, the M5StickS3 does not use
deep sleep. The PY32 PMIC controls power via a hardware button — software
`esp_deep_sleep_start()` would cause an immediate reboot loop. The device stays
awake after boot and the user powers off by holding the hardware power button.

**Files changed:**
- `src/main.cpp` — `BOARD_M5STICKS3` build flag, M5Unified display/button/power
  code, always-on boot flow, I2S PDM on GPIO 0/1
- `platformio.ini` — new `[env:m5stick_s3]` environment using
  `esp32-s3-devkitc-1` board + M5Unified library dependency
- `pio.ps1` — new convenience script for build/flash/monitor/clean commands
  across all board variants (`.\pio.ps1 build s3`, `.\pio.ps1 flash s3`, etc.)
- `HOW-TO-M5STICKS3.md` — full setup guide with M5Unified reasoning
- `ARCHITECTURE.md` — updated to list four board variants, M5StickS3 audio
  pipeline, and button mappings

### Token persistence fix in sync.py and sync_work.py

Fixed a bug where `perform_pairing_handshake()` returned only `bool`, losing
the newly-generated pairing token when connecting to an unclaimed pendant.
On reconnection in the scan loop, `token_hex` remained `None`, causing
authentication failure.

**Fix**: function now returns `tuple[bool, str | None]`, and the caller
destructures with `paired, token_hex = await perform_pairing_handshake(...)`.

**Files changed:**
- `sync.py` — `perform_pairing_handshake()` return type + call site
- `private/sync_work.py` — same fix applied to work-laptop variant

## 2026-04-09

### Background BLE sync (PendingIntent scanning + wake lock)

**Problem**: recordings on the M5StickC Plus2 would not sync to the phone
unless the screen was unlocked. The previous implementation used
`SCAN_MODE_LOW_LATENCY` callback-based scanning for both foreground and
background, which Android aggressively throttles when the app is not visible.

**Fix**: the scan strategy is now split into two modes:

- **Foreground**: callback-based `SCAN_MODE_LOW_LATENCY` (unchanged, fast
  detection when the app is open).
- **Background**: `PendingIntent`-based `SCAN_MODE_LOW_POWER`. The Bluetooth
  controller handles filter matching in hardware while the CPU sleeps. When the
  pendant advertises, Android delivers the `ScanResult` via `PendingIntent`,
  waking the service even in Doze mode. Battery impact is near-zero.

A `PARTIAL_WAKE_LOCK` is now acquired during the actual GATT connection and
data transfer, ensuring the CPU stays awake for the transfer and is released
immediately after.

**Files changed (both `middle` and `middle_2.0`)**:
- `android/app/src/main/AndroidManifest.xml` — added `WAKE_LOCK` permission
- `android/.../ble/BleConstants.kt` — removed unused background scan constants
- `android/.../ble/SyncForegroundService.kt` — replaced single scan loop with
  dual-mode scanning (foreground callback + background PendingIntent), added
  wake lock acquire/release around sync, added `ACTION_BG_SCAN_RESULT` handler

## 2026-04-08

### UNPAIR BLE command (0x07)

Added a new BLE command that erases the pairing token from the pendant's NVS,
allowing the device to be re-paired with a different phone. Previously, the
"Unpair pendant" button in the Android app only cleared the phone-side token,
leaving the pendant still locked to the old phone.

**Files changed**:
- `src/main.cpp` — `command_unpair = 0x07`, `nvs_erase_pair_token()`, command handler
- `android/.../ble/BleConstants.kt` — `COMMAND_UNPAIR`
- `android/.../ble/PendantBleManager.kt` — `unpairPendant()`
- `android/.../ble/SyncForegroundService.kt` — `performUnpair()`, `ACTION_UNPAIR`
- `android/.../viewmodel/SettingsViewModel.kt` — sends BLE UNPAIR before clearing local state
- `sync.py` — `COMMAND_UNPAIR` constant
- `ARCHITECTURE.md` — updated commands list
