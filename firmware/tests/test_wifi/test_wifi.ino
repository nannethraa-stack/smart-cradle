/*
 * WiFi Basic Test
 * Verifies Portenta H7 can connect to WiFi network
 */

#include <WiFi.h>

const char* SSID = "YOUR_WIFI_SSID";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("WiFi Test");
  Serial.print("Connecting to: ");
  Serial.println(SSID);

  WiFi.begin(SSID, PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println();
    Serial.println("ERROR: WiFi connection failed");
    Serial.println("Check SSID/password and signal strength");
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected!");
    delay(1000);
  }
  delay(5000);
}
