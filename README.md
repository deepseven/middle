# Middle

Middle is a small, rechargeable, thought-recording pendant. It's inspired by the [Pebble
Index 01](https://repebble.com/index), but I wanted it now, so I made it.

It uses a microphone and an ESP32 to record your thoughts and transfer them to your
phone/computer/whatever for later processing.

There's an Android app and a Python script so you can transfer the files to the PC.

## Features

* Press a button to record, release to stop.
* Bluetooth syncing happens after a recording, or tap the button quickly to force a BT wakeup/sync.
* IMA ADPCM encoding at 16 kHz mono (~4 KB/s on flash).
* Supports long audio files (probably many minutes).
* Transferring files to the PC is pretty fast.
* Optionally transcribes the audio files using OpenAI's GPT 4o transcribe.
* Light on battery life, since it consumes no power when not in use.
* Display auto-off after 3 seconds to save power (on boards with a screen).
* Privacy-preserving, no connections anywhere, no nothing.

## Supported boards

| Board | MCU | Microphone | Display | Guide |
|---|---|---|---|---|
| ESP32-S3-DevKitC-1 | ESP32-S3 | INMP441 (external, I2S) | None | See below |
| XIAO ESP32S3 Sense | ESP32-S3 | Built-in PDM | SSD1306 OLED 128×64 (expansion board) | [HOW-TO-XIAO.md](HOW-TO-XIAO.md) |
| M5StickC Plus2 | ESP32-PICO-V3-02 | SPM1423 PDM (built-in) | ST7789V2 TFT 135×240 | [HOW-TO-M5STICKCPLUS2.md](HOW-TO-M5STICKCPLUS2.md) |

The XIAO Sense and M5StickC Plus2 require no soldering or external wiring.

## Hardware (DevKit)

I used:

* An ESP32 S3, in a micro board with a battery charge circuit.
* A MAX 9814 mic board with AGC I had lying around.
* A button.
* A small LiPo battery.

Hook them all up, the mic goes to pin 9, the button goes between pin 12 and GND.

Flash the firmware and use the provided Python script to transfer the files from the
pendant. Done.
