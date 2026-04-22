# How to build Middle with the M5StickS3

This guide walks you through flashing the Middle firmware onto an
**M5StickS3** (K147), and explains each firmware change driven by the
M5Unified library and M5StickS3 hardware.

No soldering or external wiring is needed. The M5StickS3 has a built-in
microphone (via ES8311 codec), color TFT display, buttons, and a 120 mAh
battery all in one unit. It uses an ESP32-S3 with BLE 5.0 and NimBLE — the
fastest BLE transfer path in the Middle firmware.

---

## Hardware overview

| Component | Role |
|---|---|
| M5StickS3 | ESP32-S3-PICO-1 + built-in ES8311 codec mic + BLE 5.0 + Wi-Fi + USB-C |
| 0.96" TFT LCD | ST7789V2 135×240 — shows recording/sync status (color) |
| 120 mAh Li-Po | Internal battery, charges via USB-C |
| BMI270 IMU | 6-axis accelerometer/gyroscope (unused by Middle) |
| ES8311 codec | I2S audio codec — mic and speaker share this mono codec |
| PY32 PMIC | Power management co-processor (I2C 0x6E) |

### Pin mapping (internal, no extra wiring)

| Function | GPIO | Notes |
|---|---|---|
| Front button (BtnA) | GPIO 11 | M5Unified `M5.BtnA`, active LOW |
| Side button (BtnB) | GPIO 12 | M5Unified `M5.BtnB`, active LOW |
| Audio codec | I2S (managed by M5Unified) | ES8311 — mic and speaker, mono shared |
| Display | SPI (managed by M5Unified) | ST7789V2, 135×240 |
| Power management | I2C: SDA=GPIO 48, SCL=GPIO 47 | PY32 PMIC at 0x6E |
| RGB LED | GPIO 21 | SK6812 NeoPixel (1 LED) |
| IR LED | GPIO 19 | Infrared transmitter (unused) |

### Key differences from other boards

| Feature | DevKit (ESP32-S3) | XIAO Sense | M5StickC Plus2 | M5StickS3 |
|---|---|---|---|---|
| MCU | ESP32-S3 | ESP32-S3 | ESP32-PICO-V3-02 | ESP32-S3-PICO-1 |
| BLE stack | NimBLE | NimBLE | BlueDroid | NimBLE |
| Microphone | INMP441 (I2S) | PDM (built-in) | SPM1423 PDM | ES8311 codec (M5Unified) |
| Display | None | SSD1306 OLED 128×64 | ST7789V2 TFT 135×240 | ST7789V2 TFT 135×240 (color) |
| Display library | — | U8g2 | TFT_eSPI | M5Unified/M5GFX |
| Battery reading | ADC on GPIO 1 | Not available | ADC on GPIO 38 | PY32 PMIC via M5Unified |
| Power management | Mic power gate | None | GPIO 4 power hold | PY32 PMIC (hardware button) |
| Sleep model | Deep sleep (ext0) | Deep sleep (ext0) | Deep sleep (ext0+ext1) | Deep sleep (ext0) |
| USB | Native USB CDC | Native USB CDC | CH9102 UART bridge | USB-Serial/JTAG (CDC) |

---

## Step 1: Install software

You need **Visual Studio Code** and the **PlatformIO** extension.

1. Download VS Code from https://code.visualstudio.com/ and install it.
2. Open VS Code → Extensions (`Ctrl+Shift+X`) → search **PlatformIO IDE** → Install.
3. Wait for the PlatformIO Core toolchain to download (a few minutes on first run).
4. Restart VS Code when prompted.

---

## Step 2: Open the project

1. **File → Open Folder** → select the `middle` folder (containing `platformio.ini`).
2. PlatformIO will detect the project and download the ESP32-S3 platform and
   M5Unified library on first run.

---

## Step 3: Build the firmware

Using the convenience script (recommended):
```powershell
.\pio.ps1 build s3
```

Or using raw PlatformIO:
```
pio run -e m5stick_s3
```

Or use the PlatformIO sidebar → expand **m5stick_s3** → click **Build**.

You should see `SUCCESS` in the terminal output.

---

## Step 4: Connect and upload

1. Plug the M5StickS3 into your PC via USB-C.
2. The device uses USB-Serial/JTAG (CDC on boot) — no external driver needed.
   Windows should recognize a COM port automatically.
3. Upload:
   ```powershell
   .\pio.ps1 flash s3
   ```
   Or:
   ```
   pio run -e m5stick_s3 -t upload
   ```

> **Note:** The COM port number can change between reconnects. If upload fails
> with a port error, check Device Manager for the current port, or remove
> `upload_port` from `platformio.ini` and let PlatformIO auto-detect.

---

## Step 5: Usage

After flashing, the M5StickS3 shows a status screen briefly on first
power-on, then enters **deep sleep** within a few seconds.  This is the same
behavior as other Middle boards — the device stays dormant until BtnA is
pressed.

- **Record**: Press BtnA (front button) to wake the device and start
  recording. The TFT shows "Recording..." with a red indicator.
  Release when done.
- **Auto-sync**: After recording, the device advertises via BLE for
  ~10 seconds. If a sync client connects, recordings transfer automatically.
  Then the device returns to deep sleep.
- **Force sync**: Short-press BtnB (side button) during the awake period
  to start BLE advertising manually.
- **Oblique strategy**: Long-press BtnB (≥1 second) to display a random
  Oblique Strategy on the color TFT.
- **Status**: The device shows file count and free storage on the TFT
  when BLE advertising starts.
- **Power off**: Hold the hardware power button for 6+ seconds. The PY32
  PMIC cuts power entirely.
- **USB note**: During deep sleep the ESP32-S3's USB CDC is powered down,
  so the COM port disappears from the host. It reappears when the device
  wakes (BtnA press or power cycle).

---

## Why M5Unified changes everything

The M5StickC Plus2 uses `TFT_eSPI` for its display — a standalone SPI TFT
driver that you configure with build flags (`-DST7789_DRIVER`, `-DTFT_MOSI=15`,
etc.) and call directly. Buttons are plain `digitalRead()` on known GPIO pins.
Battery voltage is an ADC reading on GPIO 38. Power management is a GPIO hold
pin. Each peripheral is independent and configured manually.

The M5StickS3 uses **M5Unified** instead. M5Unified is M5Stack's hardware
abstraction layer — a single library that auto-detects the board and initializes
*all* peripherals (display, buttons, PMIC, IMU, speaker, I2C bus) in one
`M5.begin()` call. This replaces several independent libraries and manual GPIO
setup, but it also takes ownership of certain hardware resources.

This had cascading effects on every subsystem in the firmware:

### Display: M5GFX replaces TFT_eSPI

The M5StickS3's ST7789V2 display is managed by M5GFX (bundled inside
M5Unified). You draw via `M5.Display.*` methods instead of instantiating a
`TFT_eSPI` object. The API is similar but not identical:

- `M5.Display.fillScreen()` instead of `tft.fillScreen()`
- `M5.Display.drawString()` instead of `tft.drawString()`
- `M5.Display.setRotation(1)` for landscape (240×135)
- `M5.Display.wakeup()` / `M5.Display.sleep()` for power control

**Why not use TFT_eSPI?** The M5StickS3's display SPI pins are not documented
in public datasheets — M5Unified discovers and configures them via its board
detection logic. Trying to use TFT_eSPI would require reverse-engineering the
pin assignments and risk conflicting with M5Unified's I2C/SPI bus
initialization.

### Buttons: M5Unified button API replaces digitalRead

M5Unified debounces and manages button GPIOs internally. On the M5StickS3,
`digitalRead(35)` is unreliable because M5Unified has already configured the
pin with its own interrupt/polling logic.

The firmware uses `M5.BtnA` and `M5.BtnB` instead:
- `M5.update()` must be called each loop iteration to refresh button state.
- `M5.BtnA.isPressed()` replaces `digitalRead(pin_button) == LOW` in the
  recording loop.
- `M5.BtnA.wasPressed()`, `M5.BtnB.wasPressed()`, `M5.BtnB.wasReleased()`,
  and `M5.BtnB.pressedFor(ms)` are used in `loop()` for press/long-press
  detection.

**Consequence for recording**: the other boards use a simple
`while (digitalRead(pin_button) == LOW)` loop to record while the button
is held. The M5StickS3 must instead call `M5.update()` before each check
and use `while (M5.BtnA.isPressed())`. An extra `M5.update()` call inside
the sample-reading loop ensures the button release is detected promptly.

### Battery: PMIC API replaces ADC

The M5StickC Plus2 reads battery voltage via an ADC on GPIO 38 through a
hardware voltage divider. The M5StickS3 has no ADC pin for the battery —
the battery is managed by a PY32 PMIC co-processor on I2C (address 0x6E).

M5Unified abstracts this: `M5.Power.getBatteryVoltage()` returns millivolts
regardless of which PMIC the board has. The firmware uses this single call
instead of the 10-sample averaged ADC reading used on other boards.

### I2S microphone: managed by M5Unified (async DMA)

The M5StickS3's microphone is routed through the **ES8311 codec** — a mono
I2S audio codec shared between mic and speaker. Unlike the M5StickC Plus2
(which has a standalone SPM1423 PDM mic on dedicated GPIOs), the M5StickS3
must use M5Unified's `Mic_Class` to access the microphone.

M5Unified's mic is enabled at init:
```cpp
cfg.internal_mic = true;    // Let M5Unified manage the ES8311 mic
cfg.internal_spk = false;   // Don't init speaker — conflicts with mic
```

Recording uses `M5.Mic.record()`, which is **asynchronous** — it submits a
DMA request to a background FreeRTOS task and returns immediately. This has
critical implications for buffer management:

```cpp
// WRONG — stack buffer freed while DMA still writing → crash
int16_t buf[256];
M5.Mic.record(buf, 256, 16000, false);
// buf is freed when function returns, mic task writes to freed memory

// CORRECT — static buffer persists, wait for DMA completion
static int16_t buf[256];
M5.Mic.record(buf, 256, 16000, false);
while (M5.Mic.isRecording()) { delay(1); }  // wait for DMA
```

The speaker must be stopped before the mic can operate (mono codec):
`M5.Speaker.end()` is called before any mic initialization.

**Important:** Never call `M5.Mic.end()` between recordings — the ES8311
codec fails to reinitialize correctly after `end()` + `begin()`. Keep the
mic alive across all recording sessions.

### Power model: deep sleep with ext0 wakeup

Earlier versions of the M5StickS3 port used an always-on model because
the button GPIO was incorrectly mapped to GPIO 35 (which is not an RTC
GPIO on ESP32-S3).  The actual M5StickS3 BtnA is on **GPIO 11**, which
**is** an RTC GPIO, so ext0 deep sleep wakeup works.

The M5StickS3 now uses the same deep sleep model as all other boards:
1. Device boots, checks `esp_sleep_get_wakeup_cause()`.
2. If not woken by BtnA (ext0), shows status screen briefly, then sleeps.
3. BtnA press triggers ext0 wakeup → device reboots → records → syncs via
   BLE → sleeps again.

Before entering deep sleep, the firmware:
- Turns off the display (`M5.Display.sleep()`).
- Stops BLE advertising.
- Powers down the ES8311 codec by clearing the PMIC register
  (`M5.In_I2C.bitOff(0x6E, 0x11, 0b00001000)`).
- Calls `M5.Mic.end()` to release I2S resources.
- Calls `esp_deep_sleep_start()`.

The PY32 PMIC continues supplying power to the ESP32 during deep sleep
(unlike a full power-off), allowing the RTC domain to monitor GPIO 11
for a button press.  Deep sleep current is typically <100 µA.

**USB CDC during sleep:** The ESP32-S3's built-in USB CDC is powered by
the USB peripheral, which is off during deep sleep.  The serial port
disappears from the host while sleeping and reappears on wakeup.

### Board detection: fallback_board required

M5Unified auto-detects the board by reading I2C device signatures and GPIO
states. The M5StickS3 uses an ESP32-S3-PICO-1 in an LGA56 package, which
M5Unified sometimes misidentifies as an **AtomS3R** (a different M5Stack
product in the same package).

When misidentified, M5Unified disables the PMIC and speaker peripherals,
breaking `M5.Power.getBatteryVoltage()` and potentially other features.

The fix:
```cpp
auto cfg = M5.config();
cfg.fallback_board = m5::board_t::board_M5StickS3;
```

This tells M5Unified: "if you can't determine the board, assume M5StickS3."
In practice, this ensures correct initialization every time.

### PlatformIO configuration: board = esp32-s3-devkitc-1

The M5StickS3 does not have a dedicated PlatformIO board definition. Since
it uses an ESP32-S3-PICO-1 (same core as the ESP32-S3-DevKitC-1), we reuse
the `esp32-s3-devkitc-1` board definition with M5StickS3-specific build
flags:

```ini
[env:m5stick_s3]
board = esp32-s3-devkitc-1
build_flags =
    -DBOARD_M5STICKS3=1
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    m5stack/M5Unified @ ^0.2.13
```

Key differences from the Plus2 config:
- `board = esp32-s3-devkitc-1` (not `pico32` — that's ESP32, not ESP32-S3)
- No TFT_eSPI build flags (M5Unified handles display internally)
- `ARDUINO_USB_MODE=1` for USB-Serial/JTAG (native USB on ESP32-S3-PICO-1)
- `m5stack/M5Unified` as the only library dependency (it bundles M5GFX)

### Display color theme

Since the M5StickS3 port uses the M5GFX API directly (no TFT_eSPI abstraction),
the display code uses named color constants for visual state differentiation:

| State | Color | Hex |
|---|---|---|
| Background | Black | `0x000000` |
| Default text | White | `0xFFFFFF` |
| Recording | Red | `0xFF3333` |
| Syncing | Blue | `0x3399FF` |
| Synced | Green | `0x33CC66` |
| Status info | Amber | `0xFFCC00` |
| Dim (battery %) | Grey | `0x666666` |

The same status information is shown as on other boards (file count, free
storage, sync progress), but with color accents making it easier to read
at a glance.

---

## Summary of all firmware changes for M5StickS3

| Area | Other boards | M5StickS3 | Reason |
|---|---|---|---|
| Display library | TFT_eSPI / U8g2 | M5Unified (M5GFX) | M5Unified owns the SPI bus and pin config |
| Display init | Manual SPI pin config | `M5.begin()` + `M5.Display.*` | M5Unified auto-configures display hardware |
| Button reading | `digitalRead()` | `M5.BtnA/BtnB` API + `M5.update()` | M5Unified owns button GPIOs |
| Recording loop | `while (digitalRead(...) == LOW)` | `while (M5.BtnA.isPressed())` with `M5.update()` | Direct GPIO read unreliable under M5Unified |
| Battery voltage | ADC with voltage divider | `M5.Power.getBatteryVoltage()` | PY32 PMIC, no ADC pin exposed |
| I2S mic | Direct init (ESP-IDF) | M5Unified `Mic_Class` (async DMA) | ES8311 codec requires M5Unified management |
| Power model | Deep sleep + ext0 wakeup | Always-on, hardware power button | Deep sleep + ext0 wakeup on GPIO 11 |
| Sleep on boot | Yes (if not button wakeup) | No (stays awake) | Yes (if not button wakeup) |
| Board detection | N/A | `cfg.fallback_board = board_M5StickS3` | Prevent misidentification as AtomS3R |
| PlatformIO board | `pico32` / `seeed_xiao_esp32s3` | `esp32-s3-devkitc-1` | No dedicated PIO board def for S3-PICO-1 |
| Speaker init | N/A | `cfg.internal_spk = false` + `Speaker.end()` | Mono codec — speaker must be off for mic |
| Mic lifecycle | `i2s_del_channel()` on stop | Keep alive (never call `Mic.end()`) | ES8311 fails to reinit after end()+begin() |
| Buffer management | Stack-local OK (blocking read) | Static/heap only (async DMA!) | DMA writes after record() returns |

---

## Troubleshooting

| Problem | Solution |
|---|---|
| "No serial port" on upload | Check that USB-C cable supports data (not charge-only). Try a different port. |
| COM port keeps changing | Remove `upload_port` from `platformio.ini` to auto-detect. |
| Display shows nothing after boot | Unusual — M5StickS3 should show status on boot. Check serial monitor for errors. |
| Recording too quiet | The firmware applies 4× software gain (`pdm_gain_shift = 2`). If still quiet, increase to 3 (8×) in the `BOARD_M5STICKS3` section of `main.cpp`. |
| Build error: M5Unified not found | Run `pio pkg install -e m5stick_s3` to force library download. |
| Battery always reads 0 | Board detection failed — ensure `cfg.fallback_board = board_M5StickS3` is set before `M5.begin()`. |
| Device reboots in a loop | Check that BtnA (GPIO 11) has a proper pull-up. Verify `BOARD_M5STICKS3` is defined. |
| Linker error: "reopening .o: No such file" | Stale build artifacts. Run `.\pio.ps1 clean s3` and rebuild. |
