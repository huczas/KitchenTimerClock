// Pin assignments and functionalities:

// Buzzer is connected to pins 9 and 10 (default configuration for the toneAC library).
// Rotary encoder is connected to pins 3 (CLK) and 4 (DT).
// Encoder button is connected to pin 2.
// LDR (Light Dependent Resistor) is connected to A2 (optional).
// TM1637 display module is connected to pins 5 (DIO) and 6 (CLK).
// RTC DS1307 module is connected to SDA (A4) and SCL (A5).

// RTC DS1307 (Real-Time Clock) is optional:
// - If connected, it maintains the time even when the device is powered off.
// - If not connected, the clock is set to the compilation time.

// Clock and timer functionalities:
// - The clock can be set by holding the button for 3 seconds.
// - The timer can be set in the same way (holding the button for 3 seconds).
// - The timer can be started, stopped, and reset using the button:
//   - Press to start/stop.
//   - Hold for 3 seconds to reset.
// - When the timer reaches 0, an alarm sounds.
// - The alarm can be stopped by pressing the button and reset by holding it for 3 seconds.

// Display brightness control:
// - Automatically adjusted using the LDR sensor:
//   - Brightness is set to maximum when LDR reads > 500.
//   - Brightness is set to minimum when LDR reads ≤ 500 or is not connected.
// - When the alarm is active, brightness is always set to maximum.

// Debugging:
// - The built-in LED (pin 13) is used to indicate the presence of the RTC module.
// - Serial communication is used for debugging purposes baud rate: 115200.

#include <TM1637Display.h> // author Avishay Orpaz
#include <Encoder.h> // author Paul Stoffregen
#include <Bounce2.h> // maintainer Thomas O Fredericks
#include <toneAC.h> // author Tim Eckel
#include <Wire.h>
#include <RTClib.h> // maintainer Adafruit

// #define DEBUG // uncomment to enable general DEBUG mode
// #define DEBUG_RTC // uncomment to enable RTC debug
// #define DEBUG_LDR // uncomment to enable LDR_PIN debug
// #define DEBUG_ENCODER // uncomment to enable encoder debug

RTC_DS1307 rtc;

const byte BUTTON_PIN = 2;
const byte ENCODER_PIN_A = 3; // external interrupt pin
const byte ENCODER_PIN_B = 4;
const byte DISPLAY_DATA_PIN = 5;
const byte DISPLAY_SCLK_PIN = 6;
const byte LDR_PIN = A2;

const byte ENCODER_PULSES_PER_STEP = 4;
const unsigned DISPLAY_REFRESH_INTERVAL = 100; // milliseconds
const unsigned LONG_PUSH_INTERVAL = 3000; // miliseconds
const unsigned SET_TIME_BLINK_MILLIS = 150;
const unsigned ALARM_BLINK_MILLIS = 700;
const unsigned BELL_REPEAT_INTERVAL = 10000; // miliseconds
const unsigned TIMER_DISPLAY_TIMEOUT = 60000; // miliseconds
const unsigned MINUTE_MILIS = 60000 - 6;
const unsigned CLOCK_MILIS_CORRECTION_PER_HOUR = 75;
const unsigned LDR_MAX_LIGHT = 500; // max for max display brightness

enum {
  CLOCK,
  SET_HOUR,
  SET_MINUTE,
  SET_TIMER,
  COUNTDOWN,
  ALARM
};

TM1637Display display(DISPLAY_SCLK_PIN, DISPLAY_DATA_PIN);
Encoder encoder(ENCODER_PIN_A, ENCODER_PIN_B);
Bounce button;

byte clockHour = 0;
byte clockMinute = 0;
bool rtcPresent = true;
unsigned long minuteMillis;
unsigned timerSeconds = 0;
byte displayData[4];

void setup() {
  #ifdef DEBUG
    Serial.begin(115200);
  #endif
  button.attach(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin();
  loadRTCTime();
}

void loop() {

  static unsigned long previousMillis;
  static unsigned long displayRefreshMillis;
  static unsigned long displayTimeoutMillis;
  static unsigned long alarmStartMillis;
  static unsigned timerStartSeconds;
  static byte blink;
  static byte state = CLOCK;

  unsigned long currentMillis = millis();

  // clock
  if (state == SET_HOUR || state == SET_MINUTE) {
    if (currentMillis - previousMillis >= SET_TIME_BLINK_MILLIS) {
      previousMillis = currentMillis;
      blink = !blink;
      if (state == SET_HOUR) {
        showClock(true, blink, true);
      } else {
        showClock(true, true, blink);
      }
    }
  } else if (currentMillis - minuteMillis > MINUTE_MILIS) {
    minuteMillis += MINUTE_MILIS;
    if (clockMinute < 59) {
      clockMinute++;
    } else {
      clockMinute = 0;
      clockHour = (clockHour < 23) ? (clockHour + 1) : 0;
      minuteMillis -= CLOCK_MILIS_CORRECTION_PER_HOUR;
      saveTimeToRTC();
    }
  }

  // every second
  if (currentMillis - previousMillis >= 1000) {
    previousMillis += 1000;
    switch (state) {
      case COUNTDOWN:
        if (timerSeconds > 0) {
          timerSeconds--;
          showTimer();
          if (timerSeconds == 0) {
            state = ALARM;
            alarmStartMillis = 0;
          }
        }
        break;
      case CLOCK:
        blink = !blink;
        showClock(blink, true, true); // update time display and blink the colon
        break;
    }
    if (!rtcPresent) {
      digitalWrite(LED_BUILTIN, blink);
    }
  }

  // encoder
  int dir = encoder.read();
  if (abs(dir) >= ENCODER_PULSES_PER_STEP) {
    #ifdef DEBUG_ENCODER
      Serial.print("Encoder direction: ");
      Serial.println(dir);
    #endif
    if (state == SET_HOUR) {
      if (dir > 0) {
        clockHour = (clockHour < 23) ? (clockHour + 1) : 0;
      } else {
        clockHour = (clockHour > 0) ? (clockHour - 1) : 23;
      }
    } else if (state == SET_MINUTE) {
      if (dir > 0) {
        clockMinute = (clockMinute < 59) ? (clockMinute + 1) : 0;
      } else {
        clockMinute = (clockMinute > 0) ? (clockMinute - 1) : 59;
      }
    } else {
      displayTimeoutMillis = 0; // reset timeout
      byte step;
      if (timerSeconds + dir > 6 * 60) {
        step = 60;
      } else if (timerSeconds + dir > 3 * 60) {
        step = 30;
      } else if (timerSeconds + dir > 60) {
        step = 15;
      } else if (timerSeconds == 0) {
        step = 10;
      } else {
        step = 5;
      }
      if (dir > 0) {
        if (state != COUNTDOWN) {
          step = step - (timerSeconds % step);
        }
        if (timerSeconds + step < 6000) { // 100 minutes
          timerSeconds += step;
          showTimer();
        }
      } else {
        if (state != COUNTDOWN) {
          int m = timerSeconds % step;
          if (m != 0) {
            step = m;
          }
        }
        if (timerSeconds >= step) {
          timerSeconds -= step;
          showTimer();
        }
      }
      if (state != COUNTDOWN && dir > 0) {
        state = SET_TIMER;
      }
    }
    encoder.write(0);
  }

  // button
  static unsigned long buttonPushedMillis;
  button.update();
  if (button.fell()) {
    buttonPushedMillis = currentMillis;
    #ifdef DEBUG
      Serial.println("Button pressed");
    #endif
  }
  if (buttonPushedMillis && currentMillis - buttonPushedMillis > LONG_PUSH_INTERVAL) {
    buttonPushedMillis = 0;
    displayTimeoutMillis = 0;
    switch (state) {
      case SET_TIMER:
        timerSeconds = 0;
        timerStartSeconds = 0;
        showTimer();
        break;
      case CLOCK:
        state = SET_HOUR;
        break;
    }
  }
  if (button.rose() && buttonPushedMillis != 0) {
    buttonPushedMillis = 0;
    displayTimeoutMillis = 0;
    #ifdef DEBUG
      Serial.println("Button released");
    #endif
    switch (state) {
      case SET_HOUR:
        state = SET_MINUTE;
        break;
      case SET_MINUTE:
        state = CLOCK;
        minuteMillis = millis();
        saveTimeToRTC();
        break;
      case CLOCK:
        state = SET_TIMER;
        showTimer();
        break;
      case COUNTDOWN:
        state = SET_TIMER;
        break;
      case ALARM:
        state = SET_TIMER;
        timerSeconds = timerStartSeconds;
        showTimer();
        break;
      case SET_TIMER:
        if (timerSeconds > 0) {
          state = COUNTDOWN;
          previousMillis = millis();
          if (timerStartSeconds < timerSeconds) {
            timerStartSeconds = timerSeconds;
          }
        } else {
          state = CLOCK;
          timerStartSeconds = 0;
        }
        break;
    }
  }

  // timer display timeout
  if (state == SET_TIMER) {
    if (displayTimeoutMillis == 0) {
      displayTimeoutMillis = currentMillis;
    }
    if ((currentMillis - displayTimeoutMillis > TIMER_DISPLAY_TIMEOUT)) {
      displayTimeoutMillis = 0;
      state = CLOCK;
      timerSeconds = 0;
      timerStartSeconds = 0;
    }
  }

  // alarm
  if (state == ALARM) {
    if (currentMillis - previousMillis >= ALARM_BLINK_MILLIS) {
      previousMillis = currentMillis;
      blink = !blink;
    }
    bool resetBell = false;
    if (currentMillis - alarmStartMillis > BELL_REPEAT_INTERVAL || !alarmStartMillis) {
      alarmStartMillis = currentMillis;
      resetBell = true;
    }
    bellSound(resetBell);
  }

  // display refresh
  if (currentMillis - displayRefreshMillis > DISPLAY_REFRESH_INTERVAL) {
    displayRefreshMillis = currentMillis;
    static int lastLDRReading = 1300; // init out of range
    if (state == ALARM) {
      display.setBrightness(7, blink);
      lastLDRReading = 1300;
    } else {
      unsigned a = analogRead(LDR_PIN);
      #ifdef DEBUG_LDR
        Serial.print("LDR Reading: ");
        Serial.println(a);
      #endif
      if (abs(a - lastLDRReading) > 10) {
        lastLDRReading = a;
        byte brightness;
        if (a > LDR_MAX_LIGHT) {
          brightness = 7;
        } else {
          brightness = map(a, 0, LDR_MAX_LIGHT, 1, 1);
        }
        display.setBrightness(brightness, true);
      }
    }
    display.setSegments(displayData);
  }

}

void loadRTCTime() {
  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    Serial.flush();
    while (1) delay(10);
  }
  if (!rtcPresent) return;
  if (! rtc.isrunning()) {
    Serial.println("RTC is NOT running, let's set the time!");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }
  DateTime now = rtc.now();
  #ifdef DEBUG_RTC
    Serial.println();
    Serial.print(now.year(), DEC);
    Serial.print('/');
    Serial.print(now.month(), DEC);
    Serial.print('/');
    Serial.print(now.day(), DEC);
    Serial.print(" (");
    Serial.print(") ");
    Serial.print(now.hour(), DEC);
    Serial.print(':');
    Serial.print(now.minute(), DEC);
    Serial.print(':');
    Serial.print(now.second(), DEC);
    Serial.println();
  #endif
  
  if (!now.isValid()) {
    rtcPresent = false;
    return;
  }
  clockHour = now.hour();
  clockMinute = now.minute();
  minuteMillis = millis() - (1000L * now.second());
}

void saveTimeToRTC() {
  if (!rtcPresent) return;
  rtc.adjust(DateTime(2000, 1, 1, clockHour, clockMinute, 0));
}

void showClock(bool showColon, bool showHour, bool showMinute) {

  byte digitBuffer[4];
  digitBuffer[0] = clockHour / 10;
  digitBuffer[1] = clockHour % 10;
  digitBuffer[2] = clockMinute / 10;
  digitBuffer[3] = clockMinute % 10;

  refreshDisplay(digitBuffer, showColon, showHour, showMinute);
}

void showTimer() {

  byte minutes = timerSeconds / 60;
  byte secs = timerSeconds % 60;

  byte digitBuffer[4];
  digitBuffer[0] = minutes / 10;
  digitBuffer[1] = minutes % 10;
  digitBuffer[2] = secs / 10;
  digitBuffer[3] = secs % 10;

  refreshDisplay(digitBuffer, true, true, true);
}

void refreshDisplay(byte digitBuffer[4], bool showColon, bool showLeft, bool showRight) {
  if (showLeft) {
    displayData[0] = !digitBuffer[0] ? 0 : display.encodeDigit(digitBuffer[0]);
    displayData[1] = display.encodeDigit(digitBuffer[1]);
  } else {
    displayData[0] = 0;
    displayData[1] = 0;
  }
  if (showRight) {
    displayData[2] = display.encodeDigit(digitBuffer[2]);
    displayData[3] = display.encodeDigit(digitBuffer[3]);
  } else {
    displayData[2] = 0;
    displayData[3] = 0;
  }
  if (showColon) {
    displayData[1] |= 0x80;
  }
}

void bellSound(bool restart) {
  const byte REPEAT_COUNT = 3;
  const int BELL_FREQUENCY = 2093; // C7
  const unsigned short LENGTH = 1200;
  const byte VOLUME_STEPS = 9;
  const unsigned short STEP_LENGTH = LENGTH / VOLUME_STEPS;

  static byte volume = VOLUME_STEPS;
  static byte count = 0;
  static unsigned long previousMillis;

  if (restart) {
    count = 0;
    volume = VOLUME_STEPS;
  }
  if (count == REPEAT_COUNT)
    return;
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis > STEP_LENGTH) {
    previousMillis = currentMillis;
    toneAC(BELL_FREQUENCY, volume, STEP_LENGTH * 2, true);
    volume--;
    if (volume == 0) {
      volume = VOLUME_STEPS;
      count++;
    }
  }
}
