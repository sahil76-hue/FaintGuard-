
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <math.h>
#include <ctype.h>

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =============== WIFI ===============

const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// =============== TELEGRAM ===============

// Enter your freshly generated token locally.
// Never upload your token to GitHub.

const char* BOT_TOKEN = "9025084791:AAG-eLtfR6Jw24odU4NeyRd4WUEEkCSbeTY";
const char* CHAT_ID = "1992631616";

// =============== PIN CONFIGURATION ===============

#define HR_PIN 34
#define SPO2_PIN 35

#define BUZZER_PIN 25
#define BUTTON_PIN 26

#define SDA_PIN 21
#define SCL_PIN 22

// =============== OBJECTS ===============

Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// =============== VARIABLES ===============

int heartRate = 0;
int spo2 = 0;

bool emergency = false;
bool mpuAvailable = false;
bool impactDetected = false;
bool telegramSent = false;

String emergencyReason = "";

unsigned long impactTime = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTelegramAttempt = 0;

// =============== FALL THRESHOLDS ===============

const float IMPACT_THRESHOLD = 12.0;

const float MIN_REST_ACCEL = 7.5;
const float MAX_REST_ACCEL = 12.5;

const float REST_GYRO_THRESHOLD = 0.5;

const unsigned long IMPACT_WAIT = 2500;
const unsigned long SAMPLE_INTERVAL = 100;

// =============== FALL STATE ===============

int stillCount = 0;
unsigned long lastStillSample = 0;

// =============== WIFI CONNECTION ===============

bool connectWiFi() {

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println("Connecting to Wokwi WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 30) {

    delay(500);
    Serial.print(".");

    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("\nWiFi Connected!");

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    return true;
  }

  Serial.println("\nWiFi Connection Failed!");

  Serial.print("WiFi status: ");
  Serial.println(WiFi.status());

  return false;
}

// =============== URL ENCODING ===============

String urlEncode(String message) {

  String encoded = "";

  for (int i = 0; i < message.length(); i++) {

    unsigned char c = message.charAt(i);

    if (isalnum(c) || c == '-' || c == '_' ||
        c == '.' || c == '~') {

      encoded += (char)c;

    } else if (c == ' ') {

      encoded += "%20";

    } else if (c == '\n') {

      encoded += "%0A";

    } else {

      char buffer[4];

      sprintf(buffer, "%%%02X", c);

      encoded += buffer;
    }
  }

  return encoded;
}

// =============== TELEGRAM ===============

bool sendTelegram(String message) {

  Serial.println("\nSending Telegram message...");

  if (!connectWiFi()) {
    Serial.println("Telegram failed: WiFi unavailable");
    return false;
  }

  WiFiClientSecure client;

  // Wokwi testing only.
  // Production hardware should validate the server certificate.

  client.setInsecure();
  client.setHandshakeTimeout(30);
  client.setTimeout(30000);

  HTTPClient https;

  https.setConnectTimeout(30000);
  https.setTimeout(30000);

  String url =
    "https://api.telegram.org/bot" +
    String(BOT_TOKEN) +
    "/sendMessage";

  String postData =
    "chat_id=" + String(CHAT_ID) +
    "&text=" + urlEncode(message);

  if (!https.begin(client, url)) {

    Serial.println("HTTPS initialization failed!");

    return false;
  }

  https.addHeader(
    "Content-Type",
    "application/x-www-form-urlencoded"
  );

  Serial.println("Connecting to Telegram API...");

  int httpCode = https.POST(postData);

  Serial.print("Telegram HTTP Code: ");
  Serial.println(httpCode);

  bool success = false;

  if (httpCode > 0) {

    String response = https.getString();

    Serial.println("Telegram Response:");
    Serial.println(response);

    if (httpCode == 200 &&
        response.indexOf("\"ok\":true") >= 0) {

      Serial.println("TELEGRAM MESSAGE SENT!");

      success = true;

    } else {

      Serial.println("Telegram API rejected the request.");
    }

  } else {

    Serial.print("HTTPS Error: ");
    Serial.println(https.errorToString(httpCode));
  }

  https.end();

  return success;
}

// =============== READ VITALS ===============

// Potentiometers simulate MAX30102 readings in Wokwi.
// Replace with actual MAX30102 acquisition for hardware.

void readVitals() {

  int hrRaw = analogRead(HR_PIN);
  int spo2Raw = analogRead(SPO2_PIN);

  heartRate = map(hrRaw, 0, 4095, 40, 180);

  spo2 = map(spo2Raw, 0, 4095, 70, 100);
}

// =============== OLED DISPLAY ===============

void updateDisplay() {

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  if (emergency) {

    display.setTextSize(2);
    display.setCursor(10, 24);
    display.println("EMERGENCY");

  } else {

    // Normal display: ONLY HR and SpO2

    display.setTextSize(1);

    display.setCursor(0, 15);

    display.print("HR: ");
    display.print(heartRate);
    display.println(" BPM");

    display.setCursor(0, 35);

    display.print("SpO2: ");
    display.print(spo2);
    display.println(" %");
  }

  display.display();
}

// =============== EMERGENCY ===============

void triggerEmergency(String reason) {

  if (emergency) return;

  emergency = true;

  emergencyReason = reason;

  impactDetected = false;

  Serial.println("\n!!! EMERGENCY ACTIVATED !!!");
  Serial.println(reason);

  // Activate buzzer

  digitalWrite(BUZZER_PIN, HIGH);

  // Display emergency

  updateDisplay();

  // Reset Telegram retry timer

  lastTelegramAttempt = 0;
}

// =============== TELEGRAM RETRY ===============

void checkTelegramDelivery() {

  if (!emergency || telegramSent) return;

  // Retry every 10 seconds

  if (lastTelegramAttempt != 0 &&
      millis() - lastTelegramAttempt < 10000) {

    return;
  }

  lastTelegramAttempt = millis();

  String message =
    "FAINTGUARD EMERGENCY ALERT!\n"
    "Emergency event detected.\n"
    "Reason: " + emergencyReason +
    "\nHeart Rate: " + String(heartRate) + " BPM" +
    "\nSpO2: " + String(spo2) + "%";

  telegramSent = sendTelegram(message);

  if (!telegramSent) {

    Serial.println("Alert delivery failed. Retrying in 10 seconds.");

  }
}

// =============== MANUAL SOS BUTTON ===============

void checkButton() {

  static bool lastReading = HIGH;
  static bool stableState = HIGH;

  static unsigned long lastDebounceTime = 0;

  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastReading) {

    lastDebounceTime = millis();
  }

  if (millis() - lastDebounceTime >= 50) {

    if (reading != stableState) {

      stableState = reading;

      // INPUT_PULLUP: pressed = LOW

      if (stableState == LOW && !emergency) {

        Serial.println("\nMANUAL SOS BUTTON PRESSED!");

        triggerEmergency("Manual SOS button pressed");
      }
    }
  }

  lastReading = reading;
}

// =============== FALL DETECTION ===============

void checkFall() {

  if (!mpuAvailable || emergency) return;

  sensors_event_t accel;
  sensors_event_t gyro;
  sensors_event_t temp;

  mpu.getEvent(&accel, &gyro, &temp);

  float acceleration = sqrt(
    accel.acceleration.x * accel.acceleration.x +
    accel.acceleration.y * accel.acceleration.y +
    accel.acceleration.z * accel.acceleration.z
  );

  // STEP 1: Detect sudden impact

  if (!impactDetected &&
      acceleration > IMPACT_THRESHOLD) {

    impactDetected = true;

    impactTime = millis();

    stillCount = 0;

    Serial.println("Impact detected!");
  }

  if (!impactDetected) return;

  // STEP 2: Wait after impact

  if (millis() - impactTime < IMPACT_WAIT) {
    return;
  }

  // STEP 3: Sample inactivity every 100 ms

  if (millis() - lastStillSample < SAMPLE_INTERVAL) {
    return;
  }

  lastStillSample = millis();

  mpu.getEvent(&accel, &gyro, &temp);

  float accMagnitude = sqrt(
    accel.acceleration.x * accel.acceleration.x +
    accel.acceleration.y * accel.acceleration.y +
    accel.acceleration.z * accel.acceleration.z
  );

  float gyroMagnitude = sqrt(
    gyro.gyro.x * gyro.gyro.x +
    gyro.gyro.y * gyro.gyro.y +
    gyro.gyro.z * gyro.gyro.z
  );

  if (accMagnitude >= MIN_REST_ACCEL &&
      accMagnitude <= MAX_REST_ACCEL &&
      gyroMagnitude < REST_GYRO_THRESHOLD) {

    stillCount++;

    Serial.print("Still samples: ");
    Serial.println(stillCount);

  } else {

    stillCount = 0;

    Serial.println("Movement detected after impact.");
  }

  // STEP 4: Confirm possible fall

  if (stillCount >= 10) {

    triggerEmergency("Possible fall: impact followed by inactivity");

  }
}

// =============== SETUP ===============

void setup() {

  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);

  Wire.begin(SDA_PIN, SCL_PIN);

  // OLED initialization

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {

    Serial.println("OLED initialization failed!");

    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.display();

  // MPU6050 initialization

  if (mpu.begin(0x68)) {

    Serial.println("MPU6050 Connected!");

    mpuAvailable = true;

    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

    mpu.setGyroRange(MPU6050_RANGE_500_DEG);

    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  } else {

    Serial.println("MPU6050 not detected!");
  }

  // WiFi

  connectWiFi();

  // Initial readings

  readVitals();

  updateDisplay();

  Serial.println("\nFAINTGUARD READY!");

  Serial.println("Press GPIO 26 button for manual SOS.");
}

// =============== MAIN LOOP ===============

void loop() {

  // Always check button first

  checkButton();

  if (!emergency) {

    readVitals();

    checkFall();
  }

  // OLED refresh

  if (millis() - lastDisplayUpdate >= 300) {

    lastDisplayUpdate = millis();

    updateDisplay();
  }

  // Retry Telegram if delivery fails

  checkTelegramDelivery();

  // Keep buzzer active during emergency

  if (emergency) {

    digitalWrite(BUZZER_PIN, HIGH);
  }
}
