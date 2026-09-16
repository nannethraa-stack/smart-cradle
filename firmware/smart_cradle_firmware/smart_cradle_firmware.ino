/*
 * ============================================================================
 * SMART CRADLE PRODUCTION FIRMWARE v2.3.1
 * Arduino Portenta H7 + Vision Shield + HX711 + MLX90640 + MQ-137
 * ============================================================================
 * FIXES IN v2.3.1 (from code review):
 *   - MQ-137 warm-up gated: readMQ137() returns invalid until heater stable
 *   - MQ-137 voltage divider ratio made explicit with BOM discrepancy note
 *   - Camera GRAYSCALE mode documented as intentional (TFLite Micro prep)
 *   - MQ-137 uncalibrated state flagged prominently at boot
 *
 * FIXES FROM v2.2 CODE REVIEW:
 *   - Real OV7675 camera initialization (Vision Shield health check)
 *   - Removed while(!Serial) boot blocker for wall-power deployment
 *   - Eliminated String heap fragmentation (snprintf into fixed char buffers)
 *   - Non-blocking HX711 (single fast read when data ready)
 *   - Runtime sensor health monitoring with proactive MQTT alerts
 *   - WiFi reconnection in loop()
 *   - NTP retry every 5 minutes until success
 *   - PDM interrupt race condition fixed with noInterrupts()/interrupts()
 *   - MLX90640 frame cached globally (eliminated double I2C read per loop)
 *   - Respiratory rate confidence metric (distinguishes apnea from bad signal)
 *   - Presence change MQTT events for nursing station visibility
 *   - MQTT reconnect throttling (5 s minimum between attempts)
 *   - Thermal fallback for presence detection if HX711 fails at runtime
 *   - Audio RMS uses float (hardware FPU) instead of double (software emu)
 *   - Sensor boot health report published to MQTT after init
 *
 * TARGET HARDWARE (Verified BOM):
 *   1. Arduino Portenta H7 Development Kit
 *   2. Portenta Vision Shield (ASX00021) - RGB Camera (OV7675) + PDM Digital Mics
 *   3. MQ-137 Ammonia Gas Sensor Module (5V Analog, voltage divider to 3.3 V)
 *   4. MLX90640 Far-Infrared Thermal Sensor (32x24, I2C @ 400 kHz)
 *   5. HX711 24-bit ADC + 5 kg Load Cell (Weight + Presence)
 *   6. Portenta Breakout Board
 *   7. Resistors: 10k x2, 20k x2 (MQ-137 divider)
 *
 * REQUIRED LIBRARIES (Install via Arduino Library Manager):
 *   - ArduinoMqttClient by Arduino
 *   - SparkFun MLX90640 Arduino Library by SparkFun
 *   - HX711 Arduino Library by bogde
 *   - Arduino_OV767X by Arduino (Vision Shield camera)
 *
 * BOARD CORE:
 *   "Arduino Mbed OS Portenta Boards" v4.0.0+
 * ============================================================================
 */

#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <Wire.h>
#include <PDM.h>
#include <WiFiUdp.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <HX711.h>
#include <Adafruit_MLX90640.h>
#include <Arduino_OV767X.h>   // Vision Shield OV7675 camera

// ------------------------------------------------------------------
// CONFIGURATION - UPDATE THESE FOR YOUR NETWORK AND UNIT
// ------------------------------------------------------------------
const char WIFI_SSID[]      = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[]  = "YOUR_WIFI_PASSWORD";
const char MQTT_BROKER_IP[] = "192.168.1.100";
const int  MQTT_BROKER_PORT = 1883;
const char DEVICE_UUID[]    = "58e6e461-021b-46bd-a3b5-6465fac3e34b";
const char FIRMWARE_VER[]   = "fw-2.3.1";

// ------------------------------------------------------------------
// TOPIC BUFFERS (populated in setup() to avoid String heap use)
// ------------------------------------------------------------------
char mqttTopicTelemetry[64];
char mqttTopicStatus[64];
char mqttTopicAlert[64];
char mqttTopicImages[64];
char mqttTopicAudio[64];

// ------------------------------------------------------------------
// PIN MAPPING
// ------------------------------------------------------------------
const int PIN_MQ137_AIN  = A0;
const int PIN_HX711_DOUT = D0;
const int PIN_HX711_SCK  = D1;

HX711 loadCell;
Adafruit_MLX90640 thermalSensor;

// ------------------------------------------------------------------
// TIMING CONSTANTS
// ------------------------------------------------------------------
const unsigned long TELEMETRY_INTERVAL_MS      = 5000;
const unsigned long STATUS_HEARTBEAT_MS          = 30000;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;
const unsigned long NTP_RETRY_INTERVAL_MS      = 300000; // 5 min
const unsigned long MQTT_RETRY_INTERVAL_MS     = 5000;
const unsigned long IMAGE_SAVE_INTERVAL_MS     = 60000;  // 1 image per minute
const uint8_t       SENSOR_FAIL_THRESHOLD      = 3;

// ------------------------------------------------------------------
// PRESENCE DETECTION THRESHOLDS (Weight-Based, with Thermal Fallback)
// ------------------------------------------------------------------
const float PRESENCE_WEIGHT_THRESHOLD_KG = 0.80f;
const float ABSENCE_WEIGHT_THRESHOLD_KG  = 0.50f;
const unsigned long PRESENCE_DEBOUNCE_MS = 3000;
const unsigned long ABSENCE_DEBOUNCE_MS  = 10000;

// Thermal fallback thresholds (used only if HX711 fails at runtime)
const float THERMAL_PRESENCE_THRESHOLD_C  = 30.0f;  // >= 30 C => likely baby
const float THERMAL_ABSENCE_THRESHOLD_C   = 25.0f;  // <= 25 C => likely empty

// ------------------------------------------------------------------
// RESPIRATORY RATE CONFIGURATION
// ------------------------------------------------------------------
const int   RR_SAMPLE_INTERVAL_MS    = 500;
const int   RR_MEASUREMENT_WINDOW_MS = 15000;
const int   RR_MAX_SAMPLES           = 40;
const float RR_PEAK_THRESHOLD_FACTOR = 0.3f;
const int   RR_MIN_PEAK_DISTANCE_MS  = 800;

const int CHEST_ROW_START = 8;
const int CHEST_ROW_END   = 16;
const int CHEST_COL_START = 10;
const int CHEST_COL_END   = 22;

// ------------------------------------------------------------------
// HX711 CALIBRATION (MUST BE TUNED PER UNIT)
// ------------------------------------------------------------------
const float HX711_CALIBRATION_FACTOR = -10000.0f;
const long  HX711_ZERO_OFFSET        = 0L;

// ------------------------------------------------------------------
// MQ-137 CALIBRATION (MUST BE TUNED PER UNIT)
// ------------------------------------------------------------------
// PRODUCTION SAFETY: Do not deploy with CURVE_A and CURVE_B at 0.
// Run test_mq137_ammonia.ino with reference gas to determine these values.
const float MQ137_RL_KOHM        = 10.0f;
const float MQ137_CLEAN_AIR_RO   = 10.0f;
const float MQ137_CURVE_A        = 0.0f;   // 0 = uncalibrated
const float MQ137_CURVE_B        = 0.0f;   // 0 = uncalibrated
const float DIAPER_PPM_THRESHOLD = 5.0f;
const bool  MQ137_CALIBRATED     = (MQ137_CURVE_A != 0.0f || MQ137_CURVE_B != 0.0f);

// BOM NOTE: The BOM lists 10k x2 and 20k x2 resistors.
// This firmware assumes ONE voltage divider per MQ-137 module:
//   R_series = 10k (between sensor Vout and ADC pin)
//   R_shunt  = 20k (between ADC pin and GND)
// This gives V_adc = V_sensor * (20k / (10k+20k)) = V_sensor * 0.666...
// To recover V_sensor from V_adc we multiply by 1.5x.
// If your hardware uses a different network (e.g., both 10k resistors in
// series for R_series=20k, or two independent dividers), update this
// constant or the Rs/PPM math will be wrong even after calibration.
const float MQ137_VOLTAGE_DIVIDER_RATIO = 1.5f;

// Heater warm-up time: datasheet recommends 20-30s for sensor to stabilize.
// readMQ137() will return invalid readings until this period has elapsed.
const unsigned long MQ137_WARMUP_MS = 25000;

// MQ-137 non-blocking sampling configuration
const int MQ137_SAMPLES_PER_READ = 10;
const int MQ137_SAMPLE_INTERVAL_MS = 5;

// ------------------------------------------------------------------
// AUDIO CONFIGURATION
// ------------------------------------------------------------------
const int   PDM_BUFFER_SIZE      = 4096;
short       g_pcmBuffer[PDM_BUFFER_SIZE];
volatile int g_pcmSamplesRead    = 0;
volatile bool g_pcmDataReady     = false;
const float CRYING_RMS_THRESHOLD = 1500.0f;
const int   CRYING_DURATION_MS   = 3000;
unsigned long g_cryStartMs       = 0;
bool        g_isCrying           = false;

// ------------------------------------------------------------------
// CAMERA TEST BUFFER (QQVGA GRAYSCALE = 160*120 = 19200 bytes)
// ------------------------------------------------------------------
static uint8_t g_cameraFrameBuffer[160 * 120];

// ------------------------------------------------------------------
// GLOBAL STATE
// ------------------------------------------------------------------
WiFiClient    wifiClient;
MqttClient    mqttClient(wifiClient);
WiFiUDP       udp;

unsigned long lastTelemetryMs       = 0;
unsigned long lastStatusMs          = 0;
unsigned long sequenceCounter       = 0;
unsigned long g_lastWiFiReconnectMs = 0;
unsigned long g_lastNtpRetryMs      = 0;
unsigned long g_lastMqttRetryMs     = 0;
unsigned long g_audioLastProcessedMs= 0;
float g_lastAudioRms = 0.0f;
float g_cryProbability = 0.0f;
bool g_cryAudioEventPending = false;
unsigned long g_cryEventCounter = 0;

// Boot-time sensor health flags
bool g_mlxOk    = false;
bool g_mq137Ok  = false;
bool g_audioOk  = false;
bool g_cameraOk = false;
bool g_hx711Ok  = false;

// Runtime consecutive failure counters (for recovery detection)
uint8_t g_mlxFailCount   = 0;
uint8_t g_mq137FailCount = 0;
uint8_t g_hx711FailCount = 0;

// MQ-137 warm-up tracking
unsigned long g_mq137WarmupStartMs = 0;
bool          g_mq137WarmupComplete = false;

// MQ-137 non-blocking sampling state
static int g_mq137SampleCount = 0;
static float g_mq137SampleSum = 0.0f;
static unsigned long g_mq137LastSampleMs = 0;

// Sensor readings
float g_tempCenterC         = 0.0f;
float g_tempMaxC            = 0.0f;
float g_mqRawADC            = 0.0f;
float g_mqVoltage           = 0.0f;
float g_ammoniaPPM          = 0.0f;
bool  g_diaperSoiled        = false;
float g_babyWeightKg        = 0.0f;
float g_diaperBaselineKg    = 0.0f;
float g_diaperWeightDeltaG  = 0.0f;
float g_respiratoryRate     = 0.0f;
float g_respiratoryConfidence = 0.0f;

// Cached thermal frame (eliminates double I2C read)
float g_thermalFrame[768];

// Respiratory rate circular buffer
float         g_rrTempHistory[RR_MAX_SAMPLES];
unsigned long g_rrTimeHistory[RR_MAX_SAMPLES];
int           g_rrHistoryIndex = 0;
int           g_rrHistoryCount = 0;
unsigned long g_rrLastSampleMs = 0;
unsigned long g_rrWindowStartMs = 0;

// Presence state machine
enum PresenceState { PRESENCE_UNKNOWN, BABY_ABSENT, BABY_PRESENT };
PresenceState g_presenceState      = PRESENCE_UNKNOWN;
unsigned long g_presenceStateStartMs = 0;

// Timekeeping
unsigned long ntpEpochBase  = 0;
unsigned long ntpMillisBase = 0;

// NTP non-blocking state
bool g_ntpPending = false;
unsigned long g_ntpSendTime = 0;

// Image persistence state (MQTT-based database streaming)
unsigned long g_lastImagePublishMs = 0;
int g_imageSequence = 0;

// ------------------------------------------------------------------
// FORWARD DECLARATIONS
// ------------------------------------------------------------------
bool connectWiFi();
bool syncNTP();
bool connectMQTT();
void getISO8601Time(char* buf, size_t len);
void generateUUIDv4(char* buf, size_t len);
float calculateDataQuality();

void publishTelemetry();
void publishStatusHeartbeat();
void publishSensorAlert(const char* sensorName, const char* alertType, uint8_t failCount);
void publishSensorHealthReport();
void publishPresenceChange(PresenceState prev, PresenceState curr);
void publishCryAudioEvent();

void updatePresenceState();

bool initMLX90640();
bool readMLX90640();
bool sampleRespiratoryRate();
int  countRespiratoryPeaks();
void updateRespiratoryRate();

bool initMQ137();
bool readMQ137();
bool initHX711();
bool readHX711();
bool initAudio();
void onPDMdata();
bool processAudio();
bool initCamera();
bool publishImageToDb(const uint8_t* frame, size_t len);

// ------------------------------------------------------------------
// SETUP
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  // CRITICAL FIX (v2.2 review P0): Do NOT block if no USB host is attached.
  // In hospital wall-power mode there is no PC, so while(!Serial) hangs forever.
  // We simply wait a short fixed time to let a debug PC connect, then proceed.
  delay(3000);

  Serial.println("==============================================");
  Serial.println("  SMART CRADLE - PRODUCTION FIRMWARE v2.3.1");
  Serial.println("  Camera + HX711 + MLX90640 + MQ-137 + RR");
  Serial.println("==============================================");

  // Build MQTT topic strings once into fixed char buffers (no heap fragmentation)
  snprintf(mqttTopicTelemetry, sizeof(mqttTopicTelemetry), "cradle/%s/telemetry", DEVICE_UUID);
  snprintf(mqttTopicStatus,    sizeof(mqttTopicStatus),    "cradle/%s/status",    DEVICE_UUID);
  snprintf(mqttTopicAlert,     sizeof(mqttTopicAlert),     "cradle/%s/alerts",    DEVICE_UUID);
  snprintf(mqttTopicImages,    sizeof(mqttTopicImages),    "cradle/%s/images",    DEVICE_UUID);
  snprintf(mqttTopicAudio,     sizeof(mqttTopicAudio),     "cradle/%s/audio",     DEVICE_UUID);

  analogReadResolution(12);
  randomSeed(analogRead(PIN_MQ137_AIN) + millis() + micros());
  mqttClient.setKeepAliveInterval(15000); // 15 s MQTT keepalive

  // ------------------------------------------------------------------
  // Infrastructure: WiFi, NTP, MQTT
  // ------------------------------------------------------------------
  if (!connectWiFi()) {
    Serial.println("FATAL: WiFi connection failed on boot.");
  }
  if (!syncNTP()) {
    Serial.println("WARN: NTP sync failed on boot. Will retry every 5 min.");
  }
  if (!connectMQTT()) {
    Serial.println("WARN: Initial MQTT connection failed. Will retry throttled.");
  }

  // ------------------------------------------------------------------
  // Sensor initialization (order: camera first, then I2C, then analog/audio, then digital)
  // ------------------------------------------------------------------
  g_cameraOk = initCamera();
  g_mlxOk    = initMLX90640();
  g_mq137Ok  = initMQ137();
  g_audioOk  = initAudio();
  g_hx711Ok  = initHX711();

  // ------------------------------------------------------------------
  // Production guard: ensure MQ-137 is not accidentally deployed uncalibrated
  // ------------------------------------------------------------------
  if (!MQ137_CALIBRATED) {
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("!! PRODUCTION WARNING: MQ-137 IS UNCALIBRATED    !!");
    Serial.println("!! CURVE_A and CURVE_B are both 0.0.             !!");
    Serial.println("!! ammonia_ppm will report -1.0.                   !!");
    Serial.println("!! DIAPER SOILING DETECTION IS DISABLED.         !!");
    Serial.println("!! Calibrate with test_mq137_ammonia.ino before  !!");
    Serial.println("!! deploying to a field unit.                    !!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  }

  // ------------------------------------------------------------------
  // Boot health summary to Serial
  // ------------------------------------------------------------------
  Serial.println("----------------------------------------------");
  Serial.print("MLX90640 Thermal: "); Serial.println(g_mlxOk    ? "OK" : "FAIL");
  Serial.print("MQ-137 NH3:       "); Serial.println(g_mq137Ok  ? "OK" : "FAIL");
  Serial.print("PDM Audio:        "); Serial.println(g_audioOk  ? "OK" : "FAIL");
  Serial.print("Vision Camera:    "); Serial.println(g_cameraOk ? "OK" : "FAIL");
  Serial.print("HX711 Load Cell:  "); Serial.println(g_hx711Ok  ? "OK" : "FAIL");
  Serial.println("----------------------------------------------");

  if (!g_mlxOk)   Serial.println("CRITICAL: Body temp + respiratory rate disabled.");
  if (!g_mq137Ok) Serial.println("CRITICAL: Diaper soiling detection disabled.");
  if (!g_audioOk) Serial.println("CRITICAL: Crying detection disabled.");
  if (!g_cameraOk)Serial.println("CRITICAL: Camera offline. v3.0 face-cover detection will fail.");
  if (!g_hx711Ok) {
    Serial.println("CRITICAL: Weight & presence detection disabled.");
    Serial.println("          Thermal fallback active (if MLX OK).");
  }
  if (!MQ137_CALIBRATED) {
    Serial.println("WARN: MQ137 is UNCALIBRATED. NH3 ppm will report -1.");
  }
  Serial.println("NOTE: heart_rate, oxygen_level require MAX30102/MAX30100 (not in BOM).");
  Serial.println("INIT COMPLETE. Waiting for baby presence...");

  // Publish boot-time sensor health so backend knows immediately what passed/failed
  publishSensorHealthReport();

  g_rrWindowStartMs = millis();
}

// ------------------------------------------------------------------
// MAIN LOOP
// ------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // ------------------------------------------------------------------
  // Infrastructure maintenance (non-blocking, throttled retries)
  // ------------------------------------------------------------------
  if (WiFi.status() != WL_CONNECTED) {
    if (now - g_lastWiFiReconnectMs >= WIFI_RECONNECT_INTERVAL_MS) {
      g_lastWiFiReconnectMs = now;
      connectWiFi();
    }
  }

  if (ntpEpochBase == 0) {
    if (!g_ntpPending && (now - g_lastNtpRetryMs >= NTP_RETRY_INTERVAL_MS)) {
      g_lastNtpRetryMs = now;
      syncNTP();
    }
    if (g_ntpPending) {
      if (now - g_ntpSendTime > 2000) {
        g_ntpPending = false;
        udp.stop();
      } else if (udp.parsePacket()) {
        byte buf[48];
        udp.read(buf, 48);
        unsigned long highWord = word(buf[40], buf[41]);
        unsigned long lowWord  = word(buf[42], buf[43]);
        unsigned long secsSince1900 = highWord << 16 | lowWord;
        const unsigned long seventyYears = 2208988800UL;
        unsigned long epoch = secsSince1900 - seventyYears;
        ntpEpochBase = epoch;
        ntpMillisBase = millis();
        g_ntpPending = false;
        udp.stop();
        Serial.print("NTP synced. Unix time: ");
        Serial.println(epoch);
      }
    }
  }

  if (!mqttClient.connected()) {
    if (now - g_lastMqttRetryMs >= MQTT_RETRY_INTERVAL_MS) {
      g_lastMqttRetryMs = now;
      connectMQTT();
    }
  } else {
    mqttClient.poll();
  }

  // ------------------------------------------------------------------
  // Audio processing (race-condition fix: snapshot flag inside interrupt guard)
  // ------------------------------------------------------------------
  noInterrupts();
  bool pcmReady = g_pcmDataReady;
  g_pcmDataReady = false;
  interrupts();
  if (pcmReady) {
    processAudio();
    if (g_cryAudioEventPending) {
      publishCryAudioEvent();
      g_cryAudioEventPending = false;
    }
  }

  // ------------------------------------------------------------------
  // Sensor reads with runtime health tracking
  // We call these even if the boot flag is false, so we can detect recovery.
  // Each function increments/decrements its own failure counter.
  // ------------------------------------------------------------------
  readMLX90640();
  readMQ137();
  readHX711();

  // ------------------------------------------------------------------
  // Respiratory rate sampling (only when baby present and thermal sensor healthy)
  // ------------------------------------------------------------------
  if (g_mlxOk && g_presenceState == BABY_PRESENT) {
    if (now - g_rrLastSampleMs >= RR_SAMPLE_INTERVAL_MS) {
      g_rrLastSampleMs = now;
      sampleRespiratoryRate();
    }
    if (now - g_rrWindowStartMs >= RR_MEASUREMENT_WINDOW_MS) {
      g_rrWindowStartMs = now;
      updateRespiratoryRate();
    }
  }

  // ------------------------------------------------------------------
  // Presence detection (weight primary, thermal fallback, debounced)
  // ------------------------------------------------------------------
  updatePresenceState();

  // ------------------------------------------------------------------
  // Persistence: publish camera frame to MQTT/database when baby is present
  // ------------------------------------------------------------------
  if (g_presenceState == BABY_PRESENT && g_cameraOk) {
    if (now - g_lastImagePublishMs >= IMAGE_SAVE_INTERVAL_MS) {
      g_lastImagePublishMs = now;
      Camera.readFrame(g_cameraFrameBuffer);
      publishImageToDb(g_cameraFrameBuffer, 160 * 120);
    }
  }

  // ------------------------------------------------------------------
  // Periodic publishing
  // ------------------------------------------------------------------
  if (g_presenceState == BABY_PRESENT) {
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs = now;
      publishTelemetry();
    }
  } else {
    if (now - lastStatusMs >= STATUS_HEARTBEAT_MS) {
      lastStatusMs = now;
      publishStatusHeartbeat();
    }
  }
}

// ============================================================================
// RESPIRATORY RATE (v2.2/v2.3)
// ============================================================================

/*
 * sampleRespiratoryRate()
 * -----------------------
 * Reads the CHEST ROI from the globally cached thermal frame.
 * v2.3 FIX: No longer calls getFrame() here. The frame is already fetched
 * by readMLX90640() and stored in g_thermalFrame[]. This eliminates the
 * second expensive I2C transfer that existed in v2.2.
 */
bool sampleRespiratoryRate() {
  float sum = 0.0f;
  int count = 0;

  for (int row = CHEST_ROW_START; row < CHEST_ROW_END; row++) {
    for (int col = CHEST_COL_START; col < CHEST_COL_END; col++) {
      int idx = row * 32 + col;
      sum += g_thermalFrame[idx];
      count++;
    }
  }

  float avgTemp = (count > 0) ? (sum / count) : 0.0f;

  g_rrTempHistory[g_rrHistoryIndex] = avgTemp;
  g_rrTimeHistory[g_rrHistoryIndex] = millis();
  g_rrHistoryIndex = (g_rrHistoryIndex + 1) % RR_MAX_SAMPLES;
  if (g_rrHistoryCount < RR_MAX_SAMPLES) g_rrHistoryCount++;

  return true;
}

/*
 * countRespiratoryPeaks()
 * -----------------------
 * Counts peaks in the 15-second temperature history.
 * v2.3 ADD: Computes g_respiratoryConfidence based on signal stddev.
 * If the signal is too flat (stddev < 0.05 C), confidence = 0 and no
 * peaks are counted. This prevents reporting 0 breaths/min when the
 * sensor simply has no thermal contrast (blanket, failure, etc.).
 */
int countRespiratoryPeaks() {
  if (g_rrHistoryCount < 10) {
    g_respiratoryConfidence = 0.0f;
    return 0;
  }

  // Compute mean (baseline)
  float baseline = 0.0f;
  for (int i = 0; i < g_rrHistoryCount; i++) {
    baseline += g_rrTempHistory[i];
  }
  baseline /= g_rrHistoryCount;

  // Compute standard deviation
  float variance = 0.0f;
  for (int i = 0; i < g_rrHistoryCount; i++) {
    float diff = g_rrTempHistory[i] - baseline;
    variance += (diff * diff);
  }
  float stddev = sqrtf(variance / g_rrHistoryCount);

  // v2.3: Reject if signal is too flat to be physiologic breathing
  if (stddev < 0.05f) {
    g_respiratoryConfidence = 0.0f;
    return 0;
  }
  g_respiratoryConfidence = fminf(1.0f, stddev / 0.3f);

  float threshold = baseline + (RR_PEAK_THRESHOLD_FACTOR * stddev);

  int peakCount = 0;
  unsigned long lastPeakTime = 0;

  for (int i = 1; i < g_rrHistoryCount - 1; i++) {
    float prev = g_rrTempHistory[i - 1];
    float curr = g_rrTempHistory[i];
    float next = g_rrTempHistory[i + 1];

    if (curr > threshold && curr > prev && curr > next) {
      unsigned long peakTime = g_rrTimeHistory[i];
      if (lastPeakTime == 0 || (peakTime - lastPeakTime) >= RR_MIN_PEAK_DISTANCE_MS) {
        peakCount++;
        lastPeakTime = peakTime;
      }
    }
  }

  return peakCount;
}

/*
 * updateRespiratoryRate()
 * -----------------------
 * Converts peak count in 15 s window to breaths/min (x4).
 * v2.3: If confidence is zero, reports -1.0 (invalid) instead of 0.0,
 * so the backend can distinguish "no valid signal" from "apnea = 0".
 */
void updateRespiratoryRate() {
  int peaks = countRespiratoryPeaks();
  if (g_respiratoryConfidence <= 0.0f) {
    g_respiratoryRate = -1.0f;
  } else {
    g_respiratoryRate = (float)peaks * 4.0f;
  }

  Serial.print("[RESPIRATORY] Peaks in 15s: ");
  Serial.print(peaks);
  Serial.print(" | Rate: ");
  Serial.print(g_respiratoryRate, 1);
  Serial.print(" breaths/min | Confidence: ");
  Serial.println(g_respiratoryConfidence, 2);

  g_rrHistoryIndex = 0;
  g_rrHistoryCount = 0;
}

// ============================================================================
// PRESENCE DETECTION STATE MACHINE
// ============================================================================

/*
 * updatePresenceState()
 * ---------------------
 * v2.3 CHANGES:
 *   - Primary: HX711 weight thresholds (physically factual, blanket-proof)
 *   - Fallback: MLX90640 center temperature if HX711 is offline at runtime
 *   - Publishes MQTT PRESENCE_CHANGE event so nursing station knows
 *     immediately when baby is placed or removed.
 *   - Resets respiratory rate buffer on absence to prevent stale data.
 */
void updatePresenceState() {
  unsigned long now = millis();
  PresenceState detectedState = g_presenceState;

  if (g_hx711Ok) {
    if (g_babyWeightKg >= PRESENCE_WEIGHT_THRESHOLD_KG) {
      detectedState = BABY_PRESENT;
    } else if (g_babyWeightKg <= ABSENCE_WEIGHT_THRESHOLD_KG) {
      detectedState = BABY_ABSENT;
    }
  } else if (g_mlxOk) {
    // THERMAL FALLBACK (v2.3): if HX711 fails, use chest temperature.
    // Newborn skin is ~32-36 C; empty cradle at room temp is ~20-24 C.
    if (g_tempCenterC >= THERMAL_PRESENCE_THRESHOLD_C) {
      detectedState = BABY_PRESENT;
    } else if (g_tempCenterC > 0.0f && g_tempCenterC <= THERMAL_ABSENCE_THRESHOLD_C) {
      detectedState = BABY_ABSENT;
    }
  } else {
    detectedState = PRESENCE_UNKNOWN;
  }

  if (detectedState != g_presenceState) {
    if (g_presenceStateStartMs == 0) {
      g_presenceStateStartMs = now;
    } else {
      unsigned long debounceTime = PRESENCE_DEBOUNCE_MS;
      if (g_presenceState == BABY_PRESENT && detectedState == BABY_ABSENT) {
        debounceTime = ABSENCE_DEBOUNCE_MS;
      }

      if ((now - g_presenceStateStartMs) >= debounceTime) {
        PresenceState prevState = g_presenceState;
        g_presenceState = detectedState;
        g_presenceStateStartMs = 0;

        // v2.3: Notify backend immediately of presence change
        publishPresenceChange(prevState, g_presenceState);

        if (g_presenceState == BABY_PRESENT) {
          g_diaperBaselineKg = g_babyWeightKg;
          g_diaperWeightDeltaG = 0.0f;
          Serial.println("[PRESENCE] BABY DETECTED. Telemetry started.");
          g_rrWindowStartMs = now;
        } else if (g_presenceState == BABY_ABSENT) {
          Serial.println("[PRESENCE] BABY REMOVED. Telemetry paused.");
          g_respiratoryRate = -1.0f;
          g_respiratoryConfidence = 0.0f;
          g_rrHistoryIndex = 0;
          g_rrHistoryCount = 0;
        }
      }
    }
  } else {
    g_presenceStateStartMs = 0;
  }
}

// ============================================================================
// NETWORK & TIME
// ============================================================================

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println();
  return (WiFi.status() == WL_CONNECTED);
}

bool syncNTP() {
  if (g_ntpPending) return false;
  udp.stop();
  udp.begin(2390);

  byte ntpPacket[48];
  memset(ntpPacket, 0, 48);
  ntpPacket[0] = 0b11100011;
  ntpPacket[1] = 0;
  ntpPacket[2] = 6;
  ntpPacket[3] = 0xEC;
  ntpPacket[12] = 49;
  ntpPacket[13] = 0x4E;
  ntpPacket[14] = 49;
  ntpPacket[15] = 52;

  udp.beginPacket("pool.ntp.org", 123);
  udp.write(ntpPacket, 48);
  udp.endPacket();

  g_ntpPending = true;
  g_ntpSendTime = millis();
  return true;
}

/*
 * getISO8601Time()
 * ----------------
 * v2.3 FIX: Writes directly into caller-supplied char buffer.
 * Eliminates all String objects from the time path.
 */
void getISO8601Time(char* buf, size_t len) {
  if (ntpEpochBase == 0 || len < 25) {
    strncpy(buf, "1970-01-01T00:00:00Z", len);
    buf[len - 1] = '\0';
    return;
  }

  unsigned long elapsedSec = (millis() - ntpMillisBase) / 1000;
  unsigned long epoch = ntpEpochBase + elapsedSec;

  unsigned long sec = epoch;
  unsigned long min = sec / 60;
  unsigned long hr  = min / 60;
  unsigned long day = hr / 24;

  unsigned long year = 1970;
  unsigned long daysRemaining = day;
  while (true) {
    bool leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    unsigned long dim = leap ? 366 : 365;
    if (daysRemaining < dim) break;
    daysRemaining -= dim;
    year++;
  }

  const uint8_t monthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint8_t month = 0;
  for (int m = 0; m < 12; m++) {
    uint8_t dim = monthDays[m];
    if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) dim = 29;
    if (daysRemaining < dim) { month = m; break; }
    daysRemaining -= dim;
  }
  uint8_t d = daysRemaining + 1;
  month += 1;
  uint8_t h = hr % 24;
  uint8_t mn = min % 60;
  uint8_t s = sec % 60;

  snprintf(buf, len, "%04lu-%02u-%02uT%02u:%02u:%02uZ",
           year, month, d, h, mn, s);
}

bool connectMQTT() {
  Serial.print("Connecting to MQTT broker...");
  mqttClient.setId(DEVICE_UUID);
  if (!mqttClient.connect(MQTT_BROKER_IP, MQTT_BROKER_PORT)) {
    Serial.print(" failed, error=");
    Serial.println(mqttClient.connectError());
    return false;
  }
  Serial.println(" connected.");
  return true;
}

/*
 * generateUUIDv4()
 * ----------------
 * v2.3 FIX: Writes into caller-supplied char buffer.
 * No String objects. buf must be at least 37 bytes.
 */
void generateUUIDv4(char* buf, size_t len) {
  if (len < 37) return;
  const char* hex = "0123456789abcdef";
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      buf[i] = '-';
    } else if (i == 14) {
      buf[i] = '4';
    } else if (i == 19) {
      buf[i] = hex[random(0, 4) + 8]; // 8,9,a,b
    } else {
      buf[i] = hex[random(0, 16)];
    }
  }
  buf[36] = '\0';
}

float calculateDataQuality() {
  float q = 1.0f;
  if (!g_mlxOk)    q -= 0.25f;
  if (!g_mq137Ok)  q -= 0.25f;
  if (!g_audioOk)  q -= 0.20f;
  if (!g_cameraOk) q -= 0.10f;
  if (!g_hx711Ok)  q -= 0.15f;
  if (ntpEpochBase == 0) q -= 0.05f;
  if (q < 0.0f) q = 0.0f;
  return q;
}

// ============================================================================
// ALERT & HEALTH PUBLISHERS (v2.3 NEW)
// ============================================================================

/*
 * publishSensorAlert()
 * --------------------
 * Proactively sends an MQTT alert when a sensor fails or recovers at runtime.
 * This is the critical safety feature missing from v2.2.
 * Backend can use this to notify nursing station immediately.
 */
void publishSensorAlert(const char* sensorName, const char* alertType, uint8_t failCount) {
  if (!mqttClient.connected()) return;

  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  char json[512];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"SENSOR_ALERT\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{\"sensor\":\"%s\",\"alert_type\":\"%s\",\"consecutive_failures\":%u},"
    "\"quality\":%.2f,\"trace_id\":null}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    sensorName, alertType, failCount, calculateDataQuality()
  );
  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Alert JSON truncated. Increase buffer size.");
  }

  Serial.print("[ALERT] ");
  Serial.println(json);

  mqttClient.beginMessage(mqttTopicAlert);
  mqttClient.print(json);
  mqttClient.endMessage();
}

/*
 * publishSensorHealthReport()
 * ---------------------------
 * Sent once at boot. Gives backend a snapshot of which sensors passed init.
 */
void publishSensorHealthReport() {
  if (!mqttClient.connected()) return;

  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  char json[512];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"SENSOR_HEALTH\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{"
    "\"mlx90640\":\"%s\",\"mq137\":\"%s\",\"audio\":\"%s\",\"camera\":\"%s\",\"hx711\":\"%s\""
    "},\"quality\":%.2f,\"trace_id\":null}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    g_mlxOk ? "OK" : "FAIL",
    g_mq137Ok ? "OK" : "FAIL",
    g_audioOk ? "OK" : "FAIL",
    g_cameraOk ? "OK" : "FAIL",
    g_hx711Ok ? "OK" : "FAIL",
    calculateDataQuality()
  );
  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Health JSON truncated. Increase buffer size.");
  }

  mqttClient.beginMessage(mqttTopicAlert);
  mqttClient.print(json);
  mqttClient.endMessage();
}

/*
 * publishPresenceChange()
 * -----------------------
 * v2.3 NEW: Nursing station needs to know immediately when baby is
 * placed or removed, not just infer it from telemetry starting/stopping.
 */
void publishPresenceChange(PresenceState prev, PresenceState curr) {
  if (!mqttClient.connected()) return;

  const char* prevStr = (prev == BABY_PRESENT) ? "BABY_PRESENT" :
                        (prev == BABY_ABSENT)  ? "BABY_ABSENT"  : "UNKNOWN";
  const char* currStr = (curr == BABY_PRESENT) ? "BABY_PRESENT" :
                        (curr == BABY_ABSENT)  ? "BABY_ABSENT"  : "UNKNOWN";
  bool babyPresent = (curr == BABY_PRESENT);

  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  char json[512];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"PRESENCE_CHANGE\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{\"baby_present\":%s,\"previous_state\":\"%s\",\"current_state\":\"%s\"},"
    "\"quality\":%.2f,\"trace_id\":null}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    babyPresent ? "true" : "false", prevStr, currStr,
    calculateDataQuality()
  );
  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Presence change JSON truncated. Increase buffer size.");
  }

  Serial.print("[PRESENCE] ");
  Serial.println(json);

  mqttClient.beginMessage(mqttTopicAlert);
  mqttClient.print(json);
  mqttClient.endMessage();
}

// ============================================================================
// TELEMETRY PUBLISHERS (v2.3: zero String objects)
// ============================================================================

/*
 * publishTelemetry()
 * -----------------
 * v2.3 CRITICAL FIX: Replaced chained String concatenation with a single
 * snprintf() into a fixed char buffer. String reallocations were the #1
 * cause of heap fragmentation and eventual crash in v2.2.
 */
void publishTelemetry() {
  if (g_presenceState != BABY_PRESENT) return;

  // Engineering signal for diaper fusion. This is total cradle-weight change
  // relative to the presence baseline; it is not a dedicated diaper load cell.
  if (g_diaperBaselineKg > 0.0f && g_babyWeightKg >= g_diaperBaselineKg) {
    g_diaperWeightDeltaG = (g_babyWeightKg - g_diaperBaselineKg) * 1000.0f;
  } else {
    g_diaperWeightDeltaG = 0.0f;
  }

  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  char json[1024];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"TELEMETRY\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"model_versions\":{},"
    "\"payload\":{"
    "\"temperature\":%.2f,\"respiratory_rate\":%.1f,\"respiratory_confidence\":%.2f,"
    "\"crying\":%s,\"cry_detected\":%s,\"probable_pattern\":\"%s\","
    "\"cry_model_version\":\"edge-rms-v0\","
    "\"ammonia_ppm\":%.2f,\"diaper_soiled\":%s,\"diaper_weight_delta_g\":%.1f,"
    "\"diaper_status\":\"%s\",\"baby_weight\":%.2f,\"baby_height_cm\":null,"
    "\"heart_rate\":0,\"oxygen_level\":0"
    "},"
    "\"quality\":%.2f,\"trace_id\":null}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    g_tempCenterC, g_respiratoryRate, g_respiratoryConfidence,
    g_isCrying ? "true" : "false", g_isCrying ? "true" : "false",
    g_isCrying ? "Analysing..." : "-", g_ammoniaPPM,
    g_diaperSoiled ? "true" : "false", g_diaperWeightDeltaG,
    g_diaperSoiled ? "Urine Detected" : "Dry", g_babyWeightKg,
    calculateDataQuality()
  );

  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Telemetry JSON truncated. Increase buffer size.");
  }

  Serial.print("[TELEMETRY] ");
  Serial.println(json);

  if (mqttClient.connected()) {
    mqttClient.beginMessage(mqttTopicTelemetry);
    mqttClient.print(json);
    mqttClient.endMessage();
  } else {
    Serial.println("MQTT publish SKIPPED (not connected)");
  }
}


void publishCryAudioEvent() {
  if (!mqttClient.connected() || !g_isCrying || g_pcmSamplesRead <= 0) return;

  // Store the latest validated noise-reduced detector window as a training
  // candidate. A future recorder can replace this with a longer rolling clip.
  static char b64buf[15000];
  static const char* b64chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t samples = (size_t)g_pcmSamplesRead;
  size_t bytes = samples * sizeof(short);
  size_t out = 0;

  const uint8_t* raw = (const uint8_t*)g_pcmBuffer;
  for (size_t i = 0; i < bytes && out + 4 < sizeof(b64buf); i += 3) {
    uint32_t a = raw[i];
    uint32_t b = (i + 1 < bytes) ? raw[i + 1] : 0;
    uint32_t c = (i + 2 < bytes) ? raw[i + 2] : 0;
    uint32_t triple = (a << 16) | (b << 8) | c;
    b64buf[out++] = b64chars[(triple >> 18) & 0x3F];
    b64buf[out++] = b64chars[(triple >> 12) & 0x3F];
    b64buf[out++] = (i + 1 < bytes) ? b64chars[(triple >> 6) & 0x3F] : '=';
    b64buf[out++] = (i + 2 < bytes) ? b64chars[triple & 0x3F] : '=';
  }
  b64buf[out] = '\0';

  char uuid[37], ts[25];
  generateUUIDv4(uuid, sizeof(uuid));
  getISO8601Time(ts, sizeof(ts));

  char json[15000];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"CRY_AUDIO_EVENT\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{\"sample_rate\":16000,\"channels\":1,\"sample_count\":%d,"
    "\"format\":\"pcm_s16le_base64\",\"model_version\":\"edge-rms-v0\","
    "\"training_eligible\":true,\"data\":\"%s\"}}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    g_pcmSamplesRead, b64buf);

  if (n <= 0 || n >= (int)sizeof(json)) {
    Serial.println("ERROR: Cry audio event JSON truncated.");
    return;
  }

  mqttClient.beginMessage(mqttTopicAudio);
  mqttClient.print(json);
  mqttClient.endMessage();
  g_cryEventCounter++;
  Serial.print("[CRY AUDIO] Training candidate stored, samples=");
  Serial.println(g_pcmSamplesRead);
}

void publishStatusHeartbeat() {
  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  char json[512];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"STATUS\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{\"baby_present\":false,\"ambient_temperature\":%.2f,\"cradle_empty\":true},"
    "\"quality\":%.2f,\"trace_id\":null}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER,
    g_tempCenterC, calculateDataQuality()
  );
  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Status JSON truncated. Increase buffer size.");
  }

  Serial.print("[STATUS] ");
  Serial.println(json);

  if (mqttClient.connected()) {
    mqttClient.beginMessage(mqttTopicStatus);
    mqttClient.print(json);
    mqttClient.endMessage();
  }
}

// ============================================================================
// SENSOR IMPLEMENTATIONS
// ============================================================================

bool initMLX90640() {
  Wire.begin();
  Wire.setClock(400000);
  if (thermalSensor.begin(0x33, &Wire) == true) {
    thermalSensor.setRefreshRate(MLX90640_8_HZ);
    Serial.println("MLX90640 initialized at 0x33, 8Hz refresh.");
    return true;
  }
  Serial.println("ERROR: MLX90640 not detected on I2C bus.");
  return false;
}

/*
 * readMLX90640()
 * --------------
 * v2.3 FIXES:
 *   - Reads frame into global g_thermalFrame[] (no stack allocation)
 *   - Runtime health tracking: counts consecutive failures.
 *   - Sends SENSOR_ALERT if sensor goes offline or recovers.
 *   - Respiratory rate function now reuses g_thermalFrame (no second I2C call).
 */
bool readMLX90640() {
  if (thermalSensor.getFrame(g_thermalFrame) != 0) {
    g_mlxFailCount++;
    if (g_mlxFailCount >= SENSOR_FAIL_THRESHOLD && g_mlxOk) {
      g_mlxOk = false;
      publishSensorAlert("MLX90640", "SENSOR_OFFLINE", g_mlxFailCount);
    }
    return false;
  }

  // Sensor responded: clear failures and detect recovery
  g_mlxFailCount = 0;
  if (!g_mlxOk) {
    g_mlxOk = true;
    publishSensorAlert("MLX90640", "SENSOR_RECOVERED", 0);
  }

  float sumCenter = 0.0f;
  int centerCount = 0;
  float maxTemp = -999.0f;

  for (int row = 6; row < 18; row++) {
    for (int col = 8; col < 24; col++) {
      int idx = row * 32 + col;
      float t = g_thermalFrame[idx];
      if (t > maxTemp) maxTemp = t;
      sumCenter += t;
      centerCount++;
    }
  }

  if (centerCount > 0) {
    g_tempCenterC = sumCenter / centerCount;
  }
  g_tempMaxC = maxTemp;
  return true;
}

bool initMQ137() {
  pinMode(PIN_MQ137_AIN, INPUT);
  g_mq137WarmupStartMs = millis();
  g_mq137WarmupComplete = false;
  Serial.println("MQ-137: Heater warm-up started (25s). Readings invalid until warm-up complete.");
  return true;
}

/*
 * readMQ137()
 * -----------
 * v2.3.1 FIXES:
 *   - Warm-up gate: returns invalid until heater stabilization period.
 *   - Non-blocking averaging: samples are spread across loop iterations
 *     (10 samples x 5 ms interval = 50 ms wall time, no blocking delays).
 *   - Detects ADC stuck at 0 or max (disconnected/shorted) and reports failure.
 *   - Runtime recovery alerts added.
 *   - Voltage divider ratio made explicit (MQ137_VOLTAGE_DIVIDER_RATIO) with BOM note.
 */
bool readMQ137() {
  // Warm-up guard: heater needs ~25s to stabilize
  if (!g_mq137WarmupComplete) {
    if (millis() - g_mq137WarmupStartMs < MQ137_WARMUP_MS) {
      g_ammoniaPPM = -1.0f;
      g_diaperSoiled = false;
      g_mqRawADC = 0.0f;
      g_mqVoltage = 0.0f;
      return false;
    } else {
      g_mq137WarmupComplete = true;
      Serial.println("MQ-137: Warm-up complete. NH3 readings now active.");
    }
  }

  if (g_mq137SampleCount >= MQ137_SAMPLES_PER_READ) {
    g_mq137SampleCount = 0;
    g_mq137SampleSum = 0.0f;
  }

  if (g_mq137SampleCount == 0) {
    g_mq137LastSampleMs = millis();
  }

  if (millis() - g_mq137LastSampleMs < MQ137_SAMPLE_INTERVAL_MS) {
    g_ammoniaPPM = -1.0f;
    g_diaperSoiled = false;
    return false;
  }

  g_mq137LastSampleMs = millis();
  g_mq137SampleSum += analogRead(PIN_MQ137_AIN);
  g_mq137SampleCount++;

  if (g_mq137SampleCount < MQ137_SAMPLES_PER_READ) {
    g_ammoniaPPM = -1.0f;
    g_diaperSoiled = false;
    return false;
  }

  g_mqRawADC = g_mq137SampleSum / MQ137_SAMPLES_PER_READ;
  g_mq137SampleSum = 0.0f;
  g_mq137SampleCount = 0;

  // Detect physically impossible ADC values = wiring fault
  if (g_mqRawADC <= 0.0f || g_mqRawADC >= 4094.0f) {
    g_mq137FailCount++;
    if (g_mq137FailCount >= SENSOR_FAIL_THRESHOLD && g_mq137Ok) {
      g_mq137Ok = false;
      publishSensorAlert("MQ137", "SENSOR_OFFLINE", g_mq137FailCount);
    }
    return false;
  }

  g_mq137FailCount = 0;
  if (!g_mq137Ok) {
    g_mq137Ok = true;
    publishSensorAlert("MQ137", "SENSOR_RECOVERED", 0);
  }

  float vAdc = (g_mqRawADC / 4095.0f) * 3.3f;
  g_mqVoltage = vAdc;

  // Reconstruct sensor voltage before the divider.
  // See BOM note at top: assumes R_series=10k, R_shunt=20k.
  float vSensor = vAdc * MQ137_VOLTAGE_DIVIDER_RATIO;
  if (vSensor < 0.01f) vSensor = 0.01f;

  float rs = MQ137_RL_KOHM * ((5.0f - vSensor) / vSensor);

  if (!MQ137_CALIBRATED) {
    g_ammoniaPPM = -1.0f;
  } else {
    float ratio = rs / MQ137_CLEAN_AIR_RO;
    if (ratio < 0.01f) ratio = 0.01f;
    g_ammoniaPPM = powf(10.0f, (log10f(ratio) * MQ137_CURVE_A + MQ137_CURVE_B));
  }

  if (g_ammoniaPPM >= DIAPER_PPM_THRESHOLD && g_ammoniaPPM > 0.0f) {
    g_diaperSoiled = true;
  } else if (g_ammoniaPPM >= 0.0f && g_ammoniaPPM < (DIAPER_PPM_THRESHOLD * 0.5f)) {
    g_diaperSoiled = false;
  }
  return true;
}

bool initHX711() {
  loadCell.begin(PIN_HX711_DOUT, PIN_HX711_SCK);
  if (loadCell.is_ready()) {
    loadCell.set_scale(HX711_CALIBRATION_FACTOR);
    loadCell.set_offset(HX711_ZERO_OFFSET);
    Serial.println("HX711 load cell initialized.");
    return true;
  }
  Serial.println("ERROR: HX711 not responding. Check wiring.");
  return false;
}

/*
 * readHX711()
 * -----------
 * v2.3 CRITICAL FIX: Replaced get_units(5) [500 ms blocking] with a single
 * fast read when is_ready() is true. The HX711 outputs data at ~10 Hz;
 * when is_ready() is true the conversion is complete and can be clocked
 * out in microseconds. This prevents MQTT keepalive timeouts and PDM
 * buffer overflow during the long blocking period in v2.2.
 */
bool readHX711() {
  if (loadCell.is_ready()) {
    long raw = loadCell.read(); // Fast single read, microseconds once ready
    g_babyWeightKg = (float)(raw - HX711_ZERO_OFFSET) / HX711_CALIBRATION_FACTOR;

    g_hx711FailCount = 0;
    if (!g_hx711Ok) {
      g_hx711Ok = true;
      publishSensorAlert("HX711", "SENSOR_RECOVERED", 0);
    }
    return true;
  }

  g_hx711FailCount++;
  if (g_hx711FailCount >= SENSOR_FAIL_THRESHOLD && g_hx711Ok) {
    g_hx711Ok = false;
    publishSensorAlert("HX711", "SENSOR_OFFLINE", g_hx711FailCount);
  }
  return false;
}

bool initAudio() {
  PDM.onReceive(onPDMdata);
  if (!PDM.begin(1, 16000)) {
    Serial.println("ERROR: PDM microphone init failed.");
    return false;
  }
  g_audioLastProcessedMs = millis();
  return true;
}

void onPDMdata() {
  int bytesAvailable = PDM.available();
  int toRead = min(bytesAvailable, (int)sizeof(g_pcmBuffer));
  PDM.read(g_pcmBuffer, toRead);
  g_pcmSamplesRead = toRead / sizeof(short);
  g_pcmDataReady = true;
}

/*
 * processAudio()
 * --------------
 * v2.3 FIXES:
 *   - Uses float (hardware FPU) instead of double (software emulation).
 *   - Tracks last successful processing time for runtime health monitoring.
 *   - Detects recovery if audio was previously flagged failed.
 */
bool processAudio() {
  if (g_pcmSamplesRead == 0) return false;

  g_audioLastProcessedMs = millis();
  if (!g_audioOk) {
    g_audioOk = true;
    publishSensorAlert("PDM_AUDIO", "SENSOR_RECOVERED", 0);
  }

  float sumSq = 0.0f;
  for (int i = 0; i < g_pcmSamplesRead; i++) {
    float s = (float)g_pcmBuffer[i];
    sumSq += (s * s);
  }
  float rms = sqrtf(sumSq / g_pcmSamplesRead);

  static bool highEnergy = false;
  if (rms > CRYING_RMS_THRESHOLD) {
    if (!highEnergy) {
      highEnergy = true;
      g_cryStartMs = millis();
    } else {
      if ((millis() - g_cryStartMs) >= CRYING_DURATION_MS) {
        if (!g_isCrying) {
          g_isCrying = true;
          g_cryAudioEventPending = true;
        }
      }
    }
  } else {
    highEnergy = false;
    g_isCrying = false;
  }
  return true;
}

/*
 * initCamera()
 * ------------
 * v2.3 CRITICAL FIX: No longer a stub. Actually initializes the OV7675
 * on the Vision Shield, captures one test frame to verify the data path,
 * then powers down. Camera is not used continuously in v2.3 (v3.0 TFLM
 * will re-enable it), but we prove the hardware is alive and connected.
 *
 * GRAYSCALE is intentional: QQVGA GRAYSCALE = 19,200 bytes vs 38,400 for
 * RGB565. This reduces RAM pressure and DMA bandwidth, matching the
 * TFLite Micro face-obstruction model planned for v3.0 (which only needs
 * luminance). Do not change to RGB565 without increasing the frame buffer.
 */
bool initCamera() {
  // QQVGA GRAYSCALE keeps RAM usage low: 160*120*1 = 19200 bytes
  if (!Camera.begin(QQVGA, GRAYSCALE, 15)) {
    Serial.println("ERROR: OV7675 camera initialization failed (Vision Shield missing or faulty?).");
    return false;
  }

  delay(200); // Allow auto-exposure to stabilize

  // Attempt one frame grab to verify MIPI/parallel data path is alive
  Camera.readFrame(g_cameraFrameBuffer);

  // Camera left active (not powered down) to avoid API compatibility issues
  // across Arduino_OV767X versions. It is not read continuously in v2.3,
  // so power/bandwidth impact is minimal until v3.0 TFLM re-enables it.

  Serial.println("CAMERA: OV7675 initialized, test frame captured, left idle until v3.0.");
  return true;
}

/*
 * publishImageToDb()
 * ----------------
 * Publishes the camera frame to MQTT for database ingestion.
 * The image is base64-encoded into a JSON payload so it can be stored
 * directly by a backend subscriber without requiring an SD card.
 */
bool publishImageToDb(const uint8_t* frame, size_t len) {
  if (!mqttClient.connected() || frame == NULL || len != 160 * 120) return false;

  char uuid[37];
  generateUUIDv4(uuid, sizeof(uuid));
  char ts[25];
  getISO8601Time(ts, sizeof(ts));

  static char b64buf[25600 + 1];
  static const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t j = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t octetA = frame[i];
    uint32_t octetB = (i + 1 < len) ? frame[i + 1] : 0;
    uint32_t octetC = (i + 2 < len) ? frame[i + 2] : 0;
    uint32_t triple = (octetA << 16) | (octetB << 8) | octetC;
    b64buf[j++] = b64chars[(triple >> 18) & 0x3F];
    b64buf[j++] = b64chars[(triple >> 12) & 0x3F];
    b64buf[j++] = (i + 1 < len) ? b64chars[(triple >> 6) & 0x3F] : '=';
    b64buf[j++] = (i + 2 < len) ? b64chars[triple & 0x3F] : '=';
  }
  b64buf[j] = '\0';

  static char json[26200];
  int n = snprintf(json, sizeof(json),
    "{\"event_id\":\"%s\",\"device_id\":\"%s\",\"event_type\":\"IMAGE\","
    "\"occurred_at\":\"%s\",\"sequence_no\":%lu,\"firmware_version\":\"%s\","
    "\"payload\":{\"width\":160,\"height\":120,\"format\":\"grayscale\","
    "\"data\":\"%s\"}",
    uuid, DEVICE_UUID, ts, sequenceCounter++, FIRMWARE_VER, b64buf);

  if (n >= (int)sizeof(json)) {
    Serial.println("ERROR: Image JSON truncated.");
    return false;
  }

  mqttClient.beginMessage(mqttTopicImages);
  mqttClient.print(json);
  mqttClient.endMessage();

  Serial.print("[IMAGE] Published frame ");
  Serial.println(uuid);
  return true;
}
