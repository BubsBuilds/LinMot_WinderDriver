# CLAUDE.md — Motor Winding Tool

Guidance for AI assistants working on this project. Read this before editing.

## What this is
A **motor winding tool** for making copper coil windings for a linear motor.
A 12VDC motor spins the winder; a touchscreen GUI sets a target wrap count,
runs/pauses the motor, and tracks completed wraps.

## Hardware
- **MCU/Display:** Waveshare **ESP32-S3-Touch-LCD-1.28** (1.28" round touchscreen dev board)
  - Module: **ESP32-S3R2** — 16MB flash (W25Q128JVSIQ), 2MB **Quad** PSRAM
    (not Octal — `board_build.arduino.memory_type` must be `qio_qspi`, confirmed
    booting correctly with this setting)
  - Display: **GC9A01**, 240×240 round, SPI
  - Touch: **CST816S**, capacitive, I2C
  - Onboard QMI8658 IMU shares the I2C bus — unused here
- **Motor:** 12VDC via a driver board that takes a **single PWM** control signal
- **Feedback:** Normally-Open limit switch — closes at 0° each rotation, reopens
  a few degrees later. One closure = one wrap.

### Pin map
Onboard (fixed by the board — do not reassign):
- Display SPI: SCLK=10, MOSI=11, DC=8, CS=9, RST=14, BL=2
  (BL confirmed empirically via blink-pattern test — some Waveshare demo
  code/docs claim GPIO40 for backlight, which is wrong for this board revision)
- Touch I2C: SDA=6, SCL=7, INT=5, RST=13

Added for this project:
- **Motor PWM out: GPIO18** — LEDC channel 0, 20 kHz, 8-bit
- **Limit switch in: GPIO17** — `INPUT_PULLUP`, switch to GND, counts on **FALLING** edge

Hardware notes: ESP32 and the 12V driver **must share a common ground**; a
flyback/snubber belongs on the motor side.

## Firmware stack & conventions
- **Framework:** Arduino (not ESP-IDF). The main file is a `.ino` sketch.
- **GUI:** **LVGL 8.x** (do not upgrade to 9.x without migrating the API calls).
- **Display/touch driver:** **LovyanGFX** — the board is configured explicitly
  in the `LGFX` class inside the sketch. Keep that config; it's board-specific.
- File layout: source lives in `src/` (`src/main.ino`). PlatformIO merges
  *every* `.ino` file found in `src/` into one build — never leave more than
  one `.ino` there (e.g. a scratch/diagnostic sketch) or you'll get duplicate
  `setup()`/`loop()` link errors. Keep throwaway test sketches in their own
  folder outside `src/` and swap them in only when actually flashing them.

## Design rules (please preserve)
- **ISR does one thing:** the limit-switch interrupt only flags a candidate
  edge (`candidatePending` + timestamp) — it does **not** touch the wrap
  counter directly. `pollLimitSwitch()`, called from `loop()`, confirms a
  candidate only after the pin holds LOW continuously for `SW_CONFIRM_MS`,
  and only re-arms after it's back HIGH for `SW_CONFIRM_MS`. This replaced an
  earlier simpler design (ISR incremented the counter directly, gated by a
  time-since-last-edge lockout) after real hardware showed spurious counts
  from PWM-driven motor noise occurring well away from actual switch
  transitions — a pure post-edge lockout can't reject noise that arrives
  after the lockout window expires, but a stable-dwell confirm rejects any
  glitch shorter than `SW_CONFIRM_MS` regardless of timing. Do **not** add
  motor control, LVGL calls, or blocking work inside the ISR.
- **All control/UI logic lives in `loop()`.** This keeps `lv_timer_handler()`
  responsive.
- **State machine:** `ST_IDLE → ST_RUNNING → ST_PAUSED → ST_DONE`.
  - Start: IDLE/PAUSED → RUNNING, motor on (only if count < target)
  - Pause: RUNNING → PAUSED, motor off, count held
  - Reset: → IDLE, motor off, count = 0
  - Reaching target: **hard stop** into DONE (no ramp-down — this is intended)
- Target is adjustable in steps of 10 via ± buttons and **locked while running**.

## GUI requirements (the spec)
- Inputs: Set Target Wrap Count, Start, Pause, Reset Count
- Displayed: Current Wrap Count, Target Wrap Count

## Build notes
- **LVGL config:** LVGL needs an `lv_conf.h`. This project builds with
  `-DLV_CONF_INCLUDE_SIMPLE` and `-I src`, so place `lv_conf.h` in `src/`.
  Required settings: 16-bit color depth (`LV_COLOR_DEPTH 16`) and
  `LV_FONT_MONTSERRAT_28 1` (the big count readout uses that font).
- Flash is 16MB on the confirmed hardware; `platformio.ini` sets
  `board_upload.flash_size = 16MB` and `board_build.partitions = default_16MB.csv`
  explicitly, since the underlying `esp32-s3-devkitc-1` board entry defaults
  to 8MB/no-PSRAM and does not match this module.

## Known open items (context for future work)
1. ~~Driver enable/direction pin~~ — **confirmed resolved.** On real hardware
   the driver moves on PWM alone; no enable/direction pin was needed.
2. ~~Display orientation/color~~ — **confirmed resolved.** `invert=true` and
   `setRotation(0)` are correct as configured; colors and orientation are right
   on real hardware. (The backlight pin was wrong, see Pin map — that's what
   caused an all-black screen, not orientation/inversion.)
3. ~~Numeric keypad for exact target entry~~ — **built.** Tapping the
   `lblTarget` readout (now clickable, with a tinted background as a tap
   affordance) opens a full-screen `lv_keyboard` (`LV_KEYBOARD_MODE_NUMBER`)
   bound to a textarea pre-filled with the current target. `LV_EVENT_READY`
   (the keyboard's checkmark) commits the typed value (clamped to
   `TARGET_MAX`); `LV_EVENT_CANCEL` (the X) discards it. Locked while running,
   same rule as the ± buttons. **Not yet re-verified on real hardware** — the
   round 240x240 panel is tight for the default keyboard layout; check button
   hit targets aren't clipped/overlapping before relying on it.
   **Still not built:** persistent target across power cycles
   (NVS/Preferences), ramp-down near target.
4. **Base operation confirmed working** on real hardware (display, touch,
   motor, wrap counting) as of this writing. `RUN_DUTY` has been raised to
   255 (full speed) from the original 200 default.
5. **Wrap counter over-counting (2+ per rotation), confirmed on real
   hardware.** First attempt (bumping the old time-since-last-edge
   `SW_LOCKOUT_MS` from 30ms to 100ms) didn't fully fix it — counts were
   still appearing well away from any real switch transition, pointing to
   continuous electrical noise from the PWM-driven motor (sharing ground,
   ~50us period at 20kHz) rather than plain contact bounce clustered right
   after real edges. Replaced with a stable-dwell confirm/release scheme
   (see Design rules above): `SW_LOCKOUT_MS` → `SW_CONFIRM_MS`, and counting
   moved from the ISR into `pollLimitSwitch()` in `loop()`. **Not yet
   re-verified on hardware as of this writing** — if noise still leaks
   through, the fallback is a hardware RC filter (e.g. 100nF cap
   GPIO17→GND, possibly with an external pull-up) at the switch.

## Tuning knobs
- `RUN_DUTY` (0–255) — winding speed
- `SW_CONFIRM_MS` (default 10; replaced `SW_LOCKOUT_MS` — see Known open
  items) — required stable dwell (low-to-confirm, high-to-rearm) in ms;
  raise it if noise still leaks through, lower it only if genuine wraps
  start being missed at this winding speed (<300 RPM)
- `PWM_FREQ_HZ` (20 kHz) — kept above audible range
