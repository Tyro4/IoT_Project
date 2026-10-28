// MAIN BRANCH
#include <Arduino.h>

// -------------------------------------------------------------
// SENSOR STUFF - SORIN
// -------------------------------------------------------------

// GLOBAL VARIABLES
String fireRisk = "";
String waterStatus = "";
float temperature = 0.0;
int lightLevel = 0;
float waterPercentage = 0.0;

// GENERAL PIN DEFINITIONS
#define THERMISTOR_PIN 4
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

// SENSOR FUNCTION DECLARATIONS
void readTemperature();
void readSensors();
void calculateFireRisk();
void calculateWaterStatus();
void printStatus();

// --------------------------------------------------------------
// SENSOR FUNCTIONS - SORIN
// --------------------------------------------------------------
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
  if (temperature > 45 && lightLevel > 80) {
    fireRisk = FIRE_CATASTROPHIC;
  } else if (temperature > 35 && lightLevel > 70) {
    fireRisk = FIRE_EXTREME;
  } else if (temperature > 30 && lightLevel > 60) {
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

// -------------------------------------------------------------

void setup() {
  pinMode(trigPin, OUTPUT); // set trigPin to output mode
  pinMode(echoPin, INPUT); // set echoPin to input mode
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