# How to build Middle with the M5StickC Plus2

This guide walks you through flashing the Middle firmware onto an
**M5StickC Plus2** (K016-P2).

No soldering or external wiring is needed. The M5StickC Plus2 has a built-in
PDM microphone, TFT display, buttons, and a 200 mAh battery all in one unit.

---

## Hardware overview

| Component | Role |
|---|---|
| M5StickC Plus2 | ESP32-PICO-V3-02 + built-in SPM1423 PDM mic + BLE + Wi-Fi + USB-C |
| 1.14" TFT LCD | ST7789V2 135×240 — shows recording/sync status |
| 200 mAh Li-Po | Internal battery, charges via USB-C |

### Pin mapping (internal, no extra wiring)

| Function | GPIO | Notes |
|---|---|---|
| Record button (BtnA) | GPIO 37 | Front button, active LOW with external pull-up |
| Side button (BtnB) | GPIO 39 | Short press: force sync; long press (≥1 s): oblique strategy |
| Power button (BtnPWR) | GPIO 35 | Short press: status screen; long press (≥2 s): power off |
| PDM mic clock | GPIO 0 | SPM1423, built-in |
| PDM mic data | GPIO 34 | SPM1423, built-in |
| Power hold | GPIO 4 | Must be held HIGH to keep device powered |
| TFT MOSI | GPIO 15 | ST7789V2, SPI |
| TFT SCLK | GPIO 13 | ST7789V2, SPI |
| TFT DC | GPIO 14 | ST7789V2, SPI |
| TFT RST | GPIO 12 | ST7789V2, SPI |
| TFT CS | GPIO 5 | ST7789V2, SPI |

### Key differences from other boards

| Feature | DevKit (ESP32-S3) | XIAO Sense | M5StickC Plus2 |
|---|---|---|---|
| MCU | ESP32-S3 | ESP32-S3 | ESP32-PICO-V3-02 |
| Microphone | INMP441 (I2S) | PDM (built-in) | SPM1423 PDM (built-in) |
| Display | None | SSD1306 OLED 128×64 | ST7789V2 TFT 135×240 |
| Battery reading | ADC on GPIO 1 | Not available | Not available |
| Power management | Mic power gate | None | GPIO 4 power hold |
| USB | Native USB CDC | Native USB CDC | CH9102 UART bridge |

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
2. PlatformIO will detect the project and download the ESP32 platform and
   libraries on first run.

---

## Step 3: Build the firmware

From a terminal:
```
pio run -e m5stick_c_plus2
```

Or use the PlatformIO sidebar → expand **m5stick_c_plus2** → click **Build**.

You should see `SUCCESS` in the terminal output.

---

## Step 4: Connect and upload

1. **Power on the M5StickC Plus2**: long-press the power button (right side)
   for 2+ seconds.
2. Plug the device into your PC via USB-C.
3. Wait for Windows to recognize the COM port (CH9102 driver may install
   automatically; if not, download from https://www.wch-ic.com/downloads/CH9102SER_EXE.html).
4. Upload:
   ```
   pio run -e m5stick_c_plus2 -t upload
   ```

---

## Step 5: Usage

After flashing, the device enters deep sleep automatically on first boot.

- **Record**: Press and hold the front button (BtnA). The TFT shows
  "Recording..." briefly (the screen auto-offs after 3 seconds to save power,
  but recording continues). Release when done.
- **Sync**: After releasing the button, if recordings exist, BLE advertising
  starts. The TFT shows "BLE Active" and the file count. Connect with the
  Android app or `sync.py` to transfer recordings. During transfer the screen
  shows sync progress ("X/Y files").
- **Force sync**: Short-press BtnB (side button) to manually start BLE
  advertising, even if no new recording was just made.
- **Oblique Strategy**: Long-press BtnB (≥1 second) to display a random
  Oblique Strategy by Brian Eno on the TFT (word-wrapped, stays on for 15 s).
- **Status check**: Short-press BtnPWR (right side) to see the number of
  stored recordings and free storage space.
- **Sleep**: The device returns to deep sleep after BLE sync completes or
  after a timeout with no connection.
- **Wake**: Press BtnA to record or sync. Press BtnPWR to see the status
  screen (device returns to deep sleep automatically).
- **Power off**: Press and hold BtnPWR for 2+ seconds. The TFT shows
  "Power Off" and the device shuts down.

---

## Troubleshooting

### COM port not recognized
- Try a different USB-C cable (some cables are charge-only).
- If using a USB-C to USB-C cable, the port may not be recognized. Disconnect,
  power off the device (long-press power button until green LED lights), then
  reconnect.
- Install the CH9102 driver manually if needed.

### Device powers off immediately
- The firmware must set GPIO 4 HIGH on startup to keep the device powered.
  Make sure you're building with the `m5stick_c_plus2` environment.

### Microphone too quiet / too loud
- Adjust `pdm_gain_shift` in `src/main.cpp`. The default is 2 (4× gain).
  Increase to 3 (8×) if recordings are too quiet.

### Display not working
- The TFT is configured via build flags in `platformio.ini`. Verify you're
  using the `m5stick_c_plus2` environment.
