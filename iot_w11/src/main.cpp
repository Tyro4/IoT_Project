
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <time.h>
#include <math.h>

WiFiMulti wifiMulti;
#define DEVICE "ESP32"
#define PIN_ANALOG_IN 34
#define PIN_LIGHT 32
#define WIFI_SSID "Wiha Ratcheting Screwdriver"
// WiFi password
#define WIFI_PASSWORD "house101"

#define INFLUXDB_URL "https://eu-central-1-1.aws.cloud2.influxdata.com"
#define INFLUXDB_TOKEN "pQh4kmjiSXPl_jjLtGeWAha2J3Jtewyoc4_THV5nh2YXn98D8jRawFwm54uIC5V5LSgQlQUTAjF7kaXRz3iEvQ=="
#define INFLUXDB_ORG "965ff46b7659ef35"
#define INFLUXDB_BUCKET "temperature"
#define TZ_INFO "AEST-18AEDT,M18.1.0, M4.1.0/3"

#define GROUP_ID "Group1"
#define LOCATION "homeoffice"
#define DEVICE_TYPE "ESP32"
#define FIRMWARE_VERSION "1.0"

const int BATCH_SIZE = 3;
const int WRITE_BUFFER_SIZE = 30;
const unsigned long SAMPLE_INTERVAL = 2000;
const unsigned long RETRY_INTERVAL = 5000;

float TEMP_THRESHOLD = 30.0;
float LIGHT_THRESHOLD = 1000.0;

InfluxDBClient client(
  INFLUXDB_URL,
  INFLUXDB_ORG,
  INFLUXDB_BUCKET,
  INFLUXDB_TOKEN,
  InfluxDbCloud2CACert
);

unsigned long lastSample = 0;
unsigned long lastRetry = 0;
unsigned long sessionStart = 0;
unsigned long lastSuccessfulFlushMs = 0;

unsigned long totalAccumulatedBytes = 0;
unsigned long totalSuccessfulBytes = 0;

unsigned long totalSuccessfulPoints = 0;
unsigned long totalUploadAttempts = 0;
unsigned long totalUploadFailures = 0;

unsigned long batchEnvironmentalBytes = 0;
unsigned long batchStartMs = 0;
int batchCount = 0;

bool batchPending = false;
bool performancePointQueued = false;
unsigned long violations = 0;

float get_temp();
float get_light();

String get_status(float temperature, float light);

void collectSample();
bool flushBatch();
void printStatistics();

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  Serial.println();
  Serial.println("Connecting to WiFi...");

  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Device MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Synchronising time...");

  timeSync(TZ_INFO, "pool.ntp.org", "time.nist.gov");

  if (client.validateConnection()) {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(client.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
  }

  client.setWriteOptions(
    WriteOptions()
      .writePrecision(WritePrecision::S)
      .batchSize(WRITE_BUFFER_SIZE)
      .bufferSize(WRITE_BUFFER_SIZE)
  );

  sessionStart = millis();
  lastSample = millis();
  lastSuccessfulFlushMs = millis();

  Serial.println();
  Serial.println("Environmental monitoring started.");
  Serial.print("Environmental batch size: ");
  Serial.println(BATCH_SIZE);
  Serial.print("Sampling interval: ");
  Serial.print(SAMPLE_INTERVAL / 1000);
  Serial.println(" seconds");
  Serial.println(
    "Waiting for timestamped environmental samples..."
  );
}

void loop() {
  if (wifiMulti.run() != WL_CONNECTED) {
    Serial.println("WiFi disconnected.");

    delay(500);
    return;
  }

  if (batchPending) {
    if (millis() - lastRetry >= RETRY_INTERVAL) {
      lastRetry = millis();
      flushBatch();
    }
    delay(10);
    return;
  }

  if (millis() - lastSample >= SAMPLE_INTERVAL) {
    lastSample = millis();
    collectSample();
  }
}

void collectSample() {
  float temperature = get_temp();
  float light = get_light();

  if (isnan(temperature) || isnan(light)) {
    Serial.println("Invalid sensor reading; sample skipped.");
    return;
  }
  String status = get_status(temperature, light);
  time_t sampleTime = time(nullptr);

  if (sampleTime < 1700000000) {
    Serial.println("Clock not synchronised; sample skipped.");
    return;
  }

  if (batchCount == 0) {
    batchStartMs = millis();
    batchEnvironmentalBytes = 0;
    performancePointQueued = false;
  }


  Point sensor("tyronesesp32");

  sensor.addTag("group", GROUP_ID);
  sensor.addTag("device_id", WiFi.macAddress());
  sensor.addTag("location", LOCATION);
  sensor.addTag("device_type", DEVICE_TYPE);
  sensor.addTag("firmware_version", FIRMWARE_VERSION);

  sensor.addField("temperature", temperature);
  sensor.addField("light", light);
  sensor.addField("status", status);

  sensor.addField("wifi_rssi", WiFi.RSSI());
  sensor.addField("uptime", millis() / 1000);
  sensor.addField("upload_interval",SAMPLE_INTERVAL / 1000);

  sensor.addField("temp_threshold", TEMP_THRESHOLD);
  sensor.addField("light_threshold", LIGHT_THRESHOLD);

  if (temperature > TEMP_THRESHOLD || light < LIGHT_THRESHOLD) {
    violations++;
  }

  sensor.addField("violations", (long)violations);
  sensor.setTime(sampleTime);
  String line = client.pointToLineProtocol(sensor);
  size_t pointBytes = line.length();

  if (pointBytes == 0) {
    Serial.println("Failed to create line-protocol record.");
    return;
  }

  Serial.print("Temperature: ");
  Serial.print(temperature, 2);
  Serial.println(" C");
  Serial.print("Light level: ");
  Serial.println(light, 2);

  Serial.print("Status: ");
  Serial.println(status);
  Serial.print("Timestamp: ");
  Serial.println((unsigned long)sampleTime);

  Serial.print("Line-protocol bytes: ");
  Serial.println(pointBytes);
  Serial.print("Writing: ");
  Serial.println(line);

  // Queue the point using the InfluxDB client's buffer.
  if (!client.writePoint(sensor)) {
    Serial.print("Failed to queue environmental point: ");
    Serial.println(client.getLastErrorMessage());
    return;
  }

  batchCount++;
  batchEnvironmentalBytes += pointBytes;
  totalAccumulatedBytes += pointBytes;

  Serial.print("Buffered environmental points: ");
  Serial.print(batchCount);
  Serial.print("/");
  Serial.println(BATCH_SIZE);

  Serial.print("Environmental bytes accumulated: ");
  Serial.println(batchEnvironmentalBytes);

  if (batchCount >= BATCH_SIZE) {
    batchPending = true;
    Serial.println();
    Serial.println("Environmental batch is ready.");
    flushBatch();
  }
}

bool flushBatch() {
  if (!batchPending) {
    return true;
  }

  unsigned long now = millis();
  double elapsedSeconds = (now - sessionStart) / 1000.0;
  double rate = 0.0;

  if (elapsedSeconds > 0.0) { rate = (double)(totalSuccessfulBytes + batchEnvironmentalBytes) / elapsedSeconds;}
  if (!performancePointQueued) {
    Point performance("tyronesesp32_performance");

    performance.addTag("group", GROUP_ID);
    performance.addTag("device_id", WiFi.macAddress());
    performance.addTag("location", LOCATION);
    performance.addTag("device_type", DEVICE_TYPE);
    performance.addField("writing_rate_bps", rate);
    performance.addField("batch_bytes", (long)batchEnvironmentalBytes);

    performance.addField("batch_points", batchCount);
    performance.addField("total_uploaded_bytes", (long)(totalSuccessfulBytes + batchEnvironmentalBytes));
    performance.addField("total_uploaded_points", (long)(totalSuccessfulPoints + batchCount));
    performance.addField("upload_attempts", (long)(totalUploadAttempts + 1));
    performance.addField("upload_failures", (long)totalUploadFailures);
    performance.setTime(time(nullptr));

    if (!client.writePoint(performance)) {
      Serial.println("Failed to queue performance measurement.");
      Serial.println(client.getLastErrorMessage());
      return false;
    }
    performancePointQueued = true;
  }

  Serial.print("Environmental points: ");
  Serial.println(batchCount);
  Serial.print("Environmental bytes: ");
  Serial.println(batchEnvironmentalBytes);

  Serial.print("Estimated average rate: ");
  Serial.print(rate, 2);
  Serial.println(" B/s");
  Serial.println("Calling client.flushBuffer()...");

  totalUploadAttempts++;
  unsigned long uploadStart = millis();
  bool success = client.flushBuffer();
  unsigned long uploadDuration = millis() - uploadStart;

  if (!success) {
    totalUploadFailures++;

    Serial.println("BATCH FLUSH FAILED");
    Serial.print("InfluxDB error: ");
    Serial.println(client.getLastErrorMessage());

    Serial.println(
      "Pending batch retained for retry if supported "
      "by the installed client version."
    );

    Serial.print("Buffer full: ");
    Serial.println(client.isBufferFull() ? "Yes" : "No");
    Serial.print("Buffer empty: ");
    Serial.println(client.isBufferEmpty() ? "Yes" : "No");
    printStatistics();
    return false;
  }

  totalSuccessfulBytes += batchEnvironmentalBytes;
  totalSuccessfulPoints += batchCount;
  lastSuccessfulFlushMs = millis();

  Serial.println("BATCH FLUSH SUCCESSFUL");
  Serial.print("Upload duration: ");
  Serial.print(uploadDuration);
  Serial.println(" ms");

  Serial.print("Successfully uploaded environmental bytes: ");
  Serial.println(totalSuccessfulBytes);
  Serial.print("Successfully uploaded environmental points: ");
  Serial.println(totalSuccessfulPoints);

  Serial.print("Client buffer empty: ");
  Serial.println(client.isBufferEmpty() ? "Yes" : "No");

  batchCount = 0;
  batchEnvironmentalBytes = 0;
  batchPending = false;
  performancePointQueued = false;
  printStatistics();
  return true;
}


void printStatistics() {
  Serial.println();
  Serial.print("Accumulated environmental bytes: ");
  Serial.println(totalAccumulatedBytes);
  Serial.print("Successfully flushed environmental bytes: ");
  Serial.println(totalSuccessfulBytes);
  Serial.print("Successful environmental points: ");
  Serial.println(totalSuccessfulPoints);
  Serial.print("Flush attempts: ");
  Serial.println(totalUploadAttempts);
  Serial.print("Failed flushes: ");
  Serial.println(totalUploadFailures);

  double elapsedSeconds = (millis() - sessionStart) / 1000.0;

  if (elapsedSeconds > 0) {
    double successfulRate = (double)totalSuccessfulBytes / elapsedSeconds;
    Serial.print("Session successful-flush rate: ");
    Serial.print(successfulRate, 2);
    Serial.println(" B/s");
  }
}

float get_light() {
  int adc = analogRead(PIN_LIGHT);
  float mapped = map(
    constrain(adc, 372, 2048), 372, 2048, 0, 4095);
  return 4095.0 - mapped;
}

String get_status(float temperature, float light) {
  bool hot = temperature > TEMP_THRESHOLD;
  bool dark = light < LIGHT_THRESHOLD;

  if (hot && dark) {
    return "HOT & DARK";
  }

  if (hot) {
    return "HOT";
  }

  if (dark) {
    return "DARK";
  }
  return "NORMAL";
}

float get_temp() {
  int adcValue = analogRead(PIN_ANALOG_IN);
  if (adcValue <= 0 || adcValue >= 4095) {
    return NAN;
  }

  double voltage =(double)adcValue / 4095.0 * 3.3;
  double Rt =10.0 * voltage / (3.3 - voltage);

  double tempK = 1.0 / (
    1.0 / (273.15 + 25.0) +
    log(Rt / 10.0) / 3950.0
  );

  return (float)(tempK - 273.15);
}
