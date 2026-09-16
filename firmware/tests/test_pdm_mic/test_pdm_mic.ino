/*
 * PDM Microphone Basic Test
 * Verifies digital microphone is capturing audio
 */

#include <PDM.h>

const int SAMPLE_RATE = 16000;
const int BUFFER_SIZE = 1024;
short buffer[BUFFER_SIZE];

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("PDM Microphone Test");
  Serial.println("Make some noise near the microphone...");

  PDM.onReceive(onPDM);
  if (!PDM.begin(1, SAMPLE_RATE)) {
    Serial.println("ERROR: PDM microphone init failed");
    Serial.println("Check that Vision Shield is properly connected");
    while (1) delay(100);
  }
}

void loop() {
  // Main loop does nothing - PDM data arrives via interrupt
  delay(100);
}

void onPDM() {
  int available = PDM.available();
  int toRead = min(available, BUFFER_SIZE);
  PDM.read(buffer, toRead);

  // Calculate simple RMS
  long sumSq = 0;
  for (int i = 0; i < toRead; i++) {
    sumSq += (long)buffer[i] * buffer[i];
  }
  float rms = sqrt((float)sumSq / toRead);

  Serial.print("RMS: ");
  Serial.println(rms, 1);

  if (rms > 1000) {
    Serial.println("Loud sound detected!");
  }
}
