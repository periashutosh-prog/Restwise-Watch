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
#include <WiFiUdp.h>
#include <RTClib.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include "esp_sleep.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "time.h"

// Groq API key + model, kept out of the repo (ai_config.h is gitignored).
#if __has_include("ai_config.h")
  #include "ai_config.h"
#else
  #error "Missing ai_config.h - copy ai_config.example.h to ai_config.h and add your Groq API key."
#endif

// Master switch for the "extras" that exist on the dev watch but should not
// ship: the WiFi app (false hard-blocks the radio from ever being enabled,
// even if the UI is reachable) and the Dino Game (false hides it from the
// Apps menu). Does NOT affect the boot-time WiFi.mode(WIFI_OFF)/btStop()
// below -- the watch is always radio-off at boot regardless.
const bool WIFI_APP_ENABLED = true;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RTC_DS3231 rtc;

// I2C
#define PIN_SDA 14
#define PIN_SCL 6

// A full SSD1306 128x64 frame is 1024 GDDRAM bytes; the Adafruit driver adds a
// handful of command/control bytes per transfer (~1040 bytes total). Each I2C
// byte costs 9 bus clocks (8 data + 1 ACK), so one full-frame push costs
// ~1040*9 = 9360 bits. At a 40 FPS display target that's 9360*40 = 374,400 bps
// minimum with zero margin -- too tight to call comfortable once you add real
// I2C protocol overhead (repeated starts, clock stretching, buffer refills).
// Doubling that for real headroom lands at ~750kHz, so we run the bus at the
// nearest standard rate above it: 1MHz (Fast Mode Plus), which every SSD1306
// clone and the ESP32-C6's I2C peripheral both support.
//
// The DS3231 is NOT rated past 400kHz (Fast-mode) per its datasheet, so it
// can't share the bus at 1MHz. Since it sits on the same SDA/SCL lines as the
// OLED, the bus speed is switched down to 400kHz right around every RTC
// transaction and restored afterward -- see I2C_CLOCK_DISPLAY/I2C_CLOCK_RTC.
#define I2C_CLOCK_DISPLAY 1000000UL
#define I2C_CLOCK_RTC     400000UL

// Buttons (see wiring note above)
#define BTN_UP     23
#define BTN_DOWN   21
#define BTN_LEFT   19
#define BTN_RIGHT  20
#define BTN_CENTER 3

#define PIN_BATTERY_ADC 2 // IO2 = ADC1_CH2

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
  SCREEN_RESTWISE, SCREEN_RESTWISE_DAY, SCREEN_GOODNIGHT,
  SCREEN_ANIMATOR, SCREEN_BATTERY, SCREEN_BATTERY_INDICATOR, SCREEN_DINO,
  SCREEN_WIFI_CONFIRM, SCREEN_WIFI_SCANNING, SCREEN_WIFI_RESULTS,
  SCREEN_WIFI_PASSWORD, SCREEN_WIFI_KEYBOARD, SCREEN_WIFI_HOME,
  SCREEN_WIFI_TOGGLE_CONFIRM, SCREEN_WIFI_FORGET_CONFIRM, SCREEN_WIFI_NTP,
  SCREEN_AI_NOWIFI, SCREEN_AI_CHAT
};
ScreenState currentScreen = SCREEN_WATCHFACE;

bool lastButtonStates[5] = {false, false, false, false, false};
bool buttonJustPressed[5] = {false, false, false, false, false};

enum MenuApp {
  APP_RESTWISE, APP_STOPWATCH, APP_TIMER, APP_CALCULATOR, APP_ANIMATOR, APP_BATTERY,
  APP_WIFI, APP_AI, APP_DINO, APP_SECURITY, APP_LOCKSCREEN, APP_TERMINAL, APP_LOCK,
  NUM_MENU_APPS
};
const char* const menuAppNames[NUM_MENU_APPS] = {"Restwise", "Stopwatch", "Timer", "Calculator", "Animator", "Battery", "WiFi", "AI Chatbot", "Dino Game", "Security", "Lock Screen", "Terminal", "Lock"};
int menuApps[NUM_MENU_APPS]; // visible menu rows -> MenuApp, built once in buildMenu()
int menuCount = 0;
int menuIndex = 0;

void buildMenu() {
  menuCount = 0;
  for (int a = 0; a < NUM_MENU_APPS; a++) {
    if (a == APP_DINO && !WIFI_APP_ENABLED) continue;
    menuApps[menuCount++] = a;
  }
}

unsigned long swStartTime = 0;
unsigned long swElapsedTime = 0;
bool swRunning = false;
int swFocus = 0;

// Animator: a scripted, looping reel of predefined vector animations.
// Fully time-based (elapsed-ms driven), never frame-counted, so it stays
// smooth and reproducible regardless of loop jitter or frame rate.
int animatorSceneIndex = 0;
unsigned long animatorSceneStart = 0;
int animatorFps = 60; // user-selectable 10..60 in steps of 10

// ---- WiFi app (ported from AIO_Transmitter's WiFi UI/keyboard/focus-nav) ----
// wifiEnabled reflects the user's explicit on/off choice for this session --
// never set true automatically. There is no boot-time auto-connect by design.
bool wifiEnabled       = false;
int  wifiScanResults   = 0;
int  wifiListIndex     = -1;
int  wifiListScroll    = 0;
String wifiTargetSSID  = "";
String wifiPassword    = "";
String wifiStatusMsg   = "";
bool wifiConnecting    = false;
unsigned long wifiScanStart        = 0;
unsigned long wifiConnectStartTime = 0;
int wifiHomeCursor     = -1;
int wifiConfirmCursor  = 0;
int wifiToggleCursor   = 0;
int wifiForgetCursor   = 1;
String wifiForgetTargetSSID = "";
int wifiPasswordCursor = 1;
int wifiRetryCount     = 0;

Preferences prefs;
const int MAX_SAVED_NETS = 5;
String savedSSIDs[MAX_SAVED_NETS];
String savedPWDs[MAX_SAVED_NETS];
int savedNetCount = 0;
String lastConnectedSSID = "";

// Four independent, non-government NTP sources, queried in PARALLEL by four
// FreeRTOS tasks (raw SNTP over WiFiUDP -- bypasses the OS-level SNTP client
// entirely, sidestepping the stale-session class of bug that single-server
// configTime() hit). Whichever returns is used; if more than one has returned
// by the time we decide, the higher-priority one wins. Priority = list order
// (index 0 = highest).
const char* NTP_SERVERS[4] = {"time.google.com", "pool.ntp.org", "time.apple.com", "in.pool.ntp.org"};
const int NUM_NTP_SERVERS = 4;
const long  NTP_GMT_OFFSET_SEC = 19800; // IST (UTC+5:30)
const unsigned long NTP_TIMEOUT_MS = 20000;      // overall budget for all 4 tasks
const unsigned long NTP_PER_SERVER_TIMEOUT_MS = 8000; // one task's own UDP wait
const unsigned long NTP_RESULT_HOLD_MS = 5000;

volatile bool     ntpSlotDone[4]  = {false, false, false, false};
volatile bool     ntpSlotOk[4]    = {false, false, false, false};
volatile uint32_t ntpSlotEpoch[4] = {0, 0, 0, 0}; // Unix seconds, UTC

// Manual NTP Sync screen state. Polled non-blockingly (like the WiFi scan),
// never a single long blocking call -- that would freeze button polling and
// the "Syncing" animation for up to 20s straight.
int wifiNtpState = 0; // 0 = syncing, 1 = success, 2 = failed
int wifiNtpBestServer = -1; // index into NTP_SERVERS that actually provided the time
String wifiNtpResultMsg = "";
unsigned long wifiNtpStart  = 0;
unsigned long wifiNtpDoneAt = 0;

// Real internet reachability (not just "associated to an AP"), ported from
// AIO_Transmitter's connectivityTask -- rotates through 5 known 204-response
// endpoints, one check/sec, and treats internet as up if ANY of the last 5
// succeeded (so one dead endpoint doesn't false-negative the whole check).
bool internetOK    = false;
bool pingerStatus[5] = {false, false, false, false, false};
bool pingerStarted = false;

// 8x8 status icons for the watchface, MSB-first, one byte per row.
static const uint8_t PROGMEM wifi_bmp[] = {
  0x00, 0x7E, 0x81, 0x3C, 0x42, 0x18, 0x00, 0x18
};
static const uint8_t PROGMEM tower_bmp[] = {
  0x18, 0x24, 0x42, 0x18, 0x18, 0x18, 0x3C, 0x7E
};

// On-screen QWERTY keyboard (same grid/focus-nav as AIO_Transmitter's).
enum KBMode { KB_LOWER, KB_UPPER, KB_SYMBOL };
KBMode currentKBMode = KB_LOWER;
int kbCursorRow = 0, kbCursorCol = 0;
String kbInputBuffer = "";
int kbTextCursor   = 0;
int kbScrollOffset = 0;
ScreenState kbReturnScreen = SCREEN_WIFI_PASSWORD;

const char* kbLower[4][11] = {
  {"q","w","e","r","t","y","u","i","o","p","."},
  {"a","s","d","f","g","h","j","k","l",",","?"},
  {"z","x","c","v","b","n","m","<",">","^","BS"},
  {"1","2","3","4","5","6","7","8","9"," ","EN"}
};
const char* kbUpper[4][11] = {
  {"Q","W","E","R","T","Y","U","I","O","P","."},
  {"A","S","D","F","G","H","J","K","L",",","?"},
  {"Z","X","C","V","B","N","M","<",">","^","BS"},
  {"!","@","#","$","%","^","&","*","("," ","EN"}
};
const char* kbSymbol[4][11] = {
  {"1","2","3","4","5","6","7","8","9","0","~"},
  {"@","$","%","&","*","-","+","=","!","|","\\"},
  {"/","?",";",":","`","(",")","CL","CR","^","BS"},
  {"[","]","{","}","<",">","#","_","."," ","EN"}
};

// ---- AI Chatbot (ported from AIO_Transmitter) ----
// Transcript lives in plain globals: leaving the app and coming back keeps
// the thread, a reboot wipes it -- exactly the lifetime this needs.
struct AiMsg { bool fromUser; String text; };
const int AI_MAX_MSGS = 16;   // ring buffer; oldest drops off the top
const int AI_CTX_MSGS = 8;    // how many turns get resent as context
AiMsg aiMsgs[AI_MAX_MSGS];
int  aiMsgCount = 0;

// Layout: header 0-12, transcript 13-48, input row 49-62.
const int AI_LINE_H     = 9;
const int AI_VIEW_TOP   = 14;
const int AI_VIEW_H     = 36;   // 14..49, 4 lines
const int AI_WRAP_CHARS = 20;   // 6px glyphs, leaving room for the focus border

int  aiFocus       = 1;   // -1 = "<--", 0 = message area, 1 = input box, 2 = SEND
bool aiScrollMode  = false;
int  aiScroll      = 0;
String aiInputText = "";

// UI thread <-> network task handshake. Each flag has exactly one writer at
// any point in the cycle, so no mutex is needed: the UI fills aiPendingBody
// and raises aiRequestPending, the task consumes it and raises aiReplyReady,
// the UI drains that and drops aiBusy.
volatile bool aiRequestPending = false;
volatile bool aiReplyReady     = false;
volatile bool aiBusy           = false;
String aiPendingBody;
String aiReplyText;
bool   aiTaskStarted = false;

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

// After 90s of true inactivity the watch drops into deep sleep rather than
// just blanking the display. Only GPIO0-7 sit in the ESP32-C6's LP domain,
// so CENTER (GPIO3) is the only button physically capable of waking it from
// deep sleep -- UP/DOWN/LEFT/RIGHT cannot. Deep sleep is a full chip reset,
// so currentScreen's global initializer (SCREEN_WATCHFACE) is what actually
// guarantees "always wake on the watchface" -- nothing extra is needed for
// that part.
const unsigned long DEEP_SLEEP_TIMEOUT_MS = 90000;
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
void drawAnimator(int yOffset);
void processRestwiseSerial();
void rwLoadFromFile();
int  rwBuildDayList(int dayIdx, int* out, int maxOut);
int  rwResolveDayIndex(int item);
const char* rwDayName(int item);
bool isNightNow();
void enterDeepSleep();
void drawBattery(int yOffset);
void drawBatteryIndicator(int yOffset);
void loadBatterySettings();
void saveBatterySettings();
void drawWifiConfirm(int yOffset);
void drawWifiScanning(int yOffset);
void drawWifiResults(int yOffset);
void drawWifiPassword(int yOffset);
void drawWifiKeyboard();
void drawWifiHome(int yOffset);
void drawWifiToggleConfirm(int yOffset);
void drawWifiForgetConfirm(int yOffset);
void loadCredentials();
void saveCredential(String ssid, String pwd);
void forgetCredential(String ssid);
String findSavedPwd(String ssid);
bool ntpQueryOnce(const char* host, uint32_t &outUnixTime, unsigned long timeoutMs);
void ntpQueryTask(void *pv);
void startNtpSync();
String aiToAscii(const String &in);
void aiAppendMsg(bool fromUser, const String &text);
String aiCallGroq(const String &body);
void aiChatTask(void *p);
void aiStartTask();
void aiSendMessage();
int aiRenderChat(int baseY, bool draw, int yOffset = 0);
void drawAiNoWifi(int yOffset);
void drawAiChat(int yOffset);
void drawWifiNTP(int yOffset);
void connectivityTask(void *p);
void startConnectivityPinger();
void drawWifiStatusIcon(int x, int y);
void drawTowerStatusIcon(int x, int y);
void drawBatteryIcon(int x, int y);
void drawBatteryStatusGlyph(int rightEdgeX, int y);

/* ---------------------------- Battery monitoring ---------------------------- */
// A dedicated FreeRTOS task samples the ADC 1000x/sec and posts one averaged
// reading per second -- kept off the main loop() so UI drawing and button
// polling are never blocked by sampling. Single writer (this task), single
// reader (everything else), so plain volatiles are enough -- no mutex needed.

// Divider: BAT+ --[10k]-- IO2 --[20k]-- GND, so Vbat = Vadc * (10k+20k)/20k.
// A 4.2V-charged cell lands at ~2.8V on IO2, comfortably inside the ADC's
// usable range under 11dB attenuation.
const float BATTERY_DIVIDER_RATIO = 1.5f;
const int BATTERY_SAMPLES_PER_SEC = 1000;

volatile float batteryVoltage = 0.0f;
volatile int batteryAdcRaw = 0;        // averaged raw 12-bit ADC count (0-4095), for display/debug
volatile int batteryAdcMilliVolts = 0; // averaged, calibrated pin voltage (pre-divider), for display/debug
volatile bool batteryReadingValid = false;

// Un-averaged "instant" readings, latched from a single sample every 100ms
// (every 100th tick of the same 1000Hz loop) -- shows the raw, jittery signal
// side by side with the smoothed Voltage/Est. Battery figures above.
volatile int   batteryAdcRawInstant   = 0;
volatile float batteryVoltageInstant  = 0.0f;

// Battery app UI state.
int  battCursor    = -1; // -1 = back box, 0 = "Indicator" row
int  battIndCursor = -1; // on SCREEN_BATTERY_INDICATOR: -1 = back, 0 = Icon box, 1 = %age box
bool battShowPercent = false; // false = watchface shows icon, true = shows "NN%" text

// Rolling ~8-second history of the once-a-second averages, used to detect a
// charging cell: CC/CV charging makes voltage climb steadily, so a clear rise
// across this window means "charging", not sensor noise on a single sample.
const int BATTERY_HISTORY_LEN = 8;
volatile float batteryHistory[BATTERY_HISTORY_LEN];
volatile int batteryHistoryCount = 0;
volatile int batteryHistoryHead = 0;
volatile bool batteryCharging = false;
const float BATTERY_CHARGE_RISE_THRESHOLD = 0.02f; // volts, over the history window

// Piecewise-linear 1S LiPo voltage -> percent curve at a moderate (~500mA-ish)
// discharge rate -- far more accurate near the ends than a naive linear
// 3.0V-4.2V map, since LiPo voltage sags fast at the bottom and plateaus
// through the middle.
struct BatteryCurvePoint { float voltage; int percent; };
const int NUM_BATTERY_CURVE_PTS = 11;
const BatteryCurvePoint batteryCurve[NUM_BATTERY_CURVE_PTS] = {
  {3.27f, 0}, {3.61f, 5}, {3.69f, 10}, {3.73f, 20}, {3.77f, 30}, {3.80f, 40},
  {3.84f, 50}, {3.87f, 60}, {3.95f, 70}, {4.08f, 85}, {4.20f, 100}
};

int batteryVoltageToPercent(float v) {
  if (v <= batteryCurve[0].voltage) return 0;
  if (v >= batteryCurve[NUM_BATTERY_CURVE_PTS - 1].voltage) return 100;
  for (int i = 0; i < NUM_BATTERY_CURVE_PTS - 1; i++) {
    if (v >= batteryCurve[i].voltage && v <= batteryCurve[i + 1].voltage) {
      float span = batteryCurve[i + 1].voltage - batteryCurve[i].voltage;
      float frac = (span > 0) ? (v - batteryCurve[i].voltage) / span : 0;
      return batteryCurve[i].percent + (int)(frac * (batteryCurve[i + 1].percent - batteryCurve[i].percent));
    }
  }
  return 0;
}

void batteryAdcTask(void *pvParameters) {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  // Seed everything with one real sample right away. Without this, Voltage/
  // Est. Battery/the watchface icon would sit at 0 (uninitialized) for the
  // ~1s the first full 1000-sample average takes -- a visible jump from
  // empty-icon/"--" straight to the real level. The seed gets overwritten by
  // the first proper average a second later, so it only affects that first
  // instant on boot (and on every deep-sleep wake, which re-runs this task).
  {
    int mvSeed  = analogReadMilliVolts(PIN_BATTERY_ADC);
    int rawSeed = analogRead(PIN_BATTERY_ADC);
    float vSeed = (mvSeed / 1000.0f) * BATTERY_DIVIDER_RATIO;
    batteryVoltage        = vSeed;
    batteryAdcRaw         = rawSeed;
    batteryAdcMilliVolts  = mvSeed;
    batteryAdcRawInstant  = rawSeed;
    batteryVoltageInstant = vSeed;
    batteryReadingValid   = true;
  }

  for (;;) {
    uint32_t sumMv = 0, sumRaw = 0;
    for (int i = 0; i < BATTERY_SAMPLES_PER_SEC; i++) {
      int mvSample  = analogReadMilliVolts(PIN_BATTERY_ADC);
      int rawSample = analogRead(PIN_BATTERY_ADC);
      sumMv += mvSample;
      sumRaw += rawSample;

      if (i % 100 == 0) { // ~every 100ms: latch this single un-averaged sample
        batteryAdcRawInstant  = rawSample;
        batteryVoltageInstant = (mvSample / 1000.0f) * BATTERY_DIVIDER_RATIO;
      }

      vTaskDelay(1); // ~1ms tick -> ~1000 samples spread across ~1 second
    }
    float avgMilliVolts = (float)sumMv / BATTERY_SAMPLES_PER_SEC;
    float vBat = (avgMilliVolts / 1000.0f) * BATTERY_DIVIDER_RATIO;

    batteryVoltage = vBat;
    batteryAdcRaw = sumRaw / BATTERY_SAMPLES_PER_SEC;
    batteryAdcMilliVolts = (int)avgMilliVolts;
    batteryReadingValid = true;

    batteryHistory[batteryHistoryHead] = vBat;
    batteryHistoryHead = (batteryHistoryHead + 1) % BATTERY_HISTORY_LEN;
    if (batteryHistoryCount < BATTERY_HISTORY_LEN) batteryHistoryCount++;

    if (batteryHistoryCount == BATTERY_HISTORY_LEN) {
      int oldestIdx = batteryHistoryHead; // head now points at the oldest slot
      float oldest = batteryHistory[oldestIdx];
      batteryCharging = (vBat - oldest) > BATTERY_CHARGE_RISE_THRESHOLD;
    }
  }
}

// Small outline-plus-fill glyph, 4 segments. (x,y) is its top-left corner.
void drawBatteryIcon(int x, int y) {
  // While charging, the whole icon blinks (500ms on/off) instead of sitting
  // static, so a glance at the watchface shows charging state at a glance.
  if (batteryCharging && ((millis() / 500) % 2 == 1)) return;

  display.drawRect(x, y, 14, 8, SSD1306_WHITE);
  display.fillRect(x + 14, y + 2, 2, 4, SSD1306_WHITE); // + terminal nub

  int percent = batteryReadingValid ? batteryVoltageToPercent(batteryVoltage) : 0;
  int segments = (percent * 4 + 50) / 100;
  if (segments > 4) segments = 4;

  for (int i = 0; i < segments; i++) {
    display.fillRect(x + 1 + i * 3, y + 2, 2, 4, SSD1306_WHITE);
  }
}

// Watchface top-right battery status, in whichever mode the user picked on
// SCREEN_BATTERY_INDICATOR: the 4-segment icon, or plain "NN%" text.
// rightEdgeX is where the glyph's right edge should land.
void drawBatteryStatusGlyph(int rightEdgeX, int y) {
  if (!battShowPercent) {
    drawBatteryIcon(rightEdgeX - 16, y);
    return;
  }
  int percent = batteryReadingValid ? batteryVoltageToPercent(batteryVoltage) : 0;
  char buf[6];
  sprintf(buf, "%d%%", percent);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(rightEdgeX - w - x1, y);
  display.print(buf);
}

// Battery screen content is taller than the 64px display, so it scrolls:
// focusing the back box shows the top rows, focusing the Indicator row
// scrolls down to reveal Raw ADC / Raw Voltage / Indicator at the bottom.
const int BATT_ROW_H = 12;
const int BATT_ROW_TOP = 18;
const int BATT_SCROLL_DOWN = 36;

void drawBattery(int yOffset) {
  bool backSel = (battCursor == -1);
  drawHeader(yOffset, "BATTERY", backSel);

  float v = batteryVoltage;
  int percent = batteryVoltageToPercent(v);
  int scrollY = (battCursor == -1) ? 0 : BATT_SCROLL_DOWN;

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  char buf[24];

  // Row 0: Voltage (averaged)
  int y = BATT_ROW_TOP + 0 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    if (batteryReadingValid) sprintf(buf, "Voltage: %.2fV", v);
    else strcpy(buf, "Voltage: --");
    display.print(buf);
  }

  // Row 1: Est. Battery %
  y = BATT_ROW_TOP + 1 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    if (batteryReadingValid) sprintf(buf, "Est. Battery: %d%%", percent);
    else strcpy(buf, "Est. Battery: --");
    display.print(buf);
  }

  // Row 2: ADC (averaged)
  y = BATT_ROW_TOP + 2 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    if (batteryReadingValid) sprintf(buf, "ADC: %d (%dmV)", batteryAdcRaw, batteryAdcMilliVolts);
    else strcpy(buf, "ADC: --");
    display.print(buf);
  }

  // Row 3: Charging
  y = BATT_ROW_TOP + 3 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    display.print("Charging: ");
    display.print(batteryCharging ? "True" : "False");
  }

  // Row 4: Raw ADC (un-averaged, 100ms)
  y = BATT_ROW_TOP + 4 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    sprintf(buf, "Raw ADC: %d", batteryAdcRawInstant);
    display.print(buf);
  }

  // Row 5: Raw Voltage (un-averaged, 100ms)
  y = BATT_ROW_TOP + 5 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    display.setCursor(2, y);
    sprintf(buf, "Raw Voltage: %.2fV", batteryVoltageInstant);
    display.print(buf);
  }

  // Row 6: Indicator (focusable -- opens SCREEN_BATTERY_INDICATOR)
  y = BATT_ROW_TOP + 6 * BATT_ROW_H - scrollY + yOffset;
  if (y >= 13 + yOffset) {
    bool sel = (battCursor == 0);
    if (sel) { display.fillRect(0, y - 1, 128, BATT_ROW_H, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else      { display.setTextColor(SSD1306_WHITE); }
    display.setCursor(2, y);
    display.print("Indicator: ");
    display.print(battShowPercent ? "Percentage" : "Icon");
    display.setTextColor(SSD1306_WHITE);
  }

  // Scrollbar hint on the right edge -- two discrete positions (top/bottom).
  display.drawFastVLine(126, 13 + yOffset, 50, SSD1306_WHITE);
  int thumbY = 13 + yOffset + ((scrollY == 0) ? 0 : 30);
  display.fillRect(125, thumbY, 3, 20, SSD1306_WHITE);
}

void drawBatteryIndicator(int yOffset) {
  bool backSel = (battIndCursor == -1);
  drawHeader(yOffset, "Battery", backSel);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Choose Battery", 22 + yOffset, 1);
  drawCenteredText(display, "Indicator", 32 + yOffset, 1);
  drawBoxedCenteredText(display, "ICON", 14, 48 + yOffset, 40, 13, (battIndCursor == 0));
  drawBoxedCenteredText(display, "%AGE", 74, 48 + yOffset, 40, 13, (battIndCursor == 1));
}

// Persisted via NVS (same Preferences mechanism as WiFi credentials) so the
// choice survives deep sleep -- deep sleep re-runs setup() from scratch, so a
// plain global would otherwise silently reset to "Icon" on every wake.
void loadBatterySettings() {
  prefs.begin("batt_set", true);
  battShowPercent = prefs.getBool("pct", false);
  prefs.end();
}

void saveBatterySettings() {
  prefs.begin("batt_set", false);
  prefs.putBool("pct", battShowPercent);
  prefs.end();
}

/* ---------------------------- Dino game ---------------------------- */
// Sprites: Chromium's own T-Rex/cactus art, body-cropped and downsampled 0.45x for the OLED.

// dinoStand: 18x19
const uint8_t dinoStand[] PROGMEM = {
  0x00, 0x3f, 0x80,   //           #######
  0x00, 0x6f, 0xc0,   //          ## ######
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7c, 0x00,   //          #####
  0x00, 0x7f, 0x00,   //          #######
  0x81, 0xf8, 0x00,   // #      ######
  0xc3, 0xfc, 0x00,   // ##    ########
  0xe7, 0xfc, 0x00,   // ###  #########
  0xff, 0xf8, 0x00,   // #############
  0xff, 0xf8, 0x00,   // #############
  0x7f, 0xf0, 0x00,   //  ###########
  0x3f, 0xf0, 0x00,   //   ##########
  0x1f, 0xe0, 0x00,   //    ########
  0x0f, 0xc0, 0x00,   //     ######
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0e, 0x70, 0x00,   //     ###  ###
};

// dinoBlink: 18x19
const uint8_t dinoBlink[] PROGMEM = {
  0x00, 0x3f, 0x80,   //           #######
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7c, 0x00,   //          #####
  0x00, 0x7f, 0x00,   //          #######
  0x81, 0xf8, 0x00,   // #      ######
  0xc3, 0xfc, 0x00,   // ##    ########
  0xe7, 0xfc, 0x00,   // ###  #########
  0xff, 0xf8, 0x00,   // #############
  0xff, 0xf8, 0x00,   // #############
  0x7f, 0xf0, 0x00,   //  ###########
  0x3f, 0xf0, 0x00,   //   ##########
  0x1f, 0xe0, 0x00,   //    ########
  0x0f, 0xc0, 0x00,   //     ######
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0e, 0x70, 0x00,   //     ###  ###
};

// dinoRun1: 18x19
const uint8_t dinoRun1[] PROGMEM = {
  0x00, 0x3f, 0x80,   //           #######
  0x00, 0x6f, 0xc0,   //          ## ######
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7c, 0x00,   //          #####
  0x00, 0x7f, 0x00,   //          #######
  0x81, 0xf8, 0x00,   // #      ######
  0xc3, 0xfc, 0x00,   // ##    ########
  0xe7, 0xfc, 0x00,   // ###  #########
  0xff, 0xf8, 0x00,   // #############
  0xff, 0xf8, 0x00,   // #############
  0x7f, 0xf0, 0x00,   //  ###########
  0x3f, 0xf0, 0x00,   //   ##########
  0x1f, 0xe0, 0x00,   //    ########
  0x0f, 0xc0, 0x00,   //     ######
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0c, 0x00, 0x00,   //     ##
  0x0e, 0x00, 0x00,   //     ###
};

// dinoRun2: 18x19
const uint8_t dinoRun2[] PROGMEM = {
  0x00, 0x3f, 0x80,   //           #######
  0x00, 0x6f, 0xc0,   //          ## ######
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7c, 0x00,   //          #####
  0x00, 0x7f, 0x00,   //          #######
  0x81, 0xf8, 0x00,   // #      ######
  0xc3, 0xfc, 0x00,   // ##    ########
  0xe7, 0xfc, 0x00,   // ###  #########
  0xff, 0xf8, 0x00,   // #############
  0xff, 0xf8, 0x00,   // #############
  0x7f, 0xf0, 0x00,   //  ###########
  0x3f, 0xf0, 0x00,   //   ##########
  0x1f, 0xe0, 0x00,   //    ########
  0x0f, 0xc0, 0x00,   //     ######
  0x0c, 0x60, 0x00,   //     ##   ##
  0x00, 0x60, 0x00,   //          ##
  0x00, 0x70, 0x00,   //          ###
};

// dinoDead: 18x19
const uint8_t dinoDead[] PROGMEM = {
  0x00, 0x3f, 0x80,   //           #######
  0x00, 0x57, 0xc0,   //          # # #####
  0x00, 0x6f, 0xc0,   //          ## ######
  0x00, 0x57, 0xc0,   //          # # #####
  0x00, 0x7f, 0xc0,   //          #########
  0x00, 0x7c, 0x00,   //          #####
  0x00, 0x7f, 0x00,   //          #######
  0x81, 0xf8, 0x00,   // #      ######
  0xc3, 0xfc, 0x00,   // ##    ########
  0xe7, 0xfc, 0x00,   // ###  #########
  0xff, 0xf8, 0x00,   // #############
  0xff, 0xf8, 0x00,   // #############
  0x7f, 0xf0, 0x00,   //  ###########
  0x3f, 0xf0, 0x00,   //   ##########
  0x1f, 0xe0, 0x00,   //    ########
  0x0f, 0xc0, 0x00,   //     ######
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0c, 0x60, 0x00,   //     ##   ##
  0x0e, 0x70, 0x00,   //     ###  ###
};

// cactusS1: 7x15
const uint8_t cactusS1[] PROGMEM = {
  0x38,   //   ###
  0x38,   //   ###
  0x3a,   //   ### #
  0x3a,   //   ### #
  0xba,   // # ### #
  0xba,   // # ### #
  0xbe,   // # #####
  0xbe,   // # #####
  0xf8,   // #####
  0x78,   //  ####
  0x38,   //   ###
  0x38,   //   ###
  0x38,   //   ###
  0x38,   //   ###
  0x38,   //   ###
};

// cactusS2: 14x15
const uint8_t cactusS2[] PROGMEM = {
  0x30, 0x30,   //   ##      ##
  0x30, 0x30,   //   ##      ##
  0x37, 0xf4,   //   ## ####### #
  0x37, 0xf4,   //   ## ####### #
  0xb7, 0xf4,   // # ## ####### #
  0xb7, 0xf4,   // # ## ####### #
  0xbe, 0xf4,   // # ##### #### #
  0xbc, 0x7c,   // # ####   #####
  0xf8, 0x3c,   // #####     ####
  0x70, 0x30,   //  ###      ##
  0x30, 0x30,   //   ##      ##
  0x30, 0x30,   //   ##      ##
  0x30, 0x30,   //   ##      ##
  0x30, 0x30,   //   ##      ##
  0x30, 0x30,   //   ##      ##
};

// cactusS3: 22x15
const uint8_t cactusS3[] PROGMEM = {
  0x30, 0x30, 0x30,   //   ##      ##      ##
  0x38, 0x30, 0x70,   //   ###     ##     ###
  0x3e, 0xb1, 0xf4,   //   ##### # ##   ##### #
  0x3e, 0xb5, 0xf4,   //   ##### # ## # ##### #
  0xbe, 0xb5, 0xf4,   // # ##### # ## # ##### #
  0xbe, 0xb5, 0xf4,   // # ##### # ## # ##### #
  0xbe, 0xb5, 0xf4,   // # ##### # ## # ##### #
  0xbc, 0xb4, 0xfc,   // # ####  # ## #  ######
  0xf8, 0xb4, 0x7c,   // #####   # ## #   #####
  0x78, 0xf4, 0x70,   //  ####   #### #   ###
  0x38, 0x7c, 0x70,   //   ###    #####   ###
  0x38, 0x38, 0x70,   //   ###     ###    ###
  0x38, 0x30, 0x70,   //   ###     ##     ###
  0x38, 0x30, 0x70,   //   ###     ##     ###
  0x38, 0x30, 0x70,   //   ###     ##     ###
};

// cactusL1: 10x22
const uint8_t cactusL1[] PROGMEM = {
  0x0c, 0x00,   //     ##
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0xc0,   //    #### ##
  0xde, 0xc0,   // ## #### ##
  0xde, 0xc0,   // ## #### ##
  0xde, 0xc0,   // ## #### ##
  0xde, 0xc0,   // ## #### ##
  0xde, 0xc0,   // ## #### ##
  0xde, 0xc0,   // ## #### ##
  0xff, 0x80,   // #########
  0x7f, 0x00,   //  #######
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
  0x1e, 0x00,   //    ####
};

// cactusL2: 22x22
const uint8_t cactusL2[] PROGMEM = {
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x09, 0xc0,   //     ###     #  ###
  0x0e, 0x1d, 0xc0,   //     ###    ### ###
  0x0e, 0x1d, 0xc0,   //     ###    ### ###
  0x4e, 0xdd, 0xcc,   //  #  ### ## ### ###  ##
  0xce, 0xdd, 0xcc,   // ##  ### ## ### ###  ##
  0xce, 0xdd, 0xcc,   // ##  ### ## ### ###  ##
  0xce, 0xdd, 0xcc,   // ##  ### ## ### ###  ##
  0xce, 0xcf, 0xcc,   // ##  ### ##  ######  ##
  0xce, 0xcf, 0xcc,   // ##  ### ##  ######  ##
  0xce, 0xc3, 0xcc,   // ##  ### ##    ####  ##
  0xff, 0x81, 0xf8,   // #########      ######
  0x7e, 0x01, 0xf0,   //  ######        #####
  0x1e, 0x01, 0xc0,   //    ####        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x0e, 0x01, 0xc0,   //     ###        ###
  0x1e, 0x01, 0xc0,   //    ####        ###
  0x1e, 0x01, 0xc0,   //    ####        ###
};

// cactusL3: 33x22
const uint8_t cactusL3[] PROGMEM = {
  0x0e, 0x00, 0x00, 0x38, 0x00,   //     ###                   ###
  0x0e, 0x03, 0x00, 0x38, 0x00,   //     ###       ##          ###
  0x0e, 0x07, 0x00, 0x38, 0x00,   //     ###      ###          ###
  0x0e, 0x07, 0x41, 0xb8, 0x00,   //     ###      ### #     ## ###
  0x0e, 0x07, 0x61, 0xb8, 0x00,   //     ###      ### ##    ## ###
  0x0e, 0xc7, 0x61, 0xb9, 0x80,   //     ### ##   ### ##    ## ###  ##
  0xce, 0xc7, 0x61, 0xb9, 0x80,   // ##  ### ##   ### ##    ## ###  ##
  0xce, 0xd7, 0x61, 0xb9, 0x80,   // ##  ### ## # ### ##    ## ###  ##
  0xce, 0xdf, 0xc1, 0xb9, 0x80,   // ##  ### ## #######     ## ###  ##
  0xce, 0xdf, 0x19, 0xf9, 0x80,   // ##  ### ## #####   ##  ######  ##
  0xce, 0xdf, 0x19, 0xf9, 0x80,   // ##  ### ## #####   ##  ######  ##
  0xce, 0xdf, 0x5a, 0xf9, 0x80,   // ##  ### ## ##### # ## # #####  ##
  0xff, 0xdf, 0x7a, 0x3f, 0x00,   // ########## ##### #### #   ######
  0x7f, 0x9f, 0x7a, 0x3e, 0x00,   //  ########  ##### #### #   #####
  0x3e, 0x0f, 0x7a, 0x38, 0x00,   //   #####     #### #### #   ###
  0x0e, 0x07, 0x7e, 0x38, 0x00,   //     ###      ### ######   ###
  0x0e, 0x07, 0x3c, 0x38, 0x00,   //     ###      ###  ####    ###
  0x0e, 0x07, 0x18, 0x38, 0x00,   //     ###      ###   ##     ###
  0x0e, 0x07, 0x18, 0x38, 0x00,   //     ###      ###   ##     ###
  0x0e, 0x07, 0x18, 0x38, 0x00,   //     ###      ###   ##     ###
  0x0e, 0x07, 0x18, 0x38, 0x00,   //     ###      ###   ##     ###
  0x1e, 0x03, 0x08, 0x38, 0x00,   //    ####       ##    #     ###
};

const int DINO_W = 18, DINO_H = 19, DINO_X = 10;
const int DINO_FEET_Y = 61; // last pixel row of every sprite; the horizon line is drawn on row 62
const float DINO_GRAVITY = 906.25f; // px/s^2
const float DINO_JUMP_V = 252.0f;   // px/s -> ~35px apex, ~0.56s airtime
const float DINO_AIR_TIME = 2.0f * DINO_JUMP_V / DINO_GRAVITY;
const float DINO_SPEED_START = 100.0f; // px/s
const float DINO_SPEED_MAX = 190.0f;
const float DINO_ACCEL = 0.75f;        // px/s gained per second
const float DINO_FIRST_OBSTACLE_SEC = 1.4f;
const unsigned long DINO_CRASH_HOLD_MS = 700;
const int DINO_MAX_CACTI = 4;
const int DINO_MAX_SCORE = 99999;
const char* const DINO_HI_FILE = "/dino_hi.txt";

struct DinoCactusDef { const uint8_t* bmp; uint8_t w, h; };
const DinoCactusDef dinoCactusDefs[6] = {
  {cactusS1, 7, 15}, {cactusS2, 14, 15}, {cactusS3, 22, 15},
  {cactusL1, 10, 22}, {cactusL2, 22, 22}, {cactusL3, 33, 22}
};
struct DinoCactus { float x; uint8_t type; bool active; };
struct DinoBox { int8_t x0, x1, r0, r1; }; // sprite-local columns [x0,x1) and rows [r0,r1)
const DinoBox dinoBoxes[3] = { {9, 16, 1, 6}, {1, 12, 7, 15}, {4, 11, 15, 19} }; // head, body, feet

enum DinoState { DINO_READY, DINO_PLAYING, DINO_PAUSED, DINO_CRASHED, DINO_OVER };
DinoState dinoState = DINO_READY;
int  dinoCursor = 1; // -1 back, 0 left box, 1 right box
int  dinoHigh = 0;
int  dinoScore = 0;
float dinoY = 0, dinoVy = 0;
float dinoSpeed = DINO_SPEED_START, dinoDist = 0, dinoRunSec = 0, dinoGroundScroll = 0;
float dinoTravel = 0, dinoLastW = 0, dinoNextGap = 0;
bool dinoSpawnedAny = false;
int8_t dinoLastCat[2] = {-1, -1}; // last two spawned kinds (0 small, 1 large)
unsigned long dinoLastTick = 0, dinoCrashedAt = 0;
DinoCactus dinoCacti[DINO_MAX_CACTI];
uint8_t dinoBumps[32]; // 256-bit ground-bump pattern, scrolls with the world

int dinoRand(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }
float dinoRandf() { return (esp_random() % 10000) / 10000.0f; }

// The high score lives in a file on the same LittleFS partition the Restwise timetable uses.
int dinoLoadHigh() {
  File f = LittleFS.open(DINO_HI_FILE, "r");
  if (!f) return 0;
  int v = f.parseInt();
  f.close();
  return constrain(v, 0, DINO_MAX_SCORE);
}

void dinoSaveHigh(int v) {
  File f = LittleFS.open(DINO_HI_FILE, "w");
  if (!f) return;
  f.print(v);
  f.close();
}

void dinoMakeBumps() {
  memset(dinoBumps, 0, sizeof(dinoBumps));
  for (int i = 0; i < 256; ) {
    if (dinoRand(0, 99) < 9) {
      int len = dinoRand(1, 3);
      for (int k = 0; k < len; k++) { int p = (i + k) & 255; dinoBumps[p >> 3] |= (1 << (p & 7)); }
      i += len + 4;
    } else {
      i++;
    }
  }
}

void dinoReset() {
  dinoY = 0; dinoVy = 0; dinoScore = 0;
  dinoSpeed = DINO_SPEED_START; dinoDist = 0; dinoRunSec = 0; dinoGroundScroll = 0;
  dinoTravel = 0; dinoLastW = 0; dinoNextGap = 0; dinoSpawnedAny = false;
  dinoLastCat[0] = dinoLastCat[1] = -1;
  for (int i = 0; i < DINO_MAX_CACTI; i++) dinoCacti[i].active = false;
  dinoMakeBumps();
}

void dinoOpen() {
  dinoHigh = dinoLoadHigh();
  dinoReset();
  dinoState = DINO_READY;
  dinoCursor = 1;
}

void dinoStart(bool withJump) {
  dinoReset();
  dinoState = DINO_PLAYING;
  dinoLastTick = millis();
  if (withJump) dinoVy = DINO_JUMP_V;
}

void dinoJump() {
  if (dinoY <= 0.0f && dinoVy <= 0.0f) dinoVy = DINO_JUMP_V;
}

void dinoSpawn() {
  int slot = -1;
  for (int i = 0; i < DINO_MAX_CACTI; i++) if (!dinoCacti[i].active) { slot = i; break; }
  if (slot < 0) return;

  int cat = (dinoRand(0, 99) < 45) ? 1 : 0;
  if (dinoLastCat[0] == cat && dinoLastCat[1] == cat) cat = 1 - cat; // never three of a kind in a row
  // Big groups only once the world is fast enough that a jump can clear their width.
  int maxSize = 3;
  if (cat == 1) maxSize = (dinoSpeed >= 170.0f) ? 3 : ((dinoSpeed >= 140.0f) ? 2 : 1);
  int type = cat * 3 + dinoRand(1, maxSize) - 1;

  dinoCacti[slot] = {128.0f, (uint8_t)type, true};
  dinoLastCat[1] = dinoLastCat[0];
  dinoLastCat[0] = cat;

  dinoLastW = dinoCactusDefs[type].w;
  float minGap = dinoSpeed * DINO_AIR_TIME + 0.4f * dinoLastW; // at least one full jump of room
  dinoNextGap = minGap * (1.0f + 0.6f * dinoRandf());
  dinoTravel = 0;
  dinoSpawnedAny = true;
}

bool dinoHitsCactus(int idx) { // takes an index, not the struct: Arduino hoists prototypes above type definitions
  const DinoCactus &c = dinoCacti[idx];
  const DinoCactusDef &d = dinoCactusDefs[c.type];
  int cx0 = (int)c.x + 1, cx1 = (int)c.x + d.w - 1; // inset 1px so empty arm corners don't count
  int cy0 = DINO_FEET_Y - d.h + 1, cy1 = DINO_FEET_Y + 1;
  int top = DINO_FEET_Y - DINO_H + 1 - (int)(dinoY + 0.5f);
  for (int i = 0; i < 3; i++) {
    int bx0 = DINO_X + dinoBoxes[i].x0, bx1 = DINO_X + dinoBoxes[i].x1;
    int by0 = top + dinoBoxes[i].r0,    by1 = top + dinoBoxes[i].r1;
    if (bx1 > cx0 && bx0 < cx1 && by1 > cy0 && by0 < cy1) return true;
  }
  return false;
}

void dinoCrash() {
  dinoState = DINO_CRASHED;
  dinoCrashedAt = millis();
  if (dinoScore > dinoHigh) {
    dinoHigh = dinoScore;
    dinoSaveHigh(dinoHigh);
  }
}

void dinoTick() {
  if (dinoState == DINO_CRASHED) {
    if (millis() - dinoCrashedAt >= DINO_CRASH_HOLD_MS) { dinoState = DINO_OVER; dinoCursor = 1; }
    return;
  }
  if (dinoState != DINO_PLAYING) return;

  unsigned long now = millis();
  float dt = (now - dinoLastTick) / 1000.0f;
  if (dt <= 0.0f) return;
  dinoLastTick = now;
  if (dt > 0.05f) dt = 0.05f;

  if (dinoY > 0.0f || dinoVy > 0.0f) {
    dinoVy -= DINO_GRAVITY * dt;
    dinoY += dinoVy * dt;
    if (dinoY <= 0.0f) { dinoY = 0.0f; dinoVy = 0.0f; }
  }
  if (dinoSpeed < DINO_SPEED_MAX) dinoSpeed = min(DINO_SPEED_MAX, dinoSpeed + DINO_ACCEL * dt);

  float step = dinoSpeed * dt;
  dinoDist += step;
  dinoRunSec += dt;
  dinoGroundScroll += step;
  dinoScore = min(DINO_MAX_SCORE, (int)(dinoDist * 0.1f));

  for (int i = 0; i < DINO_MAX_CACTI; i++) {
    if (!dinoCacti[i].active) continue;
    dinoCacti[i].x -= step;
    if (dinoCacti[i].x + dinoCactusDefs[dinoCacti[i].type].w < 0) dinoCacti[i].active = false;
  }

  if (!dinoSpawnedAny) {
    if (dinoRunSec >= DINO_FIRST_OBSTACLE_SEC) dinoSpawn();
  } else {
    dinoTravel += step;
    if (dinoTravel >= dinoLastW + dinoNextGap) dinoSpawn();
  }

  for (int i = 0; i < DINO_MAX_CACTI; i++) {
    if (dinoCacti[i].active && dinoHitsCactus(i)) { dinoCrash(); return; }
  }
}

void dinoHandleButtons() {
  switch (dinoState) {
    case DINO_READY:
      if (buttonJustPressed[0]) dinoStart(true);
      if (buttonJustPressed[4]) startAnimation(SCREEN_MENU, 64);
      break;

    case DINO_PLAYING:
      if (buttonJustPressed[0]) dinoJump();
      if (buttonJustPressed[4]) { dinoState = DINO_PAUSED; dinoCursor = 0; }
      break;

    case DINO_CRASHED:
      break;

    case DINO_PAUSED:
    case DINO_OVER:
      if (buttonJustPressed[0]) dinoCursor = -1;
      if (buttonJustPressed[1] && dinoCursor == -1) dinoCursor = 0;
      if (buttonJustPressed[2]) dinoCursor = 0;
      if (buttonJustPressed[3]) dinoCursor = 1;
      if (buttonJustPressed[4]) {
        if (dinoCursor == -1) {
          startAnimation(SCREEN_MENU, 64);
        } else if (dinoState == DINO_PAUSED) {
          if (dinoCursor == 0) { dinoLastTick = millis(); dinoState = DINO_PLAYING; }
          else startAnimation(SCREEN_MENU, 64);
        } else {
          if (dinoCursor == 0) startAnimation(SCREEN_MENU, 64);
          else dinoStart(false);
        }
      }
      break;
  }
}

void drawDinoHeader(int yOffset, bool backFocused) {
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
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
  display.setCursor(28, 2 + yOffset);
  display.print("Dino Game");

  char buf[12];
  sprintf(buf, dinoHigh >= 10000 ? "HI%d" : "HI %d", dinoHigh);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(127 - w - x1, 2 + yOffset);
  display.print(buf);
}

void drawDinoGameOver(int yOffset) {
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "GAME OVER", 15 + yOffset, 2);
  char buf[24];
  sprintf(buf, "High Score: %d", dinoHigh);
  drawCenteredText(display, buf, 31 + yOffset, 1);
  sprintf(buf, "Score: %d", dinoScore);
  drawCenteredText(display, buf, 40 + yOffset, 1);
  drawBoxedCenteredText(display, "EXIT", 6, 50 + yOffset, 44, 13, (dinoCursor == 0));
  drawBoxedCenteredText(display, "PLAY AGAIN", 56, 50 + yOffset, 66, 13, (dinoCursor == 1));
}

void drawDino(int yOffset) {
  bool menuState = (dinoState == DINO_PAUSED || dinoState == DINO_OVER);
  drawDinoHeader(yOffset, menuState && dinoCursor == -1);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (dinoState == DINO_OVER) { drawDinoGameOver(yOffset); return; }

  display.drawFastHLine(0, 62 + yOffset, 128, SSD1306_WHITE);
  int scroll = (int)dinoGroundScroll;
  for (int x = 0; x < 128; x++) {
    int p = (x + scroll) & 255;
    if (dinoBumps[p >> 3] & (1 << (p & 7))) display.drawPixel(x, 63 + yOffset, SSD1306_WHITE);
  }

  for (int i = 0; i < DINO_MAX_CACTI; i++) {
    if (!dinoCacti[i].active) continue;
    const DinoCactusDef &d = dinoCactusDefs[dinoCacti[i].type];
    display.drawBitmap((int)dinoCacti[i].x, DINO_FEET_Y - d.h + 1 + yOffset, d.bmp, d.w, d.h, SSD1306_WHITE);
  }

  const uint8_t *spr;
  if (dinoState == DINO_CRASHED) spr = dinoDead;
  else if (dinoState == DINO_READY) spr = ((millis() / 150) % 12 == 0) ? dinoBlink : dinoStand;
  else if (dinoY > 0.0f) spr = dinoStand;
  else spr = (((int)(dinoRunSec * 12.0f)) & 1) ? dinoRun2 : dinoRun1;
  // The apex reaches into the header bar, so draw row by row and skip rows above the header line.
  int dinoTop = DINO_FEET_Y - DINO_H + 1 - (int)(dinoY + 0.5f) + yOffset;
  const int bytesPerRow = (DINO_W + 7) / 8;
  for (int r = 0; r < DINO_H; r++) {
    if (dinoTop + r <= 12 + yOffset) continue;
    display.drawBitmap(DINO_X, dinoTop + r, spr + r * bytesPerRow, DINO_W, 1, SSD1306_WHITE);
  }

  if (dinoState == DINO_READY) {
    drawCenteredText(display, "Press UP to start", 22 + yOffset, 1);
    drawCenteredText(display, "CENTER: exit", 32 + yOffset, 1);
  } else {
    char buf[8];
    sprintf(buf, "%05d", dinoScore);
    display.setCursor(97, 15 + yOffset);
    display.print(buf);
  }

  if (dinoState == DINO_PAUSED) {
    display.fillRect(10, 16 + yOffset, 108, 44, SSD1306_BLACK);
    display.drawRect(10, 16 + yOffset, 108, 44, SSD1306_WHITE);
    drawCenteredText(display, "Game Paused", 21 + yOffset, 1);
    drawBoxedCenteredText(display, "RESUME", 16, 36 + yOffset, 44, 16, (dinoCursor == 0));
    drawBoxedCenteredText(display, "EXIT", 68, 36 + yOffset, 44, 16, (dinoCursor == 1));
  }
}

/* ---------------------------- WiFi app ---------------------------- */
// Ported from AIO_Transmitter's WiFi UI: same screen flow, keyboard grid, and
// focus-nav logic. Trimmed down to what was actually asked for here -- no
// boot-time auto-connect, no "Auto On" toggle, no connectivity pinger/forbidden-
// network list (those exist in AIO to support its own auto-reconnect-at-boot
// feature, which this watch deliberately does not have).

void drawWifiConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiConfirmCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "WiFi is OFF", 22 + yOffset, 1);
  drawCenteredText(display, "Turn it on?",  33 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiConfirmCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiConfirmCursor == 1));
}

void drawWifiScanning(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "SCANNING...", 30 + yOffset, 1);
  int dots = ((millis() - wifiScanStart) / 500) % 4;
  String dotStr = "";
  for (int i = 0; i < dots; i++) dotStr += ".";
  drawCenteredText(display, dotStr, 42 + yOffset, 1);
}

void drawWifiResults(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiListIndex == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  if (wifiScanResults <= 0) {
    drawCenteredText(display, "No networks found", 30 + yOffset, 1);
    display.setTextWrap(true);
    return;
  }

  const int visibleItems = 4;
  if (wifiListIndex > -1) {
    if (wifiListIndex < wifiListScroll) wifiListScroll = wifiListIndex;
    if (wifiListIndex >= wifiListScroll + visibleItems) wifiListScroll = wifiListIndex - visibleItems + 1;
  }

  for (int i = 0; i < visibleItems; i++) {
    int netIdx = wifiListScroll + i;
    if (netIdx >= wifiScanResults) break;
    int y = 16 + (i * 12) + yOffset;
    bool selected = (wifiListIndex == netIdx);
    if (selected) { display.fillRect(0, y - 1, 122, 12, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else          { display.setTextColor(SSD1306_WHITE); }
    display.setCursor(4, y);
    String ssid = WiFi.SSID(netIdx);
    if (ssid.length() == 0) ssid = "(hidden)";
    if (ssid.length() > 17) ssid = ssid.substring(0, 16) + "~";
    display.print(ssid);

    int rssi = WiFi.RSSI(netIdx);
    int bars = (rssi > -60) ? 3 : (rssi > -75) ? 2 : 1;
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(118, y);
    display.print(bars == 3 ? ":" : bars == 2 ? "." : ",");
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(true);

  if (wifiScanResults > visibleItems) {
    int trackH = 48;
    int sbH    = max(4, (visibleItems * trackH) / wifiScanResults);
    int sbY    = 16 + ((wifiListScroll * (trackH - sbH)) / (wifiScanResults - visibleItems));
    display.drawFastVLine(126, 16 + yOffset, trackH, SSD1306_WHITE);
    display.fillRect(125, sbY + yOffset, 3, sbH, SSD1306_WHITE);
  }
}

void drawWifiPassword(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiPasswordCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setCursor(4, 16 + yOffset);
  String ssid = wifiTargetSSID;
  if (ssid.length() > 18) ssid = ssid.substring(0, 17) + "~";
  display.print(ssid);
  display.setTextWrap(true);

  display.drawRect(4, 25 + yOffset, 120, 13, SSD1306_WHITE);
  display.setCursor(8, 28 + yOffset);
  String pw = wifiPassword;
  if (pw.length() > 15) pw = pw.substring(pw.length() - 15);
  if (pw.length() == 0) { display.setTextColor(0xAAAA); display.print("[tap to enter]"); }
  else display.print(pw);
  display.setTextColor(SSD1306_WHITE);

  if (wifiStatusMsg.length() > 0) {
    display.setCursor(4, 40 + yOffset);
    display.print(wifiStatusMsg);
  }

  drawBoxedCenteredText(display, "KEYBOARD", 4,  51 + yOffset, 54, 12, (wifiPasswordCursor == 0));
  drawBoxedCenteredText(display, "CONNECT",  62, 51 + yOffset, 60, 12, (wifiPasswordCursor == 1));
}

void drawWifiKeyboard() {
  display.setTextColor(SSD1306_WHITE);
  display.drawRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setCursor(4, 3);

  if (kbTextCursor < kbScrollOffset) kbScrollOffset = kbTextCursor;
  if (kbTextCursor > kbScrollOffset + 17) kbScrollOffset = kbTextCursor - 17;

  String disp = kbInputBuffer.substring(kbScrollOffset);
  if (disp.length() > 18) disp = disp.substring(0, 18);

  if (kbInputBuffer.length() == 0) {
    display.setTextColor(0x5555);
    if (kbReturnScreen == SCREEN_AI_CHAT) display.print("Type message...");
    else display.print("Type password...");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.print(disp);
  }

  if ((millis() / 500) % 2 == 0) {
    int cursorPx = 4 + ((kbTextCursor - kbScrollOffset) * 6);
    display.drawFastVLine(cursorPx, 2, 9, SSD1306_WHITE);
  }

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 11; c++) {
      const char* key;
      if (currentKBMode == KB_UPPER)   key = kbUpper[r][c];
      else if (currentKBMode == KB_SYMBOL) key = kbSymbol[r][c];
      else key = kbLower[r][c];

      int x = (c * 128) / 11;
      int w = ((c + 1) * 128) / 11 - x;
      int y = 13 + (r * 51) / 4;
      int h = (13 + ((r + 1) * 51) / 4) - y;

      drawBoxedCenteredText(display, key, x, y, w, h, (r == kbCursorRow && c == kbCursorCol));
    }
  }
}

void drawWifiHome(int yOffset) {
  drawHeader(yOffset, "WiFi", (wifiHomeCursor == -1));
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  display.setCursor(4, 16 + yOffset);
  display.print("Internet: "); display.print(internetOK ? "OK" : "OFFLINE");
  display.setCursor(4, 25 + yOffset);
  display.print("SSID: ");
  String ssid = WiFi.SSID();
  if (ssid.length() > 12) ssid = ssid.substring(0, 11) + "~";
  display.print(ssid);
  display.setTextWrap(true);

  const char* opts[3] = {"WiFi: ON", "Forget Network", "NTP Sync"};
  for (int i = 0; i < 3; i++) {
    int y = 36 + (i * 10) + yOffset;
    bool sel = (wifiHomeCursor == i);
    if (sel) { display.fillRect(0, y - 1, 128, 10, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else      { display.setTextColor(SSD1306_WHITE); }
    display.setCursor(4, y);
    display.print(opts[i]);
  }
  display.setTextColor(SSD1306_WHITE);
}

void drawWifiToggleConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "Turn off WiFi?", 22 + yOffset, 1);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiToggleCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiToggleCursor == 1));
}

void drawWifiForgetConfirm(int yOffset) {
  drawHeader(yOffset, "WiFi");
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  drawCenteredText(display, "Forget Network?", 18 + yOffset, 1);
  String ssid = wifiForgetTargetSSID;
  if (ssid.length() > 18) ssid = ssid.substring(0, 17) + "~";
  String quoted = "\"" + ssid + "\"";
  drawCenteredText(display, quoted, 30 + yOffset, 1);
  display.setTextWrap(true);
  drawBoxedCenteredText(display, "YES", 14, 48 + yOffset, 40, 13, (wifiForgetCursor == 0));
  drawBoxedCenteredText(display, "NO",  74, 48 + yOffset, 40, 13, (wifiForgetCursor == 1));
}

/* ---- WiFi credential storage (NVS via Preferences, same as AIO_Transmitter) ---- */

void loadCredentials() {
  prefs.begin("wifi_creds", true);
  savedNetCount = prefs.getInt("count", 0);
  for (int i = 0; i < savedNetCount && i < MAX_SAVED_NETS; i++) {
    savedSSIDs[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    savedPWDs[i]  = prefs.getString(("pwd"  + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveCredential(String ssid, String pwd) {
  lastConnectedSSID = ssid;
  for (int i = 0; i < savedNetCount; i++) {
    if (savedSSIDs[i] == ssid) { savedPWDs[i] = pwd; goto writeAll; }
  }
  if (savedNetCount < MAX_SAVED_NETS) {
    savedSSIDs[savedNetCount] = ssid;
    savedPWDs[savedNetCount]  = pwd;
    savedNetCount++;
  }
  writeAll:
  prefs.begin("wifi_creds", false);
  prefs.putInt("count", savedNetCount);
  for (int i = 0; i < savedNetCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), savedSSIDs[i].c_str());
    prefs.putString(("pwd"  + String(i)).c_str(), savedPWDs[i].c_str());
  }
  prefs.end();
}

void forgetCredential(String ssid) {
  int found = -1;
  for (int i = 0; i < savedNetCount; i++) { if (savedSSIDs[i] == ssid) { found = i; break; } }
  if (found == -1) return;
  for (int i = found; i < savedNetCount - 1; i++) {
    savedSSIDs[i] = savedSSIDs[i + 1];
    savedPWDs[i]  = savedPWDs[i + 1];
  }
  savedNetCount--;
  prefs.begin("wifi_creds", false);
  prefs.putInt("count", savedNetCount);
  for (int i = 0; i < savedNetCount; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), savedSSIDs[i].c_str());
    prefs.putString(("pwd"  + String(i)).c_str(), savedPWDs[i].c_str());
  }
  prefs.end();
}

String findSavedPwd(String ssid) {
  for (int i = 0; i < savedNetCount; i++) { if (savedSSIDs[i] == ssid) return savedPWDs[i]; }
  return "";
}

/* ---- NTP sync: stable (non-government) server, up to 3 attempts, then give up ---- */

// Kicks off a sync attempt (or an immediate "no WiFi" failure) and hands off
// to SCREEN_WIFI_NTP's non-blocking poll (in loop()) to actually finish it.
// One raw SNTP request/response over UDP (RFC 4330 client mode). Bypasses
// the OS-level SNTP client entirely -- each of the 4 tasks below owns its own
// UDP socket and server, fully independent of the others.
bool ntpQueryOnce(const char* host, uint32_t &outUnixTime, unsigned long timeoutMs) {
  WiFiUDP udp;
  if (!udp.begin(0)) return false;

  IPAddress serverIp;
  if (!WiFi.hostByName(host, serverIp)) { udp.stop(); return false; }

  byte packet[48];
  memset(packet, 0, 48);
  packet[0] = 0b11100011; // LI=3 (unsynced), VN=4, Mode=3 (client)

  udp.beginPacket(serverIp, 123);
  udp.write(packet, 48);
  udp.endPacket();

  unsigned long start = millis();
  int size = 0;
  while (millis() - start < timeoutMs) {
    size = udp.parsePacket();
    if (size >= 48) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (size < 48) { udp.stop(); return false; }

  udp.read(packet, 48);
  udp.stop();

  uint32_t secsSince1900 = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16) |
                            ((uint32_t)packet[42] << 8)  |  (uint32_t)packet[43];
  const uint32_t SEVENTY_YEARS = 2208988800UL; // 1900 -> 1970 epoch offset
  outUnixTime = secsSince1900 - SEVENTY_YEARS;
  return true;
}

void ntpQueryTask(void *pv) {
  int slot = (int)(intptr_t)pv;
  uint32_t epoch = 0;
  bool ok = ntpQueryOnce(NTP_SERVERS[slot], epoch, NTP_PER_SERVER_TIMEOUT_MS);
  if (ok) { ntpSlotEpoch[slot] = epoch; ntpSlotOk[slot] = true; }
  ntpSlotDone[slot] = true;
  vTaskDelete(NULL);
}

void startNtpSync() {
  wifiNtpState = 0;
  wifiNtpResultMsg = "";
  wifiNtpBestServer = -1;
  wifiNtpStart = millis();

  if (WiFi.status() != WL_CONNECTED) {
    wifiNtpState = 2;
    wifiNtpResultMsg = "Failed: Internet Issue";
    wifiNtpDoneAt = millis();
    return;
  }

  for (int i = 0; i < NUM_NTP_SERVERS; i++) {
    ntpSlotDone[i] = false;
    ntpSlotOk[i] = false;
    ntpSlotEpoch[i] = 0;
    xTaskCreate(ntpQueryTask, "NtpQ", 4096, (void*)(intptr_t)i, 1, NULL);
  }
}

void drawWifiNTP(int yOffset) {
  // Deliberately no "<--"/back box anywhere on this screen -- it's fully
  // automatic (syncs, shows the result, then returns to WiFi Home on its own).
  display.drawFastHLine(0, 12 + yOffset, 128, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "NTP", 2 + yOffset, 1);

  if (wifiNtpState == 0) {
    drawCenteredText(display, "Syncing", 30 + yOffset, 1);
    int dots = ((millis() - wifiNtpStart) / 500) % 4;
    String dotStr = "";
    for (int i = 0; i < dots; i++) dotStr += ".";
    drawCenteredText(display, dotStr, 42 + yOffset, 1);
  } else {
    drawCenteredText(display, wifiNtpResultMsg, 28 + yOffset, 1);
    if (wifiNtpState == 1 && wifiNtpBestServer >= 0) {
      drawCenteredText(display, NTP_SERVERS[wifiNtpBestServer], 40 + yOffset, 1);
    }
  }
}

// The SSD1306 font is a 7-bit glyph table, so any UTF-8 the model emits (it
// likes non-breaking hyphens and curly quotes) would render as garbage. Fold
// the common punctuation down to ASCII and drop anything else non-Latin.
String aiToAscii(const String &in) {
  String out;
  out.reserve(in.length());

  unsigned int i = 0;
  while (i < in.length()) {
    uint8_t c = (uint8_t)in[i];

    if (c < 0x80) {
      if (c == '\n' || c == '\r' || c == '\t') out += ' ';
      else if (c >= 32 && c < 127)             out += (char)c;
      i++;
      continue;
    }

    uint32_t cp = 0; int len = 1;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    for (int k = 1; k < len && (i + k) < in.length(); k++) cp = (cp << 6) | ((uint8_t)in[i + k] & 0x3F);

    if      (cp == 0x2018 || cp == 0x2019 || cp == 0x2032) out += '\'';
    else if (cp == 0x201C || cp == 0x201D)                 out += '"';
    else if (cp >= 0x2010 && cp <= 0x2015)                 out += '-';
    else if (cp == 0x00A0 || cp == 0x2007 || cp == 0x202F) out += ' ';
    else if (cp == 0x2026)                                 out += "...";
    // anything else (emoji, CJK, symbols) has no glyph -- drop it

    i += len;
  }

  out.trim();
  return out;
}

void aiAppendMsg(bool fromUser, const String &text) {
  if (aiMsgCount >= AI_MAX_MSGS) {
    for (int i = 1; i < AI_MAX_MSGS; i++) aiMsgs[i - 1] = aiMsgs[i];
    aiMsgCount = AI_MAX_MSGS - 1;
  }
  aiMsgs[aiMsgCount].fromUser = fromUser;
  aiMsgs[aiMsgCount].text     = text;
  aiMsgCount++;

  // Jump to the newest line, matching how every chat app behaves.
  aiScroll = max(0, aiRenderChat(0, false) - AI_VIEW_H);
}

String aiCallGroq(const String &body) {
  if (WiFi.status() != WL_CONNECTED) return "[No WiFi]";

  WiFiClientSecure client;
  client.setInsecure();          // no cert bundle on device; TLS without pinning
  client.setTimeout(20000);

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(20000);
  if (!http.begin(client, "https://api.groq.com/openai/v1/chat/completions")) return "[Connect failed]";

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + GROQ_API_KEY);

  int code = http.POST(body);
  String out;

  if (code == 200) {
    JsonDocument filter;
    filter["choices"][0]["message"]["content"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    if (err) {
      out = "[Bad response]";
    } else {
      const char* c = doc["choices"][0]["message"]["content"];
      out = (c && *c) ? String(c) : String("[Empty reply]");
    }
  } else if (code > 0) {
    String errBody = http.getString();
    Serial.printf("[AI] HTTP %d error body: %s\n", code, errBody.c_str());

    // Groq returns a JSON body like {"error":{"message":"...","code":"..."}}
    // on failure -- pull that out for a useful chat reply instead of a bare
    // status code (e.g. "model_decommissioned" tells you exactly what's wrong).
    JsonDocument errDoc;
    DeserializationError errErr = deserializeJson(errDoc, errBody);
    if (!errErr && errDoc["error"]["message"].is<const char*>()) {
      out = "[HTTP " + String(code) + "] " + String((const char*)errDoc["error"]["message"]);
    } else {
      out = "[HTTP " + String(code) + "] " + errBody;
    }
  } else {
    out = "[Network error]";
  }

  http.end();
  return aiToAscii(out);
}

void aiChatTask(void *p) {
  for (;;) {
    if (aiRequestPending) {
      aiRequestPending = false;
      String reply = aiCallGroq(aiPendingBody);
      aiPendingBody = "";
      aiReplyText   = reply;
      aiReplyReady  = true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void aiStartTask() {
  if (aiTaskStarted) return;
  aiTaskStarted = true;
  // TLS handshake is stack-hungry -- 4096 (what the pinger uses for plain
  // HTTP) overflows here.
  xTaskCreate(aiChatTask, "AiChat", 12288, NULL, 1, NULL);
}

void aiSendMessage() {
  if (aiBusy || aiInputText.length() == 0) return;

  aiAppendMsg(true, aiInputText);
  aiInputText = "";

  // Body is built here, on the UI thread, so the network task never touches
  // aiMsgs -- that keeps the transcript single-writer and mutex-free.
  JsonDocument doc;
  doc["model"]       = GROQ_MODEL;
  doc["max_tokens"]  = 200;
  doc["temperature"] = 0.7;

  JsonArray msgs = doc["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"]    = "system";
  sys["content"] = "You are a helpful assistant answering on a 128x64 pixel OLED "
                   "handheld. Keep every reply under 40 words. Use plain ASCII only: "
                   "no markdown, no emoji, no curly quotes or long dashes.";

  int start = max(0, aiMsgCount - AI_CTX_MSGS);
  for (int i = start; i < aiMsgCount; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["role"]    = aiMsgs[i].fromUser ? "user" : "assistant";
    m["content"] = aiMsgs[i].text;
  }

  aiPendingBody = "";
  serializeJson(doc, aiPendingBody);

  aiBusy = true;
  aiStartTask();
  aiRequestPending = true;
  aiFocus = 1;
}

void drawAiNoWifi(int yOffset) {
  drawHeader(yOffset, "AI Chatbot", false);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(display, "WiFi is Off!", 24 + yOffset, 1);
  drawBoxedCenteredText(display, "OK", 44, 40 + yOffset, 40, 16, true);
}

// Lays out the whole transcript as wrapped "User: ..." / "AI: ..." lines.
// Returns total pixel height; with draw=false it's a pure measure pass, which
// is how both the auto-scroll target and the scroll clamp get computed.
int aiRenderChat(int baseY, bool draw, int yOffset) {
  int line = 0;
  const int clipTop = 13 + yOffset;
  const int clipBot = 49 + yOffset;

  for (int m = 0; m < aiMsgCount; m++) {
    String s = (aiMsgs[m].fromUser ? "User: " : "AI: ") + aiMsgs[m].text;

    int i = 0, L = s.length();
    while (i < L) {
      int take = min((int)AI_WRAP_CHARS, L - i);

      if (take == AI_WRAP_CHARS && (i + take) < L) {
        int brk = -1;
        for (int k = take - 1; k > 0; k--) {
          if (s[i + k] == ' ') { brk = k; break; }
        }
        if (brk > 0) take = brk;
      }

      if (draw) {
        int y = baseY + line * AI_LINE_H;
        if (y >= clipTop && (y + 7) <= clipBot) {
          display.setCursor(2, y);
          display.print(s.substring(i, i + take));
        }
      }
      i += take;
      while (i < L && s[i] == ' ') i++;
      line++;
    }
  }

  if (aiBusy) {
    if (draw) {
      int y = baseY + line * AI_LINE_H;
      if (y >= clipTop && (y + 7) <= clipBot) {
        display.setCursor(2, y);
        display.print("AI: thinking...");
      }
    }
    line++;
  }

  return line * AI_LINE_H;
}

void drawAiChat(int yOffset) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  int contentH  = aiRenderChat(0, false);
  int maxScroll = max(0, contentH - AI_VIEW_H);
  aiScroll = constrain(aiScroll, 0, maxScroll);

  if (aiMsgCount == 0 && !aiBusy) {
    display.setCursor(2, 23 + yOffset);
    display.print("Ask me anything.");
  } else {
    aiRenderChat(AI_VIEW_TOP + yOffset - aiScroll, true, yOffset);
  }

  if (maxScroll > 0) {
    int trackH = AI_VIEW_H;
    int thumbH = max(4, (trackH * AI_VIEW_H) / contentH);
    int thumbY = 13 + yOffset + ((trackH - thumbH) * aiScroll) / maxScroll;
    display.drawFastVLine(126, 13 + yOffset, trackH, SSD1306_WHITE);
    display.fillRect(125, thumbY, 3, thumbH, SSD1306_WHITE);
  }

  bool inputSel = (aiFocus == 1);
  if (inputSel) { display.fillRect(0, 49 + yOffset, 94, 14, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
  else          { display.drawRect(0, 49 + yOffset, 94, 14, SSD1306_WHITE); display.setTextColor(SSD1306_WHITE); }

  String shown = aiInputText;
  if (shown.length() == 0) shown = "Type msg...";
  if (shown.length() > 15) shown = shown.substring(shown.length() - 15);
  display.setCursor(3, 53 + yOffset);
  display.print(shown);
  display.setTextColor(SSD1306_WHITE);

  drawBoxedCenteredText(display, "SEND", 96, 49 + yOffset, 32, 14, (aiFocus == 2));

  drawHeader(yOffset, "AI Chatbot", (aiFocus == -1));

  if (aiFocus == 0) {
    display.drawRect(0, 12 + yOffset, 128, AI_VIEW_H + 1, SSD1306_WHITE);
    if (aiScrollMode) display.drawRect(1, 13 + yOffset, 126, AI_VIEW_H - 1, SSD1306_WHITE);
  }
  display.setTextWrap(true);
}

void connectivityTask(void *p) {
  const char* urls[] = {
    "http://www.gstatic.com/generate_204",
    "http://clients3.google.com/generate_204",
    "http://cp.cloudflare.com/generate_204",
    "http://edge-http.microsoft.com/captiveportal/generate_204",
    "http://connect.rom.miui.com/generate_204"
  };
  int urlIndex = 0;
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient h;
      h.setConnectTimeout(1200);
      h.setTimeout(1800);
      h.begin(urls[urlIndex]);
      ok = (h.GET() == 204);
      h.end();
    }

    pingerStatus[urlIndex] = ok;
    urlIndex = (urlIndex + 1) % 5;

    internetOK = pingerStatus[0] || pingerStatus[1] || pingerStatus[2] ||
                 pingerStatus[3] || pingerStatus[4];

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startConnectivityPinger() {
  if (pingerStarted) return;
  pingerStarted = true;
  xTaskCreate(connectivityTask, "NetPing", 4096, NULL, 1, NULL);
}

void drawWifiStatusIcon(int x, int y) {
  display.drawBitmap(x, y, wifi_bmp, 8, 8, SSD1306_WHITE);
}

void drawTowerStatusIcon(int x, int y) {
  display.drawBitmap(x, y, tower_bmp, 8, 8, SSD1306_WHITE);
}

void enterDeepSleep() {
  if (displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayOn = false;
  }

  // WiFi is only ever on because the user explicitly turned it on, and it
  // never survives deep sleep anyway (the whole chip powers down) -- force it
  // off cleanly rather than leaving the radio in an undefined state. It stays
  // off on wake too; nothing here re-enables it automatically.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // CENTER is active-low (pressed = LOW), and it's the only button in the
  // LP domain -- see the DEEP_SLEEP_TIMEOUT_MS comment for why.
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_CENTER, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start(); // never returns -- wake re-runs setup() from scratch
}

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

  xTaskCreate(batteryAdcTask, "BatteryADC", 2048, NULL, 1, NULL);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(I2C_CLOCK_DISPLAY); // 1MHz -- gives ~2.6x headroom over the 40 FPS OLED requirement
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
  // missing. DS3231 tops out at 400kHz, so the bus is dropped down for its
  // transactions and restored to the display speed right after.
  Wire.setClock(I2C_CLOCK_RTC);
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
  Wire.setClock(I2C_CLOCK_DISPLAY);

  Serial.println(F("Display initialized!"));

  loadSettings();
  rwLoadFromFile();
  loadCredentials(); // saved WiFi SSID/password pairs -- WiFi itself stays off until asked
  loadBatterySettings(); // watchface indicator mode (icon vs. %), survives deep sleep
  buildMenu();

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

      // Every wake -- from idle blanking or deep sleep alike -- always lands
      // on the watchface, except during configured night hours, when it
      // shows the Good Night greeting instead. A real Restwise sync still
      // finds its way back to the Restwise screen on its own the moment
      // traffic arrives (that handshake navigates there regardless of
      // currentScreen), so this doesn't drop an in-progress sync -- it just
      // stops resuming whatever screen you happened to be sitting on before.
      currentScreen = isNightNow() ? SCREEN_GOODNIGHT : SCREEN_WATCHFACE;

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
  bool dinoRunning = (currentScreen == SCREEN_DINO && dinoState == DINO_PLAYING);
  bool allowBlank = !isAnimating && currentScreen != SCREEN_ANIMATOR && !dinoRunning &&
                     currentScreen != SCREEN_WIFI_SCANNING &&
                     currentScreen != SCREEN_WIFI_PASSWORD &&
                     currentScreen != SCREEN_WIFI_NTP &&
                     !aiBusy;

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

  // Deep sleep after DEEP_SLEEP_TIMEOUT_MS of true inactivity -- but never
  // while something active is running that a full reset would silently kill:
  // a live Stopwatch/Timer, a mid-transfer Restwise sync, an in-progress
  // slide, the Animator (a passive but deliberately continuous screen), a
  // WiFi scan/password-entry/connect/NTP-sync in flight, or a pending AI
  // reply. Otherwise WiFi is free to keep running through an ordinary
  // display-off idle blank -- only deep sleep forces it off.
  bool allowDeepSleep = !isAnimating && currentScreen != SCREEN_ANIMATOR && !dinoRunning &&
                        !swRunning && tmMode != TM_RUNNING &&
                        rwSyncState != RWS_RECV &&
                        currentScreen != SCREEN_WIFI_SCANNING &&
                        currentScreen != SCREEN_WIFI_PASSWORD &&
                        currentScreen != SCREEN_WIFI_NTP &&
                        !aiBusy;
  if (allowDeepSleep && millis() - lastActivityTime > DEEP_SLEEP_TIMEOUT_MS) {
    enterDeepSleep();
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
          menuIndex = (menuIndex - 1 + menuCount) % menuCount;
        }
        if (buttonJustPressed[1]) {
          menuIndex = (menuIndex + 1) % menuCount;
        }
        if (buttonJustPressed[4]) {
          int app = menuApps[menuIndex];
          if (app == APP_RESTWISE) {
            rwCursor = -1;
            startAnimation(SCREEN_RESTWISE, -64);
          } else if (app == APP_STOPWATCH) {
            swFocus = 1;
            startAnimation(SCREEN_STOPWATCH, -64);
          } else if (app == APP_TIMER) {
            tmMode = TM_SETTING;
            tmFocus = 1;
            startAnimation(SCREEN_TIMER, -64);
          } else if (app == APP_CALCULATOR) {
            calcCursorRow = 0; calcCursorCol = 0;
            calcInput1 = ""; calcInput2 = ""; calcOp = ' '; calcIsOpSet = false; calcError = false;
            startAnimation(SCREEN_CALCULATOR, -64);
          } else if (app == APP_ANIMATOR) {
            animatorSceneIndex = 0;
            animatorSceneStart = millis();
            startAnimation(SCREEN_ANIMATOR, -64);
          } else if (app == APP_BATTERY) {
            battCursor = -1;
            startAnimation(SCREEN_BATTERY, -64);
          } else if (app == APP_WIFI) {
            if (WiFi.status() == WL_CONNECTED) {
              wifiHomeCursor = 0;
              startAnimation(SCREEN_WIFI_HOME, -64);
            } else if (wifiEnabled) {
              WiFi.scanDelete(); WiFi.disconnect();
              delay(100);
              WiFi.scanNetworks(true, false, false, 250);
              wifiScanStart = millis();
              wifiScanResults = -1;
              startAnimation(SCREEN_WIFI_SCANNING, -64);
            } else {
              wifiConfirmCursor = 0;
              startAnimation(SCREEN_WIFI_CONFIRM, -64);
            }
          } else if (app == APP_AI) {
            // Stricter than AIO_Transmitter's own gate (WiFi-connected only) --
            // this one requires real internet reachability too, not just AP
            // association, per explicit request.
            if (WiFi.status() == WL_CONNECTED && internetOK) {
              aiFocus = 1; aiScrollMode = false;
              startAnimation(SCREEN_AI_CHAT, -64);
            } else {
              startAnimation(SCREEN_AI_NOWIFI, -64);
            }
          } else if (app == APP_DINO) {
            dinoOpen();
            startAnimation(SCREEN_DINO, -64);
          } else if (app == APP_SECURITY) {
            if (pinSet) {
              pendingAction = ACT_OPEN_MENU;
              enterPinScreen(SEC_VERIFY, SCREEN_MENU);
            } else {
              confirmType = CONF_ENABLE;
              confirmSelection = 0;
              startAnimation(SCREEN_SECURITY_CONFIRM, -64);
            }
          } else if (app == APP_LOCKSCREEN) {
            lockSettingsCursor = -1;
            for (int i = 0; i < 4; i++) if (lockTimeoutOptions[i] == displayTimeoutSec) lockSettingsCursor = i;
            if (lockSettingsCursor == -1) lockSettingsCursor = 0;
            startAnimation(SCREEN_LOCK_SETTINGS, -64);
          } else if (app == APP_TERMINAL) {
            termState = TERM_IDLE;
            termLineBuf = "";
            startAnimation(SCREEN_TERMINAL, -64);
          } else if (app == APP_LOCK) {
            startAnimation(SCREEN_WATCHFACE, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_BATTERY) {
        if (buttonJustPressed[0]) { if (battCursor > -1) battCursor--; }
        if (buttonJustPressed[1]) { if (battCursor < 0) battCursor++; }
        if (buttonJustPressed[4]) {
          if (battCursor == -1) {
            startAnimation(SCREEN_MENU, 64);
          } else {
            battIndCursor = -1;
            startAnimation(SCREEN_BATTERY_INDICATOR, -64);
          }
        }
      }
      else if (currentScreen == SCREEN_BATTERY_INDICATOR) {
        if (buttonJustPressed[0]) battIndCursor = -1;
        if (buttonJustPressed[1] && battIndCursor == -1) battIndCursor = 0;
        if (buttonJustPressed[3]) battIndCursor = 1;
        if (buttonJustPressed[2] && battIndCursor == 1) battIndCursor = 0;
        if (buttonJustPressed[4]) {
          if (battIndCursor == -1) {
            battCursor = 0;
            startAnimation(SCREEN_BATTERY, 64);
          } else {
            battShowPercent = (battIndCursor == 1);
            saveBatterySettings();
            battCursor = 0;
            startAnimation(SCREEN_BATTERY, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_DINO) {
        dinoHandleButtons();
      }
      else if (currentScreen == SCREEN_WIFI_CONFIRM) {
        if (buttonJustPressed[0]) wifiConfirmCursor = -1;
        if (buttonJustPressed[1] && wifiConfirmCursor == -1) wifiConfirmCursor = 0;
        if (buttonJustPressed[3]) wifiConfirmCursor = 1;
        if (buttonJustPressed[2] && wifiConfirmCursor == 1) wifiConfirmCursor = 0;
        if (buttonJustPressed[4]) {
          if (wifiConfirmCursor == 0) {
            if (WIFI_APP_ENABLED) {
              wifiEnabled = true; wifiStatusMsg = ""; wifiRetryCount = 0;
              WiFi.mode(WIFI_STA); WiFi.scanDelete();
              WiFi.disconnect(); delay(100);
              WiFi.scanNetworks(true);
              wifiScanStart = millis(); wifiScanResults = -1;
              startAnimation(SCREEN_WIFI_SCANNING, -64);
            }
          } else {
            startAnimation(SCREEN_MENU, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_RESULTS) {
        if (buttonJustPressed[0]) { if (wifiListIndex > -1) wifiListIndex--; }
        if (buttonJustPressed[1]) { if (wifiListIndex < wifiScanResults - 1) wifiListIndex++; }
        if (buttonJustPressed[2]) { if (wifiListIndex == -1) wifiListIndex = 0; }
        if (buttonJustPressed[4]) {
          if (wifiListIndex == -1) { WiFi.disconnect(); startAnimation(SCREEN_MENU, 64); }
          else {
            wifiTargetSSID  = WiFi.SSID(wifiListIndex);
            wifiPassword    = findSavedPwd(wifiTargetSSID); // autofill from a previous connection
            kbInputBuffer   = wifiPassword;
            wifiStatusMsg   = "";
            wifiRetryCount  = 0;
            wifiPasswordCursor = -1;
            startAnimation(SCREEN_WIFI_PASSWORD, -64);
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_PASSWORD) {
        if (buttonJustPressed[0]) {
          if (wifiPasswordCursor == 0 || wifiPasswordCursor == 1) wifiPasswordCursor = -1;
        }
        if (buttonJustPressed[1]) { if (wifiPasswordCursor == -1) wifiPasswordCursor = 0; }
        if (buttonJustPressed[2]) { if (wifiPasswordCursor == 1) wifiPasswordCursor = 0; }
        if (buttonJustPressed[3]) { if (wifiPasswordCursor == 0) wifiPasswordCursor = 1; }
        if (buttonJustPressed[4]) {
          if (wifiPasswordCursor == -1) { WiFi.disconnect(); startAnimation(SCREEN_WIFI_RESULTS, 64); }
          else if (wifiPasswordCursor == 0) {
            kbInputBuffer = wifiPassword; currentKBMode = KB_LOWER;
            kbCursorRow = 0; kbCursorCol = 0;
            kbTextCursor   = kbInputBuffer.length();
            kbScrollOffset = 0;
            kbReturnScreen = SCREEN_WIFI_PASSWORD;
            currentScreen  = SCREEN_WIFI_KEYBOARD;
          } else if (wifiPasswordCursor == 1) {
            WiFi.begin(wifiTargetSSID.c_str(), wifiPassword.c_str());
            wifiConnecting = true;
            wifiConnectStartTime = millis();
            wifiStatusMsg = "Connecting...";
          }
        }

        if (wifiConnecting) {
          lastActivityTime = millis();
          if (WiFi.status() == WL_CONNECTED) {
            wifiConnecting = false; wifiRetryCount = 0;
            saveCredential(wifiTargetSSID, wifiPassword);
            lastConnectedSSID = wifiTargetSSID;
            startConnectivityPinger(); // internet-reachability icon; NTP is now manual-only (WiFi Home -> NTP Sync)
            wifiHomeCursor = 0;
            startAnimation(SCREEN_WIFI_HOME, -64);
          } else if (WiFi.status() == WL_CONNECT_FAILED) {
            wifiConnecting = false; wifiRetryCount++;
            WiFi.disconnect();
            wifiStatusMsg = (wifiRetryCount >= 5) ? "Check signal/pass" : "Failed! Wrong PW?";
          } else if (millis() - wifiConnectStartTime > 15000) {
            wifiConnecting = false; wifiRetryCount++;
            WiFi.disconnect();
            wifiStatusMsg = (wifiRetryCount >= 5) ? "Check Router dist." : "Try Again...";
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_KEYBOARD) {
        if (buttonJustPressed[0]) { if (kbCursorRow > 0) kbCursorRow--; }
        if (buttonJustPressed[1]) { if (kbCursorRow < 3) kbCursorRow++; }
        if (buttonJustPressed[2]) { kbCursorCol = (kbCursorCol > 0) ? kbCursorCol - 1 : 10; }
        if (buttonJustPressed[3]) { kbCursorCol = (kbCursorCol < 10) ? kbCursorCol + 1 : 0; }
        if (buttonJustPressed[4]) {
          const char* key;
          if (currentKBMode == KB_UPPER) key = kbUpper[kbCursorRow][kbCursorCol];
          else if (currentKBMode == KB_SYMBOL) key = kbSymbol[kbCursorRow][kbCursorCol];
          else key = kbLower[kbCursorRow][kbCursorCol];

          if (strcmp(key, "EN") == 0) {
            // Enter parks the text in the input box and puts the cursor on
            // SEND for AI Chat (so posting it is one more press, not
            // accidental), vs. committing straight into the password field.
            if (kbReturnScreen == SCREEN_AI_CHAT) {
              aiInputText = kbInputBuffer;
              aiFocus = (aiInputText.length() > 0) ? 2 : 1;
            } else {
              wifiPassword = kbInputBuffer;
            }
            currentScreen = kbReturnScreen;
          } else if (strcmp(key, "CL") == 0) {
            if (kbTextCursor > 0) kbTextCursor--;
          } else if (strcmp(key, "CR") == 0) {
            if (kbTextCursor < (int)kbInputBuffer.length()) kbTextCursor++;
          } else if (strcmp(key, "<") == 0 && currentKBMode != KB_SYMBOL) {
            if (kbTextCursor > 0) kbTextCursor--;
          } else if (strcmp(key, ">") == 0 && currentKBMode != KB_SYMBOL) {
            if (kbTextCursor < (int)kbInputBuffer.length()) kbTextCursor++;
          } else if (strcmp(key, "BS") == 0) {
            if (kbTextCursor > 0 && kbInputBuffer.length() > 0) {
              kbInputBuffer.remove(kbTextCursor - 1, 1);
              kbTextCursor--;
            }
          } else if (strcmp(key, "^") == 0) {
            if (currentKBMode == KB_LOWER) currentKBMode = KB_UPPER;
            else if (currentKBMode == KB_UPPER) currentKBMode = KB_SYMBOL;
            else currentKBMode = KB_LOWER;
          } else {
            kbInputBuffer = kbInputBuffer.substring(0, kbTextCursor) + String(key) + kbInputBuffer.substring(kbTextCursor);
            kbTextCursor += String(key).length();
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_HOME) {
        if (buttonJustPressed[0]) { if (wifiHomeCursor > -1) wifiHomeCursor--; }
        if (buttonJustPressed[1]) { if (wifiHomeCursor < 2) wifiHomeCursor++; }
        if (buttonJustPressed[2]) { if (wifiHomeCursor == -1) wifiHomeCursor = 0; }
        if (buttonJustPressed[4]) {
          if (wifiHomeCursor == -1) startAnimation(SCREEN_MENU, 64);
          else if (wifiHomeCursor == 0) { wifiToggleCursor = 1; startAnimation(SCREEN_WIFI_TOGGLE_CONFIRM, -64); }
          else if (wifiHomeCursor == 1) {
            wifiForgetTargetSSID = WiFi.SSID();
            wifiForgetCursor = 1;
            startAnimation(SCREEN_WIFI_FORGET_CONFIRM, -64);
          }
          else if (wifiHomeCursor == 2) {
            startNtpSync();
            startAnimation(SCREEN_WIFI_NTP, -64);
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_TOGGLE_CONFIRM) {
        if (buttonJustPressed[2]) wifiToggleCursor = 0;
        if (buttonJustPressed[3]) wifiToggleCursor = 1;
        if (buttonJustPressed[4]) {
          if (wifiToggleCursor == 0) {
            wifiEnabled = false;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            startAnimation(SCREEN_MENU, 64);
          } else {
            startAnimation(SCREEN_WIFI_HOME, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_WIFI_FORGET_CONFIRM) {
        if (buttonJustPressed[2]) wifiForgetCursor = 0;
        if (buttonJustPressed[3]) wifiForgetCursor = 1;
        if (buttonJustPressed[4]) {
          if (wifiForgetCursor == 0) {
            forgetCredential(wifiForgetTargetSSID);
            if (lastConnectedSSID == wifiForgetTargetSSID) lastConnectedSSID = "";
            WiFi.disconnect();
            startAnimation(SCREEN_MENU, 64);
          } else {
            startAnimation(SCREEN_WIFI_HOME, 64);
          }
        }
      }
      else if (currentScreen == SCREEN_AI_NOWIFI) {
        if (buttonJustPressed[2] || buttonJustPressed[4]) startAnimation(SCREEN_MENU, 64);
      }
      else if (currentScreen == SCREEN_AI_CHAT) {
        // Scroll mode swallows Up/Down for the transcript; center hands control back.
        if (aiScrollMode) {
          if (buttonJustPressed[0]) aiScroll -= AI_LINE_H;
          if (buttonJustPressed[1]) aiScroll += AI_LINE_H;
          if (aiScroll < 0) aiScroll = 0; // lower clamp needs render height, drawAiChat does it
          if (buttonJustPressed[4]) aiScrollMode = false;
        } else {
          if (buttonJustPressed[0]) { if (aiFocus > -1) aiFocus--; }
          if (buttonJustPressed[1]) { if (aiFocus <  2) aiFocus++; }
          // Input and SEND sit side by side, so Left/Right moves between them.
          if (buttonJustPressed[2]) { if (aiFocus == 2) aiFocus = 1; else if (aiFocus == 1) aiFocus = -1; }
          if (buttonJustPressed[3]) { if (aiFocus == 1) aiFocus = 2; }

          if (buttonJustPressed[4]) {
            if (aiFocus == -1) {
              startAnimation(SCREEN_MENU, 64);
            } else if (aiFocus == 0) {
              aiScrollMode = true;
            } else if (aiFocus == 1) {
              kbInputBuffer  = aiInputText;
              kbTextCursor   = kbInputBuffer.length();
              kbScrollOffset = 0;
              currentKBMode  = KB_LOWER;
              kbCursorRow = 0; kbCursorCol = 0;
              kbReturnScreen = SCREEN_AI_CHAT;
              currentScreen  = SCREEN_WIFI_KEYBOARD;
            } else if (aiFocus == 2) {
              aiSendMessage();
            }
          }
        }
      }
      else if (currentScreen == SCREEN_ANIMATOR) {
        // Passive playback screen -- the back box is always focused, CENTER exits.
        if (buttonJustPressed[4]) {
          startAnimation(SCREEN_MENU, 64);
        }
        if (buttonJustPressed[3] && animatorFps < 60) animatorFps += 10;
        if (buttonJustPressed[2] && animatorFps > 10) animatorFps -= 10;
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

    if (currentScreen == SCREEN_DINO && !isAnimating) dinoTick();

    if (pinErrorMsg.length() > 0 && millis() - pinErrorShownAt > 1500) pinErrorMsg = "";

    static unsigned long lastStatusUpdate = 0;
    if (millis() - lastStatusUpdate > 100) {
      statusIndex++;
      lastStatusUpdate = millis();
    }

    // Cap the actual I2C push to the OLED at 60 FPS (~17ms). Button reads and
    // animation stepping above still run every loop tick for responsiveness --
    // only the expensive full-frame display.display() transfer is throttled.
    // At the 1MHz bus speed a 60 FPS full-frame push needs ~561.6kbps, leaving
    // ~1.78x headroom -- still comfortable, just less margin than 40 FPS had.
    static unsigned long lastFrameTime = 0;
    const unsigned long FRAME_INTERVAL_MS = 17; // ~1000/60
    // The Animator lets the user pick its own cap. Animation is time-based,
    // so a lower FPS plays at the same speed, just choppier.
    unsigned long frameInterval = FRAME_INTERVAL_MS;
    if (currentScreen == SCREEN_ANIMATOR && !isAnimating) {
      frameInterval = (1000UL + animatorFps / 2) / animatorFps;
    }
    if (millis() - lastFrameTime >= frameInterval) {
      lastFrameTime = millis();
      updateDisplay();
    }
  }

  // ---- WiFi scan polling (async scanNetworks(), same pattern as AIO_Transmitter) ----
  if (currentScreen == SCREEN_WIFI_SCANNING && !isAnimating) {
    lastActivityTime = millis();
    int result = WiFi.scanComplete();
    static bool scanRetried = false;
    if (result >= 0) {
      wifiScanResults = result;
      wifiListIndex = 0; wifiListScroll = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
    } else if (result == WIFI_SCAN_FAILED) {
      if (!scanRetried) {
        scanRetried = true;
        WiFi.scanNetworks(true, false, false, 80);
      } else {
        wifiScanResults = 0;
        startAnimation(SCREEN_WIFI_RESULTS, -64);
        scanRetried = false;
      }
    } else if (millis() - wifiScanStart > 8000) {
      WiFi.scanDelete();
      wifiScanResults = 0;
      startAnimation(SCREEN_WIFI_RESULTS, -64);
      scanRetried = false;
    }
  }

  // Drain a finished AI reply on the UI thread -- the network task only ever
  // hands back a string, the transcript itself is written from here alone.
  if (aiReplyReady) {
    aiReplyReady = false;
    aiBusy       = false;
    aiAppendMsg(false, aiReplyText.length() ? aiReplyText : String("[No reply]"));
    aiReplyText  = "";
    lastActivityTime = millis(); // a reply counts as activity, don't sleep on it
  }

  // ---- NTP sync polling -- non-blocking. The 4 query tasks run independently
  // in the background; this just checks in on them each loop tick. Waits for
  // either all 4 to finish or the 20s budget to expire, then picks the
  // highest-priority (lowest index) server among whichever succeeded -- this
  // is what makes "if more than one returns, higher priority wins" correct,
  // rather than just racing to whichever task happens to finish first. ----
  if (currentScreen == SCREEN_WIFI_NTP && !isAnimating) {
    lastActivityTime = millis();
    if (wifiNtpState == 0) {
      bool allDone = ntpSlotDone[0] && ntpSlotDone[1] && ntpSlotDone[2] && ntpSlotDone[3];
      bool timedOut = millis() - wifiNtpStart > NTP_TIMEOUT_MS;
      if (allDone || timedOut) {
        int best = -1;
        for (int i = 0; i < NUM_NTP_SERVERS; i++) {
          if (ntpSlotOk[i]) { best = i; break; } // index 0 = highest priority
        }
        wifiNtpBestServer = best;
        if (best >= 0) {
          time_t t = (time_t)(ntpSlotEpoch[best] + NTP_GMT_OFFSET_SEC);
          struct tm ti;
          gmtime_r(&t, &ti);
          DateTime ntpTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
          setDeviceTime(ntpTime);
          char buf[32];
          sprintf(buf, "Synced to %02d:%02d", ti.tm_hour, ti.tm_min);
          wifiNtpResultMsg = String(buf);
          wifiNtpState = 1;
        } else {
          wifiNtpResultMsg = "Failed: Timeout Error";
          wifiNtpState = 2;
        }
        wifiNtpDoneAt = millis();
      }
    } else if (millis() - wifiNtpDoneAt > NTP_RESULT_HOLD_MS) {
      startAnimation(SCREEN_WIFI_HOME, 64);
    }
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
  if (rtcPresent) {
    Wire.setClock(I2C_CLOCK_RTC);
    DateTime t = rtc.now();
    Wire.setClock(I2C_CLOCK_DISPLAY);
    return t;
  }
  // Software clock: base time plus however long we've been running. Unsigned
  // subtraction keeps this correct across the millis() rollover.
  unsigned long elapsed = (millis() - softBaseMillis) / 1000UL;
  return softBase + TimeSpan((int32_t)elapsed);
}

void setDeviceTime(const DateTime &dt) {
  if (rtcPresent) {
    Wire.setClock(I2C_CLOCK_RTC);
    rtc.adjust(dt);
    Wire.setClock(I2C_CLOCK_DISPLAY);
  }
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
  else if (screen == SCREEN_ANIMATOR) drawAnimator(yOffset);
  else if (screen == SCREEN_BATTERY) drawBattery(yOffset);
  else if (screen == SCREEN_BATTERY_INDICATOR) drawBatteryIndicator(yOffset);
  else if (screen == SCREEN_DINO) drawDino(yOffset);
  else if (screen == SCREEN_WIFI_CONFIRM) drawWifiConfirm(yOffset);
  else if (screen == SCREEN_WIFI_SCANNING) drawWifiScanning(yOffset);
  else if (screen == SCREEN_WIFI_RESULTS) drawWifiResults(yOffset);
  else if (screen == SCREEN_WIFI_PASSWORD) drawWifiPassword(yOffset);
  else if (screen == SCREEN_WIFI_KEYBOARD) drawWifiKeyboard();
  else if (screen == SCREEN_WIFI_HOME) drawWifiHome(yOffset);
  else if (screen == SCREEN_WIFI_TOGGLE_CONFIRM) drawWifiToggleConfirm(yOffset);
  else if (screen == SCREEN_WIFI_FORGET_CONFIRM) drawWifiForgetConfirm(yOffset);
  else if (screen == SCREEN_WIFI_NTP) drawWifiNTP(yOffset);
  else if (screen == SCREEN_AI_NOWIFI) drawAiNoWifi(yOffset);
  else if (screen == SCREEN_AI_CHAT) drawAiChat(yOffset);
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
  drawBatteryStatusGlyph(128 - 2, 2 + yOffset);
  if (WiFi.status() == WL_CONNECTED) drawWifiStatusIcon(2, 2 + yOffset);
  if (internetOK) drawTowerStatusIcon(12, 2 + yOffset);

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
  int endIdx = min(startIdx + 4, menuCount);

  for (int i = startIdx; i < endIdx; i++) {
    int y = listY + ((i - startIdx) * itemH);

    if (i == menuIndex) {
      display.fillRect(0, y-1, 128, itemH, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(4, y);
    display.print(menuAppNames[menuApps[i]]);
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
  DateTime now = nowTime();
  int curMin = now.hour() * 60 + now.minute();
  int todayIdx = (now.dayOfTheWeek() + 6) % 7; // Monday-first, matches rwBlocks[].days

  // If today's synced timetable has a Sleep-labeled block, that block's own
  // window IS "night" -- overrides the static default entirely for today.
  bool hasSleepBlockToday = false;
  for (int i = 0; i < rwBlockCount; i++) {
    if (!(rwBlocks[i].days & (1 << todayIdx))) continue;
    String label = rwBlocks[i].label;
    label.toLowerCase();
    if (label.indexOf("sleep") < 0) continue;

    hasSleepBlockToday = true;
    int s = rwBlocks[i].startMin, e = rwBlocks[i].endMin;
    bool inBlock = (s <= e) ? (curMin >= s && curMin < e)
                            : (curMin >= s || curMin < e); // wraps past midnight
    if (inBlock) return true;
  }
  if (hasSleepBlockToday) return false;

  // No Sleep block scheduled today (or no timetable synced at all) -- default.
  int h = now.hour();
  return (h >= 22 || h < 6); // 10 PM .. 6 AM
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

/* ---------------------------- Animator ---------------------------- */
// An endless rainy loop: a small cloud rains continuously, and on a fixed but
// uneven schedule (so it feels occasional, not metronomic) a lightning bolt
// draws itself out of the cloud, the sky flashes white from the center
// outward, and then it goes straight back to rain. Everything is time-driven,
// never frame-counted, so it stays smooth at any frame rate. CENTER (the
// always-focused back box) exits.

const int ANIM_TOP = 13;    // just under the header bar
const int ANIM_BOTTOM = 63;
const int CLOUD_BOTTOM = 32;

void animDrawCloud(int yOffset) {
  // Circles only -- no rects, so no flat edge anywhere. A bumpy top row plus
  // an offset bottom row gives a scalloped, rounded underside.
  int cy = 23 + yOffset; // top puffs peak at y=14, clear of the header bar
  const int topX[7]  = {22, 36, 50, 64, 78, 92, 106};
  const int topR[7]  = {6,  7,  7,  7,  7,  7,  6};
  const int topDy[7] = {-1, -2, -2, -2, -2, -2, -1};
  for (int i = 0; i < 7; i++) {
    display.fillCircle(topX[i], cy + topDy[i], topR[i], SSD1306_WHITE);
  }
  const int botX[8] = {15, 29, 43, 57, 71, 85, 99, 113};
  const int botR[8] = {5,  6,  6,  6,  6,  6,  6,  5};
  for (int i = 0; i < 8; i++) {
    display.fillCircle(botX[i], cy + 3, botR[i], SSD1306_WHITE);
  }
}

struct RainDrop { uint8_t x; uint16_t speed; uint16_t phase; };
const int NUM_RAINDROPS = 10;
const RainDrop rainDrops[NUM_RAINDROPS] = {
  {16,  90,   0}, {26, 130, 300}, {36,  80, 600}, {46, 150, 100},
  {56, 100, 900}, {66, 140, 200}, {76,  85, 500}, {86, 120, 800},
  {98, 110, 400}, {110, 95, 700}
};

void animDrawRainField(int yOffset) {
  // Falls from the cloud's underside to the bottom of the constrained area.
  // Driven by absolute time (not per-scene elapsed) so drops flow
  // continuously across rain/thunder transitions instead of jumping. 64-bit
  // math so the position never glitches when millis() * speed would overflow.
  const int top = CLOUD_BOTTOM + 1, span = ANIM_BOTTOM - top;
  uint64_t now = millis();
  for (int i = 0; i < NUM_RAINDROPS; i++) {
    uint64_t t = now + rainDrops[i].phase;
    int y = top + (int)((t * rainDrops[i].speed / 1000) % span);
    int x = rainDrops[i].x;
    display.drawLine(x,     y + yOffset, x - 3, y + 7 + yOffset, SSD1306_WHITE);
    display.drawLine(x + 1, y + yOffset, x - 2, y + 7 + yOffset, SSD1306_WHITE);
  }
}

// Bolt shape as offsets from a per-strike base x, starting inside the cloud's
// underside and zigzagging down to the bottom edge.
const int NUM_BOLT_PTS = 7;
const int boltDX[NUM_BOLT_PTS] = {0, -12, -2, -18, -4, -20, -8};
const int boltY[NUM_BOLT_PTS]  = {30, 37, 41, 48, 52, 58, 63};
const unsigned long BOLT_DRAW_MS = 900;   // time to fully draw the bolt
const unsigned long WHITEOUT_MS  = 900;   // time to fade the screen to white
const unsigned long THUNDER_HOLD_MS = 200; // hold full white briefly
const unsigned long THUNDER_TOTAL_MS = BOLT_DRAW_MS + WHITEOUT_MS + THUNDER_HOLD_MS;

void animDrawBoltThick(int x0, int y0, int x1, int y1, int yOffset) {
  // 5 parallel offset strokes (~3x the previous double-stroke width).
  for (int o = -2; o <= 2; o++) {
    display.drawLine(x0 + o, y0 + yOffset, x1 + o, y1 + yOffset, SSD1306_WHITE);
  }
}

void animDrawThunder(int yOffset, unsigned long elapsed, int baseX) {
  // The bolt draws itself in, one segment at a time, instead of popping in.
  const unsigned long segMs = BOLT_DRAW_MS / (NUM_BOLT_PTS - 1);
  unsigned long boltElapsed = min(elapsed, BOLT_DRAW_MS);
  int fullSegs = boltElapsed / segMs;
  for (int i = 0; i < fullSegs && i < NUM_BOLT_PTS - 1; i++) {
    animDrawBoltThick(baseX + boltDX[i], boltY[i], baseX + boltDX[i + 1], boltY[i + 1], yOffset);
  }
  if (fullSegs < NUM_BOLT_PTS - 1) {
    float segFrac = (float)(boltElapsed - fullSegs * segMs) / segMs;
    int x0 = baseX + boltDX[fullSegs], x1 = baseX + boltDX[fullSegs + 1];
    int mx = x0 + (int)((x1 - x0) * segFrac);
    int my = boltY[fullSegs] + (int)((boltY[fullSegs + 1] - boltY[fullSegs]) * segFrac);
    animDrawBoltThick(x0, boltY[fullSegs], mx, my, yOffset);
  }

  // Once the bolt is fully drawn, the screen (constrained area only) fades to
  // white as a ring expanding outward from the exact center of the
  // constrained area -- a growing "iris" rather than a scattered fade.
  // Slightly oversized radii so the ring visibly overshoots the edges before
  // the final frame snaps to a guaranteed full fillRect (an ellipse alone
  // can never quite reach a rectangle's corners).
  if (elapsed > BOLT_DRAW_MS) {
    unsigned long whiteElapsed = elapsed - BOLT_DRAW_MS;
    if (whiteElapsed >= WHITEOUT_MS) {
      display.fillRect(0, ANIM_TOP + yOffset, 128, 50, SSD1306_WHITE);
    } else {
      float f = (float)whiteElapsed / (float)WHITEOUT_MS;
      int cx = 64, cy = ANIM_TOP + 25;
      float rx = 70.0f * f, ry = 30.0f * f;
      if (ry >= 1.0f) {
        for (int y = ANIM_TOP; y < ANIM_BOTTOM; y++) {
          float dy = (float)(y - cy);
          float t = 1.0f - (dy * dy) / (ry * ry);
          if (t > 0) {
            float halfw = rx * sqrtf(t);
            int x0 = cx - (int)halfw, x1 = cx + (int)halfw;
            if (x0 < 0) x0 = 0;
            if (x1 > 127) x1 = 127;
            if (x1 >= x0) display.drawFastHLine(x0, y + yOffset, x1 - x0 + 1, SSD1306_WHITE);
          }
        }
      }
    }
  }
}

// Rain stretches between strikes, deliberately uneven so thunder feels
// occasional. Each strike lands at its own x so no two in a row look alike.
const int NUM_STRIKES = 4;
const unsigned long rainGapMs[NUM_STRIKES] = {4000, 7000, 3000, 5500};
const int strikeX[NUM_STRIKES] = {72, 44, 94, 60};

void drawAnimator(int yOffset) {
  drawHeader(yOffset, "ANIMATOR", true); // back box always focused -- CENTER exits

  // animatorSceneIndex alternates: even = rain gap, odd = thunder strike.
  int strike = (animatorSceneIndex / 2) % NUM_STRIKES;
  bool thunder = (animatorSceneIndex % 2) == 1;
  unsigned long duration = thunder ? THUNDER_TOTAL_MS : rainGapMs[strike];

  unsigned long elapsed = millis() - animatorSceneStart;
  if (elapsed >= duration) {
    animatorSceneIndex = (animatorSceneIndex + 1) % (NUM_STRIKES * 2);
    animatorSceneStart = millis();
    elapsed = 0;
    strike = (animatorSceneIndex / 2) % NUM_STRIKES;
    thunder = (animatorSceneIndex % 2) == 1;
  }

  animDrawCloud(yOffset);
  animDrawRainField(yOffset);
  if (thunder) animDrawThunder(yOffset, elapsed, strikeX[strike]);

  // FPS box, bottom-left, drawn last with a black fill so rain and the
  // whiteout never cover it. LEFT -10 / RIGHT +10.
  display.fillRect(0, 51 + yOffset, 17, 13, SSD1306_BLACK);
  display.drawRect(0, 51 + yOffset, 17, 13, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(3, 54 + yOffset);
  display.print(animatorFps);
}
