/*
 * Backlight pin diagnostic — ESP32-S3-Touch-LCD-1.28
 * -----------------------------------------------------------
 * Online sources disagree on which GPIO drives this board's backlight
 * (GPIO2 vs GPIO40) and this varies by production revision.
 *
 * Self-identifying blink pattern, no serial monitor needed:
 *   GPIO2  blinks ONCE, then a short pause
 *   GPIO40 blinks TWICE, then a longer pause
 *   ...repeats forever.
 * Count the flashes on the screen to see which pin is actually wired
 * to the backlight (count = 1 -> GPIO2, count = 2 -> GPIO40).
 *
 * Does NOT touch SPI/display init at all — isolates the backlight
 * circuit from any panel init problems.
 */

static const int BL_CANDIDATE_A = 2;   // per Waveshare wiki text -- blinks x1
static const int BL_CANDIDATE_B = 40;  // per demo DEV_Config.h   -- blinks x2

static const uint32_t BLINK_ON_MS  = 350;
static const uint32_t BLINK_OFF_MS = 350;
static const uint32_t GROUP_GAP_MS = 1500;

static void blinkPin(int pin, int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(pin, HIGH);
    delay(BLINK_ON_MS);
    digitalWrite(pin, LOW);
    delay(BLINK_OFF_MS);
  }
}

void setup() {
  pinMode(BL_CANDIDATE_A, OUTPUT);
  pinMode(BL_CANDIDATE_B, OUTPUT);
  digitalWrite(BL_CANDIDATE_A, LOW);
  digitalWrite(BL_CANDIDATE_B, LOW);
}

void loop() {
  blinkPin(BL_CANDIDATE_A, 1);   // GPIO2  -> 1 flash
  delay(GROUP_GAP_MS);
  blinkPin(BL_CANDIDATE_B, 2);   // GPIO40 -> 2 flashes
  delay(GROUP_GAP_MS * 2);
}
