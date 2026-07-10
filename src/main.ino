/*
 * Motor Winding Tool  —  ESP32-S3-Touch-LCD-1.28 (Waveshare)
 * -----------------------------------------------------------
 * Display : GC9A01 240x240 round LCD (SPI)
 * Touch   : CST816S capacitive touch (I2C)
 * GUI     : LVGL 8.x
 *
 * Control:
 *   - 12VDC winder motor driven through a driver board via PWM pin.
 *   - Limit switch (NO) closes at 0deg each rotation -> one wrap counted.
 *   - Switch wired to GND, using internal pull-up, counted on FALLING edge.
 *
 * Wiring for your added hardware:
 *   MOTOR_PWM_PIN  (GPIO18) -> motor driver PWM input
 *   LIMIT_SW_PIN   (GPIO17) -> limit switch -> other side to GND
 *   Common ground between ESP32 and 12V driver is REQUIRED.
 *
 * Library dependencies (Arduino IDE / PlatformIO):
 *   - lvgl (8.3.x)
 *   - lovyan03/LovyanGFX   (handles GC9A01 + CST816S on this exact board)
 *
 * Board: "ESP32S3 Dev Module" (or Waveshare ESP32-S3 variant), PSRAM enabled.
 */

#include <lvgl.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ============================================================
//  Display + Touch driver (LovyanGFX config for this board)
// ============================================================
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01   _panel;
  lgfx::Bus_SPI        _bus;
  lgfx::Light_PWM      _light;
  lgfx::Touch_CST816S  _touch;

public:
  LGFX() {
    { // SPI bus for the LCD
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read  = 20000000;
      cfg.pin_sclk   = 10;
      cfg.pin_mosi   = 11;
      cfg.pin_miso   = -1;
      cfg.pin_dc     = 8;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // GC9A01 panel
      auto cfg = _panel.config();
      cfg.pin_cs        = 9;
      cfg.pin_rst       = 14;
      cfg.pin_busy      = -1;
      cfg.panel_width   = 240;
      cfg.panel_height  = 240;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.readable      = false;
      cfg.invert        = true;
      cfg.rgb_order     = false;
      cfg.dlen_16bit    = false;
      cfg.bus_shared    = false;
      _panel.config(cfg);
    }
    { // Backlight on GPIO2 (confirmed empirically on this board revision;
      // some docs/demo code claim GPIO40, which is wrong for this unit)
      auto cfg = _light.config();
      cfg.pin_bl = 2;
      cfg.invert = false;
      cfg.freq   = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    { // CST816S capacitive touch (I2C)
      auto cfg = _touch.config();
      cfg.i2c_port = 0;
      cfg.pin_sda  = 6;
      cfg.pin_scl  = 7;
      cfg.pin_int  = 5;
      cfg.pin_rst  = 13;
      cfg.freq     = 400000;
      cfg.x_min = 0; cfg.x_max = 239;
      cfg.y_min = 0; cfg.y_max = 239;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
static LGFX tft;

// ============================================================
//  Motor / switch pin definitions
// ============================================================
static const int      MOTOR_PWM_PIN = 18;
static const int      LIMIT_SW_PIN  = 17;

static const int      PWM_CHANNEL   = 0;      // LEDC channel (7 used by backlight)
static const int      PWM_FREQ_HZ   = 20000;  // 20 kHz -> inaudible
static const int      PWM_RES_BITS  = 8;      // 0..255
static const int      RUN_DUTY      = 255;    // motor speed while running (0..255)

// Debounce: a candidate edge (flagged by the ISR) is only counted once the
// pin has held LOW continuously for SW_CONFIRM_MS, and the next candidate
// isn't armed until the pin is back HIGH for SW_CONFIRM_MS. This rejects the
// microsecond-scale glitches coming off the PWM-driven motor (~50us period
// at 20kHz) regardless of when they occur, since a real switch closure dwells
// for many milliseconds at this winding speed but a noise glitch does not.
static const uint32_t SW_CONFIRM_MS = 10;

// ============================================================
//  Shared state
// ============================================================
enum WinderState { ST_IDLE, ST_RUNNING, ST_PAUSED, ST_DONE };
static volatile WinderState state = ST_IDLE;

static volatile uint32_t wrapCount     = 0;
static uint32_t          targetCount   = 100;

// ============================================================
//  Limit-switch ISR — only flags a candidate edge, nothing else
// ============================================================
static volatile bool     candidatePending = false;
static volatile uint32_t candidateMs      = 0;

void IRAM_ATTR onLimitSwitch() {
  candidatePending = true;
  candidateMs = millis();
}

// ============================================================
//  Limit-switch debounce — confirms a candidate edge from loop().
//  See SW_CONFIRM_MS above for why this rejects noise.
// ============================================================
static bool     awaitingConfirm = false;
static bool     awaitingRelease = false;
static uint32_t confirmStartMs  = 0;
static uint32_t releaseStartMs  = 0;

static void pollLimitSwitch() {
  if (candidatePending && !awaitingConfirm && !awaitingRelease) {
    noInterrupts();
    candidatePending = false;
    confirmStartMs = candidateMs;
    interrupts();
    awaitingConfirm = true;
  }

  if (awaitingConfirm) {
    if (digitalRead(LIMIT_SW_PIN) == HIGH) {
      awaitingConfirm = false;                 // reverted before confirming -> noise, discard
    } else if (millis() - confirmStartMs >= SW_CONFIRM_MS) {
      if (state == ST_RUNNING) wrapCount++;    // held LOW long enough -> real closure
      awaitingConfirm = false;
      awaitingRelease  = true;
      releaseStartMs   = millis();
    }
  }

  if (awaitingRelease) {
    if (digitalRead(LIMIT_SW_PIN) == LOW) {
      releaseStartMs = millis();               // still closed, keep waiting for release
    } else if (millis() - releaseStartMs >= SW_CONFIRM_MS) {
      awaitingRelease = false;                 // back open and stable -> ready for next candidate
    }
  }
}

// ============================================================
//  Motor helpers
// ============================================================
static inline void motorRun()  { ledcWrite(PWM_CHANNEL, RUN_DUTY); }
static inline void motorStop() { ledcWrite(PWM_CHANNEL, 0); }

// ============================================================
//  LVGL display + input plumbing
// ============================================================
static const uint16_t SCR_W = 240, SCR_H = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCR_W * 40];

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t *)px, w * h);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint16_t tx, ty;
  bool touched = tft.getTouch(&tx, &ty);
  if (touched) {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = tx;
    data->point.y = ty;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ============================================================
//  GUI widgets
// ============================================================
static lv_obj_t *lblCount;     // "current / target"
static lv_obj_t *lblTarget;    // target readout in the setter row
static lv_obj_t *lblState;     // state text
static lv_obj_t *btnStart;
static lv_obj_t *btnPause;

static lv_obj_t *keypadPopup;  // NULL when not open
static lv_obj_t *keypadTa;

// Max target the keypad will accept — keeps wrapCount/targetCount display
// and math comfortably within the wide integer types used for them.
static const uint32_t TARGET_MAX = 999999UL;

static void refreshLabels() {
  static char cbuf[24];
  snprintf(cbuf, sizeof(cbuf), "%lu / %lu",
           (unsigned long)wrapCount, (unsigned long)targetCount);
  lv_label_set_text(lblCount, cbuf);

  static char tbuf[16];
  snprintf(tbuf, sizeof(tbuf), "Target: %lu", (unsigned long)targetCount);
  lv_label_set_text(lblTarget, tbuf);

  const char *s = "IDLE";
  switch (state) {
    case ST_IDLE:    s = "IDLE";    break;
    case ST_RUNNING: s = "RUNNING"; break;
    case ST_PAUSED:  s = "PAUSED";  break;
    case ST_DONE:    s = "DONE";    break;
  }
  lv_label_set_text(lblState, s);
}

// --- Button callbacks -------------------------------------------------
static void onStart(lv_event_t *e) {
  if (state == ST_IDLE || state == ST_PAUSED) {
    if (wrapCount < targetCount) {
      state = ST_RUNNING;
      motorRun();
    }
  }
  refreshLabels();
}

static void onPause(lv_event_t *e) {
  if (state == ST_RUNNING) {
    state = ST_PAUSED;
    motorStop();
  }
  refreshLabels();
}

static void onReset(lv_event_t *e) {
  state = ST_IDLE;
  motorStop();
  wrapCount = 0;
  refreshLabels();
}

static void onTargetMinus(lv_event_t *e) {
  if (state == ST_RUNNING) return;         // don't move target mid-run
  if (targetCount >= 10) targetCount -= 10;
  else targetCount = 0;
  refreshLabels();
}

static void onTargetPlus(lv_event_t *e) {
  if (state == ST_RUNNING) return;
  targetCount += 10;
  refreshLabels();
}

// --- Numeric keypad popup (tap the target readout to enter an exact value) --
static void closeKeypad() {
  if (keypadPopup) {
    lv_obj_del(keypadPopup);
    keypadPopup = NULL;
  }
}

static void onKeypadEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *txt = lv_textarea_get_text(keypadTa);
    uint32_t v = (uint32_t)strtoul(txt, NULL, 10);
    if (v > TARGET_MAX) v = TARGET_MAX;
    targetCount = v;
    refreshLabels();
    closeKeypad();
  } else if (code == LV_EVENT_CANCEL) {
    closeKeypad();
  }
}

static void onTargetLabelClicked(lv_event_t *e) {
  if (state == ST_RUNNING) return;   // target locked while running, same as +/-
  if (keypadPopup) return;           // already open

  keypadPopup = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(keypadPopup);
  lv_obj_set_size(keypadPopup, SCR_W, SCR_H);
  lv_obj_center(keypadPopup);
  lv_obj_set_style_bg_color(keypadPopup, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(keypadPopup, LV_OPA_COVER, 0);
  lv_obj_clear_flag(keypadPopup, LV_OBJ_FLAG_SCROLLABLE);

  keypadTa = lv_textarea_create(keypadPopup);
  lv_textarea_set_one_line(keypadTa, true);
  lv_textarea_set_accepted_chars(keypadTa, "0123456789");
  lv_textarea_set_max_length(keypadTa, 6);
  lv_obj_set_size(keypadTa, 150, 32);
  lv_obj_align(keypadTa, LV_ALIGN_TOP_MID, 0, 8);
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)targetCount);
  lv_textarea_set_text(keypadTa, buf);

  lv_obj_t *kb = lv_keyboard_create(keypadPopup);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_textarea(kb, keypadTa);
  lv_obj_set_size(kb, SCR_W - 8, SCR_H - 48);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_add_event_cb(kb, onKeypadEvent, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb, onKeypadEvent, LV_EVENT_CANCEL, NULL);
}

// --- Build the screen -------------------------------------------------
static lv_obj_t *makeBtn(lv_obj_t *parent, const char *txt,
                         lv_align_t align, lv_coord_t ox, lv_coord_t oy,
                         lv_coord_t w, lv_coord_t h, lv_event_cb_t cb,
                         lv_color_t color) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_align(b, align, ox, oy);
  lv_obj_set_style_bg_color(b, color, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

static void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

  // Title
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "COIL WINDER");
  lv_obj_set_style_text_color(title, lv_color_hex(0x8fd6ff), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  // Big count readout (current / target)
  lblCount = lv_label_create(scr);
  lv_obj_set_style_text_color(lblCount, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(lblCount, &lv_font_montserrat_28, 0);
  lv_obj_align(lblCount, LV_ALIGN_TOP_MID, 0, 40);

  // State text
  lblState = lv_label_create(scr);
  lv_obj_set_style_text_color(lblState, lv_color_hex(0xffd27f), 0);
  lv_obj_align(lblState, LV_ALIGN_TOP_MID, 0, 74);

  // Target setter row:  [ - ]  Target: N  [ + ]
  // The label itself is tappable -- opens a numeric keypad for exact entry.
  lblTarget = lv_label_create(scr);
  lv_obj_set_style_text_color(lblTarget, lv_color_hex(0xcfd8e0), 0);
  lv_obj_set_style_pad_all(lblTarget, 6, 0);
  lv_obj_set_style_bg_opa(lblTarget, LV_OPA_20, 0);
  lv_obj_set_style_bg_color(lblTarget, lv_color_hex(0x8fd6ff), 0);
  lv_obj_set_style_radius(lblTarget, 6, 0);
  lv_obj_align(lblTarget, LV_ALIGN_CENTER, 0, 2);
  lv_obj_add_flag(lblTarget, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(lblTarget, onTargetLabelClicked, LV_EVENT_CLICKED, NULL);

  makeBtn(scr, "-", LV_ALIGN_CENTER, -78, 2, 40, 40,
          onTargetMinus, lv_color_hex(0x37414b));
  makeBtn(scr, "+", LV_ALIGN_CENTER,  78, 2, 40, 40,
          onTargetPlus,  lv_color_hex(0x37414b));

  // Start / Pause
  btnStart = makeBtn(scr, "START", LV_ALIGN_CENTER, -52, 52, 84, 44,
                     onStart, lv_color_hex(0x2e8b57));
  btnPause = makeBtn(scr, "PAUSE", LV_ALIGN_CENTER,  52, 52, 84, 44,
                     onPause, lv_color_hex(0xb8860b));

  // Reset
  makeBtn(scr, "RESET", LV_ALIGN_BOTTOM_MID, 0, -14, 110, 40,
          onReset, lv_color_hex(0x8b3a3a));

  refreshLabels();
}

// ============================================================
//  Setup / loop
// ============================================================
void setup() {
  // --- Motor PWM ---
  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttachPin(MOTOR_PWM_PIN, PWM_CHANNEL);
  motorStop();

  // --- Limit switch ---
  pinMode(LIMIT_SW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(LIMIT_SW_PIN), onLimitSwitch, FALLING);

  // --- Display + LVGL ---
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(200);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCR_W * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = SCR_W;
  disp_drv.ver_res  = SCR_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  buildUI();
}

void loop() {
  // Control logic in the main loop (ISR only flags candidate edges;
  // pollLimitSwitch() confirms/counts them here).
  static uint32_t lastShown = 0xFFFFFFFF;

  pollLimitSwitch();

  if (state == ST_RUNNING && wrapCount >= targetCount) {
    motorStop();
    state = ST_DONE;                 // hard stop at target
  }

  if (wrapCount != lastShown) {      // repaint only when the count changes
    lastShown = wrapCount;
    refreshLabels();
  }

  lv_timer_handler();
  delay(5);
}
