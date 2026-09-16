/*
 * MQ-137 Basic Test
 * Verifies analog gas sensor is readable
 */

const int MQ137_PIN = A0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("MQ-137 Test");
  Serial.println("Warm-up: 20-30 seconds required");
  Serial.println("ADC range: 0-4095 (12-bit)");
  Serial.println("Expected: 500-3000 in normal air");
  Serial.println("");
}

void loop() {
  int raw = analogRead(MQ137_PIN);
  float voltage = (raw / 4095.0) * 3.3;

  Serial.print("Raw ADC: ");
  Serial.print(raw);
  Serial.print(" | Voltage: ");
  Serial.print(voltage, 2);
  Serial.println(" V");

  if (raw <= 10) {
    Serial.println("WARNING: ADC stuck at 0 - check wiring");
  } else if (raw >= 4094) {
    Serial.println("WARNING: ADC stuck at max - check wiring");
  }

  delay(500);
}
