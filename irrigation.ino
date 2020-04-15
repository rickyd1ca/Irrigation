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

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Wire.h"

#define START_BUTTON 8
#define OLED_RESET 4
Adafruit_SSD1306 display(OLED_RESET);

struct DebouncedButton {
  uint8_t lastState;
  uint8_t lastDebounceTime;
  uint8_t debouncedState;
  uint8_t pin;
  boolean transition;
};

struct WateringPeriod {
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t endHour;
  uint8_t endMinute;
};

WateringPeriod wateringPeriods[] = {
  {6, 0, 6, 15},
  {22, 0, 22, 15}
};

#define AD_HOC_WATERING_PERIOD 300 // 5 minutes of ad-hoc watering
uint32_t adHocWateringEndTime = 0;
DebouncedButton startWateringButton;

struct TimeFromRtc {
  uint8_t second;
  uint8_t minute;
  uint8_t hour;
  uint8_t dayOfWeek;
  uint8_t dayOfMonth;
  uint8_t month;
  uint8_t year;
};


#define DEBOUNCE_BUTTON_DELAY 50
void DebouncedButton_init(DebouncedButton& button, int pin) {
  button.pin = pin;
  button.lastState = LOW;
  button.debouncedState = LOW;
  button.lastDebounceTime = 0;
  button.transition = false;
  pinMode(button.pin, INPUT);
}

void DebouncedButton_read(DebouncedButton& button) {
  int reading = digitalRead(button.pin);
  button.transition = false;

  if (reading != button.lastState) {
    button.lastDebounceTime = millis();
    button.lastState = reading;
  }

  if ((millis() - button.lastDebounceTime) > DEBOUNCE_BUTTON_DELAY) {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (button.lastState != button.debouncedState) {
      button.debouncedState = button.lastState;
      button.transition = true;
    }
  }
}

int DebouncedButton_getState(const DebouncedButton& button) {
  return button.debouncedState;
}

boolean DebouncedButton_getTransition(const DebouncedButton& button) {
  return button.transition;
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
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3D (for the 128x64)

  // Clear the buffer.
  display.clearDisplay();

  Wire.begin();

  pinMode(6, OUTPUT);
  digitalWrite(6, LOW);

  DebouncedButton_init(startWateringButton, START_BUTTON);
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
    uint32_t wateringPeriodStartSecs = TIME_IN_SECS(wateringPeriods[i].startHour, wateringPeriods[i].startMinute, 0);
    uint32_t wateringPeriodEndSecs = TIME_IN_SECS(wateringPeriods[i].endHour, wateringPeriods[i].endMinute, 0);
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
  remainingTime = TIME_IN_SECS(wateringPeriods[closestWateringPeriod].startHour, wateringPeriods[closestWateringPeriod].startMinute, 0);
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

void displayState(const Adafruit_SSD1306& display, const TimeFromRtc& currentTime, boolean watering, uint32_t remainingTimeSecs) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);

  // Display current time
  displayTime(currentTime.hour, currentTime.minute, currentTime.second);
  
  display.setCursor(0, 17);

  if (watering) {
    display.print("Watering");
  } else {
    display.print("Idle");
  }

  display.setCursor(0, 34);
  uint32_t remainingHours = remainingTimeSecs / 3600;
  uint32_t remainingMinutes = ((remainingTimeSecs - remainingHours * 3600) / 60);
  uint32_t remainingSeconds = ((remainingTimeSecs - remainingHours * 3600 - remainingMinutes * 60));
  displayTime( remainingHours, remainingMinutes, remainingSeconds);
  display.display();
}

void isAdHocWatering(boolean& isWateringPeriod, const DebouncedButton& button, uint32_t& wateringTime, const TimeFromRtc& currentTime, uint32_t& remainingTime) {
  uint32_t currentTimeSecs = timeInSeconds(currentTime);
  if ( DebouncedButton_getTransition(button) == true && DebouncedButton_getState(button) == HIGH ) {
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

void loop() {
  // put your main code here, to run repeatedly:
  TimeFromRtc currentTime = {};
  boolean wateringPeriod = false;
  boolean adHocWatering = false;
  uint32_t remainingTime = 0;
  uint32_t adHocRemainingTime = 0;

  // Read all inputs
  DebouncedButton_read(startWateringButton);
  readTime(currentTime, DS3231_I2C_ADDRESS);

  // Calculate state
  isWateringPeriod(wateringPeriod, currentTime, wateringPeriods, sizeof(wateringPeriods) / sizeof(WateringPeriod), remainingTime);
  isAdHocWatering(adHocWatering, startWateringButton, adHocWateringEndTime, currentTime, adHocRemainingTime);

  // Write all outputs
  writeState(wateringPeriod || adHocWatering);
  displayState(display, currentTime, wateringPeriod || adHocWatering, adHocWatering ? adHocRemainingTime : remainingTime);
}
