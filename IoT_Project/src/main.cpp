// MAIN BRANCH
#include <Arduino.h>

float temperature = 0.0;
int lightLevel = 0;
float waterPercentage = 0.0;

String fireRisk = "Moderate";
String waterStatus = "Full";

void readSensors();
void calculateFireRisk();
void calculateWaterStatus();
void printStatus();

void setup() {
  Serial.begin(115200);
  Serial.println("Bushfire Response System Initialised");
}

void loop() {
  readSensors();
  calculateFireRisk();
  calculateWaterStatus();
  printStatus();
  delay(5000); // Delay for 5 seconds before next reading
}

void readSensors() {
  // Simulate reading from sensors
  temperature = random(20, 50); // Random temperature between 20 and 50 degrees Celsius
  lightLevel = random(0, 100);   // Random light level between 0 and 100
  waterPercentage = random(0, 100); // Random water percentage between 0 and 100
}

void calculateFireRisk() {
  if (temperature > 45 && lightLevel > 80) {
    fireRisk = "Catastrophic";
  } else if (temperature > 35 && lightLevel > 70) {
    fireRisk = "Extreme";
  } else if (temperature > 30 && lightLevel > 60) {
    fireRisk = "High";
  } else {
    fireRisk = "Moderate";
  }
}

void calculateWaterStatus() {
  if (waterPercentage < 10) {
    waterStatus = "Critical";
  } else if (waterPercentage < 30) {
    waterStatus = "Low";
  } else if (waterPercentage < 60) {
    waterStatus = "Moderate";
  } else {
    waterStatus = "Full";
  }
}

void printStatus() {
  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" °C");

  Serial.print("Light Level: ");
  Serial.print(lightLevel);
  Serial.println(" %");

  Serial.print("Water Percentage: ");
  Serial.print(waterPercentage);
  Serial.println(" %");

  Serial.print("Fire Risk: ");
  Serial.println(fireRisk);

  Serial.print("Water Status: ");
  Serial.println(waterStatus);

  Serial.println("-----------------------------");
}