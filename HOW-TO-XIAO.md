# How to build Middle with the XIAO ESP32S3 Sense

This guide walks you through flashing the Middle firmware onto a **Seeed Studio
XIAO ESP32S3 Sense** (SS-102010635) mounted on a **Seeeduino XIAO Expansion
Board** (SS-103030356), powered by a **3.7V 500mAh Li-Po battery** (KW-1559).

No soldering is needed. Plug the XIAO Sense into the expansion board, connect
the battery to the XIAO's JST connector, and follow the steps below.

---

## Hardware overview

| Component | Role |
|---|---|
| XIAO ESP32S3 Sense | Microcontroller + built-in PDM microphone + BLE + Wi-Fi + USB-C |
| XIAO Expansion Board | User button (D1), OLED display (128x64 SSD1306), buzzer, SD slot, RTC |
| Li-Po 3.7V 500mAh | Battery, charges via USB-C through the XIAO's built-in charge circuit |

### Pin mapping (no extra wiring)

| Function | XIAO pin label | ESP32-S3 GPIO | Notes |
|---|---|---|---|
| Record button | D1 button on expansion board | GPIO 2 | Active LOW with internal pullup. Same GPIO as the original project. |
| PDM mic clock | (internal to Sense board) | GPIO 42 | Built-in digital microphone |
| PDM mic data | (internal to Sense board) | GPIO 41 | Built-in digital microphone |
| OLED SDA | D4 | GPIO 5 | SSD1306 on expansion board (I2C address 0x3C) |
| OLED SCL | D5 | GPIO 6 | SSD1306 on expansion board |

### What the original project uses (for reference)

| Function | Original GPIO | Your setup |
|---|---|---|
| Button | GPIO 2 | **Same** (D1 button) |
| Mic power gate | GPIO 10 | **Not needed** (built-in mic, always on) |
| I2S SCK | GPIO 6 | **Not used** (PDM mic uses GPIO 42/41 internally) |
| I2S WS | GPIO 5 | **Not used** |
| I2S SD | GPIO 13 | **Not used** |
| Battery ADC | GPIO 1 | **Not available** (no GPIO exposes battery voltage) |

---

## Step 1: Install software

You need two things on your Windows PC: **Visual Studio Code** and the
**PlatformIO** extension.

### Install VS Code

1. Download VS Code from https://code.visualstudio.com/
2. Run the installer, accept defaults.

### Install PlatformIO

1. Open VS Code.
2. Click the **Extensions** icon in the left sidebar (or press `Ctrl+Shift+X`).
3. Search for **PlatformIO IDE**.
4. Click **Install** on the one published by PlatformIO.
5. Wait for it to finish (it downloads the PlatformIO Core toolchain in the
   background — this can take a few minutes the first time).
6. Restart VS Code when prompted.

You should now see a small **alien head** icon (PlatformIO) in the left sidebar.

---

## Step 2: Open the project

1. In VS Code, go to **File → Open Folder**.
2. Navigate to the `middle` folder (the one containing `platformio.ini`).
3. Click **Select Folder**.
4. PlatformIO will detect the project and start downloading the ESP32
   platform and libraries. This takes a while on first run (several hundred MB
   of toolchains). Let it finish — watch the terminal at the bottom.

---

## Step 3: Build the firmware

1. Open the PlatformIO sidebar (alien head icon).
2. Under **PROJECT TASKS**, expand **xiao_esp32s3_sense**.
3. Click **Build**.
4. Wait for the build to complete. You should see `SUCCESS` in the terminal.

Alternatively, open the VS Code command palette (`Ctrl+Shift+P`), type
`PlatformIO: Build`, and select the `xiao_esp32s3_sense` environment.

Or from a terminal:
```
pio run -e xiao_esp32s3_sense
```

---

## Step 4: Connect and upload

1. Plug the XIAO ESP32S3 Sense into your PC via USB-C.
2. Wait for Windows to recognize it (it appears as a COM port).
3. In PlatformIO sidebar → **xiao_esp32s3_sense** → click **Upload**.
4. Or from a terminal:
   ```
   pio run -e xiao_esp32s3_sense -t upload
   ```

### If upload fails or the device is not recognized

The XIAO ESP32S3 sometimes needs to be put into **bootloader mode** manually:

1. **Hold** the BOOT button (tiny button on the XIAO board, labeled "B").
2. While holding BOOT, **press and release** the RESET button ("R").
3. Release BOOT.
4. The device should now appear as a USB drive or a new COM port.
5. Try uploading again.

---

## Step 5: Test it

1. After upload, press RESET on the XIAO or unplug/replug.
2. The device immediately enters deep sleep (normal — it only wakes on button
   press).
3. **Press and hold** the D1 button on the expansion board. The OLED should
   show "Recording...".
4. Speak, then release the button (hold for at least 1 second).
5. The OLED should briefly show "BLE Active" with a file count, then the
   device goes back to sleep.

### Serial monitor (debugging)

To see log output:
```
pio device monitor -e xiao_esp32s3_sense
```
Or in PlatformIO sidebar → **xiao_esp32s3_sense** → **Monitor**.

Baud rate is 115200 (already configured).

---

## Step 6: Sync with the Android app

1. Install the Middle Android app on your phone (build it from the `android/`
   folder with Android Studio, or have someone build you an APK).
2. Open the app. Grant Bluetooth and notification permissions when prompted.
3. Press the D1 button briefly on the pendant (< 1 second tap = triggers BLE
   advertising without recording).
4. The app should discover the pendant and sync any pending recordings.
5. First time: the app will "claim" the pendant via TOFU pairing. After that,
   only your phone can sync with it.

The Android app works **unchanged** — it communicates over BLE using the same
protocol regardless of the underlying hardware.

---

## What's different from the original hardware

| Aspect | Original | Your XIAO setup |
|---|---|---|
| Microphone | INMP441 (I2S, external) | Built-in PDM mic on Sense board |
| Audio interface | I2S standard mode, stereo 32-bit | I2S PDM RX mode, mono 16-bit |
| Mic power gating | GPIO 10 controls mic power | Not needed (built-in mic) |
| Battery voltage | ADC on GPIO 1 with voltage divider | Not available (reports 0 mV to app) |
| Display | None | OLED 128x64 on expansion board |
| Storage | LittleFS (~3 MB on-chip flash) | Same (LittleFS, ~3 MB) |
| Recording format | IMA ADPCM, 16 kHz, ~4 KB/s | Same |
| BLE protocol | Identical | Identical |
| Deep sleep | ext0 wakeup on GPIO 2 | Same (GPIO 2 = D1 button) |

---

## OLED display

The expansion board's 128x64 OLED shows status at key moments:

- **"Recording..."** — while the button is held down.
- **"BLE Active / N file(s)"** — advertising for a phone connection.
- **"Syncing... / N left"** — streaming a file to the phone.
- Display turns off before deep sleep (draws zero power when off).

The OLED is never on while the device is sleeping, so it has negligible impact
on battery life.

---

## About recording size and the SD card

The project uses **IMA ADPCM** compression, not raw WAV. The numbers:

| Duration | ADPCM size | Raw WAV 16-bit size |
|---|---|---|
| 1 minute | ~240 KB | ~1.9 MB |
| 10 minutes | ~2.4 MB | ~19 MB |
| 30 minutes | ~7.2 MB | ~58 MB |

The on-chip **LittleFS partition is ~3 MB**, enough for roughly **12 minutes**
of recording. Recordings are deleted from flash after syncing to your phone, so
unless you record a lot without syncing, this is plenty.

The **SD card slot** on the expansion board can be used for more storage, but
it requires code changes (SPI driver, file management) and draws additional
power. For most use cases, the built-in flash is sufficient. If you record many
hours between syncs, the SD card would be the upgrade path.

---

## Design limitations vs. the original pendant

The original Middle uses a minimal custom board with an ESP32-S3 module, an
external INMP441 mic, a coin cell holder, and nothing else. The XIAO Sense
setup trades simplicity-of-build for significantly higher power draw and larger
physical size.

### Power consumption comparison

| State | Original board | XIAO Sense (with Sense + Expansion) |
|---|---|---|
| Deep sleep | ~14 µA | **~3 mA** |
| BLE advertising | ~102 mA | ~102 mA (same ESP32-S3 radio) |
| Recording (mic active) | ~64 mA | ~64 mA (similar) |

The deep sleep difference is the main issue: **3 mA vs 14 µA is roughly 215x
more**. This is caused by the Sense daughter board's camera interface circuitry
(voltage regulators, level shifters) and other expansion board components
drawing quiescent current even when everything is "off".

### Battery life estimates (500 mAh Li-Po)

These are rough estimates based on the measured deep sleep current of 3 mA.

| Usage pattern | Estimated battery life |
|---|---|
| Pure standby (no recordings) | ~7 days |
| 30 min recording/day, rest in sleep | ~5–6 days |
| 2 hours recording/day | ~3–4 days |
| Heavy use (4+ hours recording/day) | ~1–2 days |
| Continuous recording | ~7–8 hours |

The original pendant with a CR2032 (220 mAh) can sleep for months because of
its 14 µA standby. The XIAO build cannot — it must be recharged regularly.
Think of it like charging a smartwatch.

### Physical size

- **Original pendant**: credit-card-sized or smaller custom PCB. Fits in a
  pendant case.
- **XIAO + Expansion Board**: roughly 43 × 36 × 18 mm (expansion board) plus
  the XIAO module on top. Noticeably bulkier. Works as a clip-on or a pocket
  device, but not as a thin pendant.

### No battery voltage monitoring

The XIAO ESP32S3 Sense does not expose the battery voltage on any ADC-readable
GPIO. The app will always show 0 mV. You will not get low-battery warnings.
Monitor usage time manually and charge when needed.

---

## Removing the camera module to save power

The XIAO ESP32S3 Sense ships with an OV5640 camera module connected via a flex
ribbon cable to the Sense daughter board (the board-to-board connector on the
back). The camera is not used by Middle at all.

### What removal means

You physically unclip and remove the **camera flex cable** from the B2B
connector on the Sense board. The camera module (small PCB with the lens) comes
off entirely. The Sense daughter board (the one with the PDM microphone) stays
attached to the XIAO.

### How to do it

1. Power off the device and disconnect the battery.
2. Locate the small plastic latch on the camera ribbon cable connector (on the
   back/bottom of the Sense board).
3. Gently flip the latch up with a fingernail or plastic spudger.
4. Slide the flex cable out.
5. Optionally, tape over the connector to prevent dust ingress.

No tools or soldering needed.

### Power savings

The camera module's standby (idle) current draw is approximately **1.3–1.5 mA**
for the OV5640 and its associated LDO regulators on the Sense board. Removing
the camera ribbon cable eliminates this standby draw because the LDOs on the
Sense board for the camera rail go unloaded/quiescent.

| State | With camera | Without camera (estimated) |
|---|---|---|
| Deep sleep | ~3 mA | **~1.5–1.7 mA** |

This is still far from the original board's 14 µA, because the Sense daughter
board's remaining circuitry (microphone bias, I2C level shifters, voltage
regulators for the digital interface) still draws current. But it roughly
doubles your standby battery life:

| Scenario | With camera | Without camera |
|---|---|---|
| Pure standby | ~7 days | ~12–14 days |
| Light use (30 min/day) | ~5–6 days | ~9–11 days |

**Recommendation**: Remove the camera. Middle does not use it, and you get a
meaningful battery life improvement for zero cost.

### What you lose

Nothing relevant to Middle. The camera is not initialized or referenced in the
firmware. If you later want to use the camera for another project, just
reconnect the ribbon cable.

---

## Troubleshooting

### Build fails with "Unknown board: seeed_xiao_esp32s3"

The pioarduino platform should include this board. If not:
1. Try `pio boards | findstr xiao` in a terminal to see available board names.
2. If the name differs (e.g. `xiaoesp32s3`), update `board = ...` in
   `platformio.ini` under `[env:xiao_esp32s3_sense]`.

### Upload hangs or fails

Enter bootloader mode: hold BOOT, press RESET, release BOOT, then retry upload.

### No sound in recordings

- Make sure you're using the **XIAO ESP32S3 Sense** (with the microphone
  daughter board attached), not the plain XIAO ESP32S3.
- The PDM mic has no power gating — it should always work when the I2S
  peripheral initializes.

### OLED shows nothing

- Verify the XIAO is fully seated in the expansion board.
- The I2C address is 0x3C (default for the expansion board's SSD1306). If your
  display uses 0x3D, the U8g2 constructor in `main.cpp` may need updating.

### Battery not charging

- The XIAO charges via USB-C. Connect the Li-Po to the JST-SH (1.25mm pitch)
  connector on the back/bottom of the XIAO board.
- There is no charge indicator LED on all XIAO variants. Charging happens
  silently when USB is connected.
