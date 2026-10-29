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
#include <Preferences.h>

//DEVICE WIFI AND MQTT VARIABLES
#define WIFI_SSID "Wiha Ratcheting Screwdriver"
#define WIFI_PASSWORD "house101"
#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883

#define TOPIC_SENSOR "firestation_esp32/sensors"
#define TZ_INFO "AEST-10AEDT,M10.1.0,M4.1.0/3"

#define GROUP_ID "Group1"
#define LOCATION "firestation"
#define DEVICE_TYPE "ESP32"

Preferences preferences;
WiFiMulti wifiMulti;
WiFiClient net;
PubSubClient mqtt(net);
String deviceId;

//BATCHING VARIABLES SETUP
const unsigned long SAMPLE_INTERVAL = 2000;
const unsigned long RETRY_INTERVAL = 5000;
unsigned long lastSample = 0, lastRetry = 0, sessionStart = 0;

//BLE VARIABLES SETUP
#define BLE_SERVER_NAME "fire_esp32"
#define BLE_TEMP_SERVICE_UUID "7ab9d8b0-27fb-4b3a-a735-c79c27ab8ef1"
#define BLE_LIGHT_SERVICE_UUID "7ab9d8b0-27fb-4b3a-a735-c79c27ab8ef2"
#define BLE_THRESHOLD_UUID "9d34f7c2-333d-473b-bc32-79c2bae6dae6"

BLECharacteristic tempHighChar("9d34f7c2-333d-473b-bc32-79c2bae6dae1", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
BLECharacteristic tempExtremeChar("9d34f7c2-333d-473b-bc32-79c2bae6dae2", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
BLECharacteristic tempCatastrophicChar("9d34f7c2-333d-473b-bc32-79c2bae6dae3", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

BLECharacteristic lightHighChar("9d34f7c2-333d-473b-bc32-79c2bae6dae4", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
BLECharacteristic lightExtremeChar("9d34f7c2-333d-473b-bc32-79c2bae6dae5", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
BLECharacteristic lightCatastrophicChar("9d34f7c2-333d-473b-bc32-79c2bae6dae6", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

//GLOBAL VARIABLES
String fireRisk = "";
String waterStatus = "";
float temperature = 0.0, waterPercentage = 0.0;
int lightLevel = 0;

// GENERAL PIN DEFINITIONS
#define THERMISTOR_PIN 32
#define LDR_PIN 34

// ULTRASONIC SENSOR PINS + DEFINITIONS
#define trigPin 13 // define trigPin
#define echoPin 14 // define echoPin.
#define MAX_DISTANCE 700 // Maximum sensor distance is rated at 400-500cm.
float timeOut = MAX_DISTANCE * 60;
int soundVelocity = 340; // define sound speed=340m/s
unsigned long pingTime;
float distance;

// LDR CHANNEL AND LIGHT LEVEL RANGE
#define CHAN 0
#define LIGHT_MIN 0
#define LIGHT_MAX 3560

// FIRE RISK LEVELS
const String FIRE_MODERATE = "Moderate";
const String FIRE_HIGH = "High";
const String FIRE_EXTREME = "Extreme";
const String FIRE_CATASTROPHIC = "Catastrophic";

// WATER STATUS LEVELS
const String WATER_CRITICAL = "Critical";
const String WATER_LOW = "Low";
const String WATER_MODERATE = "Moderate";
const String WATER_FULL = "Full";

//LEVEL THRESHOLDS
int TEMP_HIGH_THRESHOLD = 28;
int TEMP_EXTREME_THRESHOLD = 30;
int TEMP_CATASTROPHIC_THRESHOLD = 32;

int LIGHT_HIGH_THRESHOLD = 80;
int LIGHT_EXTREME_THRESHOLD = 50;
int LIGHT_CATASTROPHIC_THRESHOLD = 20;

// SENSOR FUNCTION DECLARATIONS
void readTemperature();
void readSensors();
void calculateFireRisk();
void calculateWaterStatus();
void printStatus();

//LOAD SAVED THRESHOLDS
void loadThresholds() {
    preferences.begin("fire-config", true); 

    TEMP_HIGH_THRESHOLD = preferences.getInt("tempHigh", 28);
    TEMP_EXTREME_THRESHOLD = preferences.getInt("tempExtreme", 30);
    TEMP_CATASTROPHIC_THRESHOLD = preferences.getInt("tempCat", 32);

    LIGHT_HIGH_THRESHOLD = preferences.getInt("lightHigh", 80);
    LIGHT_EXTREME_THRESHOLD = preferences.getInt("lightExtreme", 50);
    LIGHT_CATASTROPHIC_THRESHOLD = preferences.getInt("lightCat", 20);

    preferences.end();
    Serial.println("Thresholds loaded from NVS");
}

class ThresholdCallback : public BLECharacteristicCallbacks {
private:
    int* threshold;
    const char* name;
    const char* memKey;

public:
    ThresholdCallback(int* thresholdPtr, const char* label, const char* key) : threshold(thresholdPtr), name(label), memKey(key) {}

    void onWrite(BLECharacteristic* characteristic) override {
        std::string value = characteristic->getValue();
        if (value.empty()) return;

        //check if input is wihtin bounds
        char* endPtr;
        long newValue = strtol(value.c_str(), &endPtr, 10);
        if (endPtr == value.c_str() || *endPtr != '\0' || newValue < 0 || newValue > 100) {
            Serial.println("Invalid threshold value");
            characteristic->setValue(String(*threshold).c_str());
            return;
        }
        //check is input is within threshold values
        int oldValue = *threshold;
        *threshold = (int)newValue;
        bool tempValid = TEMP_HIGH_THRESHOLD < TEMP_EXTREME_THRESHOLD && TEMP_EXTREME_THRESHOLD < TEMP_CATASTROPHIC_THRESHOLD;
        bool lightValid = LIGHT_HIGH_THRESHOLD > LIGHT_EXTREME_THRESHOLD && LIGHT_EXTREME_THRESHOLD > LIGHT_CATASTROPHIC_THRESHOLD;

        if (!tempValid || !lightValid) {
            *threshold = oldValue;
            characteristic->setValue(String(oldValue).c_str());
            Serial.println("Rejected: thresholds must remain ordered");
            return;
        }

        //Open memory namespace to save new threshold
        preferences.begin("fire-config", false);
        size_t saved = preferences.putInt(memKey, *threshold);
        preferences.end();

        characteristic->setValue(String(*threshold).c_str());
        Serial.print(name);
        Serial.print(" saved: ");
        Serial.println(*threshold);
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
  //Create server and assign services
  BLEDevice::init(BLE_SERVER_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());
  BLEService* tempService = server->createService(BLE_TEMP_SERVICE_UUID);
  BLEService* lightService = server->createService(BLE_LIGHT_SERVICE_UUID);

  //Add characteristics to services
  tempService->addCharacteristic(&tempHighChar);
  tempService->addCharacteristic(&tempExtremeChar);
  tempService->addCharacteristic(&tempCatastrophicChar);

  lightService->addCharacteristic(&lightHighChar);
  lightService->addCharacteristic(&lightExtremeChar);
  lightService->addCharacteristic(&lightCatastrophicChar);

  //assign callbacks to chars
  tempHighChar.setCallbacks(new ThresholdCallback(&TEMP_HIGH_THRESHOLD, "Temperature HIGH", "tempHigh"));
  tempExtremeChar.setCallbacks(new ThresholdCallback(&TEMP_EXTREME_THRESHOLD, "Temperature EXTREME", "tempExtreme"));
  tempCatastrophicChar.setCallbacks(new ThresholdCallback(&TEMP_CATASTROPHIC_THRESHOLD, "Temperature CATASTROPHIC", "tempCat"));

  lightHighChar.setCallbacks(new ThresholdCallback(&LIGHT_HIGH_THRESHOLD, "Light HIGH", "lightHigh"));
  lightExtremeChar.setCallbacks(new ThresholdCallback(&LIGHT_EXTREME_THRESHOLD, "Light EXTREME", "lightExtreme"));
  lightCatastrophicChar.setCallbacks(new ThresholdCallback(&LIGHT_CATASTROPHIC_THRESHOLD, "Light CATASTROPHIC", "lightCat"));

  //assign threshold variables to chars
  tempHighChar.setValue(String(TEMP_HIGH_THRESHOLD).c_str());
  tempExtremeChar.setValue(String(TEMP_EXTREME_THRESHOLD).c_str());
  tempCatastrophicChar.setValue(String(TEMP_CATASTROPHIC_THRESHOLD).c_str());

  lightHighChar.setValue(String(LIGHT_HIGH_THRESHOLD).c_str());
  lightExtremeChar.setValue(String(LIGHT_EXTREME_THRESHOLD).c_str());
  lightCatastrophicChar.setValue(String(LIGHT_CATASTROPHIC_THRESHOLD).c_str());

  // Descriptions
  BLEDescriptor* tempHighDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  tempHighDesc->setValue("Temperature HIGH threshold (C)");
  tempHighChar.addDescriptor(tempHighDesc);

  BLEDescriptor* tempExtremeDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  tempExtremeDesc->setValue("Temperature EXTREME threshold (C)");
  tempExtremeChar.addDescriptor(tempExtremeDesc);

  BLEDescriptor* tempCatDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  tempCatDesc->setValue("Temperature CATASTROPHIC threshold (C)");
  tempCatastrophicChar.addDescriptor(tempCatDesc);

  BLEDescriptor* lightHighDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  lightHighDesc->setValue("Light HIGH threshold");
  lightHighChar.addDescriptor(lightHighDesc);

  BLEDescriptor* lightExtremeDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  lightExtremeDesc->setValue("Light EXTREME threshold");
  lightExtremeChar.addDescriptor(lightExtremeDesc);

  BLEDescriptor* lightCatDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  lightCatDesc->setValue("Light CATASTROPHIC threshold");
  lightCatastrophicChar.addDescriptor(lightCatDesc);

  //start services
  lightService->start();
  tempService->start();
  server->getAdvertising()->addServiceUUID(BLE_LIGHT_SERVICE_UUID);
  server->getAdvertising()->addServiceUUID(BLE_TEMP_SERVICE_UUID);
  server->getAdvertising()->start();
  Serial.println("BLE advertising: " BLE_SERVER_NAME);
}

void readTemperature() {
  int adcValueTemp = analogRead(THERMISTOR_PIN); // Read Thermistor pin
  double voltageTemp = (float)adcValueTemp / 4095.0 * 3.3; // Calculate voltage
  double RtTemperature = 10 * voltageTemp / (3.3 - voltageTemp); // Calculate resistance value of thermistor
  double tempK = 1 / (1/(273.15 + 25) + log(RtTemperature / 10)/3950.0); // Calculate temperature (Kelvin)
  double tempC = tempK - 273.15; // Calculate temperature (Celsius)
  temperature = tempC;
}

void readLightLevel() {
  int adcValueLight = analogRead(LDR_PIN); // Read LDR pin
  lightLevel = constrain(
    map(adcValueLight, LIGHT_MIN, LIGHT_MAX, 100, 0), 0, 100);
}

void readWaterLevel() {
  // make trigPin output high level lasting for 10us to trigger HC_SR04
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Wait HC-SR04 returning to the high level and measure out this waiting time
  pingTime = pulseIn(echoPin, HIGH, timeOut);

  // calculate the distance according to the time
  distance = (float)pingTime * soundVelocity / 2 / 10000;

  waterPercentage = distance;
}

void readSensors() {
  readTemperature();
  readLightLevel();
  readWaterLevel();
}

void calculateFireRisk() {
  if (temperature > TEMP_CATASTROPHIC_THRESHOLD && lightLevel < LIGHT_CATASTROPHIC_THRESHOLD) {
    fireRisk = FIRE_CATASTROPHIC;
  } else if (temperature > TEMP_EXTREME_THRESHOLD && lightLevel < LIGHT_EXTREME_THRESHOLD) {
    fireRisk = FIRE_EXTREME;
  } else if (temperature > TEMP_HIGH_THRESHOLD && lightLevel < LIGHT_HIGH_THRESHOLD) {
    fireRisk = FIRE_HIGH;
  } else {
    fireRisk = FIRE_MODERATE;
  }
}

void calculateWaterStatus() {
  if (waterPercentage < 10) {
    waterStatus = WATER_CRITICAL;
  } else if (waterPercentage < 30) {
    waterStatus = WATER_LOW;
  } else if (waterPercentage < 60) {
    waterStatus = WATER_MODERATE;
  } else {
    waterStatus = WATER_FULL;
  }
}

bool publishJson(const char* topic, JsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);
  size_t bytes = payload.length(); 
  bool ok = mqtt.connected() && mqtt.publish(topic, payload.c_str());
  Serial.printf("MQTT %s [%s] (%u bytes): %s\n", ok ? "published" : "failed", topic, (unsigned)bytes, payload.c_str());
  return ok;
}

void addTags(JsonDocument& doc) {
  doc["group"] = GROUP_ID;
  doc["device_id"] = deviceId;
  doc["location"] = LOCATION;
  doc["device_type"] = DEVICE_TYPE;
}

void collectSample() {
  readSensors();
  calculateFireRisk();
  calculateWaterStatus();

  time_t ts = time(nullptr);
  if (ts < 1700000000) {
    Serial.println("Clock not synchronized; skipped");
    return;
  }
  
  JsonDocument doc;
  addTags(doc);
  doc["timestamp"] = (int64_t)ts;
  doc["temperature"] = temperature;
  doc["light"] = lightLevel;
  doc["fire_status"] = fireRisk;
  doc["water_status"] = waterStatus;
  doc["water_percentage"] = waterPercentage;
  doc["uptime"] = millis() / 1000;
  doc["upload_interval"] = SAMPLE_INTERVAL / 1000;

  publishJson(TOPIC_SENSOR, doc);
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
  pinMode(trigPin, OUTPUT); // set trigPin to output mode
  pinMode(echoPin, INPUT); // set echoPin to input mode
  Serial.begin(115200);
  loadThresholds();
  Serial.println("Bushfire Response System Initialised");
  analogReadResolution(12);
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  while (wifiMulti.run() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }
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
    else Serial.println("MQTT offline: sample skipped");
  }
}
