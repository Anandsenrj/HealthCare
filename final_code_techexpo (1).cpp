#include <Wire.h>
#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Adafruit_MLX90614.h>
#include <time.h>

// Firebase libraries
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ==== USER CONFIG ====
#define WIFI_SSID "Cypher"
#define WIFI_PASSWORD "lenovo321"
#define API_KEY "AIzaSyBwGtPqCsq9G74bem7C6qWuJNDoqTH6sts"
#define DATABASE_URL "https://iot-health-monitor-f0530-default-rtdb.firebaseio.com/"
#define DATABASE_SECRET "4egYiPzvJxQNKmjLyNCEifLMHRHD1agyyWBiRVTr"

// ==== PINS ====
#define ECG_PIN A0
#define LEAD_OFF_RIGHT 15
#define LEAD_OFF_LEFT 13

// ==== OBJECTS ====
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ==== GLOBALS ====
float objectTemp = NAN;
float ambientTemp = NAN;
int ecgValue = 0;
unsigned long lastTempUpload = 0;
unsigned long lastECGUpload = 0;
const unsigned long ecgUploadInterval = 100;    // 10 Hz
const unsigned long tempUploadInterval = 1000;  // 1 Hz

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

// ==== MAIN SETUP ====
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n-- IoT Health Monitoring System (MLX90614 + AD8232 + Firebase) ---");

  pinMode(LEAD_OFF_RIGHT, INPUT);
  pinMode(LEAD_OFF_LEFT, INPUT);

  connectWiFi();
  initFirebase();
  initMLX();

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Syncing time...");
  delay(2000);
  Serial.println("✅ Time synced!");
  Serial.println("System ready!");
}

// ==== MAIN LOOP ====
void loop() {
  readECG();

  // ECG upload every 100 ms
  if (millis() - lastECGUpload >= ecgUploadInterval) {
    uploadECGToFirebase();
    lastECGUpload = millis();
  }

  // Temperature upload every 1 sec
  if (millis() - lastTempUpload >= tempUploadInterval) {
    if (readMLX()) uploadTempToFirebase();
    lastTempUpload = millis();
  }

  delay(10);  // smooth out sensor read timing
}
