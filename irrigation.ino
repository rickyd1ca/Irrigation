/*
Copyright 2020 Eric Dyke

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <DebouncedButton.h>
#include <CapacitiveMoistureSensor.h>
#include "Wire.h"
#include <LiquidCrystal_I2C.h>
#include <avr/wdt.h>
#include "AnalogInput.h"

// Enable to run sensor tests
//#define RUN_TESTS

#ifdef RUN_TESTS
#include "MockAnalogInput.h"
#endif

#define START_BUTTON 8
#define OLED_RESET 4

LiquidCrystal_I2C display = LiquidCrystal_I2C(0x27, 20, 4);

struct WateringPeriod {
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t startSecond;
  uint8_t endHour;
  uint8_t endMinute;
  uint8_t endSecond;
};

WateringPeriod wateringPeriods[] = {
  { 4,  0, 0,  4,  0, 30},
  { 8,  0, 0,  8,  0, 30},
  {12,  0, 0, 12,  0, 30},
  {16,  0, 0, 16,  0, 30},
  {20,  0, 0, 20,  0, 30},
  {23, 50, 0, 23, 50, 30}
};

AnalogInput sensor1(A3);
AnalogInput sensor2(A2);
AnalogInput sensor3(A1);

CapacitiveMoistureSensor humiditySensors[] = {
  sensor1,
  sensor2, 
  sensor3
}; 

#define AD_HOC_WATERING_PERIOD 300 // 5 minutes of ad-hoc watering
uint32_t adHocWateringEndTime = 0;
DebouncedButton startWateringButton(START_BUTTON);

struct TimeFromRtc {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t dayOfWeek;
  uint8_t dayOfMonth;
  uint8_t month;
  uint8_t year;
};

void SoilHumidity_readSensors(const CapacitiveMoistureSensor sensors[], size_t numSensors) {
  for( int i = 0; i < numSensors; i++ ) {
    sensors[i].read();
  }
}

boolean SoilHumidity_isSoilDry(const CapacitiveMoistureSensor sensors[], size_t numSensors, uint8_t& avgHumidity) {
  int numOperationalSensors = 0;
  float totalHumidity = 0;
  avgHumidity = 0;
  char stringBuffer[128];
  sprintf(stringBuffer, "SoilHumidity_isSoilDry: numSensors = %d", numSensors);
  Serial.println(stringBuffer);
  for(int i = 0; i < numSensors; i++ ) {
    sprintf(stringBuffer, "SoilHumidity_isSoilDry: currentSensor-1[%d]", i);
    Serial.println(stringBuffer);
    if (sensors[i].getMoisture() != CapacitiveMoistureSensor::SOIL_HUMIDITY_ERROR) {
      numOperationalSensors++;
    } else {
      // Sensor in error, continue
      sprintf(stringBuffer, "SoilHumidity_isSoilDry: currentSensor[%d] in error", i);
      Serial.println(stringBuffer);
      continue;
    }

    sprintf(stringBuffer, "SoilHumidity_isSoilDry: currentSensor[%d]", i);
    Serial.println(stringBuffer);
    totalHumidity += sensors[i].getMoistureLevel();
  }

  if(numOperationalSensors == 0) {
    // No operational sensors, return true 
    return true;
  }

  totalHumidity = totalHumidity/numOperationalSensors;
  avgHumidity = round(totalHumidity);

  return (totalHumidity < 80.0f);
}

// Convert binary coded decimal to normal decimal numbers
uint8_t bcdToDec(uint8_t val) {
  return ( (val / 16 * 10) + (val % 16) );
}

void readTime(TimeFromRtc& time, int from) {
  Wire.beginTransmission(from);
  Wire.write(0); // set DS3231 register pointer to 00h
  Wire.endTransmission();
  Wire.requestFrom(from, 7);
  // request seven uint8_ts of data from DS3231 starting from register 00h
  time.second = bcdToDec(Wire.read() & 0x7f);
  time.minute = bcdToDec(Wire.read());
  time.hour = bcdToDec(Wire.read() & 0x3f);
  time.dayOfWeek = bcdToDec(Wire.read());
  time.dayOfMonth = bcdToDec(Wire.read());
  time.month = bcdToDec(Wire.read());
  time.year = bcdToDec(Wire.read());
}

#define TIME_IN_SECS(hour, minute, second) ((uint32_t)hour * 3600 + (uint32_t)minute * 60 + (uint32_t)second)

uint32_t timeInSeconds(const TimeFromRtc& time) {
  return TIME_IN_SECS(time.hour, time.minute, time.second);
}

#define DS3231_I2C_ADDRESS 0x68

void setup() {

  // by default, we'll generate the high voltage from the 3.3v line internally! (neat!)
  display.init();
  display.backlight();

  Wire.begin();

  pinMode(6, OUTPUT);
  digitalWrite(6, LOW);

  Serial.begin(9600);

#ifndef RUN_TESTS
  wdt_enable(WDTO_1S);
#endif
}

void isWateringPeriod(boolean& watering, 
                      const TimeFromRtc& currentTime,
                      const WateringPeriod wateringPeriods[], 
                      const size_t numWateringPeriods, 
                      uint32_t& remainingTime) {
  watering = false;
  uint32_t currentTimeSecs = timeInSeconds(currentTime);
  int closestWateringPeriod = 0;
  uint32_t closestDifference = 86400UL; // maximum value of 24 hours
  for (int i = 0; i < numWateringPeriods; i++) {
    uint32_t wateringPeriodStartSecs = TIME_IN_SECS(wateringPeriods[i].startHour, wateringPeriods[i].startMinute, wateringPeriods[i].startSecond);
    uint32_t wateringPeriodEndSecs = TIME_IN_SECS(wateringPeriods[i].endHour, wateringPeriods[i].endMinute, wateringPeriods[i].endSecond);
    if ( currentTimeSecs >= wateringPeriodStartSecs && currentTimeSecs < wateringPeriodEndSecs ) {
      watering = true;
      remainingTime = wateringPeriodEndSecs - currentTimeSecs;
      return;
    } else {
      // Figure out the next watering perdiod
      if ( wateringPeriodStartSecs > currentTimeSecs && ((wateringPeriodStartSecs - currentTimeSecs) < closestDifference ))
      {
        closestDifference = wateringPeriodStartSecs - currentTimeSecs;
        closestWateringPeriod = i;
      }
    }
  }
  remainingTime = TIME_IN_SECS(wateringPeriods[closestWateringPeriod].startHour, 
                               wateringPeriods[closestWateringPeriod].startMinute, 
                               wateringPeriods[closestWateringPeriod].startSecond);
}

void writeState( boolean watering ) {
  if (watering == true) {
    digitalWrite(6, HIGH);
  } else {
    digitalWrite(6, LOW);
  }
}


void displayTime(uint32_t hours, uint32_t minutes, uint32_t seconds) {
  if (hours < 10 ) display.print(0);
  display.print(hours);
  display.print(":");
  if (minutes < 10 ) display.print(0);
  display.print(minutes);
  display.print(":");
  if (seconds < 10 ) display.print(0);
  display.print(seconds);
}

#define DISPLAY_COLUMNS_FIRST_LINE 20
void displayCentered(const LiquidCrystal_I2C& display, const char* string, uint8_t textSize) {
  uint8_t strLen = strlen(string);
  uint8_t displayColumns = DISPLAY_COLUMNS_FIRST_LINE;
  uint8_t whitespaces = displayColumns - strLen;
  uint8_t prefix = (whitespaces / 2) + (whitespaces & 0x01);

  for (int i=0; i < prefix; i++){
    display.print(" ");
  }
  display.print(string);
  
}

void displayFirstLine(const LiquidCrystal_I2C& display, const TimeFromRtc& currentTime) {
  static const char* dayOfWeeks[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

  display.setCursor(0, 0);
  if (currentTime.hour < 10) {
    display.print(0);
  }
  display.print(currentTime.hour);
  display.print(":");
  if (currentTime.minute < 10) {
    display.print(0);
  }
  display.print(currentTime.minute);
  for (uint8_t i = 0; i < 4; i++){
    display.print(" ");  
  }
  display.print(dayOfWeeks[currentTime.dayOfWeek - 1]);
  for (uint8_t i = 0; i < 3; i++){
    display.print(" ");  
  }
  
  if (currentTime.dayOfMonth < 10) {
    display.print(0);
  }
  display.print(currentTime.dayOfMonth);
  display.print("/");
  if (currentTime.month < 10) {
    display.print(0);
  }
  display.print(currentTime.month);
}

void displayState(const LiquidCrystal_I2C& display, const TimeFromRtc& currentTime, boolean watering, uint32_t remainingTimeSecs, 
                  const CapacitiveMoistureSensor humiditySensors[], uint8_t avgHumidity) {
  // Display current time
  display.setCursor(0, 0);
  displayFirstLine(display, currentTime);

  // Display state
  display.setCursor(0, 1);
  uint32_t remainingHours = remainingTimeSecs / 3600;
  uint32_t remainingMinutes = ((remainingTimeSecs - remainingHours * 3600) / 60);
  uint32_t remainingSeconds = ((remainingTimeSecs - remainingHours * 3600 - remainingMinutes * 60));
  if (watering) {
    display.print("WATERING       ");
    if (remainingMinutes < 10){
      display.print(0);
    }
    display.print(remainingMinutes);
    display.print(":");
    if (remainingSeconds < 10){
      display.print(0);
    }
    display.print(remainingSeconds);
  } else {
    display.print("NEXT           ");
    if (remainingHours < 10){
      display.print(0);
    }
    display.print(remainingHours);
    display.print(":");
    if (remainingMinutes < 10){
      display.print(0);
    }
    display.print(remainingMinutes);
  }

  display.setCursor(0, 2);
  for (int i = 0; i < 3; i++){
    display.print("CH");
    display.print(i+1);
    display.print(" ");
  }
  display.setCursor(0, 3);
  for (int i = 0; i < 3; i++){
    uint8_t moistureLevel = round(humiditySensors[i].getMoistureLevel());

    if (moistureLevel < 10) {
      display.print(" ");
    }
    display.print(round(humiditySensors[i].getMoistureLevel()));    
    display.print("  ");
  }

  display.print("      ");
  if (avgHumidity < 10) {
      display.print(" ");
  }
  display.print(avgHumidity);
}

void isAdHocWatering(boolean& isWateringPeriod, const DebouncedButton& button, uint32_t& wateringTime, const TimeFromRtc& currentTime, uint32_t& remainingTime) {
  uint32_t currentTimeSecs = timeInSeconds(currentTime);
  if ( button.getTransition() == true && button.getState() == HIGH ) {
    if ( wateringTime == 0 ) {
      wateringTime = currentTimeSecs + 30;
    } else {
      wateringTime += 30;
    }
  }

  if (wateringTime >= 86340UL) {
    wateringTime = 86340UL;
  }

  isWateringPeriod = (wateringTime > 0 && wateringTime > currentTimeSecs);

  if ( isWateringPeriod == true ) {
    remainingTime = wateringTime - currentTimeSecs;
  } else {
    wateringTime = 0;
  }
}


#ifdef RUN_TESTS
void testSoilHumidityDetection() {

  Serial.println("Starting soil test");
  MockAnalogInput mockSensor1;
  MockAnalogInput mockSensor2;
  MockAnalogInput mockSensor3;

  int mockSensor1Values[] =  {390,   400,   400,  800,   800};
  int mockSensor2Values[] =  {390,   390,   400,  800,   800};  
  int mockSensor3Values[] =  {390,   390,   390,  390,   800};
  boolean expectedOutput[] = {false, false, true, false, true};
  
  CapacitiveMoistureSensor sensors[] = {mockSensor1, mockSensor2, mockSensor3};

  char stringBuffer[256];


  for (int j = 0; j < sizeof(expectedOutput)/sizeof(expectedOutput[0]); j++){
    mockSensor1.setValue(mockSensor1Values[j]);
    mockSensor2.setValue(mockSensor2Values[j]);
    mockSensor3.setValue(mockSensor3Values[j]);
    
  
    for (int i = 0; i < 22; i++){
      SoilHumidity_readSensors(sensors, sizeof(sensors)/sizeof(sensors[0]));
      delay(250);
    }

    uint8_t avgHumidity = 0;
    boolean isSoilDry = SoilHumidity_isSoilDry(sensors, sizeof(sensors)/sizeof(sensors[0]), avgHumidity);
    if (isSoilDry == expectedOutput[j]) {
      sprintf(stringBuffer, "Soil is dry {%d,%d,%d} = %d is expected value, avg = %d", mockSensor1Values[j], mockSensor2Values[j], mockSensor3Values[j], expectedOutput[j], avgHumidity);
      Serial.println(stringBuffer);
    } else {
      sprintf(stringBuffer,"Soil is dry {%d,%d,%d} = %d is NOT expected value", mockSensor1Values[j], mockSensor2Values[j], mockSensor3Values[j], expectedOutput[j]);
      Serial.println(stringBuffer);
      sprintf(stringBuffer,"Test failed at j = %d", j);
      Serial.println(stringBuffer);
      break;
    }
  }  
  Serial.println("TESTS PASSED");
  delay(500);
}
#endif

void runIrrigation() {
  // put your main code here, to run repeatedly:
  TimeFromRtc currentTime = {};
  boolean wateringPeriod = false;
  boolean adHocWatering = false;
  uint32_t remainingTime = 0;
  uint32_t adHocRemainingTime = 0;

  // Read all inputs
  startWateringButton.read();
  readTime(currentTime, DS3231_I2C_ADDRESS);  
  SoilHumidity_readSensors(humiditySensors, sizeof(humiditySensors)/sizeof(humiditySensors[0])); 

  // Calculate state
  isWateringPeriod(wateringPeriod, currentTime, wateringPeriods, sizeof(wateringPeriods) / sizeof(WateringPeriod), remainingTime);
  isAdHocWatering(adHocWatering, startWateringButton, adHocWateringEndTime, currentTime, adHocRemainingTime);

  // Consider the 
  uint8_t avgHumidity = 0;
  boolean isSoilDry = SoilHumidity_isSoilDry(humiditySensors, sizeof(humiditySensors)/sizeof(humiditySensors[0]), avgHumidity);
  boolean wateringInPeriod = wateringPeriod && isSoilDry;

  // Write all outputs
  writeState(wateringInPeriod || adHocWatering);
  displayState(display, 
               currentTime, 
               wateringInPeriod || adHocWatering, adHocWatering ? adHocRemainingTime : remainingTime,
               humiditySensors,
               avgHumidity);

  // Kick the watchdog
  wdt_reset();             

  //send(msg.set((int32_t)));
  
}


void loop() {

#ifdef RUN_TESTS
  testSoilHumidityDetection();
#else
  runIrrigation();
#endif

}
