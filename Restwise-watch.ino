// Restwise - Restwise smartwatch firmware for the ESP32-C6 (custom PCB)
// Copyright (C) 2026 Ashutosh
//
// Port of StrawberryOS8266 (ESP8266 NodeMCU v1) to the ESP32-C6-MINI-1-N4
// custom Restwise PCB. All UI/app code is unchanged from the ESP8266 build;
// only the platform layer differs. See PORTING NOTES below.
//
// See the LICENSE file for more details.
//
// ---------------------------------------------------------------------------
// PORTING NOTES (ESP8266 NodeMCU v1 -> ESP32-C6 custom PCB)
// ---------------------------------------------------------------------------
// * WiFi library is <WiFi.h> (ESP32 core), not <ESP8266WiFi.h>. Radio-off is
//   now WiFi.mode(WIFI_OFF) + btStop() -- the ESP32-C6 also has BLE (unlike
//   the ESP8266), so both radios are explicitly killed, not just WiFi.
//   forceSleepBegin() doesn't exist on ESP32 and isn't needed: WIFI_OFF
//   already fully powers the radio down on this core.
// * EEPROM (settings) and LittleFS (Restwise timetable) work identically on
//   the ESP32 Arduino core -- no changes needed versus the ESP8266 version.
// * Buttons are still polled from loop(), same as the ESP8266 build (no
//   FreeRTOS reader task here either, for parity with that version).
// * The RTC stays OPTIONAL, same software-clock-from-millis() fallback as
//   before, seeded at 12:00 if no DS3231 answers on the bus.
// * Light sleep note: the ESP32-C6 *can* wake from light sleep on any GPIO
//   (unlike the ESP8266, which couldn't -- that's why the 8266 build dropped
//   sleep entirely). This port intentionally keeps the same behaviour as the
//   ESP8266 version for now (display-blank only, no MCU sleep) so the two
//   builds stay in parity. Real light-sleep power savings are a real ESP32-C6
//   capability worth adding later, not part of this port.
//
// WIRING (custom Restwise PCB, per schematic/netlist)
//   IO23 -> UP     IO21 -> DOWN   IO19 -> LEFT   IO20 -> RIGHT   IO3 -> CENTER
//   GPIO6 -> SCL   GPIO14 -> SDA
//   All buttons wire to GND (active-low, internal pull-ups -- no external
//   resistors needed).
//
//   Note: only GPIO0-7 are in the ESP32-C6's LP (low-power) domain and can
//   wake the chip from *deep* sleep. IO3 (CENTER) is the one button pin in
//   that range; it was deliberately chosen for CENTER for that reason, in
//   case deep-sleep wake-on-button-press is added later. Light sleep (see
//   note above) has no such restriction and works on all five button pins.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <RTClib.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RTC_DS3231 rtc;

// I2C
#define PIN_SDA 14
#define PIN_SCL 6

// Buttons (see wiring note above)
#define BTN_UP     23
#define BTN_DOWN   21
#define BTN_LEFT   19
#define BTN_RIGHT  20
#define BTN_CENTER 3

bool buttonStates[5] = {false, false, false, false, false};
const uint8_t buttonPins[5] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER};

uint8_t statusIndex = 0;
bool isStatusActive = false;

// --- Timekeeping ---------------------------------------------------------
// rtcPresent is decided once at boot. When there's no DS3231 on the bus the
// watch falls back to a software clock driven by millis(), based at 12:00, so
// it stays usable (and demo-able) instead of dying on a "RTC Error!" halt.
bool rtcPresent = false;
DateTime softBase(2026, 1, 1, 12, 0, 0);
unsigned long softBaseMillis = 0;

enum ScreenState {
  SCREEN_WATCHFACE, SCREEN_MENU, SCREEN_STOPWATCH, SCREEN_TIMER, SCREEN_TIMER_ALERT,
  SCREEN_CALCULATOR, SCREEN_CALCULATOR_SCI,
  SCREEN_PIN_ENTRY, SCREEN_SECURITY_MENU, SCREEN_SECURITY_CONFIRM,
  SCREEN_LOCK_SETTINGS, SCREEN_TERMINAL,
  SCREEN_RESTWISE, SCREEN_RESTWISE_DAY, SCREEN_GOODNIGHT
};
ScreenState currentScreen = SCREEN_WATCHFACE;

bool lastButtonStates[5] = {false, false, false, false, false};
bool buttonJustPressed[5] = {false, false, false, false, false};

const int NUM_MENU_ITEMS = 8;
const char* menuItems[NUM_MENU_ITEMS] = {"Restwise", "Stopwatch", "Timer", "Calculator", "Security", "Lock Screen", "Terminal", "Lock"};
int menuIndex = 0;

unsigned long swStartTime = 0;
unsigned long swElapsedTime = 0;
bool swRunning = false;
int swFocus = 0;

enum TimerMode { TM_SETTING, TM_READY, TM_RUNNING, TM_RINGING };
TimerMode tmMode = TM_SETTING;
int tmHours = 0, tmMinutes = 0, tmSeconds = 0;
int tmActiveUnit = 2;
int tmFocus = 1;
unsigned long tmRemainingMillis = 0;
unsigned long tmLastTick = 0;
unsigned long tmSetPressStart = 0;
bool tmLongPressTriggered = false;

bool isAnimating = false;
int animOffsetY = 0;
int animTargetY = 0;
ScreenState animNextScreen = SCREEN_WATCHFACE;

unsigned long lastActivityTime = 0;
const int lockTimeoutOptions[4] = {5, 10, 15, 30};
int displayTimeoutSec = 5;
unsigned long DISPLAY_TIMEOUT_MS = 5000;
bool displayOn = true;
int lockSettingsCursor = 0;

// Calculator variables
int calcCursorRow = 0, calcCursorCol = 0;
String calcInput1 = "";
String calcInput2 = "";
char calcOp = ' ';
bool calcIsOpSet = false;
bool calcError = false;

int calcSciRow = 0, calcSciCol = 0;

// SCI functions grid: 3 rows x 2 cols
const char* sciGrid[3][2] = {
  {"x^2",  "sqrt"},
  {"pi",   "x^3"},
  {"cbrt", "BACK"}
};

const char* calcGrid[5][4] = {
  {"7",   "8",  "9",   "/"},
  {"4",   "5",  "6",   "*"},
  {"1",   "2",  "3",   "-"},
  {"0",   ".",  "+",   "="},
  {"<--", "C",  "DEL", "SCI"}
};

// --- Security (PIN only) ---
enum SecurityFlow {
  SEC_NONE,
  SEC_LOCK,        // watchface -> menu gate
  SEC_VERIFY,      // enter current PIN (uses pendingAction)
  SEC_SET_NEW,     // enter new PIN
  SEC_SET_CONFIRM  // confirm new PIN
};
enum SecPendingAction { ACT_NONE, ACT_OPEN_MENU, ACT_CHANGE, ACT_DISABLE };
enum SecConfirmType { CONF_NONE, CONF_ENABLE, CONF_DISABLE };

SecurityFlow securityFlow = SEC_NONE;
SecPendingAction pendingAction = ACT_NONE;
SecConfirmType confirmType = CONF_NONE;
int confirmSelection = 1; // 0=Yes, 1=No
ScreenState pinReturnScreen = SCREEN_MENU;

bool pinSet = false;
String pinStored = "";
String pinBuffer = "";
String pinPendingNew = "";
int pinCursorRow = 0, pinCursorCol = 1;
String pinErrorMsg = "";
unsigned long pinErrorShownAt = 0;

int secMenuCursor = 0; // -1 = back, 0/1 = items

const char* pinKeys[4][3] = {
  {"7", "8", "9"},
  {"4", "5", "6"},
  {"1", "2", "3"},
  {"<", "0", "OK"}
};

// --- Settings storage (EEPROM) ---
#define EEPROM_SIZE 64
#define SETTINGS_MAGIC 0x52573031UL   // "RW01"

struct StoredSettings {
  uint32_t magic;
  uint8_t  pinSet;
  char     pin[5];      // 4 digits + NUL
  uint8_t  timeoutSec;
};

// --- Terminal (USB command console) ---
// Wire protocol from strawberry-terminal (Python): a line "CONFIRM <cmd> <args>"
// puts the watch into TERM_CONFIRM, showing <cmd>/<args> with a Yes/No prompt.
// The button choice is sent back as "ACK <cmd> YES" or "ACK <cmd> NO". Only
// "set_time <YYYY-MM-DD HH:MM:SS>" is understood right now.
enum TerminalState { TERM_IDLE, TERM_CONFIRM };
TerminalState termState = TERM_IDLE;
String termLineBuf = "";
String termPendingCmdName = "";
String termPendingArgs = "";
int termConfirmSelection = 0; // 0=Yes, 1=No

// strawberry-terminal sends "PING" every 2s while a session is open. A ping
// within the last 4s means a live host is attached; that's what flips the idle
// screen from "Listening" to "Connected!".
bool termConnected = false;
unsigned long termLastPingAt = 0;
const unsigned long TERM_PING_STALE_MS = 4000;

// --- Restwise (timetable app) --------------------------------------------
// The synced timetable is stored in LittleFS at /timetable.rw, one block per
// line: "<daysBitmask>|<startMin>|<endMin>|<label>". daysBitmask bit0=Mon .. bit6=Sun.
// startMin/endMin are minutes since midnight. Water breaks are just ordinary
// blocks in this list (the website already splits them in before syncing).
#define RW_MAX_BLOCKS   180
#define RW_DAY_MAX       96   // max blocks shown for a single day
#define RW_DAY_ROWS       4   // visible timetable rows in the day view
#define RW_FILE "/timetable.rw"

struct RwBlock {
  uint8_t  days;       // bit0=Mon ... bit6=Sun
  uint16_t startMin;
  uint16_t endMin;
  char     label[20];
};
RwBlock rwBlocks[RW_MAX_BLOCKS];
int  rwBlockCount = 0;
bool rwHasData = false;

int  rwCursor = -1;          // -1 = back arrow, 0 = Today, 1..6 = Mon..Sat
int  rwSelectedDayItem = 0;  // which day-list item the day view is showing
int  rwDayScroll = 0;        // first visible timetable row in the day view

// Sync protocol (over USB serial, while the Restwise screen is active):
//   Host->ESP RW_HELLO   ESP->Host RW_HELLO      (connect / keep-alive ping)
//   Host->ESP RW_BEGIN   ESP->Host RW_READY      (start upload; file opened)
//   Host->ESP <block line> ... (repeated)        (written straight to the file)
//   Host->ESP RW_END     ESP->Host RW_OK <count> (file closed, reparsed)
enum RwSyncState { RWS_IDLE, RWS_RECV };
RwSyncState rwSyncState = RWS_IDLE;
String rwLineBuf = "";
bool rwConnected = false;
unsigned long rwLastHelloAt = 0;
const unsigned long RW_HELLO_STALE_MS = 4000;
File rwFile;
int rwRecvCount = 0;

void updateDisplay();
void drawCalculator(int yOffset);
void drawCalculatorSci(int yOffset);
void drawScreen(ScreenState screen, int yOffset);
void drawHeader(int yOffset, const char* appName = nullptr, bool backFocused = false);
void drawWatchFace(int yOffset);
void drawMenu(int yOffset);
void drawStopwatch(int yOffset);
void drawTimer(int yOffset);
void drawTimerAlert(int yOffset);
void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size = 1);
void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted);
void startAnimation(ScreenState next, int targetOffset);
void readButtons();
void drawPinEntry(int yOffset);
void drawSecurityMenu(int yOffset);
void drawSecurityConfirm(int yOffset);
void handlePinSubmit();
void loadSettings();
void saveSettings();
void enterPinScreen(SecurityFlow flow, ScreenState returnTo);
void drawLockSettings(int yOffset);
void drawTerminal(int yOffset);
void processTerminalSerial();
void applyTerminalCommand(bool granted);
void setupButtons();
DateTime nowTime();
void setDeviceTime(const DateTime &dt);
void drawRestwise(int yOffset);
void drawRestwiseDay(int yOffset);
void drawGoodNight(int yOffset);
void processRestwiseSerial();
void rwLoadFromFile();
int  rwBuildDayList(int dayIdx, int* out, int maxOut);
int  rwResolveDayIndex(int item);
const char* rwDayName(int item);
bool isNightNow();

void setup() {
  Serial.begin(115200);
  delay(100);

  // Restwise is deliberately a no-radio device -- kill both radios outright
  // rather than merely leaving them unused, so the chip never transmits.
  // (The ESP32-C6 has BLE in addition to WiFi, unlike the ESP8266, so both
  // get stopped explicitly here.)
  WiFi.mode(WIFI_OFF);
  btStop();

  EEPROM.begin(EEPROM_SIZE);

  // LittleFS holds the synced Restwise timetable. Format once if the flash has
  // never been mounted (first boot on a fresh chip) so the mount always succeeds.
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }

  setupButtons();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000); // 400kHz I2C for fast, flicker-free OLED refresh
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1) { delay(100); yield(); }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
  delay(100);

  // The RTC is optional. If it isn't on the bus we log it and run on the
  // software clock -- the watch must never halt just because the DS3231 is
  // missing.
  if (rtc.begin()) {
    rtcPresent = true;
    if (rtc.lostPower()) {
      Serial.println(F("RTC lost power - seeding to 12:00"));
      rtc.adjust(DateTime(2026, 1, 1, 12, 0, 0));
    }
    Serial.println(F("RTC found."));
  } else {
    rtcPresent = false;
    softBase = DateTime(2026, 1, 1, 12, 0, 0);
    softBaseMillis = millis();
    Serial.println(F("RTC NOT found - using software clock from 12:00"));
  }

  Serial.println(F("Display initialized!"));

  loadSettings();
  rwLoadFromFile();

  lastActivityTime = millis();
}

void loop() {
  readButtons();

  bool activityDetected = false;
  for (int i = 0; i < 5; i++) {
    buttonJustPressed[i] = buttonStates[i] && !lastButtonStates[i];
    lastButtonStates[i] = buttonStates[i];
    if (buttonStates[i]) activityDetected = true;
  }

  if (activityDetected) {
    lastActivityTime = millis();
    if (!displayOn) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayOn = true;
      animOffsetY = 0;
      isAnimating = false;

      // Terminal and Restwise read Serial only while they're the active screen,
      // so waking must never bounce out of them (that would silently drop any
      // command/payload sent while the panel was blanked). For every other
      // screen: at night, greet with Good Night; otherwise snap to the
      // watchface when a PIN is set (so the lock gate re-triggers), else resume
      // whatever screen you were on.
      bool inSerialApp = (currentScreen == SCREEN_TERMINAL ||
                          currentScreen == SCREEN_RESTWISE ||
                          currentScreen == SCREEN_RESTWISE_DAY);
      if (!inSerialApp) {
        if (isNightNow()) currentScreen = SCREEN_GOODNIGHT;
        else if (pinSet)  currentScreen = SCREEN_WATCHFACE;
      }

      for (int i = 0; i < 5; i++) buttonJustPressed[i] = false;
    }
  }

  // Serial ownership: the Terminal app has its own protocol, so it reads Serial
  // while active. EVERY other screen hands Serial to the Restwise sync listener.
  // This matters because opening the USB port from a PC resets the chip -- it
  // reboots to the watchface, not the Restwise screen -- so the sync has to be
  // reachable from anywhere. processRestwiseSerial() jumps to the Restwise
  // screen on its own the moment a real sync handshake arrives.
  if (currentScreen == SCREEN_TERMINAL) {
    processTerminalSerial();
  } else {
    processRestwiseSerial();
  }

  // Idle blanking runs on every screen the user might be sitting on. The single
  // exception is animations -- blanking mid-slide would strand the transition
  // halfway. This gives the Lock Screen timeout its intended global effect.
  bool allowBlank = !isAnimating;

  if (displayOn && allowBlank) {
    if (millis() - lastActivityTime > DISPLAY_TIMEOUT_MS) {
      display.ssd1306_command(SSD1306_DISPLAYOFF);
      displayOn = false;
    }
  }

  if (!displayOn && currentScreen == SCREEN_STOPWATCH && swRunning) {
      display.ssd1306_command(SSD1306_DISPLAYON);
      displayOn = true;
      lastActivityTime = millis();
  }

  // The timer countdown must keep ticking even while the display is blanked
  // (e.g. you started it and walked back to the watchface).
  if (tmMode == TM_RUNNING) {
    unsigned long now = millis();
    unsigned long delta = now - tmLastTick;
    if (tmRemainingMillis > delta) {
      tmRemainingMillis -= delta;
      tmLastTick = now;
    } else {
      tmRemainingMillis = 0;
      tmMode = TM_RINGING;
      currentScreen = SCREEN_TIMER_ALERT;
      if (!displayOn) {
        display.ssd1306_command(SSD1306_DISPLAYON);
        displayOn = true;
      }
      lastActivityTime = millis();
    }
  }

  if (displayOn) {

    if (isAnimating) {
      if (animOffsetY < animTargetY) animOffsetY += 8;
      else if (animOffsetY > animTargetY) animOffsetY -= 8;

      if (abs(animOffsetY - animTargetY) < 8) {
        animOffsetY = 0;
        currentScreen = animNextScreen;
        isAnimating = false;
      }
    } else {

      if (currentScreen == SCREEN_WATCHFACE) {
        if (buttonJustPressed[0]) {
          if (pinSet) {
            enterPinScreen(SEC_LOCK, SCREEN_WATCHFACE);
          } else {
            startAnimation(SCREEN_MENU, -64);
          }
        }
      }
      else if (currentScreen == SCREEN_MENU) {
        if (buttonJustPressed[0]) {
          menuIndex = (menuIndex - 1 + NUM_MENU_ITEMS) % NUM_MENU_ITEMS;
        }
        if (buttonJustPressed[1]) {
          menuIndex = (menuIndex + 1) % NUM_MENU_ITEMS;
        }
        if (buttonJustPressed[4]) {
          if (menuIndex == 0) {
            rwCursor = -1;
            startAnimation(SCREEN_RESTWISE, -64);
          } else if (menuIndex == 1) {
            swFocus = 1;
            startAnimation(SCREEN_STOPWATCH, -64);
          } else if (menuIndex == 2) {
            tmMode = TM_SETTING;
            tmFocus = 1;
            startAnimation(SCREEN_TIMER, -64);
          } else if (menuIndex == 3) {
            calcCursorRow = 0; calcCursorCol = 0;
            calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
            startAnimation(SCREEN_CALCULATOR, -64);
          } else if (menuIndex == 4) {
            if (pinSet) {
              pendingAction = ACT_OPEN_MENU;
              enterPinScreen(SEC_VERIFY, SCREEN_MENU);
            } else {
              confirmType = CONF_ENABLE;
              confirmSelection = 0;
              startAnimation(SCREEN_SECURITY_CONFIRM, -64);
            }
          } else if (menuIndex == 5) {
            lockSettingsCursor = -1;
            for (int i = 0; i < 4; i++) if (lockTimeoutOptions[i] == displayTimeoutSec) lockSettingsCursor = i;
            if (lockSettingsCursor == -1) lockSettingsCursor = 0;
            startAnimation(SCREEN_LOCK_SETTINGS, -64);
          } else if (menuIndex == 6) {
            termState = TERM_IDLE;
            termLineBuf = "";
            startAnimation(SCREEN_TERMINAL, -64);
          } else if (menuIndex == 7) {
            startAnimation(SCREEN_WATCHFACE, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_STOPWATCH) {

        if (buttonJustPressed[0]) {
          if (swFocus == 1 || swFocus == 2) swFocus = 0;
        }
        if (buttonJustPressed[1]) {
          if (swFocus == 0) swFocus = 1;
        }
        if (buttonJustPressed[2]) {
          if (swFocus == 2) swFocus = 1;
        }
        if (buttonJustPressed[3]) {
          if (swFocus == 1) swFocus = 2;
        }

        if (buttonJustPressed[4]) {
          if (swFocus == 0) {
            startAnimation(SCREEN_MENU, 64);
          } else if (swFocus == 1) {
            if (swRunning) {
              swElapsedTime += millis() - swStartTime;
              swRunning = false;
            } else {
              swStartTime = millis();
              swRunning = true;
            }
          } else if (swFocus == 2) {
            if (swRunning) {
              swElapsedTime += millis() - swStartTime;
              swRunning = false;
            } else {
              swElapsedTime = 0;
            }
          }
        }
      }
      else if (currentScreen == SCREEN_TIMER) {

        if (buttonJustPressed[0]) {
          if (tmFocus > 0) tmFocus = 0;
        }
        if (buttonJustPressed[1]) {
          if (tmFocus == 0) tmFocus = 1;
        }
        if (buttonJustPressed[2]) {
          if (tmMode == TM_SETTING) {
            if (tmFocus == 2) tmFocus = 1;
            else if (tmFocus == 3) tmFocus = 2;
            else if (tmFocus == 1) tmFocus = 0;
          } else if (tmMode == TM_READY) {
            if (tmFocus == 3) tmFocus = 1;
            else if (tmFocus == 1) tmFocus = 0;
          }
        }
        if (buttonJustPressed[3]) {
          if (tmMode == TM_SETTING) {
            if (tmFocus == 1) tmFocus = 2;
            else if (tmFocus == 2) tmFocus = 3;
          } else if (tmMode == TM_READY) {
            if (tmFocus == 1) tmFocus = 3;
          }
        }

        if (buttonStates[4] && tmFocus == 2 && tmMode == TM_SETTING) {
          if (tmSetPressStart == 0) tmSetPressStart = millis();
          if (millis() - tmSetPressStart > 2000 && !tmLongPressTriggered) {
            tmMode = TM_READY;
            tmFocus = 3;
            tmLongPressTriggered = true;
          }
        } else {
          tmSetPressStart = 0;
          tmLongPressTriggered = false;
        }

        if (buttonJustPressed[4]) {
          if (tmFocus == 0) {
            startAnimation(SCREEN_MENU, 64);
          }
          else if (tmMode == TM_SETTING) {
            if (tmFocus == 1) {
              if (tmActiveUnit == 0) tmHours = (tmHours + 1) % 24;
              else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 1) % 60;
              else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 1) % 60;
            } else if (tmFocus == 2) {
              tmActiveUnit = (tmActiveUnit + 1) % 3;
            } else if (tmFocus == 3) {
              if (tmActiveUnit == 0) tmHours = (tmHours + 23) % 24;
              else if (tmActiveUnit == 1) tmMinutes = (tmMinutes + 59) % 60;
              else if (tmActiveUnit == 2) tmSeconds = (tmSeconds + 59) % 60;
            }
          }
          else if (tmMode == TM_READY) {
            if (tmFocus == 1) tmMode = TM_SETTING;
            else if (tmFocus == 3) {
              tmRemainingMillis = ((unsigned long)tmHours * 3600 + tmMinutes * 60 + tmSeconds) * 1000;
              if (tmRemainingMillis > 0) {
                tmMode = TM_RUNNING;
                tmLastTick = millis();
              }
            }
          }
          else if (tmMode == TM_RUNNING) {
            if (tmFocus == 1) tmMode = TM_READY;
            else if (tmFocus == 3) {

            }
          }
        }
      }
      else if (currentScreen == SCREEN_TIMER_ALERT) {
        if (buttonJustPressed[0] || buttonJustPressed[1] || buttonJustPressed[2] ||
            buttonJustPressed[3] || buttonJustPressed[4]) {
          tmMode = TM_SETTING;
          startAnimation(SCREEN_WATCHFACE, 64);
        }
      }
      else if (currentScreen == SCREEN_CALCULATOR) {
        // Navigation: 5 rows x 4 cols, wrap around
        if (buttonJustPressed[0]) calcCursorRow = (calcCursorRow > 0) ? calcCursorRow - 1 : 4;
        if (buttonJustPressed[1]) calcCursorRow = (calcCursorRow < 4) ? calcCursorRow + 1 : 0;
        if (buttonJustPressed[2]) {
          calcCursorCol--;
          if (calcCursorCol < 0) calcCursorCol = 3;
        }
        if (buttonJustPressed[3]) {
          calcCursorCol++;
          if (calcCursorCol > 3) calcCursorCol = 0;
        }

        if (buttonJustPressed[4]) {
          const char* key = calcGrid[calcCursorRow][calcCursorCol];
          String keyStr = String(key);

          if (keyStr == "<--") {
            calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
            startAnimation(SCREEN_MENU, 64);

          } else if (keyStr == "C") {
            calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;

          } else if (keyStr == "DEL") {
            if (calcError) { calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false; }
            else if (calcIsOpSet) {
              if (calcInput2.length() > 0) calcInput2.remove(calcInput2.length() - 1);
              else { calcIsOpSet = false; calcOp = ' '; }
            } else {
              if (calcInput1.length() > 0) calcInput1.remove(calcInput1.length() - 1);
            }

          } else if (keyStr == "=") {
            if (!calcError && calcIsOpSet && calcInput2.length() > 0) {
              float v1 = calcInput1.toFloat(), v2 = calcInput2.toFloat(), res = 0;
              bool dz = false;
              if (calcOp=='+') res=v1+v2;
              else if (calcOp=='-') res=v1-v2;
              else if (calcOp=='*') res=v1*v2;
              else if (calcOp=='/') { if (v2!=0) res=v1/v2; else dz=true; }
              if (dz) { calcError = true; }
              else {
                calcInput1 = String(res, 4);
                while (calcInput1.endsWith("0") && calcInput1.indexOf(".")!=-1) calcInput1.remove(calcInput1.length()-1);
                if (calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length()-1);
                calcInput2 = ""; calcOp = ' '; calcIsOpSet = false;
              }
            }

          } else if (keyStr=="+" || keyStr=="-" || keyStr=="*" || keyStr=="/") {
            char op = key[0];
            if (!calcError && calcInput1.length() > 0) {
              if (calcIsOpSet && calcInput2.length() > 0) {
                float v1=calcInput1.toFloat(), v2=calcInput2.toFloat(), res=0;
                bool dz=false;
                if (calcOp=='+') res=v1+v2;
                else if (calcOp=='-') res=v1-v2;
                else if (calcOp=='*') res=v1*v2;
                else if (calcOp=='/') { if (v2!=0) res=v1/v2; else dz=true; }
                if (dz) { calcError=true; }
                else {
                  calcInput1=String(res,4);
                  while(calcInput1.endsWith("0")&&calcInput1.indexOf(".")!=-1) calcInput1.remove(calcInput1.length()-1);
                  if(calcInput1.endsWith(".")) calcInput1.remove(calcInput1.length()-1);
                  calcInput2=""; calcOp=op; calcIsOpSet=true;
                }
              } else { calcOp=op; calcIsOpSet=true; }
            }

          } else if (keyStr == "SCI") {
            calcSciRow = 0; calcSciCol = 0;
            startAnimation(SCREEN_CALCULATOR_SCI, -64);

          } else if (keyStr != "") {
            // digit or .
            if (calcError) { calcInput1=""; calcInput2=""; calcOp=' '; calcIsOpSet=false; calcError=false; }
            if (calcIsOpSet) {
              if (keyStr=="." && calcInput2.indexOf(".")!=-1) {}
              else if (calcInput2.length()<10) calcInput2 += keyStr;
            } else {
              if (keyStr=="." && calcInput1.indexOf(".")!=-1) {}
              else if (calcInput1.length()<10) calcInput1 += keyStr;
            }
          }
        }
      }

      else if (currentScreen == SCREEN_CALCULATOR_SCI) {
        if (buttonJustPressed[0]) calcSciRow = (calcSciRow > 0) ? calcSciRow - 1 : 2;
        if (buttonJustPressed[1]) calcSciRow = (calcSciRow < 2) ? calcSciRow + 1 : 0;
        if (buttonJustPressed[2]) calcSciCol = (calcSciCol > 0) ? calcSciCol - 1 : 1;
        if (buttonJustPressed[3]) calcSciCol = (calcSciCol < 1) ? calcSciCol + 1 : 0;

        if (buttonJustPressed[4]) {
          const char* key = sciGrid[calcSciRow][calcSciCol];

          // BACK just goes back; every other key applies a function first.
          if (strcmp(key, "BACK") != 0) {
            String* activeInput = calcIsOpSet ? &calcInput2 : &calcInput1;
            float v = activeInput->toFloat();
            bool hasVal = (activeInput->length() > 0 && !calcError);
            String result = "";
            bool sciError = false;

            if (strcmp(key, "x^2") == 0) {
              if (hasVal) result = String(v * v, 4);
            } else if (strcmp(key, "sqrt") == 0) {
              if (hasVal && v >= 0) result = String(sqrt(v), 4);
              else sciError = true;
            } else if (strcmp(key, "pi") == 0) {
              result = "3.14159265";
            } else if (strcmp(key, "x^3") == 0) {
              if (hasVal) result = String(v * v * v, 4);
            } else if (strcmp(key, "cbrt") == 0) {
              if (hasVal) {
                float cb = (v >= 0) ? pow(v, 1.0f/3.0f) : -pow(-v, 1.0f/3.0f);
                result = String(cb, 4);
              }
            }

            if (sciError) {
              calcError = true;
            } else if (result.length() > 0) {
              // Trim trailing zeros
              while (result.endsWith("0") && result.indexOf(".") != -1) result.remove(result.length()-1);
              if (result.endsWith(".")) result.remove(result.length()-1);
              if (strcmp(key, "pi") == 0) {
                if (!calcIsOpSet) calcInput1 = result;
                else calcInput2 = result;
              } else {
                *activeInput = result;
              }
            }
          }
          startAnimation(SCREEN_CALCULATOR, 64);
        }
      }

      else if (currentScreen == SCREEN_PIN_ENTRY) {
        if (buttonJustPressed[0]) pinCursorRow = (pinCursorRow > 0) ? pinCursorRow - 1 : 3;
        if (buttonJustPressed[1]) pinCursorRow = (pinCursorRow < 3) ? pinCursorRow + 1 : 0;
        if (buttonJustPressed[2]) pinCursorCol = (pinCursorCol > 0) ? pinCursorCol - 1 : 2;
        if (buttonJustPressed[3]) pinCursorCol = (pinCursorCol < 2) ? pinCursorCol + 1 : 0;

        if (buttonJustPressed[4]) {
          const char* key = pinKeys[pinCursorRow][pinCursorCol];
          if (strcmp(key, "<") == 0) {
            if (pinBuffer.length() > 0) {
              pinBuffer.remove(pinBuffer.length() - 1);
            } else {
              pinBuffer = ""; pinPendingNew = ""; pinErrorMsg = "";
              securityFlow = SEC_NONE;
              pendingAction = ACT_NONE;
              startAnimation(pinReturnScreen, 64);
            }
          } else if (strcmp(key, "OK") == 0) {
            if (pinBuffer.length() == 4) handlePinSubmit();
          } else {
            if (pinBuffer.length() < 4) {
              pinBuffer += key;
              if (pinBuffer.length() == 4) handlePinSubmit();
            }
          }
        }
      }
      else if (currentScreen == SCREEN_SECURITY_MENU) {
        if (buttonJustPressed[0]) {
          secMenuCursor = (secMenuCursor > -1) ? secMenuCursor - 1 : 1;
        }
        if (buttonJustPressed[1]) {
          secMenuCursor = (secMenuCursor < 1) ? secMenuCursor + 1 : -1;
        }
        if (buttonJustPressed[4]) {
          if (secMenuCursor == -1) {
            startAnimation(SCREEN_MENU, 64);
          } else if (secMenuCursor == 0) {
            pendingAction = ACT_CHANGE;
            enterPinScreen(SEC_VERIFY, SCREEN_SECURITY_MENU);
          } else if (secMenuCursor == 1) {
            pendingAction = ACT_DISABLE;
            enterPinScreen(SEC_VERIFY, SCREEN_SECURITY_MENU);
          }
        }
      }
      else if (currentScreen == SCREEN_SECURITY_CONFIRM) {
        if (buttonJustPressed[2]) confirmSelection = 0;
        if (buttonJustPressed[3]) confirmSelection = 1;
        if (buttonJustPressed[4]) {
          bool yes = (confirmSelection == 0);
          if (confirmType == CONF_ENABLE) {
            if (yes) {
              pinReturnScreen = SCREEN_MENU;
              pendingAction = ACT_OPEN_MENU;
              pinBuffer = ""; pinPendingNew = ""; pinErrorMsg = "";
              pinCursorRow = 0; pinCursorCol = 1;
              securityFlow = SEC_SET_NEW;
              startAnimation(SCREEN_PIN_ENTRY, -64);
            } else {
              confirmType = CONF_NONE;
              startAnimation(SCREEN_MENU, 64);
            }
          } else if (confirmType == CONF_DISABLE) {
            if (yes) {
              pinSet = false;
              pinStored = "";
              saveSettings();
              confirmType = CONF_NONE;
              pendingAction = ACT_NONE;
              startAnimation(SCREEN_MENU, 64);
            } else {
              confirmType = CONF_NONE;
              startAnimation(SCREEN_SECURITY_MENU, 64);
            }
          }
        }
      }
      else if (currentScreen == SCREEN_LOCK_SETTINGS) {
        if (buttonJustPressed[0]) {
          lockSettingsCursor = (lockSettingsCursor > -1) ? lockSettingsCursor - 1 : 3;
        }
        if (buttonJustPressed[1]) {
          lockSettingsCursor = (lockSettingsCursor < 3) ? lockSettingsCursor + 1 : -1;
        }
        if (buttonJustPressed[4]) {
          if (lockSettingsCursor == -1) {
            startAnimation(SCREEN_MENU, 64);
          } else {
            int chosen = lockTimeoutOptions[lockSettingsCursor];
            if (chosen != displayTimeoutSec) {
              displayTimeoutSec = chosen;
              DISPLAY_TIMEOUT_MS = (unsigned long)displayTimeoutSec * 1000UL;
              saveSettings();
            }
          }
        }
      }
      else if (currentScreen == SCREEN_TERMINAL) {
        if (termState == TERM_IDLE) {
          if (buttonJustPressed[4]) {
            startAnimation(SCREEN_MENU, 64);
          }
        } else if (termState == TERM_CONFIRM) {
          if (buttonJustPressed[2]) termConfirmSelection = 0; // LEFT  -> Yes
          if (buttonJustPressed[3]) termConfirmSelection = 1; // RIGHT -> No
          if (buttonJustPressed[4]) {
            applyTerminalCommand(termConfirmSelection == 0);
            termState = TERM_IDLE;
          }
        }
      }
      else if (currentScreen == SCREEN_RESTWISE) {
        // Focus moves over the back arrow (-1) and the 7 day items (0..6).
        // The day list only exists once a timetable has been synced; with no
        // data the only thing to focus is the back arrow.
        if (rwHasData) {
          if (buttonJustPressed[0]) rwCursor = (rwCursor > -1) ? rwCursor - 1 : 6;
          if (buttonJustPressed[1]) rwCursor = (rwCursor < 6) ? rwCursor + 1 : -1;
        } else {
          rwCursor = -1;
        }
        if (buttonJustPressed[4]) {
          if (rwCursor == -1) {
            startAnimation(SCREEN_MENU, 64);
          } else if (rwHasData) {
            rwSelectedDayItem = rwCursor;
            rwDayScroll = 0;
            startAnimation(SCREEN_RESTWISE_DAY, -64);
          }
        }
      }
      else if (currentScreen == SCREEN_RESTWISE_DAY) {
        // Passive scroll only -- rows aren't focusable. LEFT is the sole way back.
        int idx[RW_DAY_MAX];
        int n = rwBuildDayList(rwResolveDayIndex(rwSelectedDayItem), idx, RW_DAY_MAX);
        int maxScroll = (n > RW_DAY_ROWS) ? (n - RW_DAY_ROWS) : 0;
        if (buttonJustPressed[0] && rwDayScroll > 0) rwDayScroll--;
        if (buttonJustPressed[1] && rwDayScroll < maxScroll) rwDayScroll++;
        if (buttonJustPressed[2]) startAnimation(SCREEN_RESTWISE, 64);
      }
      else if (currentScreen == SCREEN_GOODNIGHT) {
        // A passive bedtime greeting -- UP scrolls up to the watchface.
        if (buttonJustPressed[0]) startAnimation(SCREEN_WATCHFACE, -64);
      }
    }

    if (pinErrorMsg.length() > 0 && millis() - pinErrorShownAt > 1500) pinErrorMsg = "";

    static unsigned long lastStatusUpdate = 0;
    if (millis() - lastStatusUpdate > 100) {
      statusIndex++;
      lastStatusUpdate = millis();
    }

    updateDisplay();
  }

  delay(5);
}

/* ---------------- Platform layer: buttons, time, settings ---------------- */

void setupButtons() {
  for (int i = 0; i < 5; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
  }
}

void readButtons() {
  // 50ms poll, also acting as the debounce (a bounce settles well inside one
  // interval).
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < 50) return;
  lastPoll = millis();

  buttonStates[0] = !digitalRead(BTN_UP);
  buttonStates[1] = !digitalRead(BTN_DOWN);
  buttonStates[2] = !digitalRead(BTN_LEFT);
  buttonStates[3] = !digitalRead(BTN_RIGHT);
  buttonStates[4] = !digitalRead(BTN_CENTER);
}

DateTime nowTime() {
  if (rtcPresent) return rtc.now();
  // Software clock: base time plus however long we've been running. Unsigned
  // subtraction keeps this correct across the millis() rollover.
  unsigned long elapsed = (millis() - softBaseMillis) / 1000UL;
  return softBase + TimeSpan((int32_t)elapsed);
}

void setDeviceTime(const DateTime &dt) {
  if (rtcPresent) rtc.adjust(dt);
  // Always re-base the software clock too, so `set_time` works identically
  // whether or not a DS3231 happens to be attached.
  softBase = dt;
  softBaseMillis = millis();
}

void loadSettings() {
  StoredSettings s;
  EEPROM.get(0, s);

  if (s.magic != SETTINGS_MAGIC) {
    // Blank/never-written EEPROM -- fall back to defaults.
    pinSet = false;
    pinStored = "";
    displayTimeoutSec = 5;
  } else {
    pinSet = (s.pinSet != 0);
    s.pin[4] = '\0';
    pinStored = String(s.pin);
    displayTimeoutSec = s.timeoutSec;

    // Guard against a corrupt value putting the timeout somewhere the Lock
    // Screen menu can't represent (which would leave the cursor unselectable).
    bool valid = false;
    for (int i = 0; i < 4; i++) if (lockTimeoutOptions[i] == displayTimeoutSec) valid = true;
    if (!valid) displayTimeoutSec = 5;

    if (pinStored.length() != 4) { pinSet = false; pinStored = ""; }
  }

  DISPLAY_TIMEOUT_MS = (unsigned long)displayTimeoutSec * 1000UL;
}

void saveSettings() {
  StoredSettings s;
  s.magic = SETTINGS_MAGIC;
  s.pinSet = pinSet ? 1 : 0;
  memset(s.pin, 0, sizeof(s.pin));
  pinStored.toCharArray(s.pin, sizeof(s.pin));
  s.timeoutSec = (uint8_t)displayTimeoutSec;

  EEPROM.put(0, s);
  EEPROM.commit();
}

/* ---------------------------- UI / rendering ---------------------------- */

void startAnimation(ScreenState next, int targetOffset) {
  isAnimating = true;
  animNextScreen = next;
  animOffsetY = 0;
  animTargetY = targetOffset;
}

void updateDisplay() {
  display.clearDisplay();

  if (isAnimating) {
    if (animTargetY < 0) {
      drawScreen(currentScreen, animOffsetY);
      drawScreen(animNextScreen, animOffsetY + 64);
    } else {
      drawScreen(currentScreen, animOffsetY);
      drawScreen(animNextScreen, animOffsetY - 64);
    }
  } else {
    drawScreen(currentScreen, 0);
  }

  display.display();
}

void drawScreen(ScreenState screen, int yOffset) {
  if (screen == SCREEN_WATCHFACE) drawWatchFace(yOffset);
  else if (screen == SCREEN_MENU) drawMenu(yOffset);
  else if (screen == SCREEN_STOPWATCH) drawStopwatch(yOffset);
  else if (screen == SCREEN_TIMER) drawTimer(yOffset);
  else if (screen == SCREEN_TIMER_ALERT) drawTimerAlert(yOffset);
  else if (screen == SCREEN_CALCULATOR) drawCalculator(yOffset);
  else if (screen == SCREEN_CALCULATOR_SCI) drawCalculatorSci(yOffset);
  else if (screen == SCREEN_PIN_ENTRY) drawPinEntry(yOffset);
  else if (screen == SCREEN_SECURITY_MENU) drawSecurityMenu(yOffset);
  else if (screen == SCREEN_SECURITY_CONFIRM) drawSecurityConfirm(yOffset);
  else if (screen == SCREEN_LOCK_SETTINGS) drawLockSettings(yOffset);
  else if (screen == SCREEN_TERMINAL) drawTerminal(yOffset);
  else if (screen == SCREEN_RESTWISE) drawRestwise(yOffset);
  else if (screen == SCREEN_RESTWISE_DAY) drawRestwiseDay(yOffset);
  else if (screen == SCREEN_GOODNIGHT) drawGoodNight(yOffset);
}

void drawHeader(int yOffset, const char* appName, bool backFocused) {

  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);

  if (appName != nullptr) {
    display.setTextSize(1);

    if (backFocused) {
      display.fillRect(0, yOffset, 24, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(2, 2 + yOffset);
    display.print("<--");

    display.setTextColor(SSD1306_WHITE);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(appName, 0, 0, &x1, &y1, &w, &h);

    display.setCursor((128 - w) / 2 - x1, 12/2 - h/2 - y1 + yOffset);
    display.print(appName);
  } else {
    display.setTextColor(SSD1306_WHITE);
    if (isStatusActive) {
      uint8_t dotPos = (statusIndex % 8) * 16;
      display.fillRect(dotPos, 2 + yOffset, 8, 4, SSD1306_WHITE);
    }
  }
}

void drawWatchFace(int yOffset) {

  drawHeader(yOffset);

  DateTime now = nowTime();
  uint16_t hour = now.hour();
  bool isPM = hour >= 12;
  if (hour > 12) hour -= 12;
  else if (hour == 0) hour = 12;

  char timeBuf[6];
  sprintf(timeBuf, "%02d:%02d", hour, now.minute());
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeBuf, 0, 0, &x1, &y1, &w, &h);

  int16_t xPos = (128 - w + 1) / 2 - x1;
  int16_t yPos = 13 + (43 - h + 1) / 2 - y1 + yOffset;
  display.setCursor(xPos, yPos);
  display.print(timeBuf);

  display.setTextSize(1);
  display.setCursor(128 - 14, 56 + yOffset);
  display.print(isPM ? "PM" : "AM");

  display.setCursor(0, 56 + yOffset);
  const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  char dateBuf[16];
  sprintf(dateBuf, "%02d/%02d/%04d %s", now.day(), now.month(), now.year(), days[now.dayOfTheWeek()]);
  display.print(dateBuf);
}

void drawMenu(int yOffset) {
  drawHeader(yOffset);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "APPS";
  int titleX = 64 - (strlen(title) * 3);
  display.setCursor(max(0, titleX), 2 + yOffset);
  display.print(title);

  int itemH = 12;
  int listY = 16 + yOffset;

  int startIdx = (menuIndex < 4) ? 0 : menuIndex - 3;
  int endIdx = min(startIdx + 4, (int)NUM_MENU_ITEMS);

  for (int i = startIdx; i < endIdx; i++) {
    int y = listY + ((i - startIdx) * itemH);

    if (i == menuIndex) {
      display.fillRect(0, y-1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(4, y);
    display.print(menuItems[i]);
  }
}

void drawStopwatch(int yOffset) {
  drawHeader(yOffset, "STOPWATCH", (swFocus == 0));

  unsigned long currentTotal = swElapsedTime;
  if (swRunning) {
    currentTotal += millis() - swStartTime;
  }

  unsigned long ms = (currentTotal % 1000) / 10;
  unsigned long totalSecs = currentTotal / 1000;
  unsigned long m = totalSecs / 60;
  unsigned long s = totalSecs % 60;

  char timeStr[10];
  sprintf(timeStr, "%02lu:%02lu", m, s);
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int16_t xPos = (SCREEN_WIDTH - w) / 2 - x1;
  int16_t yPos = 10 + (42 - h) / 2 - y1 + yOffset;
  display.setCursor(xPos, yPos);
  display.print(timeStr);

  char msStr[4];
  sprintf(msStr, "%02lu", ms);
  display.setTextSize(1);
  display.setCursor(SCREEN_WIDTH - 16, 16 + yOffset);
  display.print(msStr);

  display.setTextSize(1);

  const char* leftBtn = swRunning ? "PAUSE" : "START";
  int16_t lx1, ly1; uint16_t lw, lh;
  display.getTextBounds(leftBtn, 0, 0, &lx1, &ly1, &lw, &lh);
  int leftBtnX = (64 - (lw + 8)) / 2;
  drawBoxedCenteredText(display, leftBtn, leftBtnX, SCREEN_HEIGHT - 12 + yOffset, lw + 8, 11, (swFocus == 1));

  const char* rightBtn = swRunning ? "STOP" : "RESET";
  int16_t rx1, ry1; uint16_t rw, rh;
  display.getTextBounds(rightBtn, 0, 0, &rx1, &ry1, &rw, &rh);
  int rightBtnX = 64 + (64 - (rw + 8)) / 2;
  drawBoxedCenteredText(display, rightBtn, rightBtnX, SCREEN_HEIGHT - 12 + yOffset, rw + 8, 11, (swFocus == 2));
}

void drawTimer(int yOffset) {
  drawHeader(yOffset, "TIMER", (tmFocus == 0));

  char timeStr[10];
  sprintf(timeStr, "%02d:%02d:%02d", tmHours, tmMinutes, tmSeconds);
  if (tmMode == TM_RUNNING) {
    unsigned long secs = tmRemainingMillis / 1000;
    sprintf(timeStr, "%02lu:%02lu:%02lu", secs / 3600, (secs % 3600) / 60, secs % 60);
  }

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int16_t tx = (128 - w) / 2 - x1;
  int16_t ty = 16 + (28 - h) / 2 - y1 + yOffset;

  display.setCursor(tx, ty);
  display.print(timeStr);

  if (tmMode == TM_SETTING) {
    int charW = 12;
    int16_t unitX = tx + (tmActiveUnit * 3 * charW);
    display.drawFastHLine(unitX, ty + h + 2, 22, SSD1306_WHITE);
  }

  if (tmMode == TM_SETTING) {

    drawBoxedCenteredText(display, "+", 10, 48 + yOffset, 20, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, "SET", 44, 48 + yOffset, 40, 11, (tmFocus == 2));
    drawBoxedCenteredText(display, "-", 98, 48 + yOffset, 20, 11, (tmFocus == 3));
  } else if (tmMode == TM_READY || tmMode == TM_RUNNING) {

    const char* mainBtn = (tmMode == TM_RUNNING) ? "RESET" : "START";
    drawBoxedCenteredText(display, "SET", 15, 48 + yOffset, 40, 11, (tmFocus == 1));
    drawBoxedCenteredText(display, mainBtn, 70, 48 + yOffset, 45, 11, (tmFocus == 3));
  }
}

void drawTimerAlert(int yOffset) {
  bool isFlashOn = (millis() % 1000 < 500);

  if (isFlashOn) {

    display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {

    display.setTextColor(SSD1306_WHITE);
  }

  drawCenteredText(display, "TIMER DONE", 15 + yOffset, 2);
  drawCenteredText(display, "Press any key", 40 + yOffset, 1);

  int x = 51, y = 51 + yOffset, w = 26, h = 11;
  if (isFlashOn) {

    display.fillRect(x, y, w, h, SSD1306_BLACK);
    display.setTextColor(SSD1306_WHITE);
  } else {

    display.fillRect(x, y, w, h, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  }

  int16_t x1, y1; uint16_t tw, th;
  display.getTextBounds("OK", 0, 0, &x1, &y1, &tw, &th);
  display.setCursor(x + (w - tw) / 2 - x1, y + (h - th) / 2 - y1);
  display.print("OK");

  display.setTextColor(SSD1306_WHITE);
}

void drawCalculator(int yOffset) {
  // Build display string
  String disp;
  if (calcError) {
    disp = "Err: Div/0";
  } else {
    disp = (calcInput1.length() > 0) ? calcInput1 : "0";
    if (calcIsOpSet) {
      disp += ' '; disp += calcOp;
      if (calcInput2.length() > 0) { disp += ' '; disp += calcInput2; }
    }
  }
  // Clamp display to 18 chars (18*6=108px, leaving room)
  if ((int)disp.length() > 18) disp = disp.substring(disp.length() - 18);

  // --- Display bar: y=0..12 (13px tall) ---
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int dispX = 128 - (int)disp.length() * 6 - 1;
  if (dispX < 0) dispX = 0;
  display.setCursor(dispX, 3 + yOffset);
  display.print(disp);

  // Separator line
  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  // --- Button grid: y=14..63 = 50px for 5 rows ---
  const int GY   = 14 + yOffset;
  const int CW   = 32;
  const int CH   = 10;

  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 4; c++) {
      const char* lbl = calcGrid[r][c];
      if (lbl[0] == '\0') continue; // skip empty cell

      int cx = c * CW;
      int cy = GY + r * CH;
      bool sel = (r == calcCursorRow && c == calcCursorCol);

      if (sel) {
        display.fillRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.drawRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_WHITE);
        // Emphasize operator column (col 3) with an extra pixel width
        if (c == 3) display.drawFastVLine(cx + 1, cy, CH, SSD1306_WHITE);
      }
      int tw = strlen(lbl) * 6;
      int tx = cx + (CW - tw) / 2;
      int ty = cy + (CH - 7) / 2;
      display.setCursor(tx, ty);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawCalculatorSci(int yOffset) {
  // --- Display bar: show current active value (top 15px) ---
  String activeVal = calcIsOpSet ? calcInput2 : calcInput1;
  if (activeVal.length() == 0) activeVal = "0";
  if (calcError) activeVal = "Error";

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(1, 3 + yOffset);
  display.print("SCI");

  int valX = 128 - (int)activeVal.length() * 6 - 1;
  if (valX < 25) valX = 25;
  display.setCursor(valX, 3 + yOffset);
  display.print(activeVal);

  display.drawFastHLine(0, 13 + yOffset, 128, SSD1306_WHITE);

  // --- SCI grid: 3 rows x 2 cols ---
  const int GY = 15 + yOffset;
  const int CW = 64;
  const int CH = 16;

  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 2; c++) {
      int cx = c * CW;
      int cy = GY + r * CH;
      bool sel = (r == calcSciRow && c == calcSciCol);
      const char* lbl = sciGrid[r][c];
      if (lbl[0] == '\0') continue;

      if (sel) {
        display.fillRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.drawRect(cx, cy, CW, CH, SSD1306_WHITE);
        display.setTextColor(SSD1306_WHITE);
      }
      int tw = strlen(lbl) * 6;
      int tx = cx + (CW - tw) / 2;
      int ty = cy + (CH - 7) / 2;
      display.setCursor(tx, ty);
      display.print(lbl);
    }
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawCenteredText(Adafruit_SSD1306 &d, const String &text, int16_t y, uint8_t size) {
  int16_t x1, y1; uint16_t w, h;
  d.setTextSize(size);
  d.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  d.setCursor((SCREEN_WIDTH - w) / 2, y);
  d.print(text);
}

void drawBoxedCenteredText(Adafruit_SSD1306 &d, const char* text, int x, int y, int w, int h, bool inverted) {
  int16_t x1, y1; uint16_t tw, th;
  d.setTextSize(1);
  d.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  if (inverted) {
    d.fillRect(x, y, w, h, SSD1306_WHITE);
    d.setTextColor(SSD1306_BLACK);
  } else {
    d.drawRect(x, y, w, h, SSD1306_WHITE);
    d.setTextColor(SSD1306_WHITE);
  }
  d.setCursor(x + (w - tw) / 2, y + (h - th) / 2);
  d.print(text);
  d.setTextColor(SSD1306_WHITE);
}

void enterPinScreen(SecurityFlow flow, ScreenState returnTo) {
  pinBuffer = ""; pinPendingNew = ""; pinErrorMsg = "";
  pinCursorRow = 0; pinCursorCol = 1;
  securityFlow = flow;
  pinReturnScreen = returnTo;
  startAnimation(SCREEN_PIN_ENTRY, -64);
}

void handlePinSubmit() {
  if (securityFlow == SEC_LOCK) {
    if (pinBuffer == pinStored) {
      pinBuffer = "";
      securityFlow = SEC_NONE;
      startAnimation(SCREEN_MENU, -64);
    } else {
      pinErrorMsg = "Wrong PIN";
      pinErrorShownAt = millis();
      pinBuffer = "";
    }
  } else if (securityFlow == SEC_VERIFY) {
    if (pinBuffer == pinStored) {
      pinBuffer = "";
      if (pendingAction == ACT_OPEN_MENU) {
        secMenuCursor = 0;
        securityFlow = SEC_NONE;
        pendingAction = ACT_NONE;
        startAnimation(SCREEN_SECURITY_MENU, -64);
      } else if (pendingAction == ACT_CHANGE) {
        securityFlow = SEC_SET_NEW;
        pinCursorRow = 0; pinCursorCol = 1;
      } else if (pendingAction == ACT_DISABLE) {
        confirmType = CONF_DISABLE;
        confirmSelection = 1;
        securityFlow = SEC_NONE;
        pendingAction = ACT_NONE;
        startAnimation(SCREEN_SECURITY_CONFIRM, -64);
      }
    } else {
      pinErrorMsg = "Wrong PIN";
      pinErrorShownAt = millis();
      pinBuffer = "";
    }
  } else if (securityFlow == SEC_SET_NEW) {
    pinPendingNew = pinBuffer;
    pinBuffer = "";
    securityFlow = SEC_SET_CONFIRM;
  } else if (securityFlow == SEC_SET_CONFIRM) {
    if (pinBuffer == pinPendingNew) {
      pinStored = pinBuffer;
      pinSet = true;
      saveSettings();
      pinBuffer = ""; pinPendingNew = "";
      securityFlow = SEC_NONE;
      SecPendingAction wasAction = pendingAction;
      pendingAction = ACT_NONE;
      // From Change flow -> back to security menu; from Enable flow -> main menu.
      startAnimation((wasAction == ACT_CHANGE) ? SCREEN_SECURITY_MENU : SCREEN_MENU, 64);
    } else {
      pinErrorMsg = "PINs didn't match";
      pinErrorShownAt = millis();
      pinBuffer = ""; pinPendingNew = "";
      securityFlow = SEC_SET_NEW;
    }
  }
}

void drawPinEntry(int yOffset) {
  const char* hdr = "PIN";
  if (securityFlow == SEC_VERIFY || securityFlow == SEC_LOCK) hdr = "Enter PIN";
  else if (securityFlow == SEC_SET_NEW) hdr = "New PIN";
  else if (securityFlow == SEC_SET_CONFIRM) hdr = "Confirm PIN";

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int hdrLen = strlen(hdr);
  int hdrX = 64 - (hdrLen * 3);
  display.setCursor(max(0, hdrX), 2 + yOffset);
  display.print(hdr);

  // Digit indicators: 4 boxes (10 wide x 9 tall), y=10..19.
  const int boxW = 10, boxH = 9, boxGap = 6;
  const int totalW = boxW * 4 + boxGap * 3;
  const int startX = (128 - totalW) / 2;
  const int boxY = 10 + yOffset;

  if (pinErrorMsg.length() > 0) {
    drawCenteredText(display, pinErrorMsg, 12 + yOffset, 1);
  } else {
    for (int i = 0; i < 4; i++) {
      int x = startX + i * (boxW + boxGap);
      if (i < (int)pinBuffer.length()) {
        display.fillRect(x, boxY, boxW, boxH, SSD1306_WHITE);
      } else {
        display.drawRect(x, boxY, boxW, boxH, SSD1306_WHITE);
      }
    }
  }

  // Numpad: uniform 4x3 grid, y=22..62.
  const int padX = 34;
  const int padY = 22 + yOffset;
  const int keyW = 18, keyH = 10, colStep = 20, rowStep = 10;

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      int x = padX + c * colStep;
      int y = padY + r * rowStep;
      bool sel = (pinCursorRow == r && pinCursorCol == c);
      drawBoxedCenteredText(display, pinKeys[r][c], x, y, keyW, keyH, sel);
    }
  }

  display.setTextColor(SSD1306_WHITE);
}

void drawSecurityMenu(int yOffset) {
  display.setTextSize(1);
  bool backSel = (secMenuCursor == -1);
  if (backSel) {
    display.fillRect(0, yOffset, 22, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  const char* title = "Security";
  int titleX = 64 - (strlen(title) * 3);
  display.setCursor(max(0, titleX), 2 + yOffset);
  display.print(title);
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);

  const char* items[2] = {"Change PIN", "Turn Off Security"};
  for (int i = 0; i < 2; i++) {
    int y = 18 + (i * 14) + yOffset;
    if (i == secMenuCursor) {
      display.fillRect(2, y - 1, 124, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(6, y);
    display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawSecurityConfirm(int yOffset) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* q1 = "";
  const char* q2 = "";
  if (confirmType == CONF_ENABLE) {
    q1 = "Turn on security?";
    q2 = "";
  } else {
    q1 = "Turn off security?";
    q2 = "Are you sure?";
  }

  drawCenteredText(display, q1, 12 + yOffset, 1);
  if (q2[0]) drawCenteredText(display, q2, 26 + yOffset, 1);

  int by = 46 + yOffset;
  int bw = 40, bh = 12;
  drawBoxedCenteredText(display, "Yes", 14, by, bw, bh, (confirmSelection == 0));
  drawBoxedCenteredText(display, "No",  74, by, bw, bh, (confirmSelection == 1));

  display.setTextColor(SSD1306_WHITE);
}

void drawLockSettings(int yOffset) {
  display.setTextSize(1);
  bool backSel = (lockSettingsCursor == -1);
  if (backSel) {
    display.fillRect(0, yOffset, 22, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(2, 2 + yOffset);
  display.print("<--");

  display.setTextColor(SSD1306_WHITE);
  const char* title = "Lock Screen";
  int titleX = 64 - (strlen(title) * 3);
  display.setCursor(max(0, titleX), 2 + yOffset);
  display.print(title);
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);

  for (int i = 0; i < 4; i++) {
    int y = 16 + (i * 11) + yOffset;
    char label[16];
    sprintf(label, "%d seconds", lockTimeoutOptions[i]);

    if (i == lockSettingsCursor) {
      display.fillRect(2, y - 1, 124, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(6, y);
    display.print(label);
    if (lockTimeoutOptions[i] == displayTimeoutSec) display.print(" (ON)");
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawTerminal(int yOffset) {
  bool idle = (termState == TERM_IDLE);
  drawHeader(yOffset, "TERMINAL", idle);
  display.setTextColor(SSD1306_WHITE);

  if (termState == TERM_CONFIRM) {
    drawCenteredText(display, termPendingCmdName, 18 + yOffset, 1);
    if (termPendingArgs.length() > 0) {
      String args = termPendingArgs;
      if ((int)args.length() > 21) args = args.substring(0, 21);
      drawCenteredText(display, args, 28 + yOffset, 1);
    }

    int by = 46 + yOffset;
    int bw = 40, bh = 12;
    drawBoxedCenteredText(display, "Yes", 14, by, bw, bh, (termConfirmSelection == 0));
    drawBoxedCenteredText(display, "No",  74, by, bw, bh, (termConfirmSelection == 1));
  } else {
    drawCenteredText(display, "USB command console", 20 + yOffset, 1);

    bool hostConnected = termConnected && (millis() - termLastPingAt < TERM_PING_STALE_MS);

    // A small sweeping bar to signal "listening for a command" while idle.
    const int boxX = 14, boxY = 38 + yOffset, boxW = 100, boxH = 16;
    display.drawRect(boxX, boxY, boxW, boxH, SSD1306_WHITE);
    drawCenteredText(display, hostConnected ? "Connected!" : "Listening", boxY + 3, 1);

    const int innerW = boxW - 4;
    const int sweepW = 16;
    const int period = (innerW - sweepW) * 2;
    int t = (statusIndex * 2) % period;
    int sweepX = (t <= (innerW - sweepW)) ? t : (period - t);
    display.fillRect(boxX + 2 + sweepX, boxY + boxH - 4, sweepW, 2, SSD1306_WHITE);
  }

  display.setTextColor(SSD1306_WHITE);
}

void processTerminalSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      termLineBuf.trim();
      if (termLineBuf == "PING") {
        termConnected = true;
        termLastPingAt = millis();
      } else if (termLineBuf.startsWith("CONFIRM ")) {
        String rest = termLineBuf.substring(8);
        int sp = rest.indexOf(' ');
        termPendingCmdName = (sp == -1) ? rest : rest.substring(0, sp);
        termPendingArgs = (sp == -1) ? "" : rest.substring(sp + 1);
        termConfirmSelection = 0;
        termState = TERM_CONFIRM;

        // A command arriving must be seen -- wake the display immediately.
        if (!displayOn) {
          display.ssd1306_command(SSD1306_DISPLAYON);
          displayOn = true;
        }
        lastActivityTime = millis();
      }
      termLineBuf = "";
    } else if (c != '\r') {
      if (termLineBuf.length() < 96) termLineBuf += c; // guard against a runaway line
    }
  }
}

void applyTerminalCommand(bool granted) {
  if (granted) {
    if (termPendingCmdName == "set_time") {
      int y, mo, d, h, mi, s;
      if (sscanf(termPendingArgs.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
        setDeviceTime(DateTime(y, mo, d, h, mi, s));
      }
    }
    Serial.print("ACK "); Serial.print(termPendingCmdName); Serial.println(" YES");
  } else {
    Serial.print("ACK "); Serial.print(termPendingCmdName); Serial.println(" NO");
  }
  termPendingCmdName = "";
  termPendingArgs = "";
}

/* ------------------------------ Restwise app ---------------------------- */

bool isNightNow() {
  int h = nowTime().hour();
  return (h >= 21 || h < 6);   // 9 PM .. 6 AM
}

// Day-list item -> Monday-first index (0=Mon .. 6=Sun).
// Item 0 = "Today" resolves against the clock; 1..6 = Monday..Saturday.
int rwResolveDayIndex(int item) {
  if (item == 0) {
    // RTClib dayOfTheWeek(): 0=Sun..6=Sat. Shift to Monday-first.
    return (nowTime().dayOfTheWeek() + 6) % 7;
  }
  return item - 1;
}

const char* rwDayName(int item) {
  if (item == 0) return "Today";
  static const char* names[6] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return names[item - 1];
}

// Collect the block indices that apply to `dayIdx` (Mon-first), sorted by start.
int rwBuildDayList(int dayIdx, int* out, int maxOut) {
  int n = 0;
  for (int i = 0; i < rwBlockCount && n < maxOut; i++) {
    if (rwBlocks[i].days & (1 << dayIdx)) out[n++] = i;
  }
  // insertion sort by start time (n is small)
  for (int i = 1; i < n; i++) {
    int key = out[i];
    int j = i - 1;
    while (j >= 0 && rwBlocks[out[j]].startMin > rwBlocks[key].startMin) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  return n;
}

static void rwParseLine(const String &line) {
  int p1 = line.indexOf('|');
  int p2 = line.indexOf('|', p1 + 1);
  int p3 = line.indexOf('|', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) return;

  long days = line.substring(0, p1).toInt();
  long sm   = line.substring(p1 + 1, p2).toInt();
  long em   = line.substring(p2 + 1, p3).toInt();
  String label = line.substring(p3 + 1);
  if (days < 0 || days > 127) return;
  if (sm < 0 || sm > 1440 || em < 0 || em > 1440) return;
  if (rwBlockCount >= RW_MAX_BLOCKS) return;

  RwBlock &b = rwBlocks[rwBlockCount];
  b.days = (uint8_t)days;
  b.startMin = (uint16_t)sm;
  b.endMin = (uint16_t)em;
  label.toCharArray(b.label, sizeof(b.label));
  rwBlockCount++;
}

void rwLoadFromFile() {
  rwBlockCount = 0;
  rwHasData = false;

  File f = LittleFS.open(RW_FILE, "r");
  if (!f) return;

  while (f.available() && rwBlockCount < RW_MAX_BLOCKS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) rwParseLine(line);
  }
  f.close();
  rwHasData = (rwBlockCount > 0);
}

void processRestwiseSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      rwLineBuf.trim();
      String line = rwLineBuf;
      rwLineBuf = "";

      // Any recognised traffic wakes the panel so a sync is always visible.
      if (line.length() > 0) {
        if (!displayOn) {
          display.ssd1306_command(SSD1306_DISPLAYON);
          displayOn = true;
        }
        lastActivityTime = millis();
      }

      if (rwSyncState == RWS_RECV) {
        if (line == "RW_END") {
          if (rwFile) rwFile.close();
          rwSyncState = RWS_IDLE;
          rwLoadFromFile();
          rwCursor = -1;
          rwSelectedDayItem = 0;
          Serial.print("RW_OK "); Serial.println(rwBlockCount);
        } else if (line.length() > 0) {
          if (rwFile) rwFile.println(line);
          rwRecvCount++;
        }
      } else { // RWS_IDLE
        if (line == "RW_HELLO") {
          rwConnected = true;
          rwLastHelloAt = millis();
          Serial.println("RW_HELLO");
          // A sync is starting -- surface the Restwise screen so the user sees
          // it (the port-open reset likely bounced us to the watchface).
          if (currentScreen != SCREEN_RESTWISE && currentScreen != SCREEN_RESTWISE_DAY) {
            currentScreen = SCREEN_RESTWISE;
            isAnimating = false;
            animOffsetY = 0;
            rwCursor = -1;
          }
        } else if (line.startsWith("RW_TIME ")) {
          // The PC sets the watch clock as part of each sync: "RW_TIME
          // YYYY-MM-DD HH:MM:SS". This also feeds the night-time Good Night
          // greeting, which keys off the real hour.
          int y, mo, d, h, mi, s;
          if (sscanf(line.c_str() + 8, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
            setDeviceTime(DateTime(y, mo, d, h, mi, s));
            Serial.println("RW_TIME_OK");
          } else {
            Serial.println("RW_ERR time");
          }
        } else if (line == "RW_BEGIN") {
          rwFile = LittleFS.open(RW_FILE, "w");
          if (rwFile) {
            rwSyncState = RWS_RECV;
            rwRecvCount = 0;
            Serial.println("RW_READY");
          } else {
            Serial.println("RW_ERR open");
          }
          if (currentScreen != SCREEN_RESTWISE && currentScreen != SCREEN_RESTWISE_DAY) {
            currentScreen = SCREEN_RESTWISE;
            isAnimating = false;
            animOffsetY = 0;
            rwCursor = -1;
          }
        }
      }
    } else if (c != '\r') {
      if (rwLineBuf.length() < 120) rwLineBuf += c;  // guard runaway line
    }
  }
}

void drawRestwise(int yOffset) {
  drawHeader(yOffset, "RESTWISE", (rwCursor == -1));

  bool receiving = (rwSyncState == RWS_RECV);
  bool connected = rwConnected && (millis() - rwLastHelloAt < RW_HELLO_STALE_MS);

  if (receiving) {
    drawCenteredText(display, "Receiving...", 30 + yOffset, 1);
    char buf[20];
    sprintf(buf, "%d blocks", rwRecvCount);
    drawCenteredText(display, buf, 44 + yOffset, 1);
    return;
  }

  if (!rwHasData) {
    drawCenteredText(display, "No Data :(", 24 + yOffset, 2);
    drawCenteredText(display, connected ? "Connected - syncing" : "Sync from the PC", 50 + yOffset, 1);
    return;
  }

  // Day list, windowed to 4 visible rows (mirrors the main menu behaviour).
  const char* items[7] = {"Today", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  int itemH = 12;
  int listY = 16 + yOffset;
  int cur = (rwCursor < 0) ? 0 : rwCursor;
  int startIdx = (cur < 4) ? 0 : cur - 3;
  int endIdx = min(startIdx + 4, 7);

  for (int i = startIdx; i < endIdx; i++) {
    int y = listY + ((i - startIdx) * itemH);
    if (i == rwCursor) {
      display.fillRect(0, y - 1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawRestwiseDay(int yOffset) {
  // Header band: day name centred, NO back arrow (LEFT is the way back).
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  const char* title = rwDayName(rwSelectedDayItem);
  int16_t bx, by; uint16_t bw, bh;
  display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
  display.setCursor((128 - bw) / 2 - bx, 2 + yOffset);
  display.print(title);

  int idx[RW_DAY_MAX];
  int n = rwBuildDayList(rwResolveDayIndex(rwSelectedDayItem), idx, RW_DAY_MAX);

  if (n == 0) {
    drawCenteredText(display, "No blocks", 32 + yOffset, 1);
    return;
  }

  int rowH = 12;
  int topY = 15 + yOffset;
  for (int k = 0; k < RW_DAY_ROWS; k++) {
    int i = rwDayScroll + k;
    if (i >= n) break;
    int y = topY + k * rowH;
    if (k > 0) display.drawFastHLine(0, y - 2, 128, SSD1306_WHITE);

    RwBlock &b = rwBlocks[idx[i]];
    char t[6];
    sprintf(t, "%02d:%02d", b.startMin / 60, b.startMin % 60);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y);
    display.print(t);

    // Label after the time, truncated to what fits (15 chars from x=38).
    display.setCursor(38, y);
    int maxChars = 15;
    if ((int)strlen(b.label) <= maxChars) {
      display.print(b.label);
    } else {
      char trunc[16];
      strncpy(trunc, b.label, maxChars - 1);
      trunc[maxChars - 1] = '\0';
      display.print(trunc);
      display.print(".");
    }
  }

  // Scroll hints on the far right when there's more above/below.
  if (rwDayScroll > 0) {
    display.fillTriangle(122, 16 + yOffset, 118, 20 + yOffset, 126, 20 + yOffset, SSD1306_WHITE);
  }
  if (rwDayScroll + RW_DAY_ROWS < n) {
    display.fillTriangle(122, 62 + yOffset, 118, 58 + yOffset, 126, 58 + yOffset, SSD1306_WHITE);
  }
}

void drawGoodNight(int yOffset) {
  // Crescent: a white disc with an offset black disc carving the curve out.
  int cx = 64, cy = 26 + yOffset, r = 15;
  display.fillCircle(cx, cy, r, SSD1306_WHITE);
  display.fillCircle(cx + 7, cy - 4, r, SSD1306_BLACK);

  // A few stars for flair, kept clear of the moon.
  display.fillCircle(28, 14 + yOffset, 1, SSD1306_WHITE);
  display.fillCircle(100, 16 + yOffset, 1, SSD1306_WHITE);
  display.fillCircle(94, 36 + yOffset, 1, SSD1306_WHITE);

  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Good Night!", 50 + yOffset, 1);
}
