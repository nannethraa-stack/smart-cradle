/*
 * OV7675 Camera Basic Test
 * Verifies Vision Shield camera is working
 */

#include <Arduino_OV767X.h>

#define WIDTH 160
#define HEIGHT 120
#define BUFFER_SIZE (WIDTH * HEIGHT)

uint8_t frame[BUFFER_SIZE];

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("OV7675 Camera Test");

  if (!Camera.begin(QQVGA, GRAYSCALE, 15)) {
    Serial.println("ERROR: Camera init failed");
    Serial.println("Check Vision Shield connection");
    while (1) delay(100);
  }

  Serial.println("Camera initialized");
  Serial.println("Capturing test frame...");

  delay(200); // Auto-exposure settle

  Camera.readFrame(frame);

  Serial.print("Frame captured: ");
  Serial.print(BUFFER_SIZE);
  Serial.println(" bytes");

  // Print first 20 pixel values
  Serial.print("First 20 pixels: ");
  for (int i = 0; i < 20; i++) {
    Serial.print(frame[i]);
    Serial.print(" ");
  }
  Serial.println();

  // Check for non-zero pixels
  int nonZero = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (frame[i] > 0) nonZero++;
  }
  Serial.print("Non-zero pixels: ");
  Serial.print(nonZero);
  Serial.print(" / ");
  Serial.print(BUFFER_SIZE);
  Serial.println(" (should be > 1000 if camera sees something)");

  Camera.end();
  Serial.println("Test complete");
}

void loop() {
  // No continuous capture in test
  delay(1000);
}
