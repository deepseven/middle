# How to build Middle with the M5StickS3

This guide walks you through flashing the Middle firmware onto an
**M5StickS3** (K147).

No soldering or external wiring is needed. The M5StickS3 has a built-in
PDM microphone, color TFT display, buttons, and a 120 mAh battery all in
one unit. It uses an ESP32-S3 with BLE 5.0 and NimBLE — the fastest BLE
transfer path in the Middle firmware.

---

## Hardware overview

| Component | Role |
|---|---|
| M5StickS3 | ESP32-S3-PICO-1 + built-in SPM1423 PDM mic + BLE 5.0 + Wi-Fi + USB-C |
| 0.96" TFT LCD | ST7789V2 135×240 — shows recording/sync status (color) |
| 120 mAh Li-Po | Internal battery, charges via USB-C |
| BMI270 IMU | 6-axis accelerometer/gyroscope (unused by Middle) |
| ES8311 codec | I2S audio codec with speaker (unused by Middle) |
| PY32 PMIC | Power management co-processor (I2C 0x6E) |

### Pin mapping (internal, no extra wiring)

| Function | GPIO | Notes |
|---|---|---|
| Front button (BtnA) | GPIO 35 | M5Unified `M5.BtnA`, active LOW |
| Side button (BtnB) | GPIO 37 | M5Unified `M5.BtnB`, active LOW |
| PDM mic clock | GPIO 0 | SPM1423, built-in |
| PDM mic data | GPIO 1 | SPM1423, built-in |
| Display | SPI (managed by M5Unified) | ST7789V2, 135×240 |
| Power management | I2C: SDA=GPIO 48, SCL=GPIO 47 | PY32 PMIC at 0x6E |
| RGB LED | GPIO 21 | SK6812 NeoPixel (1 LED) |
| IR LED | GPIO 19 | Infrared transmitter (unused) |

### Key differences from other boards

| Feature | DevKit (ESP32-S3) | XIAO Sense | M5StickC Plus2 | M5StickS3 |
|---|---|---|---|---|
| MCU | ESP32-S3 | ESP32-S3 | ESP32-PICO-V3-02 | ESP32-S3-PICO-1 |
| BLE stack | NimBLE | NimBLE | BlueDroid | NimBLE |
| Microphone | INMP441 (I2S) | PDM (built-in) | SPM1423 PDM | SPM1423 PDM |
| Display | None | SSD1306 OLED 128×64 | ST7789V2 TFT 135×240 | ST7789V2 TFT 135×240 (color) |
| Display library | — | U8g2 | TFT_eSPI | M5Unified/M5GFX |
| Battery reading | ADC on GPIO 1 | Not available | ADC on GPIO 38 | PY32 PMIC |
| Power management | Mic power gate | None | GPIO 4 power hold | PY32 PMIC |
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

From a terminal:
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
   ```
   pio run -e m5stick_s3 -t upload
   ```

> **Note:** The COM port number can change between reconnects. If upload fails
> with a port error, check Device Manager for the current port, or remove
> `upload_port` from `platformio.ini` and let PlatformIO auto-detect.

---

## Step 5: Usage

After flashing, the device enters deep sleep automatically on first boot.

- **Record**: Press and hold the front button (BtnA). The TFT shows
  "Recording..." with a red indicator. Release when done.
- **Auto-sync**: After recording, the device advertises via BLE for
  ~10 seconds. If a sync client connects, recordings transfer automatically.
- **Force sync**: Short-press the side button (BtnB) to start BLE
  advertising manually (useful to sync without making a new recording).
- **Oblique strategy**: Long-press BtnB (≥1 second) to display a random
  Oblique Strategy on the color TFT.
- **Status**: The device shows file count and free storage on the TFT
  when BLE advertising starts.
- **Deep sleep**: The device sleeps after sync is complete or after the
  BLE advertising window expires (~30 seconds).

---

## Board detection gotcha

The M5StickS3 uses an ESP32-S3-PICO-1 in an LGA56 package. M5Unified's
auto-detection can misidentify it as an AtomS3R, which disables the speaker
and PMIC peripherals.

The firmware sets `cfg.fallback_board = m5::board_t::board_M5StickS3` before
`M5.begin()` to ensure correct hardware initialization. This is handled
automatically — no manual configuration needed.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| "No serial port" on upload | Check that USB-C cable supports data (not charge-only). Try a different port. |
| COM port keeps changing | Remove `upload_port` from `platformio.ini` to auto-detect. |
| Display shows nothing | Normal on first boot — device enters deep sleep immediately. Press BtnA to record. |
| Recording too quiet | The firmware applies 8× software gain (`pdm_gain_shift = 3`). If still quiet, increase to 4 (16×) in the `BOARD_M5STICKS3` section of `main.cpp`. |
| Build error: M5Unified not found | Run `pio pkg install -e m5stick_s3` to force library download. |
