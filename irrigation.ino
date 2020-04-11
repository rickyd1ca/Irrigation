#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Wire.h"

#define START_BUTTON 8
#define OLED_RESET 4
Adafruit_SSD1306 display(OLED_RESET);

struct DebouncedButton {
  int lastState;
  int lastDebounceTime;
  int debouncedState;
  int pin;
  boolean transition;
};



DebouncedButton startWateringButton;

struct WateringPeriod {
  byte startHour;
  byte startMinute;
  byte endHour;
  byte endMinute;
};

WateringPeriod wateringPeriods[] = {
  {6, 0, 6, 15},
  {18, 0, 18, 15},
  {19, 0, 19, 15},
  {21, 0, 21, 5},
  {21, 45, 21, 50},
  {22, 0, 22, 5}
};


#define AD_HOC_WATERING_PERIOD 300 // 5 minutes of ad-hoc watering
unsigned int adHocWateringEndTime = 0;

struct TimeFromRtc {
  byte second;
  byte minute;
  byte hour;
  byte dayOfWeek;
  byte dayOfMonth;
  byte month;
  byte year;
};


#define DS3231_I2C_ADDRESS 0x68


// Convert normal decimal numbers to binary coded decimal
byte decToBcd(byte val) {
  return ( (val / 10 * 16) + (val % 10) );
}
// Convert binary coded decimal to normal decimal numbers
byte bcdToDec(byte val) {
  return ( (val / 16 * 10) + (val % 16) );
}

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

void setDS3231time(byte second, byte minute, byte hour, byte dayOfWeek, byte
                   dayOfMonth, byte month, byte year) {
  // sets time and date data to DS3231
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0); // set next input to start at the seconds register
  Wire.write(decToBcd(second)); // set seconds
  Wire.write(decToBcd(minute)); // set minutes
  Wire.write(decToBcd(hour)); // set hours
  Wire.write(decToBcd(dayOfWeek)); // set day of week (1=Sunday, 7=Saturday)
  Wire.write(decToBcd(dayOfMonth)); // set date (1 to 31)
  Wire.write(decToBcd(month)); // set month
  Wire.write(decToBcd(year)); // set year (0 to 99)
  Wire.endTransmission();
}

void readTime(TimeFromRtc& time, int from) {
  Wire.beginTransmission(from);
  Wire.write(0); // set DS3231 register pointer to 00h
  Wire.endTransmission();
  Wire.requestFrom(from, 7);
  // request seven bytes of data from DS3231 starting from register 00h
  time.second = bcdToDec(Wire.read() & 0x7f);
  time.minute = bcdToDec(Wire.read());
  time.hour = bcdToDec(Wire.read() & 0x3f);
  time.dayOfWeek = bcdToDec(Wire.read());
  time.dayOfMonth = bcdToDec(Wire.read());
  time.month = bcdToDec(Wire.read());
  time.year = bcdToDec(Wire.read());
}

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

unsigned int timeInSeconds(const TimeFromRtc& time) {
  return time.hour * 3600 + time.minute * 60 + time.second;
}

void isWateringPeriod(boolean& watering, 
                      const TimeFromRtc& currentTime,
                      const WateringPeriod wateringPeriods[], 
                      const size_t numWateringPeriods, 
                      unsigned int& remainingTime) {
  watering = false;
  unsigned int currentTimeSecs = timeInSeconds(currentTime);
  int closestWateringPeriod = 0;
  unsigned int closestDifference = 24*3600; // maximum value of 24 hours
  for (int i = 0; i < numWateringPeriods; i++) {
    unsigned int wateringPeriodStartSecs = wateringPeriods[i].startHour * 3600 + wateringPeriods[i].startMinute * 60;
    unsigned int wateringPeriodEndSecs = wateringPeriods[i].endHour * 3600 + wateringPeriods[i].endMinute * 60;
    if ( currentTimeSecs >= wateringPeriodStartSecs && currentTimeSecs < wateringPeriodEndSecs ) {
      watering = true;
      remainingTime = wateringPeriodEndSecs - wateringPeriodEndSecs;
    }
  }
}

void writeState( boolean watering ) {
  if (watering == true) {
    digitalWrite(6, HIGH);
  } else {
    digitalWrite(6, LOW);
  }
}

void displayTime(byte hours, byte minutes, byte seconds) {
  if (hours < 10 ) display.print(0);
  display.print(hours);
  display.print(":");
  if (minutes < 10 ) display.print(0);
  display.print(minutes);
  display.print(":");
  if (seconds < 10 ) display.print(0);
  display.print(seconds);
}

void displayState(const Adafruit_SSD1306& display, const TimeFromRtc& currentTime, boolean watering, unsigned int remainingTimeSecs) {
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
  byte remainingHours = remainingTimeSecs / 3600;
  byte remainingMinutes = (remainingTimeSecs - remainingHours * 3600) / 60;
  byte remainingSeconds = (remainingTimeSecs - remainingHours * 3600 - remainingMinutes * 60);
  displayTime( remainingHours, remainingMinutes, remainingSeconds);

  display.display();
}

void isAdHocWatering(boolean& isWateringPeriod, const DebouncedButton& button, unsigned int& wateringTime, const TimeFromRtc& currentTime, unsigned int& remainingTime) {
  unsigned int currentTimeSecs = timeInSeconds(currentTime);
  if ( DebouncedButton_getTransition(button) == true && DebouncedButton_getState(button) == HIGH ) {
    if ( wateringTime == 0 ) {
      wateringTime = currentTimeSecs + 30;
    } else {
      wateringTime += 30;
    }
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
  unsigned int remainingTime = 0;
  unsigned int adHocRemainingTime = 0;

  // Read all inputs
  DebouncedButton_read(startWateringButton);
  readTime(currentTime, DS3231_I2C_ADDRESS);

  // Calculate state
  isWateringPeriod(wateringPeriod, currentTime, wateringPeriods, sizeof(wateringPeriods) / sizeof(WateringPeriod), remainingTime);
  isAdHocWatering(adHocWatering, startWateringButton, adHocWateringEndTime, currentTime, adHocRemainingTime);

  // Write all outputs
  writeState(wateringPeriod || adHocWatering);
  displayState(display, currentTime, wateringPeriod || adHocWatering, adHocRemainingTime > remainingTime ? adHocRemainingTime : remainingTime);
}
