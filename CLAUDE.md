# CLAUDE.md — Motor Winding Tool

Guidance for AI assistants working on this project. Read this before editing.

## What this is
A **motor winding tool** for making copper coil windings for a linear motor.
A 12VDC motor spins the winder; a touchscreen GUI sets a target wrap count,
runs/pauses the motor, and tracks completed wraps.

## Hardware
- **MCU/Display:** Waveshare **ESP32-S3-Touch-LCD-1.28** (1.28" round touchscreen dev board)
  - Display: **GC9A01**, 240×240 round, SPI
  - Touch: **CST816S**, capacitive, I2C
  - Onboard QMI8658 IMU shares the I2C bus — unused here
- **Motor:** 12VDC via a driver board that takes a **single PWM** control signal
- **Feedback:** Normally-Open limit switch — closes at 0° each rotation, reopens
  a few degrees later. One closure = one wrap.

### Pin map
Onboard (fixed by the board — do not reassign):
- Display SPI: SCLK=10, MOSI=11, DC=8, CS=9, RST=14, BL=40
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
- File layout: source lives in `src/` (`src/motor_winder.ino`).

## Design rules (please preserve)
- **ISR does one thing:** the limit-switch interrupt only increments a
  `volatile` wrap counter, with a ~30 ms debounce lockout (`SW_LOCKOUT_MS`) so
  one make-then-break doesn't double-count. Do **not** add motor control, LVGL
  calls, or blocking work inside the ISR.
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
- Flash size assumes 16MB; adjust in `platformio.ini` if your board differs.

## Known open items (context for future work)
1. **Driver enable/direction pin — verify.** The code assumes the driver moves
   on PWM alone. Many "single PWM" boards also need an **enable** or
   **direction** pin held high. If the driver model requires one, add it as a
   dedicated GPIO and drive it in `motorRun()`/`motorStop()`.
2. **Display orientation/color:** `invert=true` and `setRotation(0)` are typical
   for this board. Flip if colors are inverted or the image is mirrored.
3. **Possible enhancements (not yet built):** numeric keypad for exact target
   entry, persistent target across power cycles (NVS/Preferences), ramp-down
   near target.

## Tuning knobs
- `RUN_DUTY` (0–255) — winding speed
- `SW_LOCKOUT_MS` (default 30) — debounce window; lower it only if spinning
  faster than ~2000 RPM and counts are missed
- `PWM_FREQ_HZ` (20 kHz) — kept above audible range
