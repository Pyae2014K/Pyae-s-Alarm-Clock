#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include "time.h"

const char* ssid = "your-wifi-name";
const char* password = "your-wifi-password";

// --- Display pins ---
#define TFT_SCLK 0
#define TFT_MOSI 1
#define TFT_RST 2
#define TFT_DC 3
#define TFT_CS 4
// BL hardwired to GND (active-low display, always on) — no GPIO needed.

// --- Matrix pins (2 rows x 3 cols = 6 switches) ---
#define ROW0 5
#define ROW1 6
#define COL0 8
#define COL1 9
#define COL2 10

// --- Buzzer ---
#define BUZZER_PIN 7

// setOffsets() isn't public on the base class, so expose it via a subclass
class MyST7789 : public Adafruit_ST7789 {
public:
  MyST7789(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk, int8_t rst)
    : Adafruit_ST7789(cs, dc, mosi, sclk, rst) {}
  void setOffsets(uint8_t col, uint8_t row) {
    _colstart = _colstart2 = col;
    _rowstart = _rowstart2 = row;
  }
};

MyST7789 Pyae(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// --- Modes ---
enum Mode { MODE_CLOCK, MODE_SET_TIME, MODE_SET_ALARM, MODE_SET_TIMER };
Mode currentMode = MODE_CLOCK;

// --- Alarm / timer state ---
int alarmHour = 7;
int alarmMinute = 0;
bool alarmEnabled = true;
bool alarmRinging = false;

long timerSecondsRemaining = 0;
bool timerRunning = false;
unsigned long lastTimerTick = 0;

// --- Manual time-set staging values (used while in MODE_SET_TIME) ---
int setHour = 0;
int setMinute = 0;

int lastDisplayedMinute = -1;

// --- Matrix scanning with simple debounce ---
const int rowPins[2] = { ROW0, ROW1 };
const int colPins[3] = { COL0, COL1, COL2 };

int lastSwitchPressed = 0; // 0 = none, 1-6 = SW1-SW6
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// Returns 1-6 for SW1-SW6 if a NEW press is detected this call, else 0.
// SW1,SW2,SW3 = Row0 x Col0,Col1,Col2
// SW4,SW5,SW6 = Row1 x Col0,Col1,Col2
int scanMatrixForNewPress() {
  int pressed = 0;

  for (int r = 0; r < 2; r++) {
    // Drive only this row LOW, others stay HIGH (input pull-up columns read LOW when pressed)
    digitalWrite(rowPins[0], HIGH);
    digitalWrite(rowPins[1], HIGH);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(5); // let the line settle

    for (int c = 0; c < 3; c++) {
      if (digitalRead(colPins[c]) == LOW) {
        pressed = (r * 3) + c + 1; // 1-6
      }
    }
  }
  digitalWrite(rowPins[0], HIGH);
  digitalWrite(rowPins[1], HIGH);

  if (pressed != lastSwitchPressed) {
    lastDebounceTime = millis();
  }

  int result = 0;
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (pressed != 0 && lastSwitchPressed == 0) {
      result = pressed; // new press confirmed after debounce
    }
  }
  lastSwitchPressed = pressed;
  return result;
}

void drawCentered(const char* text) {
  Pyae.fillScreen(ST77XX_BLACK);
  Pyae.setCursor(0, 0);
  Pyae.setTextColor(ST77XX_WHITE);
  Pyae.setTextSize(6);
  Pyae.print(text);
}

void ringBuzzer(bool on) {
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);

  pinMode(ROW0, OUTPUT);
  pinMode(ROW1, OUTPUT);
  pinMode(COL0, INPUT_PULLUP);
  pinMode(COL1, INPUT_PULLUP);
  pinMode(COL2, INPUT_PULLUP);
  digitalWrite(ROW0, HIGH);
  digitalWrite(ROW1, HIGH);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Pyae.init(76, 284);
  Pyae.setOffsets(82, 18);
  Pyae.invertDisplay(false);
  Pyae.setRotation(1);
  Pyae.fillScreen(ST77XX_BLACK);
  Serial.println("TFT Initialized!");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
}

void loop() {
  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo);

  int pressedSwitch = scanMatrixForNewPress();

  // --- Handle switch presses ---
  if (pressedSwitch == 1) { // SW1: Stop alarm
    alarmRinging = false;
    ringBuzzer(false);
    if (timerRunning && timerSecondsRemaining <= 0) timerRunning = false;
    currentMode = MODE_CLOCK;
  } else if (pressedSwitch == 2) { // SW2: Timer -> set timer duration mode
    currentMode = MODE_SET_TIMER;
  } else if (pressedSwitch == 3) { // SW3: Change time -> set clock mode
    if (haveTime) {
      setHour = timeinfo.tm_hour;
      setMinute = timeinfo.tm_min;
    }
    currentMode = MODE_SET_TIME;
  } else if (pressedSwitch == 4) { // SW4: Set alarm mode
    currentMode = MODE_SET_ALARM;
  } else if (pressedSwitch == 5) { // SW5: Up
    if (currentMode == MODE_SET_TIME) {
      setHour = (setHour + 1) % 24;
    } else if (currentMode == MODE_SET_ALARM) {
      alarmHour = (alarmHour + 1) % 24;
    } else if (currentMode == MODE_SET_TIMER) {
      timerSecondsRemaining += 60; // add a minute per press
    }
  } else if (pressedSwitch == 6) { // SW6: Down
    if (currentMode == MODE_SET_TIME) {
      setHour = (setHour + 23) % 24;
    } else if (currentMode == MODE_SET_ALARM) {
      alarmHour = (alarmHour + 23) % 24;
    } else if (currentMode == MODE_SET_TIMER) {
      timerSecondsRemaining = max(0L, timerSecondsRemaining - 60);
    }
  }

  // --- Alarm check (only in clock mode, only once per minute) ---
  if (haveTime && currentMode == MODE_CLOCK && alarmEnabled && !alarmRinging) {
    if (timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMinute && timeinfo.tm_sec == 0) {
      alarmRinging = true;
    }
  }
  if (alarmRinging) {
    ringBuzzer((millis() / 500) % 2 == 0); // simple on/off beep pattern
  }

  // --- Timer countdown ---
  if (timerRunning && millis() - lastTimerTick >= 1000) {
    lastTimerTick = millis();
    timerSecondsRemaining--;
    if (timerSecondsRemaining <= 0) {
      timerSecondsRemaining = 0;
      timerRunning = false;
      alarmRinging = true; // reuse the same ringing/dismiss logic
    }
  }

  // --- Display ---
  char buf[16];
  switch (currentMode) {
    case MODE_CLOCK:
      if (haveTime && timeinfo.tm_min != lastDisplayedMinute) {
        lastDisplayedMinute = timeinfo.tm_min;
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
        drawCentered(buf);
      }
      break;
    case MODE_SET_TIME:
      snprintf(buf, sizeof(buf), "%02d:%02d", setHour, setMinute);
      drawCentered(buf);
      break;
    case MODE_SET_ALARM:
      snprintf(buf, sizeof(buf), "A%02d:%02d", alarmHour, alarmMinute);
      drawCentered(buf);
      break;
    case MODE_SET_TIMER:
      snprintf(buf, sizeof(buf), "T%02ld:%02ld", timerSecondsRemaining / 60, timerSecondsRemaining % 60);
      drawCentered(buf);
      break;
  }

  delay(20); // keep matrix scanning responsive without hammering the CPU
}
