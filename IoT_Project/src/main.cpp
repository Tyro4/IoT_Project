#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#define PIN_ANALOG_IN 34
#define PIN_LIGHT 32
#define WIFI_SSID "Wiha Ratcheting Screwdriver"
#define WIFI_PASSWORD "house101"
#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883

#define TOPIC_SENSOR "firestation_esp32/sensors"
#define TOPIC_PERFORMANCE "firestation_esp32/performance"
#define TZ_INFO "AEST-10AEDT,M10.1.0,M4.1.0/3"

#define GROUP_ID "Group1"
#define LOCATION "firestation"
#define DEVICE_TYPE "ESP32"

const unsigned long SAMPLE_INTERVAL = 2000;
const unsigned long RETRY_INTERVAL = 5000;
const int BATCH_SIZE = 3;
volatile float TEMP_THRESHOLD = 30.0f;

#define BLE_SERVER_NAME "firestation_esp32"
#define BLE_SERVICE_UUID "7ab9d8b0-27fb-4b3a-a735-c79c27ab8ef1"
#define BLE_THRESHOLD_UUID "9d34f7c2-333d-473b-bc32-79c2bae6dae6"
BLECharacteristic* thresholdCharacteristic = nullptr;

const float LIGHT_THRESHOLD = 1000.0f;
WiFiMulti wifiMulti;
WiFiClient net;
PubSubClient mqtt(net);
String deviceId;

unsigned long lastSample = 0, lastRetry = 0, sessionStart = 0;
unsigned long violations = 0, totalPublishedBytes = 0;
unsigned long totalPublishedPoints = 0, publishAttempts = 0, publishFailures = 0;
unsigned long intervalBytes = 0;
int intervalPoints = 0;

class ThresholdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    auto raw = characteristic->getValue();
    String input(raw.c_str());
    input.trim();

    char* end = nullptr;
    errno = 0;
    float value = strtof(input.c_str(), &end);

    if (input.length() == 0 || end == input.c_str() || *end != '\0' || errno == ERANGE || !isfinite(value) || value < 0.0f || value > 100.0f) {
      Serial.println("BLE: invalid threshold (enter -20 to 100 Celsius)");
      characteristic->setValue(String((float)TEMP_THRESHOLD, 2).c_str());
      return;
    }

    TEMP_THRESHOLD = value;
    characteristic->setValue(String(value, 2).c_str());
    Serial.printf("BLE - temperature threshold changed to %.2f C\n", value);
  }
};

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { Serial.println("BLE connected"); }
  void onDisconnect(BLEServer* server) override {
    Serial.println("BLE disconnected");
    server->startAdvertising();
  }
};

void setupBLE() {
  BLEDevice::init(BLE_SERVER_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());
  BLEService* service = server->createService(BLE_SERVICE_UUID);

  thresholdCharacteristic = service->createCharacteristic(BLE_THRESHOLD_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  thresholdCharacteristic->setCallbacks(new ThresholdCallbacks());
  thresholdCharacteristic->setValue(String((float)TEMP_THRESHOLD, 2).c_str());

  BLEDescriptor* description = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  description->setValue("Temperature threshold in Celsius (0 to 100)");
  thresholdCharacteristic->addDescriptor(description);

  service->start();
  server->getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  server->getAdvertising()->start();
  Serial.println("BLE advertising: " BLE_SERVER_NAME);
}


float get_temp() {
  int adcValue = analogRead(PIN_ANALOG_IN);
  if (adcValue <= 0 || adcValue >= 4095) return NAN;

  double voltage = (double)adcValue / 4095.0 * 3.3;
  double Rt = 10.0 * voltage / (3.3 - voltage);
  double tempK = 1.0 / (1.0 / (273.15 + 25.0) + log(Rt / 10.0) / 3950.0);

  return (float)(tempK - 273.15);
}

float get_light() {
  int adc = analogRead(PIN_LIGHT);
  float mapped = map(constrain(adc, 372, 2048), 372, 2048, 0, 4095);
  return 4095.0f - mapped;
}

String get_status(float temperature, float light) {
  bool hot = temperature > TEMP_THRESHOLD;
  bool dark = light < LIGHT_THRESHOLD;

  if (hot && dark) return "HOT & DARK";
  if (hot) return "HOT TEMPERSTURE WARNING";
  if (dark) return "SMOKE WARNING";
  return "NORMAL";
}

bool publishJson(const char* topic, JsonDocument& doc, size_t& bytes) {
  String payload;
  serializeJson(doc, payload);
  bytes = payload.length(); 
  publishAttempts++;
  bool ok = mqtt.connected() && mqtt.publish(topic, payload.c_str());

  if (!ok) publishFailures++;
  Serial.printf("MQTT %s [%s] (%u bytes): %s\n", ok ? "published" : "failed", topic, (unsigned)bytes, payload.c_str());
  return ok;
}

void addTags(JsonDocument& doc) {
  doc["group"] = GROUP_ID;
  doc["device_id"] = deviceId;
  doc["location"] = LOCATION;
  doc["device_type"] = DEVICE_TYPE;
}

void publishPerformance() {
  if (intervalPoints == 0) return;
  JsonDocument doc;
  addTags(doc);

  doc["timestamp"] = (int64_t)time(nullptr);
  doc["batch_bytes"] = intervalBytes;
  doc["batch_points"] = intervalPoints;
  doc["total_uploaded_bytes"] = totalPublishedBytes;
  doc["total_uploaded_points"] = totalPublishedPoints;
  doc["upload_attempts"] = publishAttempts;
  doc["upload_failures"] = publishFailures;
  double seconds = (millis() - sessionStart) / 1000.0;
  doc["writing_rate_bps"] = seconds > 0 ? totalPublishedBytes / seconds : 0.0;
  
  size_t bytes = 0;
  if (publishJson(TOPIC_PERFORMANCE, doc, bytes)) {
    intervalBytes = 0;
    intervalPoints = 0;
  }
}

void collectSample() {
  float temperature = get_temp();
  float light = get_light();

  if (isnan(temperature) || isnan(light)) {
    Serial.println("Invalid sensor reading; skipped");
    return;
  }

  time_t ts = time(nullptr);
  if (ts < 1700000000) {
    Serial.println("Clock not synchronized; skipped");
    return;
  }
  
  if (temperature > TEMP_THRESHOLD || light < LIGHT_THRESHOLD) violations++;
  JsonDocument doc;
  addTags(doc);

  doc["timestamp"] = (int64_t)ts;
  doc["temperature"] = temperature;
  doc["light"] = light;
  doc["status"] = get_status(temperature, light);
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["upload_interval"] = SAMPLE_INTERVAL / 1000;
  doc["temp_threshold"] = TEMP_THRESHOLD;
  doc["light_threshold"] = LIGHT_THRESHOLD;
  doc["violations"] = violations;

  size_t bytes = 0;
  if (publishJson(TOPIC_SENSOR, doc, bytes)) {
    totalPublishedBytes += bytes;
    totalPublishedPoints++;
    intervalBytes += bytes;
    intervalPoints++;
    if (intervalPoints >= BATCH_SIZE) publishPerformance();
  }
}

void connectMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastRetry < RETRY_INTERVAL) return;

  lastRetry = millis();
  String clientId = "esp32-" + deviceId;
  clientId.replace(":", "");
  if (mqtt.connect(clientId.c_str())) Serial.println("MQTT connected");
  else Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  while (wifiMulti.run() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");
  deviceId = WiFi.macAddress();

  setupBLE();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  sessionStart = millis();
  lastSample = millis();
  lastRetry = millis() - RETRY_INTERVAL;
  connectMqtt();
}

void loop() {
  wifiMulti.run();
  connectMqtt();

  if (mqtt.connected()) mqtt.loop();
  if (millis() - lastSample >= SAMPLE_INTERVAL) {
    lastSample = millis();
    if (mqtt.connected()) collectSample();
    else Serial.println("MQTT offline; sample skipped");
  }
}
