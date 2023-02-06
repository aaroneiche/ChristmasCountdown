
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

#include <WiFi.h>

#include <WiFiUdp.h>
#include "NTP.h"

#include "credentials.h"
#include <Bounce2.h>

#include <TimeLib.h>

#include <Preferences.h>

// The I2C address of the RTC.
int address = 0x51;

// The structure that we manage a date in.
struct DateTime
{
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

DateTime remaining;

int secondC = 0;
int halfSecondC = 0;
int quarterSecondC = 0;
int tenthSecondC = 0;

int completeDisplayCounter = 0;
int ledOffset = 0;

int secondOverflowCounter = 0;
int halfsecondOverflowCounter = 0;
int quarterSecondOverflowCounter = 0;

int leftButtonHeldCounter = 0;
int rightButtonHeldCounter = 0;

bool secondOverflow = false;
bool halfSecondOverflow = false;
bool quarterSecondOverflow = false;

//Debounce init.
Bounce bounceRight = Bounce();
Bounce bounceLeft = Bounce();

enum DisplayMode
{
  Day = 1,
  Minute = 2,
  Set = 3,
  Complete = 4
};

DisplayMode currentDisplayMode = Minute;

enum SettingValue
{
  SetDay = 1,
  SetHour = 2,
  SetMinute = 3,
  SetSecond = 4,
  SetMonth = 5,
  SetYear = 6,
  SetTargetDay = 7,
  SetTargetHour = 8,
  SetTargetMinute = 9,
  SetTargetSecond = 10,
  SetTargetMonth = 11,
  SetTargetYear = 12
};

SettingValue currentSettingValue = SetYear;

// counting occurs every 1/4 sec, so 48 * .25 = 12s between display modes.
int switchPeriod = 12;
int switchCounter = 0;

#define clockInputPin 5
#define rightButtonPin 19
#define leftButtonPin 2
#define LED_PIN 3

/*
Wifi Credentials are stored in a file called "credentials.h"
The contents of that should look like this:

#define WIFI_SSID "WifiNetworkName"
#define WIFI_PASSWORD "WifiPassword"

Make sure you don't commit this file into your repo.
*/
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

Adafruit_NeoPixel leds(14, LED_PIN, NEO_GRB + NEO_KHZ800);

int timesInterruptCalled = 0;
bool updateDisplayFlag = false;
bool clockInterruptFlag = false;
bool leftPressedFlag = false;
bool rightPressedFlag = false;

bool clockRunning = false;
bool settingBlinkOn = false;

uint32_t dimRed = leds.Color(30, 0, 0);
uint32_t dimGreen = leds.Color(0, 30, 0);
uint32_t dimWhite = leds.Color(25, 25, 25);
uint32_t dimBlue = leds.Color(0, 0, 25);

uint32_t brightRed = leds.Color(255, 255, 255);
uint32_t brightGreen = leds.Color(255, 255, 255);
uint32_t brightWhite = leds.Color(255, 255, 255);

uint32_t off = leds.Color(0, 0, 0);

uint32_t displayBuffer[14];

tmElements_t tm;
tmElements_t ctm;
time_t targetDT;
time_t currentDT;

Preferences preferences;

WiFiUDP wifiUdp;
NTP ntp(wifiUdp);

uint8_t bcdToDec(uint8_t b)
{
  return (((b >> 4) * 10) + (b % 16));
}

byte decToBcd(byte b){
  Serial.print((b / 10 * 16) + (b % 10), HEX);
  return ( (b / 10 * 16) + (b % 10) );
}

/* RTC interactions */

tmElements_t readTimeFromRTC()
{
  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x01); // Seconds Address
  Wire.endTransmission();

  tmElements_t timeReadOut;

  Wire.requestFrom(address, 7);
  timeReadOut.Second = bcdToDec(Wire.read() & 0x7f);
  timeReadOut.Minute = bcdToDec(Wire.read() & 0x7f);
  timeReadOut.Hour = bcdToDec(Wire.read() & 0x3f);
  
  timeReadOut.Day = bcdToDec(Wire.read() & 0xff);
  //Skip the day of the week.
  Wire.read();
  timeReadOut.Month = bcdToDec(Wire.read() & 0xff);
  timeReadOut.Year = bcdToDec(Wire.read() & 0xff);

  Wire.endTransmission();
  // currentDT = makeTime(ctm);
  return timeReadOut;
}

void writeTimeToRTC(tmElements_t time) {  

  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x01); // Start register
  Wire.write(decToBcd(time.Second));
  Wire.write(decToBcd(time.Minute));
  Wire.write(decToBcd(time.Hour));

  Wire.write(decToBcd(time.Day));
  Wire.write(0); //We don't care about the day of the week.
  Wire.write(decToBcd(time.Month));
  Wire.write(decToBcd(time.Year));
  Wire.endTransmission();
}

void pauseRTC()
{
  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x2e);
  Wire.write((uint8_t)0x01);
  Wire.endTransmission();
}

void unPauseRTC()
{
  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x2e);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
}


void readPreferences() {
  preferences.begin("prefs", false);
  preferences.getInt("target_year", -1);
  preferences.getInt("target_month", -1);
  preferences.getInt("target_day", -1);
  preferences.getInt("target_hour",-1);
  preferences.getInt("target_minute",-1);
  preferences.getInt("target_second",-1);

}

/* 
  A test function for reading from the RTC
  This prints out the current hour, minute and second.
*/
void clockTest()
{
  Wire.beginTransmission(address);
  Wire.write((uint8_t)0x01); // Seconds
  Wire.endTransmission();

  Wire.requestFrom(address, 7);
  int sec = bcdToDec(Wire.read() & 0x7f);
  int min = bcdToDec(Wire.read() & 0x7f);
  int hour = bcdToDec(Wire.read() & 0x3f);
  
  int day = bcdToDec(Wire.read() & 0x3f);
  Wire.read(); //read the day of week - don't care.
  int month = bcdToDec(Wire.read() & 0xf);
  int year = bcdToDec(Wire.read() & 0xff);
  
  Wire.endTransmission();

  Serial.printf("%i/%i/%i %i:%i:%i\n",year-30, month, day, hour, min, sec);
}

/* Sets remaining vars to remaining time. return true is target is met,
or false if time remains */
bool utToRemaining(time_t &current, time_t &target)
{
  long diff = target - current;
  if (diff < 0)
  {
    // If we're at or have surpassed the target
    //  return;
    return true;
  }

  int days = floor(int(diff / 86400));
  long diffHours = diff - (86400 * days);
  int hours = floor(int(diffHours / 3600));
  long diffMin = diffHours - (3600 * hours);
  int minutes = floor(int(diffMin / 60));
  int seconds = diffMin - (60 * minutes);

  // Update our "remaining" values. These are used in the display.
  remaining.day = days;
  remaining.hour = hours;
  remaining.minute = minutes;
  remaining.second = seconds;
  return false;
}

void IRAM_ATTR clockInterrupt()
{
  clockInterruptFlag = true;
}

void setup()
{

  // Buttons have hardware pullups, so no need to set them here.
  bounceLeft.attach(leftButtonPin, INPUT);
  bounceRight.attach(rightButtonPin, INPUT);

  // This line is important for if you need to use pin18 or 19 internal pullups.
  //  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);

  leds.begin();           // INITIALIZE NeoPixel leds object (REQUIRED)
  leds.show();            // Turn OFF all pixels ASAP
  leds.setBrightness(50); // Set BRIGHTNESS to about 1/5 (max = 255)

  // Serial output for debug and info.
  Serial.begin(115200);
  Serial.println("ChristmasCountdown, Started up.");


  // Startup comms with the RTC.
  Wire.begin(7, 6);

  tmElements_t time = readTimeFromRTC();
  Serial.printf("Time read from RTC: %i/%i/%i %i:%i:%i", time.Year, time.Month, time.Day, time.Hour, time.Minute, time.Second);
  //We really only need to know if the year is 0, if so, the clock is not set.
  if(time.Year == 0) {
    //Clock not set. Set defaults, setup clock. 

    //Default RTC settings
    Wire.beginTransmission(address);
    Wire.write((uint8_t)0x23); // Start register
    Wire.write((uint8_t)0x00); // 0x23
    Wire.write((uint8_t)0x00); // 0x24 Two's complement offset value
    Wire.write((uint8_t)0x00); // 0x25 Normal offset correction, enable low-jitter mode, set load caps to 7.0pF
    Wire.write((uint8_t)0x00); // 0x26 Battery switch reg, same as after a reset
    Wire.write((uint8_t)0x00); // 0x27 Enable CLK pin, using bits set in reg 0x28
    Wire.write((uint8_t)0x45); // 0x28 Realtime clock mode, periodic interrupts, CLK pin generates 1024hz signal
    Wire.write((uint8_t)0x40); // 0x29
    Wire.write((uint8_t)0x00); // 0x2a
    Wire.endTransmission();

    // create a default current time (2022/12/01 0:0:0)
    tmElements_t defaults;
    defaults.Year = 52;
    defaults.Month = 12;
    defaults.Day = 1;
    defaults.Hour = 0;
    defaults.Minute = 0;
    defaults.Second = 0;
    
    writeTimeToRTC(defaults);

    //Read the time from the RTC.
    time = readTimeFromRTC();
    
    //Just to confirm our updated time. 
    Serial.printf("Time set to: %i/%i/%i %i:%i:%i", time.Year, time.Month, time.Day, time.Hour, time.Minute, time.Second);
  }else{
    time = readTimeFromRTC();
  }
  currentDT = makeTime(time);

  //Read target time from preferences. 

  //Set default time TEMPORARY - this should be from preferences.
  tm.Day = 25;
  tm.Hour = 0;
  tm.Minute = 0;
  tm.Second = 0;
  tm.Month = 12;
  tm.Year = 52;

  targetDT = makeTime(tm);


  Serial.print("The target: ");
  Serial.println(targetDT);
  Serial.print("Current: ");
  Serial.println(currentDT);
  
  //The last thing we'll do on setup is enable the clock interrupt pin, so we can start counting.
  pinMode(clockInputPin, INPUT_PULLUP);
  attachInterrupt(clockInputPin, clockInterrupt, FALLING);
}
unsigned long last = 0;

/*
 This abstracts away writing to the display from the actual contents of it.
 To turn LEDS on, generate a uint32_t array of colors, and pass it to this.
*/
void displayFromBuffer(uint32_t *dataBuffer)
{
  leds.clear();
  for (int i = 0; i < 14; i++)
  {
    // Serial.printf("this: %p", dataBuffer[i]);
    leds.setPixelColor(i, dataBuffer[i]);
  }
  leds.show();
}

void clearBuffer(uint32_t *dataBuffer)
{
  for (int i = 0; i < 14; i++)
  {
    dataBuffer[i] = off;
  }
}

void setTopRow(int value, uint32_t buffer[])
{
  for (int i = 0; i < 6; i++)
  {
    if ((value & (1 << i)) > 0)
    {
      // Set the LED - offset i by 6 because we're 6 LEDs into the strand
      buffer[i + 6] = dimRed;
    }
  }
}

void setBottomRow(int value, uint32_t buffer[])
{
  for (int i = 5; i >= 0; i--)
  {
    if ((value & (1 << i)) > 0)
    {
      buffer[abs(i - 5)] = dimGreen;
    }
  }
}

/*
  Sets the provided buffer to the current remaining time, targetDT and currentDT.
*/
void timeRemainingToBuffer(uint32_t buffer[])
{
  clearBuffer(buffer);
  int topVal = (currentDisplayMode == Day) ? remaining.day : remaining.minute;
  int bottomVal = (currentDisplayMode == Day) ? remaining.hour : remaining.second;

  if (currentDisplayMode == Day)
  {
    buffer[12] = dimWhite;
  }
  else if (currentDisplayMode == Minute)
  {
    buffer[13] = dimWhite;
  }

  setTopRow(topVal, buffer);
  setBottomRow(bottomVal, buffer);
}

/* 
 Writes the "set time" displays to the buffer. 
*/
void settingTimeToBuffer(uint32_t buffer[])
{
  clearBuffer(buffer);
  Serial.printf("Setting val: %i", currentSettingValue);
  switch (currentSettingValue)
  {
  case SetDay:
    if (settingBlinkOn == true)
    {
      setBottomRow(55, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Day, buffer);
    break;
  case SetHour:
    if (settingBlinkOn == true)
    {
      setBottomRow(59, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Hour, buffer);
    break;
  case SetMinute:
    if (settingBlinkOn == true)
    {
      setBottomRow(61, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Minute, buffer);
    break;
  case SetSecond:
    if (settingBlinkOn == true)
    {
      setBottomRow(62, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Second, buffer);
    break;
  case SetMonth:
    if (settingBlinkOn == true)
    {
      setBottomRow(47, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Month, buffer);
    break;
  case SetYear:
    if (settingBlinkOn == true)
    {
      setBottomRow(31, buffer);
    }
    else
    {
      setBottomRow(63, buffer);
    }
    setTopRow(ctm.Year - 30, buffer);
    break;
  case SetTargetDay:
    if (settingBlinkOn == true)
    {
      setBottomRow(8, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Day, buffer);
    break;
  case SetTargetHour:
    if (settingBlinkOn == true)
    {
      setBottomRow(4, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Hour, buffer);
    break;
  case SetTargetMinute:
    if (settingBlinkOn == true)
    {
      setBottomRow(2, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Minute, buffer);
    break;
  case SetTargetSecond:
    if (settingBlinkOn == true)
    {
      setBottomRow(1, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Second, buffer);
    break;
  case SetTargetMonth:
    if (settingBlinkOn == true)
    {
      setBottomRow(16, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Month, buffer);
    break;
  case SetTargetYear:
    if (settingBlinkOn == true)
    {
      setBottomRow(32, buffer);
    }
    else
    {
      setBottomRow(0, buffer);
    }
    setTopRow(tm.Year - 30, buffer);
    break;
  }
}


void ledChaseToBuffer(uint32_t color, uint32_t *dataBuffer, int offset)
{
  clearBuffer(dataBuffer);

  // For each 3rd pixel, display it at this color:
  for (int l = offset; l <= 13; l = l + 3)
  {
     dataBuffer[l] = color;
  }
}

void printTime()
{
  // Serial.printf("%i/%i %i:%i:%i\n", current.month, current.day, current.hour, current.minute, current.second);
  Serial.printf("Remaining: %i days %i:%i:%i\n", remaining.day, remaining.hour, remaining.minute, remaining.second);
}

void leftPressedChange() {

  if(currentDisplayMode == Minute) {
    currentDisplayMode = Day;
  }else if(currentDisplayMode == Day) {
    currentDisplayMode = Minute;
  }

  //Temporary testing
  if(currentDisplayMode == Complete) {
    currentDisplayMode = Minute;
  }


  if (currentDisplayMode == Set) {

    /* Set Current Values */
    if (currentSettingValue == SetYear)
    {
      currentSettingValue = SetMonth;
    }
    else if (currentSettingValue == SetMonth)
    {
      currentSettingValue = SetDay;
    }
    else if (currentSettingValue == SetDay)
    {
      currentSettingValue = SetHour;
    }
    else if (currentSettingValue == SetHour)
    {
      currentSettingValue = SetMinute;
    }
    else if (currentSettingValue == SetMinute)
    {
      currentSettingValue = SetSecond;
    }
    /* Set Target Values */

    else if(currentSettingValue == SetSecond)
    {
      currentSettingValue = SetTargetYear;
    }
    else if (currentSettingValue == SetTargetYear)
    {
      currentSettingValue = SetTargetMonth;
    }
    else if (currentSettingValue == SetTargetMonth)
    {
      currentSettingValue = SetTargetDay;
    }
    else if (currentSettingValue == SetTargetDay)
    {
      currentSettingValue = SetTargetHour;
    }
    else if (currentSettingValue == SetTargetHour)
    {
      currentSettingValue = SetTargetMinute;
    }
    else if (currentSettingValue == SetTargetMinute)
    {
      currentSettingValue = SetTargetSecond;
    } 
    else if (currentSettingValue == SetTargetSecond) 
    {
      currentSettingValue = SetYear;
    }
    Serial.printf("Current Setting value: %i",currentSettingValue);
  }
}

void rightPressedChange() {

      if (currentDisplayMode == Minute || currentDisplayMode == Day)
      {
        if (currentDisplayMode == Minute)
        {
          currentDisplayMode = Day;
        }
        else
        {
          currentDisplayMode = Minute;
        }
      }

      if (currentDisplayMode == Set)
      {

        switch (currentSettingValue)
        {
        case SetDay:
          ctm.Day++;
          if (ctm.Day > 31)
          {
            ctm.Day = 1;
          }
          break;
        case SetHour:
          ctm.Hour++;
          if (ctm.Hour > 23)
          {
            ctm.Hour = 0;
          }
          break;
        case SetMinute:
          ctm.Minute++;
          if (ctm.Minute > 59)
          {
            ctm.Minute = 0;
          }
          break;
        case SetSecond:
          ctm.Second++;
          if (ctm.Second > 59)
          {
            ctm.Second = 0;
          }
          break;
        case SetMonth:
          ctm.Month++;
          if (ctm.Month > 12)
          {
            ctm.Month = 1;
          }
          break;
        case SetYear:
          ctm.Year++;
          // The datetime lib works off of 1970, so all values are years since 1970.
          // The overflow is 2038, as the unix timestamp wraps then. I don't know if the
          // Library I'm using deals with it.
          if (ctm.Year > 68)
          {
            ctm.Year = 50;
          }
          break;
        case SetTargetDay:
          tm.Day++;
          if (tm.Day > 31)
          {
            tm.Day = 1;
          }
          break;
        case SetTargetHour:
          tm.Hour++;
          if (tm.Hour > 23)
          {
            tm.Hour = 0;
          }
          break;
        case SetTargetMinute:
          tm.Minute++;
          if (tm.Minute > 59)
          {
            tm.Minute = 0;
          }
          break;
        case SetTargetSecond:
          tm.Second++;
          if (tm.Second > 59)
          {
            tm.Second = 0;
          }
          break;
        case SetTargetMonth:
          tm.Month++;
          if (tm.Month > 12)
          {
            tm.Month = 1;
          }
          break;
        case SetTargetYear:
          tm.Year++;
          // The datetime lib works off of 1970, so all values are years since 1970.
          // The overflow is 2038, as the unix timestamp wraps then. I don't know if the
          // Library I'm using deals with it.
          if (tm.Year > 68)
          {
            tm.Year = 50;
          }
          break;

        }

        switchCounter = 0;
      }  
}


void loop()
{
  // Update the button states
  bounceRight.update();
  bounceLeft.update();

  // If the right button has changed...
  if (bounceRight.changed())
  {
    int debouncedRight = bounceRight.read();
    // LOW is active.
    if (debouncedRight == false)
    {
      rightPressedChange();
      rightPressedFlag = true;
      Serial.println("Right Pressed");
      updateDisplayFlag = true;
    }
    else
    {
      // If changed to LOW, do this
      Serial.println("Right Released");
      rightPressedFlag = false;
      rightButtonHeldCounter = 0;
    }
  }

  // If the left button has changed...
  if (bounceLeft.changed())
  {

    int debouncedLeft = bounceLeft.read();
    // LOW is active.
    if (debouncedLeft == false)
    {
      Serial.println("Left pressed");
      leftPressedFlag = true;
      leftPressedChange();
      updateDisplayFlag = true;
    }
    else
    {
      Serial.println("Left Released");
      leftPressedFlag = false;
      leftButtonHeldCounter = 0;
    }
  }

  // The Clock interrupt is responsible to making sure events are triggered.
  // The RTC clock pin is configured to fire 1024 times a second. It triggers according to 
  // time divisions. 1024 is 1 second. 512 is 0.5 seconds. Counters will increment, and 
  // overflow when they hit the appropriate number. This is also a good place to 
  // time how long and event has occurred (such as a button press)
  if (clockInterruptFlag)
  {
    secondOverflowCounter++;
    halfsecondOverflowCounter++;
    quarterSecondOverflowCounter++;

    // Second overflow count. RTC runs at 1024/s
    if (secondOverflowCounter > 1024)
    {
      secondOverflow = true;
      secondOverflowCounter = 0;
      switchCounter++;
    }

    if (leftPressedFlag)
    {
      leftButtonHeldCounter++;
      if (leftButtonHeldCounter >= 3072)
      {
        if (currentDisplayMode == Minute || currentDisplayMode == Day || currentDisplayMode == Complete)
        {
          currentDisplayMode = Set;
          leftButtonHeldCounter = 0;
          //pause RTC
          pauseRTC();

          ctm = readTimeFromRTC();
          currentDT = makeTime(ctm);
          
          settingBlinkOn = true;
          updateDisplayFlag = true;
        }
        else
        {
          // Update the countdown value with the newly set time 
          // If you haven't changed the time, then it will set it to whatever the current time is.

          //Set the TargetDT to the new (or unchanged) value.
          targetDT = makeTime(tm);

          //Make sure we pause the RTC before updating the time. 
          pauseRTC();

          //Write time to RTC
          writeTimeToRTC(ctm);

          //Unpause after we've written;
          unPauseRTC();

          //Get the time from the RTC, set the currentDT
          ctm = readTimeFromRTC();
          currentDT = makeTime(ctm);

          currentDisplayMode = Minute;
          leftButtonHeldCounter = 0;
        }
      }
    }

    /* Currently I don't have anything running on the right button 3s hold.  */
    if (rightPressedFlag)
    {
      rightButtonHeldCounter++;
      if (rightButtonHeldCounter >= 3072)
      {
        // Serial.println("held right button for 3+ seconds.");
      }
    }

    if (halfsecondOverflowCounter > 512)
    {
      halfSecondOverflow = true;
      halfsecondOverflowCounter = 0;
    }

    if (quarterSecondOverflowCounter > 256)
    {
      quarterSecondOverflow = true;
      quarterSecondOverflowCounter = 0;
    }

    //Clear the flag once we've done everything associated with it.
    clockInterruptFlag = false;
  }

  if (quarterSecondOverflow) {
    if (currentDisplayMode == Complete)
    {
      // Sequence through the day mode complete methods
      ledOffset++;
      if (ledOffset > 2)
      {
        ledOffset = 0;
      }
      completeDisplayCounter = (completeDisplayCounter >= 12) ? 0 : completeDisplayCounter + 1;
      updateDisplayFlag = true;
    }

    quarterSecondOverflow = false;
  }

  if (halfSecondOverflow)
  {

    /* If we're setting the time, toggle the settingBlinkOn to create blinking. */
    if (currentDisplayMode == Set)
    {
      settingBlinkOn = !settingBlinkOn;
      updateDisplayFlag = true;
    }

    halfSecondOverflow = false;
  }

  if (secondOverflow)
  {

    if (currentDisplayMode == Minute || currentDisplayMode == Day)
    {
      // Add one to currentDT.
      currentDT++;

      if(switchCounter == switchPeriod) {
        switchCounter = 0;
        //Switch the display every n seconds automatically.
        currentDisplayMode = (currentDisplayMode == Minute) ? Day : Minute;
      }

      // Check for complete countdown.
      bool complete = utToRemaining(currentDT, targetDT);

      if (complete)
      {
        currentDisplayMode = Complete;
      }

      updateDisplayFlag = true;
      secondOverflow = false;
    }
  }

  // Any of the above methods might set the updateDisplayFlag.
  // Update display should always be the last thing that happens.
  if (updateDisplayFlag)
  {

    if(currentDisplayMode == Complete) {
      ledChaseToBuffer(dimRed, displayBuffer, ledOffset);
    }

    if (currentDisplayMode == Minute || currentDisplayMode == Day)
    {
      timeRemainingToBuffer(displayBuffer);
    }

    if (currentDisplayMode == Set)
    {
      settingTimeToBuffer(displayBuffer);
    }

    displayFromBuffer(displayBuffer);
    updateDisplayFlag = false;
  }
}