/*
 * MLX90640 Basic Test
 * Verifies thermal sensor is detected and can read temperatures
 */

#include <Wire.h>
#include <Adafruit_MLX90640.h>

Adafruit_MLX90640 mlx = Adafruit_MLX90640();

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("MLX90640 Test");

  if (!mlx.begin(0x33, &Wire)) {
    Serial.println("ERROR: MLX90640 not found at 0x33");
    Serial.println("Check wiring: SDA, SCL, 3.3V, GND");
    while (1) delay(100);
  }

  Serial.println("MLX90640 found!");
  mlx.setRefreshRate(MLX90640_8_HZ);
  Serial.println("Refresh rate: 8 Hz");

  Serial.println("Reading frame...");
  float frame[768];
  if (mlx.getFrame(frame) == 0) {
    Serial.println("Frame read OK");
    Serial.print("Pixel 0 (top-left): ");
    Serial.print(frame[0], 1);
    Serial.println(" C");
    Serial.print("Pixel 384 (center): ");
    Serial.print(frame[384], 1);
    Serial.println(" C");
  } else {
    Serial.println("ERROR: Failed to read frame");
  }
}

void loop() {
  float frame[768];
  if (mlx.getFrame(frame) == 0) {
    Serial.print("Center temp: ");
    Serial.print(frame[384], 1);
    Serial.println(" C");
  } else {
    Serial.println("Frame read failed");
  }
  delay(1000);
}
