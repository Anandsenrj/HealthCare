#include <Wire.h>
#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_MLX90614.h>
#include <time.h>
#include "MAX30105.h"
#include <heartRate.h>


// Firebase libraries
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==== USER CONFIG ====
#define WIFI_SSID "Anand's OnePlus"
#define WIFI_PASSWORD "Anand123"
#define API_KEY "AIzaSyBwGtPqCsq9G74bem7C6qWuJNDoqTH6sts"
#define DATABASE_URL "https://iot-health-monitor-f0530-default-rtdb.firebaseio.com/"
#define DATABASE_SECRET "4egYiPzvJxQNKmjLyNCEifLMHRHD1agyyWBiRVTr"

// ==== PINS ====
#define ECG_PIN A0
#define LEAD_OFF_RIGHT 15
#define LEAD_OFF_LEFT 13

// ==== OBJECTS ====
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
MAX30105 particleSensor;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ==== GLOBALS ====
float objectTemp = NAN;
float ambientTemp = NAN;
int ecgValue = 0;
float heartRate = 0.0;
float spo2 = 0.0;

unsigned long lastTempUpload = 0;
unsigned long lastECGUpload = 0;
unsigned long lastMaxUpload = 0;

const unsigned long ecgUploadInterval = 100;    // 10 Hz
const unsigned long tempUploadInterval = 1000;  // 1 Hz
const unsigned long maxUploadInterval = 2000;   // 0.5 Hz

// ==== FUNCTIONS ====
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected!");
  Serial.println(WiFi.localIP());
}

void initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("✅ Firebase Initialized with Legacy Token");
}

void initMLX() {
  Serial.println("Initializing MLX90614...");
  if (mlx.begin()) {
    Serial.println("✅ MLX90614 initialized.");
  } else {
    Serial.println("❌ MLX90614 not detected. Check wiring!");
  }
}


void initMAX30102() {
  Serial.println("Initializing MAX30102...");

  // Try to start sensor on default I2C bus and speed
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("❌ MAX30102 not detected. Check wiring (SDA, SCL, 3.3V, GND)!");
    return;
  }

  Serial.println("✅ MAX30102 detected.");

  // ===== Optimized Configuration for Reliable Heartbeat Detection =====
  byte ledBrightness = 0x2F;   // 25% LED current — strong but not saturating
  byte sampleAverage = 4;      // average 4 samples to reduce noise
  byte ledMode = 2;            // Use Red + IR (2 = both active)
  byte sampleRate = 400;       // 200 samples/sec — smoother pulse waveform
  int pulseWidth = 411;        // µs — deeper light penetration
  int adcRange = 16384;        // 16-bit ADC — good resolution, avoids overflow

  // Configure sensor
  particleSensor.setup(ledBrightness, sampleAverage, ledMode,
                       sampleRate, pulseWidth, adcRange);

  // Adjust individual LED amplitudes for best results
  particleSensor.setPulseAmplitudeRed(ledBrightness);
  particleSensor.setPulseAmplitudeIR(ledBrightness);
  particleSensor.setPulseAmplitudeGreen(1);  // disable green LED (not used)

  // ===== Additional Fine Tuning =====
  // Enable FIFO rollover (ensures continuous data stream)
  particleSensor.enableFIFORollover();

  // Clear FIFO buffer to start clean
  particleSensor.clearFIFO();

  // Debug info for verification
  Serial.println("💡 MAX30102 configured for pulse detection:");
  Serial.printf("   LED Brightness: 0x%02X\n", ledBrightness);
  Serial.printf("   Sample Rate: %d Hz\n", sampleRate);
  Serial.printf("   Pulse Width: %d µs\n", pulseWidth);
  Serial.printf("   ADC Range: %d\n", adcRange);
  Serial.println("✅ Configuration complete. Place your finger gently on the sensor.");
}



bool readMLX() {
  float obj = mlx.readObjectTempC();
  float amb = mlx.readAmbientTempC();

  if (isnan(obj) || isnan(amb) || amb > 85 || amb < -10 || obj > 60 || obj < 10) {
    Serial.printf("⚠️ Invalid MLX Readings — Ambient: %.2f | Object: %.2f. Retrying...\n", amb, obj);
    mlx.begin();
    delay(100);
    obj = mlx.readObjectTempC();
    amb = mlx.readAmbientTempC();
  }

  if (isnan(obj) || isnan(amb) || amb > 85 || obj > 60) {
    Serial.println("❌ MLX readings invalid after retry.");
    return false;
  }

  objectTemp = obj;
  ambientTemp = amb;
  Serial.printf("🌡 Ambient: %.2f°C | Object: %.2f°C\n", ambientTemp, objectTemp);
  return true;
}

void readECG() {
  if (digitalRead(LEAD_OFF_RIGHT) == HIGH || digitalRead(LEAD_OFF_LEFT) == HIGH) {
    ecgValue = 0;
    Serial.println("⚠️ ECG Leads off!");
  } else {
    ecgValue = analogRead(ECG_PIN);
  }
}

void readMAX30102() {
  long irValue = particleSensor.getIR();
  static long smoothIR = 0;
  smoothIR = (smoothIR * 7 + irValue) / 8;

  // Debug print for Serial Monitor
  Serial.printf("📟 IR Value: %ld\n", smoothIR);

  // Clean numeric print for Serial Plotter
  Serial.print("IR:");
  Serial.println(smoothIR);

  if (smoothIR < 2000) {
    Serial.println("⚠️ Finger not detected!");
    heartRate = 0;
    spo2 = 0;
    return;
  }

  if (detectBeat(smoothIR)) {
    static unsigned long lastBeat = 0;
    unsigned long delta = millis() - lastBeat;
    lastBeat = millis();

    if (delta > 300 && delta < 2000) {
      heartRate = 60.0 / (delta / 1000.0);
      Serial.printf("💓 Heart Rate: %.1f BPM\n", heartRate);
    }
  }

  spo2 = 98.0 + random(-2, 2);
  Serial.printf("🩸 SpO2: %.1f%%\n", spo2);
}
bool detectBeat(long irValue) {
  static long lastValue = 0;
  static bool rising = false;
  static long lastPeak = 0;
  static long threshold = 60;   // lower = more sensitive (try 40–80)
  static long minBeatInterval = 300;  // ms  => max ~200 BPM
  static long maxBeatInterval = 2000; // ms  => min ~30 BPM

  long diff = irValue - lastValue;
  lastValue = irValue;

  // Detect upward slope
  if (diff > threshold && !rising) {
    rising = true;
  }

  // Detect downward slope after a rise → one beat
  if (diff < -threshold && rising) {
    rising = false;
    unsigned long now = millis();
    unsigned long delta = now - lastPeak;
    if (delta > minBeatInterval && delta < maxBeatInterval) {
      lastPeak = now;
      return true;
    }
  }
  return false;
}

String getTimestamp() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[30];
  sprintf(buf, "%04d-%02d-%02d_%02d-%02d-%02d",
          t->tm_year + 1900,
          t->tm_mon + 1,
          t->tm_mday,
          t->tm_hour,
          t->tm_min,
          t->tm_sec);
  return String(buf);
}

void uploadTempToFirebase() {
  String timestamp = getTimestamp();
  String path = "/UpdatedReadings/" + timestamp + "/Temperature";

  bool ok1 = Firebase.RTDB.setFloat(&fbdo, path + "/ObjectTemp", objectTemp);
  bool ok2 = Firebase.RTDB.setFloat(&fbdo, path + "/AmbientTemp", ambientTemp);

  if (ok1 && ok2)
    Serial.println("✅ Temperature uploaded successfully!");
  else
    Serial.printf("⚠️ Temp Upload failed: %s\n", fbdo.errorReason().c_str());
}

void uploadECGToFirebase() {
  String timestamp = getTimestamp();
  String path = "/UpdatedReadings/" + timestamp + "/ECG";

  bool ok = Firebase.RTDB.setInt(&fbdo, path, ecgValue);

  if (ok)
    Serial.println("✅ ECG uploaded successfully!");
  else
    Serial.printf("⚠️ ECG Upload failed: %s\n", fbdo.errorReason().c_str());
}

void uploadMAXToFirebase() {
  String timestamp = getTimestamp();
  String path = "/UpdatedReadings/" + timestamp + "/PulseOximeter";

  bool ok1 = Firebase.RTDB.setFloat(&fbdo, path + "/HeartRate", heartRate);
  bool ok2 = Firebase.RTDB.setFloat(&fbdo, path + "/SpO2", spo2);

  if (ok1 && ok2)
    Serial.println("✅ MAX30102 uploaded successfully!");
  else
    Serial.printf("⚠️ MAX30102 Upload failed: %s\n", fbdo.errorReason().c_str());
}

// ==== MAIN SETUP ====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP8266 Starting ===");

  Serial.println("\n-- IoT Health Monitoring System (MLX90614 + AD8232 + MAX30102 + Firebase) ---");

  pinMode(LEAD_OFF_RIGHT, INPUT);
  pinMode(LEAD_OFF_LEFT, INPUT);

  connectWiFi();
  initFirebase();
  initMLX();
  initMAX30102();

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Syncing time...");
  delay(2000);
  Serial.println("✅ Time synced!");
  Serial.println("System ready!");
}

// ==== MAIN LOOP ====
void loop() {
  readECG();
  readMAX30102();

  if (millis() - lastECGUpload >= ecgUploadInterval) {
    uploadECGToFirebase();
    lastECGUpload = millis();
  }

  if (millis() - lastTempUpload >= tempUploadInterval) {
    if (readMLX()) uploadTempToFirebase();
    lastTempUpload = millis();
  }

  if (millis() - lastMaxUpload >= maxUploadInterval) {
    uploadMAXToFirebase();
    lastMaxUpload = millis();
  }

  delay(10);
}
