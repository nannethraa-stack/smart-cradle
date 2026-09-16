/*
 * HX711 Basic Test
 * Verifies load cell is responding and reading weight
 */

#include <HX711.h>

const int DT_PIN = D0;
const int SCK_PIN = D1;

HX711 scale;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("HX711 Test");

  scale.begin(DT_PIN, SCK_PIN);

  Serial.println("Wait 2 seconds for sensor to settle...");
  delay(2000);

  if (scale.is_ready()) {
    Serial.println("HX711 ready");
    long raw = scale.read();
    Serial.print("Raw reading: ");
    Serial.println(raw);
    Serial.println("If reading changes when you press on the load cell, it's working.");
  } else {
    Serial.println("ERROR: HX711 not responding");
    Serial.println("Check wiring: DT -> D0, SCK -> D1, VCC -> 5V, GND -> GND");
  }
}

void loop() {
  if (scale.is_ready()) {
    long raw = scale.read();
    Serial.print("Raw: ");
    Serial.println(raw);
  } else {
    Serial.println("HX711 not ready");
  }
  delay(500);
}
