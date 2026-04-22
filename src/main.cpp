#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <LittleFS.h>
#ifdef BOARD_XIAO_SENSE
#include <driver/i2s_pdm.h>
#include <U8g2lib.h>
#include <Wire.h>
#elif defined(BOARD_M5STICKCPLUS2)
#include <driver/i2s_pdm.h>
#include <TFT_eSPI.h>
#elif defined(BOARD_M5STICKS3)
#include <driver/i2s_std.h>
#include <M5Unified.h>
#else
#include <driver/i2s_std.h>
#endif
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>
// NimBLE API for direct notification calls with congestion retry. The Arduino
// BLE wrapper calls ble_gatts_notify_custom but silently aborts on non-zero
// return (e.g. BLE_HS_ENOMEM when the mbuf pool is exhausted). We call it
// ourselves so we can retry instead of losing data.
//
// On ESP32 (original) the Arduino framework uses BlueDroid instead of NimBLE,
// so these headers are only available on NimBLE-enabled targets.
#if CONFIG_BT_NIMBLE_ENABLED
#include <host/ble_gatt.h>
#include <host/ble_hs_mbuf.h>
#endif
#include <nvs.h>
#include <nvs_flash.h>

#define DEBUG 1

#if DEBUG
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif

#ifdef BOARD_XIAO_SENSE
// XIAO ESP32S3 Sense pin mapping.
static const int pin_button = 2;   // D1 on XIAO expansion board
// PDM microphone pins (internal to Sense board).
static const int pin_pdm_clk = 42;
static const int pin_pdm_data = 41;
#elif defined(BOARD_M5STICKCPLUS2)
// M5StickC Plus2 pin mapping.
static const int pin_button = 37;       // BtnA (front button), active LOW
static const int pin_button_b = 39;     // BtnB (side button), active LOW
static const int pin_button_pwr = 35;   // Power button, active LOW
static const int pin_power_hold = 4;    // Hold HIGH to keep device powered
static const int pin_battery = 38;      // Battery voltage via 1:1 divider (ADC1)
// SPM1423 PDM microphone pins (built into device).
static const int pin_pdm_clk = 0;
static const int pin_pdm_data = 34;
#elif defined(BOARD_M5STICKS3)
// M5StickS3 pin mapping. Buttons are read via M5Unified (BtnA=GPIO11,
// BtnB=GPIO12) but we still need pin_button for ext0 deep sleep wakeup.
static const int pin_button = 11;       // BtnA (front button), for wakeup
// Mic I2S pins are managed by M5Unified (internal_mic=true).
#else
static const int pin_button = 2;
static const int pin_battery = 1;
static const int pin_mic_power = 10;

// INMP441 I2S pin assignments.
static const int pin_i2s_sck = 6;
static const int pin_i2s_ws = 5;
static const int pin_i2s_sd = 13;
#endif

// OLED display on the XIAO expansion board (SSD1306 128x64 I2C).
#ifdef BOARD_XIAO_SENSE
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
static bool oled_ready = false;
#endif

// TFT display on M5StickC Plus2 (ST7789V2 135x240 SPI).
#ifdef BOARD_M5STICKCPLUS2
static TFT_eSPI tft = TFT_eSPI();
static bool tft_ready = false;
#endif

// Display auto-off to save power. After any display update the screen stays
// on for display_timeout_ms then turns off automatically.
#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
static const unsigned long display_timeout_ms = 3000;
static unsigned long display_off_at_ms = 0;
static bool display_is_on = false;
#endif

// Dev mode: hold BtnB at boot to disable automatic sleep. Useful for
// iterative flashing during development.
#ifdef BOARD_M5STICKS3
static bool dev_mode = false;
#endif

// Forward declaration so display helpers can read battery voltage.
static uint16_t read_battery_millivolts();

// Battery voltage range for percentage calculation (millivolts).
// These depend on cell chemistry and board circuitry.
#if defined(BOARD_M5STICKCPLUS2)
static const uint16_t bat_mv_empty = 3300;  // 120 mAh LiPo, cut-off ~3.3 V
static const uint16_t bat_mv_full  = 4200;  // Fully charged Li-ion/LiPo
#elif defined(BOARD_M5STICKS3)
static const uint16_t bat_mv_empty = 3300;  // 120 mAh LiPo
static const uint16_t bat_mv_full  = 4200;
#elif !defined(BOARD_XIAO_SENSE)
static const uint16_t bat_mv_empty = 3300;  // DevKitC external Li-ion cell
static const uint16_t bat_mv_full  = 4200;
#endif

static const int sample_rate = 16000;
static const unsigned long minimum_recording_milliseconds = 1000;

// Samples to discard after I2S init to skip the INMP441's internal startup
// transient (~100ms at 16 kHz).
static const size_t i2s_startup_discard_samples = 1600;

#ifdef BOARD_XIAO_SENSE
// Software gain for the XIAO Sense's built-in PDM mic, which produces
// lower-amplitude samples than the INMP441.  Expressed as a left-shift
// count: 2 = 4x gain.  Increase to 3 (8x) if still too quiet.
static const int pdm_gain_shift = 3;
#elif defined(BOARD_M5STICKCPLUS2)
// Software gain for the M5StickC Plus2's SPM1423 PDM mic.
// 8x gain (shift 3).
static const int pdm_gain_shift = 3;
#elif defined(BOARD_M5STICKS3)
// Software gain for the M5StickS3 mic (via ES8311 codec).
// The codec's ADC gain is set to max (reg 0x17=0xFF), so less
// software gain may be needed. Start with 2 (4x).
static const int pdm_gain_shift = 2;
#endif

// IMA ADPCM step size table — indexed by step_index (0..88).
static const int16_t adpcm_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// Maps each encoded nibble to a step_index adjustment.
static const int8_t adpcm_index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                              -1, -1, -1, -1, 2, 4, 6, 8};

struct adpcm_state {
  int16_t predicted_sample;
  uint8_t step_index;
};

// Encode one 16-bit signed PCM sample into a 4-bit IMA ADPCM nibble.
static uint8_t adpcm_encode_sample(int16_t sample, adpcm_state &state) {
  int32_t difference = sample - state.predicted_sample;
  uint8_t nibble = 0;
  if (difference < 0) {
    nibble = 8;
    difference = -difference;
  }

  int16_t step = adpcm_step_table[state.step_index];
  // Quantize the difference against the current step size. Each bit in the
  // nibble represents whether the difference exceeds successively halved
  // fractions of the step.
  int32_t delta = step >> 3;
  if (difference >= step) {
    nibble |= 4;
    difference -= step;
    delta += step;
  }
  step >>= 1;
  if (difference >= step) {
    nibble |= 2;
    difference -= step;
    delta += step;
  }
  step >>= 1;
  if (difference >= step) {
    nibble |= 1;
    delta += step;
  }

  // Apply the reconstructed delta so the decoder stays in sync with us.
  if (nibble & 8) {
    state.predicted_sample -= delta;
  } else {
    state.predicted_sample += delta;
  }
  if (state.predicted_sample > 32767) {
    state.predicted_sample = 32767;
  } else if (state.predicted_sample < -32768) {
    state.predicted_sample = -32768;
  }

  int new_index = state.step_index + adpcm_index_table[nibble];
  if (new_index < 0) {
    new_index = 0;
  } else if (new_index > 88) {
    new_index = 88;
  }
  state.step_index = (uint8_t)new_index;

  return nibble;
}

// Lock-free single-producer single-consumer ring buffer for draining ADPCM
// output to LittleFS. The sampling loop (producer) and a separate flash-
// writer FreeRTOS task (consumer) run on different cores so flash page-erase
// stalls never block sample capture. At 16 kHz ADPCM (8 KB/s), 32 KB gives
// ~4 seconds of headroom to absorb worst-case LittleFS page-erase stalls.
static const size_t ring_buffer_capacity = 32768;
static uint8_t ring_buffer[ring_buffer_capacity];
static volatile size_t ring_buffer_head = 0; // read index (consumer)
static volatile size_t ring_buffer_tail = 0; // write index (producer)

static void ring_buffer_reset() {
  ring_buffer_head = 0;
  ring_buffer_tail = 0;
}

static void ring_buffer_push(uint8_t byte) {
  size_t next_tail = (ring_buffer_tail + 1) % ring_buffer_capacity;
  if (next_tail == ring_buffer_head) {
    // Buffer full — sample lost. Shouldn't happen with the writer task
    // draining continuously, but prevents corruption if it does.
    return;
  }
  ring_buffer[ring_buffer_tail] = byte;
  ring_buffer_tail = next_tail;
}

// Writer task state — offloads flash writes to core 0 so the sampling
// loop on core 1 never stalls on LittleFS page erases.
static volatile bool writer_active = false;
static volatile bool writer_error = false;
static volatile bool writer_done = false;

static void flash_writer_task(void *param) {
  File *file = (File *)param;
  while (writer_active || ring_buffer_head != ring_buffer_tail) {
    size_t h = ring_buffer_head;
    size_t t = ring_buffer_tail;
    if (h == t) {
      vTaskDelay(1);
      continue;
    }
    // Write the largest contiguous chunk available.
    size_t contiguous = (t > h) ? (t - h) : (ring_buffer_capacity - h);
    size_t written = file->write(&ring_buffer[h], contiguous);
    if (written != contiguous) {
      writer_error = true;
      break;
    }
    ring_buffer_head = (h + written) % ring_buffer_capacity;
  }
  writer_done = true;
  vTaskDelete(nullptr);
}

static const char *service_uuid = "19b10000-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_file_count_uuid =
    "19b10001-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_file_info_uuid =
    "19b10002-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_audio_data_uuid =
    "19b10003-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_command_uuid =
    "19b10004-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_voltage_uuid =
    "19b10005-e8f2-537e-4f6c-d104768a1214";
static const char *characteristic_pairing_uuid =
    "19b10006-e8f2-537e-4f6c-d104768a1214";

static const uint8_t command_request_next = 0x01;
static const uint8_t command_ack_received = 0x02;
static const uint8_t command_sync_done = 0x03;
static const uint8_t command_start_stream = 0x04;
static const uint8_t command_enter_bootloader = 0x05;
static const uint8_t command_skip_file = 0x06;
static const uint8_t command_unpair = 0x07;

static const unsigned long ble_keepalive_milliseconds = 10000;

static BLEServer *ble_server = nullptr;
static BLECharacteristic *file_count_characteristic = nullptr;
static BLECharacteristic *file_info_characteristic = nullptr;
static BLECharacteristic *audio_data_characteristic = nullptr;
static BLECharacteristic *voltage_characteristic = nullptr;
static BLECharacteristic *pairing_characteristic = nullptr;
static BLEAdvertising *ble_advertising = nullptr;

// Lock-free single-producer (BLE callback) single-consumer (loop()) command
// queue. Replaces the old single-byte pending_command which silently dropped
// commands when a new one arrived before the previous was processed.
static const size_t command_queue_capacity = 8;
static volatile uint8_t command_queue[command_queue_capacity];
static volatile size_t command_queue_head = 0;
static volatile size_t command_queue_tail = 0;

static void command_queue_push(uint8_t command) {
  size_t next_tail = (command_queue_tail + 1) % command_queue_capacity;
  if (next_tail == command_queue_head) {
    return;
  }
  command_queue[command_queue_tail] = command;
  command_queue_tail = next_tail;
}

// Returns the next command, or 0 if the queue is empty.
static uint8_t command_queue_pop() {
  if (command_queue_head == command_queue_tail) {
    return 0;
  }
  uint8_t command = command_queue[command_queue_head];
  command_queue_head = (command_queue_head + 1) % command_queue_capacity;
  return command;
}

static void command_queue_clear() {
  command_queue_head = 0;
  command_queue_tail = 0;
}

static volatile bool client_connected = false;
static volatile bool connection_authenticated = false;
static volatile uint16_t pending_recording_count = 0;
static uint16_t sync_total_files = 0;
static volatile bool sleep_requested = false;
static bool littlefs_ready = false;
static bool littlefs_mount_attempted = false;
static String current_stream_path = "";
static File pending_stream_file;
static unsigned long ble_active_until_milliseconds = 0;
static unsigned long hard_sleep_deadline_milliseconds = 0;

static String normalize_path(const char *name) {
  if (name == nullptr) {
    return "";
  }
  String path = String(name);
  if (path.startsWith("/")) {
    return path;
  }
  return String("/") + path;
}

#ifdef BOARD_XIAO_SENSE
static void oled_init() {
  Wire.begin();
  oled_ready = oled.begin();
  if (oled_ready) {
    oled.setFont(u8g2_font_6x10_tr);
  }
}

static void oled_show(const char *line1, const char *line2 = nullptr) {
  if (!oled_ready) return;
  if (!display_is_on) {
    oled.setPowerSave(0);
    display_is_on = true;
  }
  oled.clearBuffer();
  if (line1) oled.drawStr(0, 24, line1);
  if (line2) oled.drawStr(0, 48, line2);
  oled.sendBuffer();
  display_off_at_ms = millis() + display_timeout_ms;
}

static void oled_off() {
  if (!oled_ready) return;
  oled.clearBuffer();
  oled.sendBuffer();
  oled.setPowerSave(1);
  display_is_on = false;
}
#endif

#ifdef BOARD_M5STICKCPLUS2
static void tft_init() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);   // Backlight off during init to hide artifacts
  tft.init();
  tft.setRotation(3);  // Landscape: 240x135, M5 button on the left
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  // Don't turn backlight on yet — let tft_show() handle it when there
  // is actually content to display. Avoids a flash on cold boot.
  display_is_on = false;
  tft_ready = true;
}

// Draw a small battery percentage in the bottom-right corner of the TFT.
// Uses text size 1 (6x8 px) to stay unobtrusive.
static void tft_draw_battery() {
  uint16_t mv = read_battery_millivolts();
  if (mv == 0) return;
  int pct = (int)((mv - bat_mv_empty) * 100 / (bat_mv_full - bat_mv_empty));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  char bat[8];
  snprintf(bat, sizeof(bat), "%d%%", pct);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(bat, 240 - strlen(bat) * 6 - 4, 135 - 10);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void tft_show(const char *line1, const char *line2 = nullptr) {
  if (!tft_ready) return;
  if (!display_is_on) {
    tft.writecommand(0x11);  // SLPOUT
    delay(5);
    digitalWrite(TFT_BL, HIGH);
    display_is_on = true;
  }
  tft.fillScreen(TFT_BLACK);
  if (line1) tft.drawString(line1, 10, 30);
  if (line2) tft.drawString(line2, 10, 70);
  tft_draw_battery();
  display_off_at_ms = millis() + display_timeout_ms;
}

static void tft_off() {
  if (!tft_ready) return;
  tft.fillScreen(TFT_BLACK);
  digitalWrite(TFT_BL, LOW);
  tft.writecommand(0x10);  // ST7789 SLPIN (enter sleep mode)
  display_is_on = false;
}

// Word-wrapping text display for longer messages (e.g. oblique strategies).
// Renders text at the given text size, breaking at word boundaries.
static void tft_show_wrapped(const char *text, uint8_t size = 2) {
  if (!tft_ready) return;
  if (!display_is_on) {
    tft.writecommand(0x11);  // SLPOUT
    delay(5);
    digitalWrite(TFT_BL, HIGH);
    display_is_on = true;
  }
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(size);
  int char_w = 6 * size;
  int line_h = 8 * size + 2;
  int max_chars = (240 - 10) / char_w;
  int y = 10;
  const char *p = text;
  while (*p && y + line_h <= 135) {
    int len = strlen(p);
    int line_len = (len <= max_chars) ? len : max_chars;
    if (len > max_chars) {
      int last_space = -1;
      for (int i = 0; i < line_len; i++) {
        if (p[i] == ' ') last_space = i;
      }
      if (last_space > 0) line_len = last_space;
    }
    char line_buf[42];
    int copy_len = (line_len < (int)sizeof(line_buf) - 1) ? line_len : (int)sizeof(line_buf) - 1;
    memcpy(line_buf, p, copy_len);
    line_buf[copy_len] = '\0';
    tft.drawString(line_buf, 5, y);
    y += line_h;
    p += line_len;
    while (*p == ' ') p++;
  }
  tft.setTextSize(2);
  display_off_at_ms = millis() + display_timeout_ms;
}
#endif // BOARD_M5STICKCPLUS2 tft functions

// Oblique strategies array shared by M5StickC Plus2 and M5StickS3.
#if defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
static const char *const oblique_strategies[] = {
  "A line has two sides",
  "Abandon desire",
  "Abandon normal instructions",
  "Accept advice",
  "Adding on",
  "Always the first steps",
  "Ask people to work against their better judgement",
  "Ask your body",
  "Be dirty",
  "Be extravagant",
  "Be less critical",
  "Breathe more deeply",
  "Bridges -build -burn",
  "Change ambiguities to specifics",
  "Change nothing and continue consistently",
  "Change specifics to ambiguities",
  "Consider transitions",
  "Courage!",
  "Cut a vital connection",
  "Decorate",
  "Destroy nothing; Destroy the most important thing",
  "Discard an axiom",
  "Disciplined self-indulgence",
  "Discover your formulas and abandon them",
  "Display your talent",
  "Distort time",
  "Do nothing for as long as possible",
  "Do something boring",
  "Do something sudden",
  "Do the last thing first",
  "Do the words need changing?",
  "Don't avoid what is easy",
  "Don't break the silence",
  "Don't stress one thing more than another",
  "Emphasize differences",
  "Emphasize the flaws",
  "Faced with a choice, do both",
  "Find a safe part and use it as an anchor",
  "Give the game away",
  "Give way to your worst impulse",
  "Go outside. Shut the door.",
  "Go to an extreme, move back to a more comfortable place",
  "Honor thy error as a hidden intention",
  "How would someone else do it?",
  "How would you have done it?",
  "In total darkness, or in a very large room, very quietly",
  "Is it finished?",
  "Is something missing?",
  "Is the style right?",
  "It is simply a matter of work",
  "Just carry on",
  "Listen to the quiet voice",
  "Look at the order in which you do things",
  "Magnify the most difficult details",
  "Make it more sensual",
  "Make what's perfect more human",
  "Move towards the unimportant",
  "Not building a wall; making a brick",
  "Once the search has begun, something will be found",
  "Only a part, not the whole",
  "Only one element of each kind",
  "Openly resist change",
  "Question the heroic",
  "Remember quiet evenings",
  "Remove a restriction",
  "Repetition is a form of change",
  "Retrace your steps",
  "Reverse",
  "Simple subtraction",
  "Slow preparation, fast execution",
  "State the problem as clearly as possible",
  "Take a break",
  "Take away the important parts",
  "The inconsistency principle",
  "The most easily forgotten thing is the most important",
  "Think - inside the work - outside the work",
  "Tidy up",
  "Try faking it",
  "Turn it upside down",
  "Use an old idea",
  "Use cliches",
  "Use filters",
  "Use something nearby as a model",
  "Use your own ideas",
  "Voice your suspicions",
  "Water",
  "What context would look right?",
  "What is the simplest solution?",
  "What mistakes did you make last time?",
  "What to increase? What to reduce?",
  "What were you really thinking about just now?",
  "What would your closest friend do?",
  "What wouldn't you do?",
  "When is it for?",
  "Where is the edge?",
  "Which parts can be grouped?",
  "Work at a different speed",
  "Would anyone want it?",
  "Your mistake was a hidden intention",
};
static const int oblique_strategy_count =
    sizeof(oblique_strategies) / sizeof(oblique_strategies[0]);

#ifdef BOARD_M5STICKCPLUS2
static void show_oblique_strategy() {
  uint32_t index = esp_random() % oblique_strategy_count;
  tft_show_wrapped(oblique_strategies[index]);
  display_off_at_ms = millis() + 15000;
}
#endif // BOARD_M5STICKCPLUS2 show_oblique_strategy
#endif // shared oblique strategies

#ifdef BOARD_M5STICKS3
// --- M5StickS3 display via M5Unified ---
// Color theme: use the color TFT for visual state differentiation.
static const uint32_t COLOR_BG         = 0x000000u;  // Black
static const uint32_t COLOR_TEXT       = 0xFFFFFFu;  // White
static const uint32_t COLOR_RECORDING  = 0xFF3333u;  // Red
static const uint32_t COLOR_SYNCING    = 0x3399FFu;  // Blue
static const uint32_t COLOR_SYNCED     = 0x33CC66u;  // Green
static const uint32_t COLOR_STATUS     = 0xFFCC00u;  // Amber
static const uint32_t COLOR_DIM        = 0x666666u;  // Grey

static bool m5_display_ready = false;

static void m5_display_init() {
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickS3;
  cfg.internal_mic = true;   // Let M5Unified handle ES8311 codec + I2S (HAL clock magic)
  cfg.internal_spk = false;  // Don't init speaker — it interferes with mic codec config
  M5.begin(cfg);
  // Power on the ES8311 codec via the PY32 PMIC.  M5Unified's mic callback
  // does NOT touch PMIC (only the speaker callback does), so we must set
  // bit 3 of register 0x11 ourselves.
  // IMPORTANT: M5.begin() with internal_mic=true already called the mic
  // callback which wrote ES8311 registers — but the codec was unpowered at
  // that point, so those writes were lost.  We must power the PMIC first,
  // then restart the mic so the callback re-writes the codec registers to
  // a now-powered ES8311.
  M5.In_I2C.bitOn(0x6E, 0x11, 0b00001000, 100000);
  delay(50);  // let ES8311 power domain stabilize
  // ES8311 codec is shared between mic and speaker — the official tutorial
  // requires Speaker.end() before Mic.begin() to avoid codec conflicts.
  M5.Speaker.end();
  M5.Mic.end();
  // Reduce M5Unified mic gain: default magnification=16 with over_sampling=2
  // gives 4x internal gain.  The ES8311 ADC is already at +32dB (reg 0x17=0xFF).
  // Set magnification=1 so M5Unified applies ~0.25x, and let our
  // pdm_gain_shift handle the rest.
  {
    auto mic_cfg = M5.Mic.config();
    mic_cfg.magnification = 1;
    M5.Mic.config(mic_cfg);
  }
  M5.Mic.begin();
  M5.Display.setRotation(1);  // Landscape: 240x135
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2);
  display_is_on = false;
  m5_display_ready = true;
}

static uint16_t cached_battery_mv = 0;

static void m5_draw_battery() {
  uint16_t mv = read_battery_millivolts();
  if (mv != 0) {
    cached_battery_mv = mv;
  } else {
    Serial.printf("[display] battery I2C read returned 0, using cached %u mV\r\n", cached_battery_mv);
    mv = cached_battery_mv;
  }
  if (mv == 0) return;
  int pct = (int)((mv - bat_mv_empty) * 100 / (bat_mv_full - bat_mv_empty));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  char bat[8];
  snprintf(bat, sizeof(bat), "%d%%", pct);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_DIM, COLOR_BG);
  M5.Display.drawString(bat, 240 - strlen(bat) * 6 - 4, 135 - 10);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
}

static void m5_display_show(const char *line1, const char *line2 = nullptr,
                            uint32_t accent_color = COLOR_TEXT) {
  if (!m5_display_ready) return;
  if (!display_is_on) {
    M5.Display.wakeup();
    M5.Display.setBrightness(80);
    display_is_on = true;
  }
  M5.Display.fillScreen(TFT_BLACK);
  // Draw a small colored accent bar at the top to indicate state.
  M5.Display.fillRect(0, 0, 240, 4, accent_color);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
  if (line1) M5.Display.drawString(line1, 10, 30);
  if (line2) M5.Display.drawString(line2, 10, 70);
  m5_draw_battery();
  display_off_at_ms = millis() + display_timeout_ms;
}

static void m5_display_off() {
  if (!m5_display_ready) return;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.sleep();
  M5.Display.setBrightness(0);
  display_is_on = false;
}

// Word-wrapping text display for longer messages (oblique strategies).
static void m5_display_show_wrapped(const char *text, uint8_t size = 2) {
  if (!m5_display_ready) return;
  if (!display_is_on) {
    M5.Display.wakeup();
    M5.Display.setBrightness(80);
    display_is_on = true;
  }
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(COLOR_STATUS, COLOR_BG);
  int char_w = 6 * size;
  int line_h = 8 * size + 2;
  int max_chars = (240 - 10) / char_w;
  int y = 10;
  const char *p = text;
  while (*p && y + line_h <= 135) {
    int len = strlen(p);
    int line_len = (len <= max_chars) ? len : max_chars;
    if (len > max_chars) {
      int last_space = -1;
      for (int i = 0; i < line_len; i++) {
        if (p[i] == ' ') last_space = i;
      }
      if (last_space > 0) line_len = last_space;
    }
    char line_buf[42];
    int copy_len = (line_len < (int)sizeof(line_buf) - 1) ? line_len : (int)sizeof(line_buf) - 1;
    memcpy(line_buf, p, copy_len);
    line_buf[copy_len] = '\0';
    M5.Display.drawString(line_buf, 5, y);
    y += line_h;
    p += line_len;
    while (*p == ' ') p++;
  }
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
  display_off_at_ms = millis() + display_timeout_ms;
}

static void show_oblique_strategy() {
  uint32_t index = esp_random() % oblique_strategy_count;
  m5_display_show_wrapped(oblique_strategies[index]);
  display_off_at_ms = millis() + 15000;
}
#endif

// Turn off the display if the auto-off timer has expired.
#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
static void display_check_timeout() {
  if (display_is_on && (long)(millis() - display_off_at_ms) >= 0) {
#ifdef BOARD_XIAO_SENSE
    oled_off();
#elif defined(BOARD_M5STICKS3)
    m5_display_off();
#else
    tft_off();
#endif
  }
}

#endif

static void set_status_led_off() {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
#endif

#if defined(PIN_NEOPIXEL)
  pinMode(PIN_NEOPIXEL, OUTPUT);
  digitalWrite(PIN_NEOPIXEL, LOW);
#endif
}

static bool ble_window_active() {
  return (long)(ble_active_until_milliseconds - millis()) > 0;
}

#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
static uint32_t free_storage_kb();
#endif

static void start_ble_advertising() {
  if (ble_advertising == nullptr) {
    return;
  }
  ble_active_until_milliseconds = millis() + ble_keepalive_milliseconds;
  hard_sleep_deadline_milliseconds = millis() + 30000;
  if (!ble_advertising->start()) {
    DBG("[ble] advertising start failed\r\n");
  }
#ifdef BOARD_XIAO_SENSE
  char buf[22];
  snprintf(buf, sizeof(buf), "%u files %luKB",
           (unsigned)pending_recording_count, (unsigned long)free_storage_kb());
  oled_show("BLE Active", buf);
#elif defined(BOARD_M5STICKCPLUS2)
  char buf[22];
  snprintf(buf, sizeof(buf), "%u files %luKB",
           (unsigned)pending_recording_count, (unsigned long)free_storage_kb());
  tft_show("BLE Active", buf);
#elif defined(BOARD_M5STICKS3)
  char buf[22];
  snprintf(buf, sizeof(buf), "%u files %luKB",
           (unsigned)pending_recording_count, (unsigned long)free_storage_kb());
  m5_display_show("BLE Active", buf, COLOR_SYNCING);
#endif
  sync_total_files = pending_recording_count;
}

// Restarts advertising after a disconnect without touching either sleep timer.
// Only start_ble_advertising() (called on initial BLE bring-up) is allowed to
// set the deadlines; unauthenticated connect/disconnect cycles must not extend
// them.
static void resume_ble_advertising() {
  if (ble_advertising == nullptr) {
    return;
  }
  if (!ble_advertising->start()) {
    DBG("[ble] advertising resume failed\r\n");
  }
}

static void configure_button_wakeup() {
#ifdef BOARD_M5STICKCPLUS2
  // ext0 for BtnA (record), ext1 for BtnPWR (status check).
  // ESP32 ext1 only supports ALL_LOW, but with a single-pin mask that
  // behaves like "wake when this pin goes LOW".
  esp_sleep_enable_ext0_wakeup((gpio_num_t)pin_button, 0);
  esp_sleep_enable_ext1_wakeup(1ULL << pin_button_pwr, ESP_EXT1_WAKEUP_ALL_LOW);
#elif defined(BOARD_M5STICKS3)
  // BtnA on GPIO 11 is an RTC GPIO — use ext0 wakeup with active-LOW.
  esp_sleep_enable_ext0_wakeup((gpio_num_t)pin_button, 0);
  rtc_gpio_pullup_en((gpio_num_t)pin_button);
  rtc_gpio_pulldown_dis((gpio_num_t)pin_button);
#else
  esp_sleep_enable_ext0_wakeup((gpio_num_t)pin_button, 0);
#ifndef BOARD_XIAO_SENSE
  rtc_gpio_pullup_en((gpio_num_t)pin_button);
  rtc_gpio_pulldown_dis((gpio_num_t)pin_button);
#endif
#endif
}

static void enter_deep_sleep() {
  set_status_led_off();
#ifdef BOARD_XIAO_SENSE
  oled_off();
#elif defined(BOARD_M5STICKCPLUS2)
  tft_off();
#elif defined(BOARD_M5STICKS3)
  m5_display_off();
#endif
  if (ble_advertising != nullptr) {
    ble_advertising->stop();
  }
  delay(20);
#ifdef BOARD_M5STICKCPLUS2
  // Keep the power hold pin HIGH during deep sleep so the device stays on.
  // Hold TFT backlight LOW so it cannot float HIGH and drain the battery.
  // Hold PDM clock LOW so the mic's internal pull-up on GPIO 0 does not
  // keep it clocked during sleep.
  gpio_hold_en((gpio_num_t)pin_power_hold);
  gpio_hold_en((gpio_num_t)TFT_BL);
  gpio_hold_en((gpio_num_t)pin_pdm_clk);
  gpio_deep_sleep_hold_en();
#elif defined(BOARD_M5STICKS3)
  // Power down the ES8311 codec via PMIC before sleeping.
  M5.Mic.end();
  M5.In_I2C.bitOff(0x6E, 0x11, 0b00001000, 100000);
  // Power off the PMIC to ensure the green LED turns off. The device wakes
  // via the hardware power button (not BtnA). powerOff() internally calls
  // esp_deep_sleep_start() so code after it is unreachable.
  Serial.printf("[sleep] PMIC power off\r\n");
  M5.Power.powerOff();
#elif !defined(BOARD_XIAO_SENSE)
  // Hold the mic power pin LOW during deep sleep so the INMP441's internal
  // pull-up cannot draw current while the chip is sleeping.
  gpio_hold_en((gpio_num_t)pin_mic_power);
  gpio_deep_sleep_hold_en();
#endif
  esp_deep_sleep_start();
}

static bool ensure_littlefs_ready() {
  if (littlefs_ready) {
    return true;
  }
  if (littlefs_mount_attempted) {
    return false;
  }

  littlefs_mount_attempted = true;
  littlefs_ready = LittleFS.begin(false);
  if (!littlefs_ready) {
    littlefs_ready = LittleFS.begin(true);
  }
  return littlefs_ready;
}

// Extracts the numeric ID from a recording filename like "rec_000012.ima".
// Returns -1 if the name doesn't match the expected pattern.
static long parse_recording_id(const String &name) {
  String stripped = name;
  if (stripped.startsWith("/")) {
    stripped = stripped.substring(1);
  }
  if (!stripped.startsWith("rec_")) {
    return -1;
  }
  int dot = stripped.indexOf('.');
  if (dot < 0) {
    return -1;
  }
  String suffix = stripped.substring(dot);
  if (suffix != ".ima" && suffix != ".raw") {
    return -1;
  }
  String id_str = stripped.substring(4, dot);
  if (id_str.length() == 0) {
    return -1;
  }
  return id_str.toInt();
}

// Returns the next available recording ID by scanning existing filenames.
static long next_recording_id() {
  if (!ensure_littlefs_ready()) {
    return 1;
  }

  long max_id = 0;
  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    long id = parse_recording_id(String(entry.name()));
    if (id > max_id) {
      max_id = id;
    }
    entry = root.openNextFile();
  }
  return max_id + 1;
}

// Remove any 0-byte recording files left behind by failed writes or
// filesystem corruption. These zombie files can't be streamed or deleted
// during normal sync, so they cause the transfer loop to repeat forever.
static void remove_empty_recordings() {
  if (!ensure_littlefs_ready()) {
    return;
  }

  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  // Collect paths first — modifying the filesystem while iterating is unsafe.
  String to_remove[32];
  int remove_count = 0;
  while (entry && remove_count < 32) {
    String name = String(entry.name());
    if (parse_recording_id(name) >= 0 && entry.size() == 0) {
      to_remove[remove_count++] = normalize_path(entry.name());
    }
    entry = root.openNextFile();
  }

  for (int i = 0; i < remove_count; i++) {
    LittleFS.remove(to_remove[i]);
  }
}

static int count_recordings() {
  if (!ensure_littlefs_ready()) {
    return 0;
  }

  int count = 0;
  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    if (parse_recording_id(String(entry.name())) >= 0) {
      count++;
    }
    entry = root.openNextFile();
  }
  return count;
}

#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
static uint32_t free_storage_kb() {
  if (!ensure_littlefs_ready()) return 0;
  return (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;
}

static void show_status_screen() {
  if (!ensure_littlefs_ready()) return;
  int files = count_recordings();
  uint32_t free_kb = free_storage_kb();
  char line1[22], line2[22];
  snprintf(line1, sizeof(line1), "%d file(s)", files);
  snprintf(line2, sizeof(line2), "%lu KB free", (unsigned long)free_kb);
#ifdef BOARD_XIAO_SENSE
  oled_show(line1, line2);
#elif defined(BOARD_M5STICKS3)
  m5_display_show(line1, line2, COLOR_STATUS);
#else
  tft_show(line1, line2);
#endif
}
#endif

// Returns the path of the oldest recording (lowest numeric ID).
static String next_recording_path() {
  if (!ensure_littlefs_ready()) {
    return "";
  }

  long lowest_id = -1;
  String lowest_path = "";

  File root = LittleFS.open("/");
  File entry = root.openNextFile();
  while (entry) {
    String name = String(entry.name());
    long id = parse_recording_id(name);
    if (id >= 0 && (lowest_id < 0 || id < lowest_id)) {
      lowest_id = id;
      lowest_path = normalize_path(entry.name());
    }
    entry = root.openNextFile();
  }
  return lowest_path;
}

static void update_file_count() {
  uint16_t file_count = (uint16_t)count_recordings();
  pending_recording_count = file_count;
  if (file_count_characteristic != nullptr) {
    file_count_characteristic->setValue(file_count);
  }
}

// Buffer size for each i2s_channel_read() call. In stereo mode each frame
// contains a left and a right 32-bit sample, so 512 frames = 1024 int32_t
// values and yields 256 usable mono samples (~16ms at 16 kHz).
static const size_t i2s_read_frames = 512;

static i2s_chan_handle_t i2s_rx_channel = nullptr;

#if defined(BOARD_M5STICKS3)
// M5StickS3: mic is managed by M5Unified (internal_mic=true).
// Mic stays alive from m5_display_init() — M5.Mic.begin() after end()
// fails to reinit the ES8311 codec.  i2s_init just flushes stale DMA data.
static bool i2s_init() {
  // M5.Mic.record() is async: it submits a request to the mic task and
  // returns immediately while DMA fills the buffer in the background.
  // The warmup buffer MUST be static — a stack buffer would be freed
  // before the mic task finishes writing, corrupting the stack.
  static int16_t warmup[256];
  if (!M5.Mic.isEnabled()) {
    DBG("[rec] mic not enabled, attempting begin()\r\n");
    M5.In_I2C.bitOn(0x6E, 0x11, 0b00001000, 100000);
    delay(50);
    M5.Speaker.end();
    auto mic_cfg = M5.Mic.config();
    mic_cfg.magnification = 1;
    M5.Mic.config(mic_cfg);
    if (!M5.Mic.begin()) {
      DBG("[rec] M5.Mic.begin() failed\r\n");
      return false;
    }
    M5.Mic.record(warmup, 256, sample_rate, false);
    delay(500);
    for (int i = 0; i < 4; i++) {
      M5.Mic.record(warmup, 256, sample_rate, false);
    }
  } else {
    // Mic already running — just flush stale DMA data.
    for (int i = 0; i < 4; i++) {
      M5.Mic.record(warmup, 256, sample_rate, false);
    }
  }
  // Wait for the last async record() to complete before returning,
  // otherwise the mic task continues writing to warmup in the background.
  while (M5.Mic.isRecording()) { delay(1); }
  DBG("[rec] M5Unified mic ready\r\n");
  return true;
}
#elif defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2)
static bool i2s_init() {
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
      I2S_NUM_AUTO, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel_config, nullptr, &i2s_rx_channel) != ESP_OK) {
    DBG("[rec] i2s_new_channel failed\r\n");
    return false;
  }

  i2s_pdm_rx_config_t pdm_config = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
      .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .clk = (gpio_num_t)pin_pdm_clk,
          .din = (gpio_num_t)pin_pdm_data,
          .invert_flags = {.clk_inv = false},
      },
  };

  if (i2s_channel_init_pdm_rx_mode(i2s_rx_channel, &pdm_config) != ESP_OK) {
    DBG("[rec] i2s_channel_init_pdm_rx_mode failed\r\n");
    i2s_del_channel(i2s_rx_channel);
    i2s_rx_channel = nullptr;
    return false;
  }

  if (i2s_channel_enable(i2s_rx_channel) != ESP_OK) {
    DBG("[rec] i2s_channel_enable failed\r\n");
    i2s_del_channel(i2s_rx_channel);
    i2s_rx_channel = nullptr;
    return false;
  }

  return true;
}
#else
static bool i2s_init() {
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
      I2S_NUM_AUTO, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel_config, nullptr, &i2s_rx_channel) != ESP_OK) {
    DBG("[rec] i2s_new_channel failed\r\n");
    return false;
  }

  i2s_std_config_t std_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)pin_i2s_sck,
          .ws   = (gpio_num_t)pin_i2s_ws,
          .dout = I2S_GPIO_UNUSED,
          .din  = (gpio_num_t)pin_i2s_sd,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv   = false,
          },
      },
  };

  if (i2s_channel_init_std_mode(i2s_rx_channel, &std_config) != ESP_OK) {
    DBG("[rec] i2s_channel_init_std_mode failed\r\n");
    i2s_del_channel(i2s_rx_channel);
    i2s_rx_channel = nullptr;
    return false;
  }

  if (i2s_channel_enable(i2s_rx_channel) != ESP_OK) {
    DBG("[rec] i2s_channel_enable failed\r\n");
    i2s_del_channel(i2s_rx_channel);
    i2s_rx_channel = nullptr;
    return false;
  }

  return true;
}
#endif

static void i2s_deinit() {
#if defined(BOARD_M5STICKS3)
  // Keep mic alive — M5.Mic.begin() after end() fails to reinit the
  // ES8311 codec, producing all-zero samples on subsequent recordings.
  DBG("[rec] mic kept alive (M5Unified)\r\n");
#else
  if (i2s_rx_channel != nullptr) {
    i2s_channel_disable(i2s_rx_channel);
    i2s_del_channel(i2s_rx_channel);
    i2s_rx_channel = nullptr;
  }
#endif
}

static bool record_and_save() {
  bool recording_saved = false;

#ifdef BOARD_XIAO_SENSE
  oled_show("Recording...");
#elif defined(BOARD_M5STICKCPLUS2)
  tft_show("Recording...");
#elif defined(BOARD_M5STICKS3)
  m5_display_show("Recording...", nullptr, COLOR_RECORDING);
#else
  digitalWrite(pin_mic_power, HIGH);
#endif

  if (!i2s_init()) {
#if !defined(BOARD_XIAO_SENSE) && !defined(BOARD_M5STICKCPLUS2) && !defined(BOARD_M5STICKS3)
    digitalWrite(pin_mic_power, LOW);
#endif
    return false;
  }

  do {
    if (!ensure_littlefs_ready()) {
      break;
    }

    char filename[40];
    snprintf(filename, sizeof(filename), "/rec_%06ld.ima", next_recording_id());

    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
      break;
    }

    // Reserve space for the sample count header — we'll fill it in after
    // recording finishes, once we know the actual count.
    uint32_t placeholder = 0;
    file.write((uint8_t *)&placeholder, sizeof(placeholder));

    ring_buffer_reset();
    adpcm_state encoder_state = {0, 0};

    // Start the flash writer on core 0 so page-erase stalls never block
    // the sampling loop running here on core 1.
    writer_active = true;
    writer_error = false;
    writer_done = false;
    TaskHandle_t writer_handle = nullptr;
    if (xTaskCreatePinnedToCore(flash_writer_task, "flash_wr", 4096, &file,
                                1, &writer_handle, 0) != pdPASS) {
      file.close();
      LittleFS.remove(filename);
      break;
    }

#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
    // PDM / ES8311 codec modes produce 16-bit mono samples directly.
    static int16_t i2s_buf[i2s_read_frames];
#else
    // In stereo mode each frame has two 32-bit slots (left + right).
    // The INMP441 outputs 24-bit audio left-justified in the left slot;
    // >> 16 yields a signed 16-bit sample.
    static int32_t i2s_buf[i2s_read_frames * 2];
#endif
    size_t bytes_read = 0;

    // Discard the first ~100ms of samples to skip the microphone startup
    // transient.
    size_t discarded = 0;
    while (discarded < i2s_startup_discard_samples) {
#if defined(BOARD_M5STICKS3)
      if (!M5.Mic.record(i2s_buf, i2s_read_frames, sample_rate, false)) break;
      bytes_read = i2s_read_frames * sizeof(int16_t);
#else
      esp_err_t err = i2s_channel_read(i2s_rx_channel, i2s_buf, sizeof(i2s_buf),
                                       &bytes_read, portMAX_DELAY);
      if (err != ESP_OK) {
        DBG("[rec] i2s_channel_read error %d in discard loop\r\n", err);
        break;
      }
#endif
#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
      discarded += bytes_read / sizeof(int16_t);
#else
      discarded += bytes_read / sizeof(int32_t) / 2;
#endif
    }

    // Debug: log first few samples after discard to diagnose silence.
#if DEBUG && (defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3))
    {
#if defined(BOARD_M5STICKS3)
      M5.Mic.record(i2s_buf, i2s_read_frames, sample_rate, false);
      bytes_read = i2s_read_frames * sizeof(int16_t);
#else
      esp_err_t err = i2s_channel_read(i2s_rx_channel, i2s_buf, sizeof(i2s_buf),
                                       &bytes_read, portMAX_DELAY);
#endif
      size_t n = bytes_read / sizeof(int16_t);
      int16_t mn = 0, mx = 0;
      for (size_t i = 0; i < n; i++) {
        if (i2s_buf[i] < mn) mn = i2s_buf[i];
        if (i2s_buf[i] > mx) mx = i2s_buf[i];
      }
      DBG("[rec] first %u samples: min=%d max=%d [0]=%d [1]=%d [2]=%d [3]=%d\r\n",
          (unsigned)n, mn, mx, n>0?i2s_buf[0]:0, n>1?i2s_buf[1]:0,
          n>2?i2s_buf[2]:0, n>3?i2s_buf[3]:0);
    }
#endif

    unsigned long record_start_milliseconds = millis();
    uint32_t sample_count = 0;
    // Tracks whether we're holding an incomplete byte (the low nibble has
    // been written but the high nibble hasn't arrived yet).
    bool nibble_pending = false;
    uint8_t packed_byte = 0;

#if defined(BOARD_M5STICKS3)
    // M5Unified manages the button GPIO; raw digitalRead is unreliable.
    // Poll via M5.BtnA.isPressed() instead, refreshing state each iteration.
    M5.update();
    while (M5.BtnA.isPressed() && !writer_error) {
#else
    while (digitalRead(pin_button) == LOW && !writer_error) {
#endif
#if defined(BOARD_M5STICKS3)
      if (!M5.Mic.record(i2s_buf, i2s_read_frames, sample_rate, false)) {
        DBG("[rec] M5.Mic.record() failed\r\n");
        break;
      }
      bytes_read = i2s_read_frames * sizeof(int16_t);
#else
      esp_err_t err = i2s_channel_read(i2s_rx_channel, i2s_buf,
                                       sizeof(i2s_buf), &bytes_read,
                                       portMAX_DELAY);
      if (err != ESP_OK) {
        DBG("[rec] i2s_channel_read error %d\r\n", err);
        break;
      }
#endif

#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
      size_t total_samples = bytes_read / sizeof(int16_t);
      for (size_t i = 0; i < total_samples; i++) {
        int32_t amplified = (int32_t)i2s_buf[i] << pdm_gain_shift;
        if (amplified > 32767) amplified = 32767;
        else if (amplified < -32768) amplified = -32768;
        int16_t sample_16 = (int16_t)amplified;
#else
      size_t total_samples = bytes_read / sizeof(int32_t);
      for (size_t i = 0; i < total_samples; i += 2) {
        // Stereo interleave: even indices are left channel (INMP441 data).
        int16_t sample_16 = (int16_t)(i2s_buf[i] >> 16);
#endif
        uint8_t nibble = adpcm_encode_sample(sample_16, encoder_state);
        sample_count++;

        // Pack two nibbles per byte, low nibble first.
        if (!nibble_pending) {
          packed_byte = nibble & 0x0F;
          nibble_pending = true;
        } else {
          packed_byte |= (nibble << 4);
          ring_buffer_push(packed_byte);
          nibble_pending = false;
        }
      }
#if defined(BOARD_M5STICKS3)
      M5.update();
#endif
    }

    // Flush the trailing nibble if the sample count was odd.
    if (nibble_pending) {
      ring_buffer_push(packed_byte);
    }

    // Signal the writer task to drain remaining data and wait for it.
    writer_active = false;
    while (!writer_done) {
      delay(1);
    }

    unsigned long duration_milliseconds = millis() - record_start_milliseconds;
    if (duration_milliseconds < minimum_recording_milliseconds || writer_error) {
      file.close();
      LittleFS.remove(filename);
      break;
    }

    // Seek back and write the actual sample count into the header.
    file.seek(0);
    file.write((uint8_t *)&sample_count, sizeof(sample_count));
    file.close();

    recording_saved = true;
    update_file_count();
  } while (false);

  i2s_deinit();
#if !defined(BOARD_XIAO_SENSE) && !defined(BOARD_M5STICKCPLUS2) && !defined(BOARD_M5STICKS3)
  digitalWrite(pin_mic_power, LOW);
#endif
  return recording_saved;
}

// Send a BLE notification via NimBLE's ble_gatts_notify_custom(), retrying
// when the call fails due to mbuf pool exhaustion (BLE_HS_ENOMEM) or other
// transient congestion. The Arduino BLE wrapper also calls this function
// internally, but on any non-zero return it aborts the entire transfer —
// which caused ~70% of file data to be silently lost during streaming.
//
// On BlueDroid (ESP32 original), the Arduino wrapper handles congestion via
// an internal semaphore, so we fall back to the standard notify() call.
#if CONFIG_BT_NIMBLE_ENABLED
static bool send_notification(uint16_t connection_id, uint16_t attribute_handle,
                              uint8_t *data, int length) {
  for (int attempt = 0; attempt < 200; attempt++) {
    // ble_gatts_notify_custom consumes the mbuf regardless of success or
    // failure, so we must allocate a fresh one on every attempt.
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (om == nullptr) {
      delay(5);
      continue;
    }

    int rc = ble_gatts_notify_custom(connection_id, attribute_handle, om);
    if (rc == 0) {
      return true;
    }
    // Non-zero means congestion (BLE_HS_ENOMEM = 6, BLE_HS_EBUSY = 15, etc).
    // Wait briefly for the BLE stack to drain and retry.
    delay(5);
  }
  return false;
}
#else
static bool send_notification(uint16_t connection_id, uint16_t attribute_handle,
                              uint8_t *data, int length) {
  (void)connection_id;
  (void)attribute_handle;
  audio_data_characteristic->setValue(data, length);
  audio_data_characteristic->notify();
  // BlueDroid's notify() silently drops data when the TX queue is full.
  // A brief delay after each notification gives the BLE controller time
  // to transmit the packet before we enqueue the next one.
  delay(8);
  return true;
}
#endif

// Opens the next recording file and sets file_info_characteristic so the
// client can read the file size before streaming begins. The file handle is
// kept open in pending_stream_file for stream_prepared_file() to consume.
static void prepare_current_file() {
  if (!client_connected || ble_server == nullptr) {
    Serial.printf("[ble] prepare_current_file: not connected, skipping\r\n");
    return;
  }

  current_stream_path = next_recording_path();
  if (current_stream_path.length() == 0) {
    Serial.printf("[ble] prepare_current_file: no files to send\r\n");
    uint32_t empty = 0;
    file_info_characteristic->setValue(empty);
    return;
  }

  pending_stream_file = LittleFS.open(current_stream_path, FILE_READ);
  if (!pending_stream_file) {
    Serial.printf("[ble] prepare_current_file: failed to open %s\r\n",
                  current_stream_path.c_str());
    current_stream_path = "";
    uint32_t empty = 0;
    file_info_characteristic->setValue(empty);
    return;
  }

  uint32_t file_size = pending_stream_file.size();
  file_info_characteristic->setValue(file_size);
  Serial.printf("[ble] prepared %s (%u bytes)\r\n",
                current_stream_path.c_str(), file_size);
}

// Streams the file prepared by prepare_current_file() via BLE notifications,
// then closes the file handle. No-op if no file was prepared.
static void stream_prepared_file() {
  if (!pending_stream_file) {
    DBG("[ble] stream_prepared_file called with no prepared file\r\n");
    return;
  }

  if (!client_connected || ble_server == nullptr) {
    Serial.printf("[ble] stream: client disconnected before start\r\n");
    pending_stream_file.close();
    return;
  }

  uint16_t connection_id = ble_server->getConnId();
  uint16_t attribute_handle = audio_data_characteristic->getHandle();

  // BLE notification payload is MTU minus 3 bytes of ATT header. Fall back to
  // 20 if the server reports an unexpectedly low value.
  uint16_t mtu = ble_server->getPeerMTU(connection_id);
  int chunk_size = (mtu > 3) ? (mtu - 3) : 20;
  uint8_t chunk[512];
  if (chunk_size > (int)sizeof(chunk)) {
    chunk_size = sizeof(chunk);
  }

  uint32_t total_sent = 0;
  int chunks_since_yield = 0;
  Serial.printf("[ble] streaming %s, mtu=%u chunk=%d\r\n",
                current_stream_path.c_str(), mtu, chunk_size);

  while (pending_stream_file.available() && client_connected) {
    int bytes_read = pending_stream_file.read(chunk, chunk_size);
    if (bytes_read > 0) {
      if (!send_notification(connection_id, attribute_handle, chunk,
                             bytes_read)) {
        Serial.printf("[ble] stream: send_notification failed at %u bytes\r\n",
                      total_sent);
        break;
      }
      total_sent += bytes_read;
      chunks_since_yield++;
      // Yield briefly every 4 chunks to let the remote BLE stack drain
      // its receive buffer. Without this, some Android phones (especially
      // older models) silently drop notifications under sustained load.
      if (chunks_since_yield >= 4) {
        chunks_since_yield = 0;
        delay(2);
      }
    }
  }
  pending_stream_file.close();
  Serial.printf("[ble] stream done: %u bytes sent, connected=%d\r\n",
                total_sent, (int)client_connected);
}

static uint16_t read_battery_millivolts() {
#if defined(BOARD_XIAO_SENSE)
  // XIAO Sense does not expose battery voltage to a GPIO.
  return 0;
#elif defined(BOARD_M5STICKCPLUS2)
  // GPIO38 through a 1:1 voltage divider (ratio 2.0x), same as M5Unified.
  analogRead(pin_battery);
  delayMicroseconds(100);
  uint32_t sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(pin_battery);
  }
  return (uint16_t)((sum / 10) * 2);
#elif defined(BOARD_M5STICKS3)
  // M5StickS3: battery voltage via M5Unified Power API (PY32 PMIC).
  return (uint16_t)M5.Power.getBatteryVoltage();
#else
  // Throwaway read to pre-charge the ADC's sample-and-hold capacitor,
  // which otherwise doesn't fully settle through the 180k voltage divider.
  analogRead(pin_battery);
  delayMicroseconds(100);

  uint32_t sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogReadMilliVolts(pin_battery);
  }
  // Voltage divider halves the battery voltage, so multiply by 2 to recover it.
  // Non-linear correction for ADC reading low due to 180k source impedance.
  // Correction factor = 1.302 - 0.000065 * raw_mV, i.e. ~1.04 at 4V, ~1.05 at 3.85V.
  uint32_t raw = (sum / 10) * 2;
  uint32_t factor = 13020 - 65 * raw / 100;
  return (uint16_t)(raw * factor / 10000);
#endif
}

static const size_t pairing_token_length = 16;
static const char *nvs_namespace = "middle";
static const char *nvs_key_pair_token = "pair_token";

// Reads the stored pairing token from NVS into `out_token`. Returns true if a
// token was found (pendant is claimed), false if absent (unclaimed).
static bool nvs_read_pair_token(uint8_t out_token[pairing_token_length]) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  if (err != ESP_OK) {
    DBG("[ble] nvs_open read failed: %d\r\n", err);
    return false;
  }
  size_t length = pairing_token_length;
  err = nvs_get_blob(handle, nvs_key_pair_token, out_token, &length);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return false;
  }
  if (err != ESP_OK || length != pairing_token_length) {
    DBG("[ble] nvs_get_blob failed: %d\r\n", err);
    return false;
  }
  return true;
}

// Writes `token` to NVS, claiming the pendant.
static bool nvs_write_pair_token(const uint8_t token[pairing_token_length]) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    DBG("[ble] nvs_open write failed: %d\r\n", err);
    return false;
  }
  err = nvs_set_blob(handle, nvs_key_pair_token, token, pairing_token_length);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    DBG("[ble] nvs_set_blob/commit failed: %d\r\n", err);
    return false;
  }
  return true;
}

static bool nvs_erase_pair_token() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    DBG("[ble] nvs_open erase failed: %d\r\n", err);
    return false;
  }
  err = nvs_erase_key(handle, nvs_key_pair_token);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  if (err != ESP_OK) {
    DBG("[ble] nvs_erase/commit failed: %d\r\n", err);
    return false;
  }
  return true;
}

class server_callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    client_connected = true;
    Serial.printf("[ble] client connected\r\n");
  }

  void onDisconnect(BLEServer *server) override {
    Serial.printf("[ble] client disconnected\r\n");
    client_connected = false;
    connection_authenticated = false;
    command_queue_clear();
    if (pending_stream_file) {
      pending_stream_file.close();
    }
    if (pending_recording_count > 0) {
      resume_ble_advertising();
    } else {
      sleep_requested = true;
    }
  }
};

class command_callbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() > 0) {
      command_queue_push((uint8_t)value[0]);
      Serial.printf("[ble] command received: 0x%02x\r\n", (uint8_t)value[0]);
    }
  }
};

class pairing_callbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *characteristic) override {
    uint8_t stored_token[pairing_token_length];
    uint8_t status = nvs_read_pair_token(stored_token) ? 0x01 : 0x00;
    characteristic->setValue(&status, 1);
  }

  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if ((size_t)value.length() != pairing_token_length) {
      DBG("[ble] pairing write wrong length %d, disconnecting\r\n",
          value.length());
      ble_server->disconnect(ble_server->getConnId());
      return;
    }

    const uint8_t *written_token = (const uint8_t *)value.c_str();
    uint8_t stored_token[pairing_token_length];
    bool claimed = nvs_read_pair_token(stored_token);

    if (!claimed) {
      if (!nvs_write_pair_token(written_token)) {
        // NVS write failed — disconnect rather than silently grant access.
        ble_server->disconnect(ble_server->getConnId());
        return;
      }
      DBG("[ble] paired with new token\r\n");
      connection_authenticated = true;
      ble_active_until_milliseconds = millis() + ble_keepalive_milliseconds;
      hard_sleep_deadline_milliseconds = millis() + 30000;
    } else {
      if (memcmp(written_token, stored_token, pairing_token_length) != 0) {
        DBG("[ble] token mismatch, disconnecting\r\n");
        ble_server->disconnect(ble_server->getConnId());
        return;
      }
      DBG("[ble] token verified\r\n");
      connection_authenticated = true;
      ble_active_until_milliseconds = millis() + ble_keepalive_milliseconds;
      hard_sleep_deadline_milliseconds = millis() + 30000;
    }
  }
};

static void init_ble() {
  BLEDevice::init("Middle");
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
  BLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
  BLEDevice::setMTU(517);
  ble_server = BLEDevice::createServer();
  ble_server->setCallbacks(new server_callbacks());

  BLEService *service = ble_server->createService(
      BLEUUID(service_uuid), 20);
  file_count_characteristic = service->createCharacteristic(
      characteristic_file_count_uuid, BLECharacteristic::PROPERTY_READ);
  file_info_characteristic = service->createCharacteristic(
      characteristic_file_info_uuid, BLECharacteristic::PROPERTY_READ);
  audio_data_characteristic = service->createCharacteristic(
      characteristic_audio_data_uuid, BLECharacteristic::PROPERTY_NOTIFY);
#if !CONFIG_BT_NIMBLE_ENABLED
  // BlueDroid requires an explicit CCCD descriptor for notification
  // characteristics. NimBLE adds it automatically.
  audio_data_characteristic->addDescriptor(new BLE2902());
#endif

  BLECharacteristic *command_characteristic = service->createCharacteristic(
      characteristic_command_uuid, BLECharacteristic::PROPERTY_WRITE);
  command_characteristic->setCallbacks(new command_callbacks());

  voltage_characteristic = service->createCharacteristic(
      characteristic_voltage_uuid, BLECharacteristic::PROPERTY_READ);
  uint16_t initial_voltage = 0;
  voltage_characteristic->setValue(initial_voltage);

  pairing_characteristic = service->createCharacteristic(
      characteristic_pairing_uuid,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pairing_characteristic->setCallbacks(new pairing_callbacks());

  update_file_count();
  uint32_t file_size = 0;
  file_info_characteristic->setValue(file_size);

  service->start();
  ble_advertising = BLEDevice::getAdvertising();
  ble_advertising->addServiceUUID(service_uuid);
  ble_advertising->setScanResponse(true);
}

static bool ble_initialized = false;

static void start_ble_if_needed() {
  update_file_count();
  if (pending_recording_count > 0) {
    if (!ble_initialized) {
      init_ble();
      ble_initialized = true;
    }
    uint16_t millivolts = read_battery_millivolts();
    voltage_characteristic->setValue(millivolts);
    DBG("[bat] Battery: %u mV\r\n", millivolts);
    start_ble_advertising();
  }
}

void setup() {
  set_status_led_off();

#if DEBUG
  Serial.begin(115200);
  delay(1500);  // Give USB-Serial/JTAG time to stabilize
  Serial.printf("\r\n\r\n[setup] === BOOT START ===\r\n");
#endif

  // M5StickS3 buttons are managed by M5Unified — skip raw GPIO setup.
#if !defined(BOARD_M5STICKS3)
  pinMode(pin_button, INPUT_PULLUP);
#endif
#ifdef BOARD_XIAO_SENSE
  oled_init();
#elif defined(BOARD_M5STICKCPLUS2)
  // Release deep sleep hold before reconfiguring held pins.
  gpio_hold_dis((gpio_num_t)pin_power_hold);
  gpio_hold_dis((gpio_num_t)TFT_BL);
  gpio_hold_dis((gpio_num_t)pin_pdm_clk);
  gpio_deep_sleep_hold_dis();
  pinMode(pin_power_hold, OUTPUT);
  digitalWrite(pin_power_hold, HIGH);
  pinMode(pin_button_b, INPUT);
  pinMode(pin_button_pwr, INPUT);
  tft_init();
#elif defined(BOARD_M5STICKS3)
  m5_display_init();
  Serial.printf("[setup] m5_display_init done, display_is_on=%d, m5_display_ready=%d\r\n", display_is_on, m5_display_ready);
#else
  // Release the GPIO hold set in enter_deep_sleep() before reconfiguring the
  // pin. If the hold is still active, pinMode() fights the latched state.
  gpio_hold_dis((gpio_num_t)pin_mic_power);
  gpio_deep_sleep_hold_dis();
  pinMode(pin_mic_power, OUTPUT);
  digitalWrite(pin_mic_power, LOW);
#endif

  // NVS must be initialized before any NVS reads, including the pairing token
  // check in init_ble(). nvs_flash_init() is safe to call on every boot.
  nvs_flash_init();

  configure_button_wakeup();

#if defined(BOARD_M5STICKS3)
  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[setup] wakeup_cause=%d (EXT0=%d)\r\n", wakeup_cause, ESP_SLEEP_WAKEUP_EXT0);

#if DEBUG
  // === Boot-time mic diagnostic using M5Unified ===
  Serial.printf("[diag] --- M5Unified mic diagnostic start ---\r\n");

  // Power on the ES8311 via PMIC (M5Unified mic callback doesn't do this).
  {
    uint8_t before = 0;
    M5.In_I2C.readRegister(0x6E, 0x11, &before, 1, 100000);
    Serial.printf("[diag] PMIC 0x11 before: 0x%02X\r\n", before);
    M5.In_I2C.bitOn(0x6E, 0x11, 0b00001000, 100000);
    delay(50);
    uint8_t after = 0;
    M5.In_I2C.readRegister(0x6E, 0x11, &after, 1, 100000);
    Serial.printf("[diag] PMIC 0x11 after:  0x%02X\r\n", after);
  }

  // Test M5Unified's own mic system (it handles ES8311 + I2S + HAL clocks).
  // Buffer MUST be static: M5.Mic.record() is async — the mic task writes
  // to the buffer in the background after record() returns.
  {
    static int16_t test_buf[512];
    memset(test_buf, 0, sizeof(test_buf));
    Serial.printf("[diag] Calling M5.Mic.record(512, %d)...\r\n", sample_rate);
    bool ok = M5.Mic.record(test_buf, 512, sample_rate, false);
    while (M5.Mic.isRecording()) { delay(1); }
    Serial.printf("[diag] M5.Mic.record() = %s\r\n", ok ? "OK" : "FAIL");

    int16_t mn = 0, mx = 0;
    int32_t sum = 0;
    int non_zero = 0;
    for (int i = 0; i < 512; i++) {
      if (test_buf[i] < mn) mn = test_buf[i];
      if (test_buf[i] > mx) mx = test_buf[i];
      sum += test_buf[i];
      if (test_buf[i] != 0) non_zero++;
    }
    Serial.printf("[diag] 512 samples: min=%d max=%d avg=%d non_zero=%d\r\n",
                  mn, mx, (int)(sum / 512), non_zero);
    Serial.printf("[diag] first 8: %d %d %d %d %d %d %d %d\r\n",
                  test_buf[0], test_buf[1], test_buf[2], test_buf[3],
                  test_buf[4], test_buf[5], test_buf[6], test_buf[7]);

    // Second read to skip startup transient
    memset(test_buf, 0, sizeof(test_buf));
    M5.Mic.record(test_buf, 512, sample_rate, false);
    while (M5.Mic.isRecording()) { delay(1); }
    mn = 0; mx = 0; sum = 0; non_zero = 0;
    for (int i = 0; i < 512; i++) {
      if (test_buf[i] < mn) mn = test_buf[i];
      if (test_buf[i] > mx) mx = test_buf[i];
      sum += test_buf[i];
      if (test_buf[i] != 0) non_zero++;
    }
    Serial.printf("[diag] 2nd read: min=%d max=%d avg=%d non_zero=%d\r\n",
                  mn, mx, (int)(sum / 512), non_zero);
    Serial.printf("[diag] first 8: %d %d %d %d %d %d %d %d\r\n",
                  test_buf[0], test_buf[1], test_buf[2], test_buf[3],
                  test_buf[4], test_buf[5], test_buf[6], test_buf[7]);

    // Third read after more delay
    delay(500);
    memset(test_buf, 0, sizeof(test_buf));
    M5.Mic.record(test_buf, 512, sample_rate, false);
    while (M5.Mic.isRecording()) { delay(1); }
    mn = 0; mx = 0; sum = 0; non_zero = 0;
    for (int i = 0; i < 512; i++) {
      if (test_buf[i] < mn) mn = test_buf[i];
      if (test_buf[i] > mx) mx = test_buf[i];
      sum += test_buf[i];
      if (test_buf[i] != 0) non_zero++;
    }
    Serial.printf("[diag] 3rd read (after 500ms): min=%d max=%d avg=%d non_zero=%d\r\n",
                  mn, mx, (int)(sum / 512), non_zero);
  }

  Serial.printf("[diag] --- M5Unified mic diagnostic end ---\r\n");
#endif

  if (wakeup_cause != ESP_SLEEP_WAKEUP_EXT0) {
    // Fresh boot (hardware power button) — show status, sleep after timeout.
    // Use a longer window (10s) since every boot is a power-button boot and
    // the user may need time to press BtnA to record.
    Serial.printf("[setup] not EXT0, showing status then sleeping\r\n");
    show_status_screen();
    ble_active_until_milliseconds = millis() + 10000;
  }
#else
  esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[setup] wakeup_cause=%d (EXT0=%d, EXT1=%d)\r\n", wakeup_cause, ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_EXT1);

  if (wakeup_cause != ESP_SLEEP_WAKEUP_EXT0 &&
      wakeup_cause != ESP_SLEEP_WAKEUP_EXT1) {
    Serial.printf("[setup] not EXT0/EXT1, entering deep sleep\r\n");
    enter_deep_sleep();
  }

  Serial.printf("[setup] passed wakeup check, proceeding\r\n");

#ifdef BOARD_M5STICKCPLUS2
  // Power button wakeup: show status and keep awake long enough for
  // BtnB/BtnPWR handlers in loop() to work.
  if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
    show_status_screen();
    ble_active_until_milliseconds = millis() + display_timeout_ms;
  }
#endif
#endif  // BOARD_M5STICKS3

  remove_empty_recordings();

#if defined(BOARD_M5STICKS3)
  // M5Unified manages BtnA on GPIO 11 — use its API, not raw digitalRead.
  M5.update();
  // Dev mode: hold BtnB at boot to keep device awake for flashing.
  if (M5.BtnB.isPressed()) {
    dev_mode = true;
    Serial.printf("[setup] BtnB held at boot — dev mode, sleep disabled\r\n");
    m5_display_show("DEV MODE", "sleep off", COLOR_STATUS);
  }
  if (M5.BtnA.isPressed()) {
    record_and_save();
  }
#else
  int button = digitalRead(pin_button);
  if (button == LOW) {
    record_and_save();
  }
#endif

#if defined(BOARD_M5STICKS3)
  // Refresh the status screen and BLE window after any recording attempt
  // so the user sees the result before the device sleeps again.
  show_status_screen();
  ble_active_until_milliseconds = millis() + 10000;
#endif
}

void loop() {
  set_status_led_off();
  static int last_button_state = HIGH;
  static bool initial_ble_check_done = false;

  // After the first recording in setup(), we enter loop with the button
  // already released. Check once whether there are files to sync.
  if (!initial_ble_check_done) {
    initial_ble_check_done = true;
    start_ble_if_needed();
  }

#if !defined(BOARD_M5STICKS3)
  // M5StickS3 uses M5Unified BtnA API below instead of raw digitalRead.
  int button_state = digitalRead(pin_button);
  if (button_state != last_button_state) {
    last_button_state = button_state;
    if (button_state == LOW) {
      record_and_save();
      start_ble_if_needed();
    }
  }
#else
  int button_state = HIGH;  // placeholder for sleep logic below
#endif

  uint8_t command;
  while ((command = command_queue_pop()) != 0) {
    if (!connection_authenticated) {
      DBG("[ble] command rejected, not authenticated\r\n");
    } else if (command == command_request_next) {
      prepare_current_file();
    } else if (command == command_start_stream) {
      stream_prepared_file();
#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
      {
        unsigned int sent = (sync_total_files > pending_recording_count)
            ? sync_total_files - pending_recording_count + 1
            : 1;
        char sync_buf[22];
        snprintf(sync_buf, sizeof(sync_buf), "%u/%u files",
                 sent, (unsigned)sync_total_files);
#ifdef BOARD_XIAO_SENSE
        oled_show("Syncing...", sync_buf);
#elif defined(BOARD_M5STICKS3)
        m5_display_show("Syncing...", sync_buf, COLOR_SYNCING);
#else
        tft_show("Syncing...", sync_buf);
#endif
      }
#endif
    } else if (command == command_ack_received) {

      // Close any file handle left open by prepare_current_file(). When the
      // client skips START_STREAM (e.g. because the file was 0 bytes),
      // stream_prepared_file() is never called and the handle leaks — which
      // prevents LittleFS.remove() from deleting the file.
      if (pending_stream_file) {
        pending_stream_file.close();
      }

      String path_to_delete = current_stream_path;
      if (path_to_delete.length() == 0) {
        path_to_delete = next_recording_path();
      }

      if (path_to_delete.length() == 0) {
      } else {
        bool removed = LittleFS.remove(path_to_delete);
        DBG("[ble] remove %s: %s\r\n",
            path_to_delete.c_str(), removed ? "OK" : "FAILED");
        if (removed) {
          current_stream_path = "";
        }
      }
      update_file_count();
#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
      {
        unsigned int sent = (sync_total_files > pending_recording_count)
            ? sync_total_files - pending_recording_count : sync_total_files;
        char sync_buf[22];
        snprintf(sync_buf, sizeof(sync_buf), "%u/%u synced",
                 sent, (unsigned)sync_total_files);
#ifdef BOARD_XIAO_SENSE
        oled_show("Synced", sync_buf);
#elif defined(BOARD_M5STICKS3)
        m5_display_show("Synced", sync_buf, COLOR_SYNCED);
#else
        tft_show("Synced", sync_buf);
#endif
      }
#endif
    } else if (command == command_sync_done) {
    } else if (command == command_skip_file) {
      // Client failed to transfer the current file. Delete it so the sync
      // queue is not permanently stuck on an untransferable recording.
      if (pending_stream_file) {
        pending_stream_file.close();
      }

      String path_to_delete = current_stream_path;
      if (path_to_delete.length() == 0) {
        path_to_delete = next_recording_path();
      }

      if (path_to_delete.length() > 0) {
        bool removed = LittleFS.remove(path_to_delete);
        Serial.printf("[ble] skip+delete %s: %s\r\n",
            path_to_delete.c_str(), removed ? "OK" : "FAILED");
        if (removed) {
          current_stream_path = "";
        }
      }
      update_file_count();
    } else if (command == command_unpair) {
      // Erase the stored pairing token so the pendant becomes unclaimed.
      // The authenticated client asked to release ownership.
      if (nvs_erase_pair_token()) {
        Serial.printf("[ble] unpaired, token erased\r\n");
      }
      connection_authenticated = false;
      ble_server->disconnect(ble_server->getConnId());
    } else if (command == command_enter_bootloader) {
      // Set the ROM download mode flag before restarting so the bootloader
      // stays in USB/UART download mode rather than booting the application.
      // This allows flashing without physical access to the boot button.
#if defined(RTC_CNTL_OPTION1_REG)
      REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#endif
      esp_restart();
    }
  }

#ifdef BOARD_M5STICKS3
  if (dev_mode) {
    sleep_requested = false;  // Discard sleep requests in dev mode.
  } else
#endif
  {
    if (button_state == HIGH && hard_sleep_deadline_milliseconds != 0 &&
        !client_connected &&
        (long)(millis() - hard_sleep_deadline_milliseconds) >= 0) {
      enter_deep_sleep();
    }

    if (sleep_requested ||
        (!client_connected && command_queue_head == command_queue_tail && button_state == HIGH &&
         !ble_window_active())) {
      sleep_requested = false;
      enter_deep_sleep();
    }
  }

#ifdef BOARD_M5STICKCPLUS2
  // BtnB: short press = force BLE advertising, long press (>=1s) = oblique strategy.
  static int last_btnb_state = HIGH;
  static unsigned long btnb_press_start = 0;
  static bool btnb_long_handled = false;
  int btnb_state = digitalRead(pin_button_b);
  if (btnb_state != last_btnb_state) {
    last_btnb_state = btnb_state;
    if (btnb_state == LOW) {
      btnb_press_start = millis();
      btnb_long_handled = false;
    } else {
      // Released — short press triggers sync if long press wasn't handled.
      if (!btnb_long_handled) {
        if (!ble_initialized) {
          init_ble();
          ble_initialized = true;
        }
        uint16_t millivolts = read_battery_millivolts();
        voltage_characteristic->setValue(millivolts);
        update_file_count();
        sync_total_files = pending_recording_count;
        start_ble_advertising();
      }
    }
  }
  if (btnb_state == LOW && !btnb_long_handled &&
      (millis() - btnb_press_start >= 1000)) {
    btnb_long_handled = true;
    show_oblique_strategy();
  }

  // BtnPWR: short press = status screen, long press (>=2s) = power off.
  static int last_pwr_state = HIGH;
  static unsigned long pwr_press_start = 0;
  static bool pwr_long_handled = false;
  int pwr_state = digitalRead(pin_button_pwr);
  if (pwr_state != last_pwr_state) {
    last_pwr_state = pwr_state;
    if (pwr_state == LOW) {
      pwr_press_start = millis();
      pwr_long_handled = false;
      show_status_screen();
      // Keep device awake while user is interacting.
      ble_active_until_milliseconds = millis() + display_timeout_ms;
    }
  }
  if (pwr_state == LOW && !pwr_long_handled &&
      (millis() - pwr_press_start >= 2000)) {
    pwr_long_handled = true;
    tft_show("Power Off");
    delay(500);
    tft_off();
    digitalWrite(pin_power_hold, LOW);
  }
#endif

#ifdef BOARD_M5STICKS3
  // M5StickS3: use M5Unified button API for both BtnA and BtnB.
  M5.update();

  // BtnA (front): press = record.
  if (M5.BtnA.wasPressed()) {
    record_and_save();
    start_ble_if_needed();
    show_status_screen();
    ble_active_until_milliseconds = millis() + display_timeout_ms;
  }

  // BtnB (side): short press = force BLE advertising, long press (>=1s) = oblique strategy.
  static bool m5s3_btnb_long_handled = false;
  if (M5.BtnB.wasPressed()) {
    m5s3_btnb_long_handled = false;
  }
  if (M5.BtnB.pressedFor(1000) && !m5s3_btnb_long_handled) {
    m5s3_btnb_long_handled = true;
    show_oblique_strategy();
  }
  if (M5.BtnB.wasReleased()) {
    if (!m5s3_btnb_long_handled) {
      // Short press — force sync.
      if (!ble_initialized) {
        init_ble();
        ble_initialized = true;
      }
      uint16_t millivolts = read_battery_millivolts();
      voltage_characteristic->setValue(millivolts);
      update_file_count();
      sync_total_files = pending_recording_count;
      start_ble_advertising();
    }
  }
#endif

#if defined(BOARD_XIAO_SENSE) || defined(BOARD_M5STICKCPLUS2) || defined(BOARD_M5STICKS3)
  display_check_timeout();
#endif

  delay(20);
}
