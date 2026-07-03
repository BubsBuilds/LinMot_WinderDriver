/*
 * Hardware bring-up test — Motor Winding Tool
 * ESP32-S3-Touch-LCD-1.28 (Waveshare)
 * -----------------------------------------------------------
 * Purpose: verify USB serial + the two pins added for this project
 * BEFORE bringing LVGL/display/touch into the picture.
 *
 *   - Serial (115200)   -> confirms USB CDC enumerates and prints work
 *   - GPIO18 (motor PWM) -> toggled HIGH/LOW once a second; probe with a
 *                           multimeter/scope, or clip an LED+resistor
 *                           (anode -> GPIO18, cathode -> resistor -> GND)
 *   - GPIO17 (limit sw)  -> INPUT_PULLUP, printed every loop and on change;
 *                           short it to GND to simulate the switch closing
 *
 * This sketch does NOT touch the display/touch pins or LVGL at all, so a
 * failure here isolates itself to wiring/board bring-up, not GUI code.
 *
 * Build: Arduino IDE, board "ESP32S3 Dev Module" (or the Waveshare variant),
 * USB CDC On Boot: Enabled — same settings as the main sketch.
 */

static const int MOTOR_PWM_PIN = 18;
static const int LIMIT_SW_PIN  = 17;

static const uint32_t BLINK_PERIOD_MS = 1000;

static bool     ledState   = false;
static uint32_t lastBlinkMs = 0;
static int      lastSwitchState = -1; // force a print on the first read

void setup() {
  Serial.begin(115200);
  uint32_t bootStart = millis();
  while (!Serial && millis() - bootStart < 3000) {
    // wait briefly for USB CDC to enumerate, but don't hang forever
  }

  pinMode(MOTOR_PWM_PIN, OUTPUT);
  digitalWrite(MOTOR_PWM_PIN, LOW);

  pinMode(LIMIT_SW_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("=== Motor Winder hardware bring-up test ===");
  Serial.printf("MOTOR_PWM_PIN = GPIO%d (blinking every %lu ms)\n",
                MOTOR_PWM_PIN, (unsigned long)BLINK_PERIOD_MS);
  Serial.printf("LIMIT_SW_PIN  = GPIO%d (INPUT_PULLUP, short to GND to test)\n",
                LIMIT_SW_PIN);
  Serial.println("--------------------------------------------");
}

void loop() {
  uint32_t now = millis();

  // --- Blink the motor PWM pin so it can be probed/observed ---
  if (now - lastBlinkMs >= BLINK_PERIOD_MS) {
    lastBlinkMs = now;
    ledState = !ledState;
    digitalWrite(MOTOR_PWM_PIN, ledState ? HIGH : LOW);
    Serial.printf("[%8lu ms] GPIO%d -> %s\n",
                  (unsigned long)now, MOTOR_PWM_PIN, ledState ? "HIGH" : "LOW");
  }

  // --- Report the limit switch state whenever it changes ---
  int swState = digitalRead(LIMIT_SW_PIN);
  if (swState != lastSwitchState) {
    lastSwitchState = swState;
    Serial.printf("[%8lu ms] GPIO%d (limit switch) -> %s\n",
                  (unsigned long)now, LIMIT_SW_PIN,
                  swState == LOW ? "CLOSED (LOW)" : "OPEN (HIGH)");
  }

  delay(5);
}
