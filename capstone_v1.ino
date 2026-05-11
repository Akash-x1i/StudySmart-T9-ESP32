// ════════════════════════════════════════════════════════════════════
//  ESP32 Device Firmware 
//   • Generic text-input screen (reused for rename, WiFi password, etc.)
//   • App registry: add apps by adding one struct, zero boilerplate
//   • Dirty-flag display: only redraws when something actually changed
//   • Unified button handler with debounce struct
//   • Scrollable lists (notes, networks, apps)
//   • New apps: Settings, Todo
//   • WiFi password entry via T9 keyboard
// ════════════════════════════════════════════════════════════════════

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "MCP23S17.h"
#include <WiFi.h>
#include <time.h>

// ── Hardware ───────────────────────────────────────────────────────
MCP23S17 MCP(22, 0x00, &SPI);

SPIClass hspi(HSPI);
#define TFT_CS   32
#define TFT_RST  33
#define TFT_DC   27
Adafruit_ST7735 tft = Adafruit_ST7735(&hspi, TFT_CS, TFT_DC, TFT_RST);

#define BTN_BACK   4
#define BTN_ENTER  16
#define BTN_MENU   17
#define BUZZER     25




int           pendingKey  = -1;
int           pendingIdx  = 0;
unsigned long lastKeyTime = 0;

// ── Shift / caps state ────────────────────────────────────────────
// SHIFT_OFF   : lowercase
// SHIFT_ONCE  : next committed char is uppercase, then reverts to OFF
// SHIFT_CAPS  : all chars uppercase until toggled off
enum ShiftState { SHIFT_OFF, SHIFT_ONCE, SHIFT_CAPS };
ShiftState shiftState = SHIFT_OFF;

char applyShift(char c, bool consume = true) {
  if (shiftState == SHIFT_OFF) return c;

  char out = (c >= 'a' && c <= 'z') ? c - 32 : c;

  if (consume && shiftState == SHIFT_ONCE)
    shiftState = SHIFT_OFF;

  return out;
}

// ── MCP hold detection ────────────────────────────────────────────
#define MCP_HOLD_MS   400
#define MCP_STAR_PIN  2

unsigned long mcpPressTime[16];
bool          mcpHoldFired[16];




// ── Display constants ──────────────────────────────────────────────
#define SCREEN_W      160
#define SCREEN_H      128
#define CHAR_W        6
#define CHAR_H        8
#define LINE_H        10
#define TEXT_X        4
#define TEXT_Y        4
#define CHARS_PER_LINE ((SCREEN_W - TEXT_X * 2) / CHAR_W)   // 25
#define TITLE_H       12
#define FOOTER_H      12
#define LIST_ITEM_H   12
#define LIST_Y_START  (TITLE_H + 4)
#define LIST_VISIBLE  ((SCREEN_H - TITLE_H - FOOTER_H - 4) / LIST_ITEM_H)  // ~8

// ── T9 keymap ──────────────────────────────────────────────────────
const char* KEY_MAP[] = {
  "1",       // 0  → key 1
  "abc2",    // 1  → key 2
  "def3",    // 2  → key 3
  "ghi4",    // 3  → key 4
  "jkl5",    // 4  → key 5
  "mno6",    // 5  → key 6
  "pqrs7",   // 6  → key 7
  "tuv8",    // 7  → key 8
  "wxyz9",   // 8  → key 9
  " 0",      // 9  → key 0
  ".,!?*",   // 10 → key *
  "#@_-+",   // 11 → key #
};

// MCP pin → KEY_MAP index  (-1 = navigation / unmapped)
int mcpPinToKeyIndex(int pin) {
  switch (pin) {
    case 0:  return 9;   // GPA0 = 0
    case 1:  return 11;  // GPA1 = #
    case 2:  return 10;  // GPA2 = *
    case 3:  return 8;   // GPA3 = 9
    case 8:  return 0;   // GPB0 = 1
    case 9:  return 1;   // GPB1 = 2
    case 10: return 2;   // GPB2 = 3
    case 11: return 3;   // GPB3 = 4
    case 12: return 4;   // GPB4 = 5
    case 13: return 5;   // GPB5 = 6
    case 14: return 6;   // GPB6 = 7
    case 15: return 7;   // GPB7 = 8
    default: return -1;
  }
}

// MCP nav-pin enum (pins 4-7 on GPA)
#define NAV_DOWN   4   // GPA4
#define NAV_LEFT   5   // GPA5
#define NAV_RIGHT  6   // GPA6
#define NAV_UP     7   // GPA7

// ════════════════════════════════════════════════════════════════════
//  SCREEN STATE MACHINE
// ════════════════════════════════════════════════════════════════════
enum ScreenState {
  SCREEN_WIFI_PROMPT,
  SCREEN_HOME,
  SCREEN_APP_MENU,
  // Generic text-input screen (can be pushed from anywhere)
  SCREEN_TEXT_INPUT,
  // WiFi
  SCREEN_WIFI_APP,
  SCREEN_WIFI_NETWORKS,
  // Notepad
  SCREEN_NOTE_LIST,
  SCREEN_NOTE_VIEW,
  SCREEN_NOTE_EDIT,
  SCREEN_NOTE_MENU,
  // Settings
  SCREEN_SETTINGS,
  // Todo
  SCREEN_TODO_LIST,
  SCREEN_TODO_MENU,
};

ScreenState currentScreen = SCREEN_WIFI_PROMPT;
bool        needsRedraw   = true;   // dirty flag

// Macro avoids Arduino's broken auto-prototype for custom-type params
#define gotoScreen(s)  do { currentScreen = (s); needsRedraw = true; } while(0)

// ════════════════════════════════════════════════════════════════════
//  GENERIC TEXT INPUT  (replaces scattered edit logic)
// ════════════════════════════════════════════════════════════════════
//  Callers fill TextInputCtx then gotoScreen(SCREEN_TEXT_INPUT).
//  On commit, onCommit(buf) is called; on cancel, onCancel() is called.

#define TI_MAX_LEN 64

struct TextInputCtx {
  char        title[24];
  char        buf[TI_MAX_LEN + 1];
  int         len;
  int         cursor;
  bool        password;            // mask with '*'
  void      (*onCommit)(const char* text);
  void      (*onCancel)();
};

TextInputCtx ti;   // single shared context (one TI at a time)

void tiInit(const char* title, const char* initial,
            bool password,
            void (*onCommit)(const char*), void (*onCancel)()) {
  strncpy(ti.title, title, sizeof(ti.title) - 1);
  ti.title[sizeof(ti.title) - 1] = '\0';
  strncpy(ti.buf, initial ? initial : "", TI_MAX_LEN);
  ti.buf[TI_MAX_LEN] = '\0';
  ti.len      = strlen(ti.buf);
  ti.cursor   = ti.len;
  ti.password = password;
  ti.onCommit = onCommit;
  ti.onCancel = onCancel;
  // Always clear T9 and shift state
  pendingKey  = -1;
  pendingIdx  = 0;
  lastKeyTime = 0;
  shiftState  = SHIFT_OFF;
}

// ── T9 pending-key state (shared by NOTE_EDIT and TEXT_INPUT) ──────
#define COMMIT_DELAY_MS 600


// Commit pending char into a buffer at cursor position
void commitPendingTo(char* buf, int& len, int& cursor, int maxLen) {
  if (pendingKey < 0) return;
  if (len < maxLen) {
    memmove(&buf[cursor + 1], &buf[cursor], len - cursor + 1);
    buf[cursor] = applyShift(KEY_MAP[pendingKey][pendingIdx], true);
    len++;
    cursor++;
  }
  pendingKey = -1;
  pendingIdx = 0;
}

void handleT9Key(int keyIdx, char* buf, int& len, int& cursor, int maxLen) {
  unsigned long now = millis();
  if (pendingKey == keyIdx && (now - lastKeyTime) < COMMIT_DELAY_MS) {
    pendingIdx = (pendingIdx + 1) % strlen(KEY_MAP[keyIdx]);
  } else {
    commitPendingTo(buf, len, cursor, maxLen);
    pendingKey = keyIdx;
    pendingIdx = 0;
  }
  lastKeyTime = now;
  needsRedraw = true;
}

// Backspace one char at cursor (for BACK button in text inputs)
bool backspaceAt(char* buf, int& len, int& cursor) {
  if (cursor == 0) return false;
  memmove(&buf[cursor - 1], &buf[cursor], len - cursor + 1);
  len--;
  cursor--;
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  BUTTON HANDLER  (edge-detect + debounce)
// ════════════════════════════════════════════════════════════════════
struct Btn {
  int  pin;
  bool last = HIGH;
  bool fell()  { bool c = digitalRead(pin); bool r = (last == HIGH && c == LOW);  last = c; return r; }
  bool rose()  { bool c = digitalRead(pin); bool r = (last == LOW  && c == HIGH); last = c; return r; }
  void update(){ last = digitalRead(pin); }
};

Btn btnBack  = { BTN_BACK };
Btn btnEnter = { BTN_ENTER };
Btn btnMenu  = { BTN_MENU };

// ════════════════════════════════════════════════════════════════════
//  WiFi
// ════════════════════════════════════════════════════════════════════
#define MAX_WIFI_NETWORKS 10

const char* DEFAULT_SSID = "Nue";
const char* DEFAULT_PASS = "aaaaaaaa";

bool   wifiEnabled   = false;
bool   wifiConnected = false;
char   wifiPassword[65] = "";    // user-entered password
int    wifiPromptSel = 0;

String  wifiNetNames[MAX_WIFI_NETWORKS];
bool    wifiNetOpen [MAX_WIFI_NETWORKS];
int     wifiNetRssi [MAX_WIFI_NETWORKS];
int     wifiNetCount = 0;
int     wifiNetSel   = 0;
int     wifiNetScroll= 0;
String  wifiStatus   = "";

bool connectWiFi(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  unsigned long t = millis();
  while (millis() - t < 10000) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) return false;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "IST-5:30", 1);
  tzset();
  struct tm tm;
  return getLocalTime(&tm, 5000);
}

void scanWifi() {
  wifiNetCount = 0;
  wifiNetSel   = 0;
  wifiNetScroll= 0;
  wifiStatus   = "Scanning...";
  int n = WiFi.scanNetworks();
  if (n <= 0) { wifiStatus = "No networks"; return; }
  wifiNetCount = min(n, MAX_WIFI_NETWORKS);
  for (int i = 0; i < wifiNetCount; i++) {
    wifiNetNames[i] = WiFi.SSID(i);
    wifiNetOpen [i] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    wifiNetRssi [i] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  wifiStatus = "Select network";
}

bool getCurrentTime(char* dateBuf, int dateLen, char* timeBuf, int timeLen) {
  struct tm tm;
  if (!getLocalTime(&tm, 1000)) return false;
  strftime(dateBuf, dateLen, "%d %b %Y", &tm);
  strftime(timeBuf, timeLen, "%H:%M",    &tm);
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  NOTEPAD
// ════════════════════════════════════════════════════════════════════
#define MAX_NOTES        10
#define NOTE_NAME_LEN    20
#define NOTE_CONTENT_LEN 200

struct Note {
  char name[NOTE_NAME_LEN + 1];
  char content[NOTE_CONTENT_LEN + 1];
  int  contentLen;
};

Note notes[MAX_NOTES];
int  totalNotes   = 0;
int  noteSelIdx   = 0;
int  noteScroll   = 0;
int  noteMenuSel  = 0;

// Edit buffer (used for both NOTE_EDIT and TEXT_INPUT)
char editBuf[NOTE_CONTENT_LEN + 1] = "";
int  editLen = 0;
int  editCursor = 0;

void notesInit() {
  strcpy(notes[0].name,    "Welcome");
  strcpy(notes[0].content, "Welcome to Notepad!\nUse Menu to open options.");
  notes[0].contentLen = strlen(notes[0].content);
  totalNotes = 1;
}

void noteCreate() {
  if (totalNotes >= MAX_NOTES) return;
  char nm[NOTE_NAME_LEN + 1];
  snprintf(nm, sizeof(nm), "Note %d", totalNotes + 1);
  strcpy(notes[totalNotes].name, nm);
  notes[totalNotes].content[0] = '\0';
  notes[totalNotes].contentLen  = 0;
  totalNotes++;
}

void noteDelete(int idx) {
  if (idx < 0 || idx >= totalNotes) return;
  for (int i = idx; i < totalNotes - 1; i++) notes[i] = notes[i + 1];
  totalNotes--;
  if (noteSelIdx >= totalNotes) noteSelIdx = max(0, totalNotes - 1);
}

void noteSave(int idx) {
  if (idx < 0 || idx >= totalNotes) return;
  strncpy(notes[idx].content, editBuf, NOTE_CONTENT_LEN);
  notes[idx].content[NOTE_CONTENT_LEN] = '\0';
  notes[idx].contentLen = editLen;
}

// Cursor movement helpers
int editLineStart(int pos) {
  int start = 0, col = 0;
  for (int i = 0; i < pos && i < editLen; i++) {
    if (editBuf[i] == '\n') { start = i + 1; col = 0; }
    else if (++col >= CHARS_PER_LINE) { start = i + 1; col = 0; }
  }
  return start;
}

int editLineLen(int start) {
  int len = 0;
  for (int i = start; i < editLen && editBuf[i] != '\n' && len < CHARS_PER_LINE; i++) len++;
  return len;
}

void editCursorUp() {
  int cur  = editLineStart(editCursor);
  if (cur == 0) return;
  int prev = editLineStart(cur - 1);
  int col  = editCursor - cur;
  editCursor = prev + min(col, editLineLen(prev));
}

void editCursorDown() {
  int cur  = editLineStart(editCursor);
  int len  = editLineLen(cur);
  int next = cur + len;
  if (next >= editLen) return;
  if (editBuf[next] == '\n') next++;
  int col = editCursor - cur;
  editCursor = next + min(col, editLineLen(next));
}

int editCursorX(int pos) {
  int x = TEXT_X, col = 0;
  for (int i = 0; i < pos && i < editLen; i++) {
    if (editBuf[i] == '\n') { x = TEXT_X; col = 0; }
    else { x += CHAR_W; if (++col >= CHARS_PER_LINE) { x = TEXT_X; col = 0; } }
  }
  return x;
}

int editCursorY(int pos) {
  int y = TITLE_H + 3, col = 0;
  for (int i = 0; i < pos && i < editLen; i++) {
    if (editBuf[i] == '\n') { y += LINE_H; col = 0; }
    else if (++col >= CHARS_PER_LINE) { y += LINE_H; col = 0; }
  }
  return y;
}

// ════════════════════════════════════════════════════════════════════
//  TODO APP
// ════════════════════════════════════════════════════════════════════
#define MAX_TODOS    20
#define TODO_LEN     40

struct Todo {
  char text[TODO_LEN + 1];
  bool done;
};

Todo todos[MAX_TODOS];
int  totalTodos  = 0;
int  todoSelIdx  = 0;
int  todoScroll  = 0;
int  todoMenuSel = 0;

void todoCreate(const char* text) {
  if (totalTodos >= MAX_TODOS) return;
  strncpy(todos[totalTodos].text, text, TODO_LEN);
  todos[totalTodos].text[TODO_LEN] = '\0';
  todos[totalTodos].done = false;
  totalTodos++;
}

void todoDelete(int idx) {
  if (idx < 0 || idx >= totalTodos) return;
  for (int i = idx; i < totalTodos - 1; i++) todos[i] = todos[i + 1];
  totalTodos--;
  if (todoSelIdx >= totalTodos) todoSelIdx = max(0, totalTodos - 1);
}

// ════════════════════════════════════════════════════════════════════
//  SETTINGS
// ════════════════════════════════════════════════════════════════════
struct Settings {
  char  timezone[32];
  bool  autoWifi;
  bool  soundEnabled;
};

Settings cfg = { "IST-5:30", false, true };

const char* settingLabels[] = { "Timezone", "Auto WiFi", "Sound" };
const int   numSettings     = 3;
int         settingSel      = 0;

// ════════════════════════════════════════════════════════════════════
//  APP REGISTRY  – add an app by adding one entry here
// ════════════════════════════════════════════════════════════════════
struct AppEntry {
  const char* name;
  ScreenState screen;
};

const AppEntry APPS[] = {
  { "Notepad",  SCREEN_NOTE_LIST },
  { "WiFi",     SCREEN_WIFI_APP  },
  { "Todo",     SCREEN_TODO_LIST },
  { "Settings", SCREEN_SETTINGS  },
};
const int NUM_APPS   = 4;
int       appSelIdx  = 0;
int       appScroll  = 0;

// ════════════════════════════════════════════════════════════════════
//  DRAW HELPERS
// ════════════════════════════════════════════════════════════════════
void beep(int ms = 30) {
  if (!cfg.soundEnabled) return;
  digitalWrite(BUZZER, HIGH);
  delay(ms);
  digitalWrite(BUZZER, LOW);
}

void drawTitleBar(const char* title, uint16_t bg = ST77XX_BLUE) {
  tft.fillRect(0, 0, SCREEN_W, TITLE_H, bg);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  // Centre the title
  int len = strlen(title);
  int x   = max(0, (SCREEN_W - len * CHAR_W) / 2);
  tft.setCursor(x, 2);
  tft.print(title);

  // Shift indicator
  if (shiftState != SHIFT_OFF) {
    uint16_t col = (shiftState == SHIFT_CAPS) ? ST77XX_YELLOW : 0x07E0;
    tft.fillRect(SCREEN_W - 14, 1, 12, TITLE_H - 2, col);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(SCREEN_W - 12, 2);
    tft.print(shiftState == SHIFT_CAPS ? "CA" : "SH");
    tft.setTextColor(ST77XX_WHITE);
  }
}

void drawFooter(const char* left, const char* right) {
  tft.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - FOOTER_H + 2);
  tft.print(left);
  if (right && right[0]) {
    int rx = SCREEN_W - strlen(right) * CHAR_W - 2;
    tft.setCursor(rx, SCREEN_H - FOOTER_H + 2);
    tft.print(right);
  }
}

// Generic scrollable list  (items[] = array of C-strings, n = count,
//   selected = highlighted index, scroll = first visible index)
// Returns true if scroll was auto-adjusted.
void drawList(const char** items, int n, int selected, int& scroll,
              bool showCreate = false) {
  // Keep selected visible
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + LIST_VISIBLE) scroll = selected - LIST_VISIBLE + 1;

  tft.setTextSize(1);
  int y = LIST_Y_START;

  if (showCreate) {
    // "+" row always shown at top-of-list
    if (selected == n) {
      tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(0x07E0); // green
    }
    tft.setCursor(6, y + 1);
    tft.print("+ New");
    tft.setTextColor(ST77XX_WHITE);
    y += LIST_ITEM_H;
  }

  for (int i = scroll; i < n && y < SCREEN_H - FOOTER_H; i++) {
    bool sel = (i == selected);
    if (sel) {
      tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(6, y + 1);
    // Truncate to fit
    char tmp[24];
    strncpy(tmp, items[i], 23);
    tmp[23] = '\0';
    tft.print(tmp);
    tft.setTextColor(ST77XX_WHITE);
    y += LIST_ITEM_H;
  }
}

// Draw edit buffer content (shared by NOTE_EDIT and TEXT_INPUT)
// password=true replaces chars with '*'
void drawEditContent(const char* buf, int len, int cursorPos,
                     int startY, bool password = false) {
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  int x = TEXT_X, y = startY, col = 0;
  for (int i = 0; i < len; i++) {
    if (buf[i] == '\n' && !password) {
      x = TEXT_X; y += LINE_H; col = 0;
    } else {
      tft.setCursor(x, y);
      tft.print(password ? '*' : buf[i]);
      x += CHAR_W;
      if (++col >= CHARS_PER_LINE) { x = TEXT_X; y += LINE_H; col = 0; }
    }
  }
}

// ════════════════════════════════════════════════════════════════════
//  SCREEN DRAW FUNCTIONS
// ════════════════════════════════════════════════════════════════════

void drawWifiPrompt() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("WiFi Setup");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 32);
  tft.print("Enable WiFi?");
  const char* opts[] = { "Yes", "No" };
  int y = 52;
  for (int i = 0; i < 2; i++) {
    if (wifiPromptSel == i) {
      tft.fillRect(10, y - 2, SCREEN_W - 20, 14, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(20, y);
    tft.print(opts[i]);
    y += 18;
  }
  drawFooter("Toggle", "Skip");
}

void drawHome() {
  tft.fillScreen(ST77XX_BLACK);
  char date[20] = "Akash Bauri";
  char time_[20] = "";
  if (wifiConnected) {
    if (!getCurrentTime(date, sizeof(date), time_, sizeof(time_))) {
      strcpy(date, "Sync failed");
      strcpy(time_, "");
    }
  }
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  if (time_[0]) {
    int tx = (SCREEN_W - strlen(time_) * 12) / 2;
    tft.setCursor(tx, 36);
    tft.print(time_);
  }
  tft.setTextSize(1);
  int dx = (SCREEN_W - strlen(date) * CHAR_W) / 2;
  tft.setCursor(dx, 62);
  tft.print(date);
  tft.setCursor(0, 84);
  if (wifiConnected)    { tft.setTextColor(0x07E0); tft.print(" WiFi connected"); }
  else if (wifiEnabled) { tft.setTextColor(ST77XX_YELLOW); tft.print(" WiFi enabled"); }
  else                  { tft.setTextColor(0xF800); tft.print(" WiFi off"); }
  tft.setTextColor(0x7BEF);
  tft.setCursor(2, SCREEN_H - FOOTER_H - 10);
  tft.print("Press MENU for apps");
}

void drawAppMenu() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Apps");
  const char* names[NUM_APPS];
  for (int i = 0; i < NUM_APPS; i++) names[i] = APPS[i].name;
  drawList(names, NUM_APPS, appSelIdx, appScroll);
  drawFooter("Open", "Home");
}

// ── Text Input ─────────────────────────────────────────────────────
void drawTextInput() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar(ti.title);

  // Draw the input field box
  tft.drawRect(TEXT_X, TITLE_H + 4, SCREEN_W - TEXT_X * 2, CHAR_H + 6, ST77XX_CYAN);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);

  // Content (masked or plain)
  int x = TEXT_X + 3, y = TITLE_H + 7;
  // How many character slots fit in the box
  int boxChars = (SCREEN_W - TEXT_X * 2 - 6) / CHAR_W;
  // Reserve one slot for the pending char when active
  int slots    = (pendingKey >= 0) ? boxChars - 1 : boxChars;
  // Scroll window: keep cursor character always in view
  int startChar = 0;
  if (ti.cursor >= slots) startChar = ti.cursor - slots + 1;
  for (int i = startChar; i < ti.len && x < SCREEN_W - TEXT_X - 3; i++) {
    tft.setCursor(x, y);
    tft.print(ti.password ? '*' : ti.buf[i]);
    x += CHAR_W;
  }

  // Pending char highlight
  if (pendingKey >= 0) {
    tft.fillRect(x, y - 1, CHAR_W, CHAR_H + 1, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(x, y);
    tft.print(applyShift(KEY_MAP[pendingKey][pendingIdx], false));
    tft.setTextColor(ST77XX_WHITE);
    x += CHAR_W;
  }
  // Cursor bar
  tft.fillRect(x, y - 1, 1, CHAR_H + 1, ST77XX_GREEN);

  // Hint text
  tft.setTextColor(0x7BEF);
  tft.setCursor(TEXT_X, TITLE_H + 26);
  tft.print("T9: tap=cycle, wait=commit");
  tft.setCursor(TEXT_X, TITLE_H + 36);
  tft.print("BACK=del, MENU=save, #=space");

  drawFooter("Save", "Cancel");
}

// ── WiFi screens ───────────────────────────────────────────────────
void drawWifiApp() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("WiFi Control");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);

  uint16_t col = wifiConnected ? 0x07E0 : (wifiEnabled ? ST77XX_YELLOW : 0xF800);
  tft.setTextColor(col);
  tft.setCursor(6, 24);
  tft.print(wifiConnected ? "Connected" : (wifiEnabled ? "Enabled" : "Disabled"));
  tft.setTextColor(ST77XX_WHITE);

  if (wifiConnected) {
    tft.setCursor(6, 36);
    tft.print(WiFi.SSID());
    tft.setCursor(6, 48);
    tft.print(WiFi.localIP().toString());
  } else {
    tft.setCursor(6, 36);
    tft.print(wifiStatus);
  }

  tft.setTextColor(0x7BEF);
  tft.setCursor(6, 70);  tft.print("ENTER = Toggle WiFi");
  tft.setCursor(6, 80);  tft.print("MENU  = Scan networks");
  tft.setCursor(6, 90);  tft.print("BACK  = Apps");
  drawFooter("Toggle", "Back");
}

void drawWifiNetworks() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Networks");
  if (wifiNetCount == 0) {
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(6, 30);
    tft.print(wifiStatus);
  } else {
    int y = LIST_Y_START;
    if (wifiNetSel < wifiNetScroll) wifiNetScroll = wifiNetSel;
    if (wifiNetSel >= wifiNetScroll + LIST_VISIBLE) wifiNetScroll = wifiNetSel - LIST_VISIBLE + 1;
    for (int i = wifiNetScroll; i < wifiNetCount && y < SCREEN_H - FOOTER_H; i++) {
      if (i == wifiNetSel) {
        tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN);
        tft.setTextColor(ST77XX_BLACK);
      } else {
        tft.setTextColor(ST77XX_WHITE);
      }
      tft.setCursor(4, y + 1);
      // Truncate name to leave room for lock/rssi
      char nm[16];
      strncpy(nm, wifiNetNames[i].c_str(), 15); nm[15] = '\0';
      tft.print(nm);
      tft.setCursor(SCREEN_W - 36, y + 1);
      tft.print(wifiNetOpen[i] ? "OPEN" : "LOCK");
      tft.setTextColor(ST77XX_WHITE);
      y += LIST_ITEM_H;
    }
  }
  drawFooter("Connect", "Rescan");
}

// ── Notepad screens ────────────────────────────────────────────────
void drawNoteList() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Notes");
  const char* names[MAX_NOTES];
  for (int i = 0; i < totalNotes; i++) names[i] = notes[i].name;
  // Shift selection: index totalNotes = "create"
  int sel = (noteSelIdx == totalNotes) ? totalNotes : noteSelIdx;
  // We pass n=totalNotes so drawList treats index n as special
  drawList(names, totalNotes, sel, noteScroll, true);
  drawFooter("Open", "Home");
}

void drawNoteView() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar(notes[noteSelIdx].name);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  int x = TEXT_X, y = TITLE_H + 4, col = 0;
  for (int i = 0; i < notes[noteSelIdx].contentLen; i++) {
    char c = notes[noteSelIdx].content[i];
    if (c == '\n') { x = TEXT_X; y += LINE_H; col = 0; }
    else {
      tft.setCursor(x, y);
      tft.print(c);
      x += CHAR_W;
      if (++col >= CHARS_PER_LINE) { x = TEXT_X; y += LINE_H; col = 0; }
    }
    if (y >= SCREEN_H - FOOTER_H) break;
  }
  drawFooter("Options", "Back");
}

void drawNoteEdit() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Edit Note");

  // Draw text
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  int x = TEXT_X, y = TITLE_H + 4, col = 0;
  for (int i = 0; i < editLen; i++) {
    if (editBuf[i] == '\n') { x = TEXT_X; y += LINE_H; col = 0; }
    else {
      tft.setCursor(x, y);
      tft.print(editBuf[i]);
      x += CHAR_W;
      if (++col >= CHARS_PER_LINE) { x = TEXT_X; y += LINE_H; col = 0; }
    }
  }

  // Pending
  int cx = editCursorX(editCursor);
  int cy = editCursorY(editCursor);
  if (pendingKey >= 0) {
    tft.fillRect(cx, cy, CHAR_W, CHAR_H, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(cx, cy);
    tft.print(applyShift(KEY_MAP[pendingKey][pendingIdx], false));
    tft.setTextColor(ST77XX_WHITE);
    cx += CHAR_W;
  }
  tft.fillRect(cx, cy, 1, CHAR_H, ST77XX_GREEN);

  drawFooter("Save", "Back");
}

void drawNoteMenu() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Note Options");
  const char* opts[] = { "View", "Edit", "Rename", "Delete" };
  int y = 28;
  for (int i = 0; i < 4; i++) {
    if (i == noteMenuSel) {
      tft.fillRect(5, y, SCREEN_W - 10, 14, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setTextSize(1);
    tft.setCursor(20, y + 3);
    tft.print(opts[i]);
    tft.setTextColor(ST77XX_WHITE);
    y += 18;
  }
  drawFooter("Select", "Back");
}

// ── Todo screens ───────────────────────────────────────────────────
void drawTodoList() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Todo");
  if (todoSelIdx < todoScroll) todoScroll = todoSelIdx;
  if (todoSelIdx >= todoScroll + LIST_VISIBLE) todoScroll = todoSelIdx - LIST_VISIBLE + 1;

  int y = LIST_Y_START;
  // "+ New" row (index totalTodos)
  {
    bool sel = (todoSelIdx == totalTodos);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(0x07E0); }
    tft.setTextSize(1);
    tft.setCursor(6, y + 1);
    tft.print("+ New task");
    tft.setTextColor(ST77XX_WHITE);
    y += LIST_ITEM_H;
  }

  for (int i = todoScroll; i < totalTodos && y < SCREEN_H - FOOTER_H; i++) {
    bool sel = (i == todoSelIdx);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1);
    tft.print(todos[i].done ? "[x]" : "[ ]");
    tft.setCursor(26, y + 1);
    char tmp[20]; strncpy(tmp, todos[i].text, 19); tmp[19] = '\0';
    tft.print(tmp);
    tft.setTextColor(ST77XX_WHITE);
    y += LIST_ITEM_H;
  }
  drawFooter("Check", "Home");
}

void drawTodoMenu() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Task Options");
  const char* opts[] = { "Toggle done", "Delete" };
  int y = 36;
  for (int i = 0; i < 2; i++) {
    if (i == todoMenuSel) {
      tft.fillRect(5, y, SCREEN_W - 10, 14, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setTextSize(1);
    tft.setCursor(20, y + 3);
    tft.print(opts[i]);
    tft.setTextColor(ST77XX_WHITE);
    y += 20;
  }
  drawFooter("Select", "Back");
}

// ── Settings screen ────────────────────────────────────────────────
void drawSettings() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Settings");
  tft.setTextSize(1);
  int y = LIST_Y_START;

  // Row 0: Timezone
  {
    bool sel = (settingSel == 0);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1); tft.print("TZ:");
    tft.setCursor(30, y + 1); tft.print(cfg.timezone);
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  // Row 1: Auto WiFi
  {
    bool sel = (settingSel == 1);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1); tft.print("Auto WiFi:");
    tft.setCursor(70, y + 1); tft.print(cfg.autoWifi ? "ON" : "OFF");
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  // Row 2: Sound
  {
    bool sel = (settingSel == 2);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1); tft.print("Sound:");
    tft.setCursor(50, y + 1); tft.print(cfg.soundEnabled ? "ON" : "OFF");
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  drawFooter("Edit", "Back");
}

// ════════════════════════════════════════════════════════════════════
//  MASTER REDRAW DISPATCHER
// ════════════════════════════════════════════════════════════════════
void redrawDisplay() {
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:   drawWifiPrompt();   break;
    case SCREEN_HOME:          drawHome();         break;
    case SCREEN_APP_MENU:      drawAppMenu();      break;
    case SCREEN_TEXT_INPUT:    drawTextInput();    break;
    case SCREEN_WIFI_APP:      drawWifiApp();      break;
    case SCREEN_WIFI_NETWORKS: drawWifiNetworks(); break;
    case SCREEN_NOTE_LIST:     drawNoteList();     break;
    case SCREEN_NOTE_VIEW:     drawNoteView();     break;
    case SCREEN_NOTE_EDIT:     drawNoteEdit();     break;
    case SCREEN_NOTE_MENU:     drawNoteMenu();     break;
    case SCREEN_SETTINGS:      drawSettings();     break;
    case SCREEN_TODO_LIST:     drawTodoList();     break;
    case SCREEN_TODO_MENU:     drawTodoMenu();     break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  TEXT INPUT CALLBACKS  (defined after state vars are available)
// ════════════════════════════════════════════════════════════════════

// -- Rename note --
void onNoteRenameCommit(const char* text) {
  strncpy(notes[noteSelIdx].name, text, NOTE_NAME_LEN);
  notes[noteSelIdx].name[NOTE_NAME_LEN] = '\0';
  gotoScreen(SCREEN_NOTE_LIST);
}
void onNoteRenameCancel() { gotoScreen(SCREEN_NOTE_LIST); }

// -- WiFi password entry --
void onWifiPassCommit(const char* text) {
  strncpy(wifiPassword, text, 64);
  wifiPassword[64] = '\0';
  // Attempt connection with user-entered password
  wifiStatus = "Connecting...";
  gotoScreen(SCREEN_WIFI_APP);
  redrawDisplay();
  if (connectWiFi(wifiNetNames[wifiNetSel].c_str(), wifiPassword)) {
    wifiConnected = true;
    wifiEnabled   = true;
    wifiStatus    = "Connected";
  } else {
    wifiConnected = false;
    wifiStatus    = "Failed – bad password?";
  }
  needsRedraw = true;
}
void onWifiPassCancel() { gotoScreen(SCREEN_WIFI_NETWORKS); }

// -- Timezone edit --
void onTimezoneCommit(const char* text) {
  strncpy(cfg.timezone, text, 31);
  cfg.timezone[31] = '\0';
  setenv("TZ", cfg.timezone, 1);
  tzset();
  gotoScreen(SCREEN_SETTINGS);
}
void onTimezoneCancel() { gotoScreen(SCREEN_SETTINGS); }

// -- New todo --
void onTodoCommit(const char* text) {
  todoCreate(text);
  gotoScreen(SCREEN_TODO_LIST);
}
void onTodoCancel() { gotoScreen(SCREEN_TODO_LIST); }

// ════════════════════════════════════════════════════════════════════
//  INPUT HANDLERS  (one per screen, called from loop)
// ════════════════════════════════════════════════════════════════════

void handleBack() {
  beep(80);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiEnabled = wifiConnected = false;
      gotoScreen(SCREEN_HOME);
      break;
    case SCREEN_HOME: break; // nowhere to go
    case SCREEN_APP_MENU:
      appSelIdx = 0;
      gotoScreen(SCREEN_HOME);
      break;
    case SCREEN_TEXT_INPUT:
      if (pendingKey >= 0) {
        // Pending char exists: commit it first, then delete it
        // This way BACK always removes the most recently entered character
        commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
        backspaceAt(ti.buf, ti.len, ti.cursor);
        needsRedraw = true;
      } else if (ti.len > 0) {
        // Normal backspace: delete char before cursor
        backspaceAt(ti.buf, ti.len, ti.cursor);
        needsRedraw = true;
      } else {
        // Field empty — cancel and go back
        if (ti.onCancel) ti.onCancel();
      }
      break;
    case SCREEN_WIFI_APP:    gotoScreen(SCREEN_APP_MENU);      break;
    case SCREEN_WIFI_NETWORKS: gotoScreen(SCREEN_WIFI_APP);    break;
    case SCREEN_NOTE_LIST:   gotoScreen(SCREEN_HOME);          break;
    case SCREEN_NOTE_VIEW:   gotoScreen(SCREEN_NOTE_LIST);     break;
    case SCREEN_NOTE_EDIT:
      // BACK = backspace (or exit if nothing left)
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (!backspaceAt(editBuf, editLen, editCursor)) {
        gotoScreen(SCREEN_NOTE_VIEW);
      }
      needsRedraw = true;
      break;
    case SCREEN_NOTE_MENU:
      noteMenuSel = 0;
      gotoScreen(SCREEN_NOTE_LIST);
      break;
    case SCREEN_SETTINGS:    gotoScreen(SCREEN_APP_MENU);      break;
    case SCREEN_TODO_LIST:   gotoScreen(SCREEN_HOME);          break;
    case SCREEN_TODO_MENU:   gotoScreen(SCREEN_TODO_LIST);     break;
  }
}

void handleEnter() {
  beep(50);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiEnabled = (wifiPromptSel == 0);
      if (wifiEnabled) wifiConnected = connectWiFi(DEFAULT_SSID, DEFAULT_PASS);
      gotoScreen(SCREEN_HOME);
      break;
    case SCREEN_HOME: break;
    case SCREEN_APP_MENU:
      gotoScreen(APPS[appSelIdx].screen);
      if (APPS[appSelIdx].screen == SCREEN_NOTE_LIST) {
        noteSelIdx = totalNotes; // preselect "create"
      }
      break;
    case SCREEN_TEXT_INPUT:
      commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
      if (ti.onCommit) ti.onCommit(ti.buf);
      break;
    case SCREEN_WIFI_APP:
      if (wifiEnabled) {
        WiFi.disconnect(true);
        wifiEnabled = wifiConnected = false;
        wifiStatus = "Disabled";
      } else {
        wifiEnabled = true;
        wifiStatus  = "Enabled";
        wifiConnected = connectWiFi(DEFAULT_SSID, DEFAULT_PASS);
        wifiStatus = wifiConnected ? "Connected" : "Connect failed";
      }
      needsRedraw = true;
      break;
    case SCREEN_WIFI_NETWORKS:
      if (wifiNetCount > 0) {
        if (wifiNetOpen[wifiNetSel]) {
          // Open network – connect directly
          wifiStatus = "Connecting...";
          needsRedraw = true;
          redrawDisplay();
          if (connectWiFi(wifiNetNames[wifiNetSel].c_str(), "")) {
            wifiConnected = true;
            wifiEnabled   = true;
            wifiStatus = "Connected";
          } else {
            wifiConnected = false;
            wifiStatus = "Connect failed";
          }
          gotoScreen(SCREEN_WIFI_APP);
        } else {
          // Locked – ask for password via text input
          tiInit("WiFi Password", "", true, onWifiPassCommit, onWifiPassCancel);
          gotoScreen(SCREEN_TEXT_INPUT);
        }
      }
      break;
    case SCREEN_NOTE_LIST:
      if (noteSelIdx == totalNotes) {
        noteCreate();
        noteSelIdx = totalNotes - 1;
        // Open blank note for editing
        editBuf[0] = '\0'; editLen = 0; editCursor = 0;
        pendingKey = -1;
        gotoScreen(SCREEN_NOTE_EDIT);
      } else {
        gotoScreen(SCREEN_NOTE_VIEW);
      }
      break;
    case SCREEN_NOTE_VIEW: break;
    case SCREEN_NOTE_EDIT:
      // ENTER = newline
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (editLen < NOTE_CONTENT_LEN) {
        memmove(&editBuf[editCursor + 1], &editBuf[editCursor], editLen - editCursor + 1);
        editBuf[editCursor] = '\n';
        editLen++; editCursor++;
      }
      needsRedraw = true;
      break;
    case SCREEN_NOTE_MENU:
      switch (noteMenuSel) {
        case 0: gotoScreen(SCREEN_NOTE_VIEW); break;
        case 1: // Edit
          strncpy(editBuf, notes[noteSelIdx].content, NOTE_CONTENT_LEN);
          editLen    = notes[noteSelIdx].contentLen;
          editCursor = 0;
          pendingKey = -1;
          gotoScreen(SCREEN_NOTE_EDIT);
          break;
        case 2: // Rename via text input
          tiInit("Rename Note", notes[noteSelIdx].name, false,
                 onNoteRenameCommit, onNoteRenameCancel);
          gotoScreen(SCREEN_TEXT_INPUT);
          break;
        case 3: // Delete
          noteDelete(noteSelIdx);
          gotoScreen(SCREEN_NOTE_LIST);
          break;
      }
      noteMenuSel = 0;
      break;
    case SCREEN_SETTINGS:
      // ENTER on a setting
      switch (settingSel) {
        case 0: // Edit timezone
          tiInit("Timezone", cfg.timezone, false, onTimezoneCommit, onTimezoneCancel);
          gotoScreen(SCREEN_TEXT_INPUT);
          break;
        case 1: cfg.autoWifi     = !cfg.autoWifi;     needsRedraw = true; break;
        case 2: cfg.soundEnabled = !cfg.soundEnabled; needsRedraw = true; break;
      }
      break;
    case SCREEN_TODO_LIST:
      if (todoSelIdx == totalTodos) {
        // Create new todo via text input
        tiInit("New Task", "", false, onTodoCommit, onTodoCancel);
        gotoScreen(SCREEN_TEXT_INPUT);
      } else {
        // Toggle done inline
        todos[todoSelIdx].done = !todos[todoSelIdx].done;
        needsRedraw = true;
      }
      break;
    case SCREEN_TODO_MENU:
      switch (todoMenuSel) {
        case 0: todos[todoSelIdx].done = !todos[todoSelIdx].done; break;
        case 1: todoDelete(todoSelIdx); break;
      }
      todoMenuSel = 0;
      gotoScreen(SCREEN_TODO_LIST);
      break;
  }
}

void handleMenu() {
  beep(30);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiPromptSel = (wifiPromptSel + 1) % 2;
      needsRedraw = true;
      break;
    case SCREEN_HOME:
      appSelIdx = 0;
      gotoScreen(SCREEN_APP_MENU);
      break;
    case SCREEN_APP_MENU:
      appSelIdx = (appSelIdx + 1) % NUM_APPS;
      needsRedraw = true;
      break;
    case SCREEN_TEXT_INPUT:
      // MENU = save/commit
      commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
      if (ti.onCommit) ti.onCommit(ti.buf);
      break;
    case SCREEN_WIFI_APP:
      scanWifi();
      gotoScreen(SCREEN_WIFI_NETWORKS);
      break;
    case SCREEN_WIFI_NETWORKS:
      scanWifi();
      needsRedraw = true;
      break;
    case SCREEN_NOTE_LIST:
      // No action – placeholder for future sort/filter
      break;
    case SCREEN_NOTE_VIEW:
      noteMenuSel = 0;
      gotoScreen(SCREEN_NOTE_MENU);
      break;
    case SCREEN_NOTE_EDIT:
      // MENU = save & return to view
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      noteSave(noteSelIdx);
      editLen = 0; editCursor = 0; pendingKey = -1;
      gotoScreen(SCREEN_NOTE_VIEW);
      break;
    case SCREEN_NOTE_MENU:
      noteMenuSel = (noteMenuSel + 1) % 4;
      needsRedraw = true;
      break;
    case SCREEN_SETTINGS:
      settingSel = (settingSel + 1) % numSettings;
      needsRedraw = true;
      break;
    case SCREEN_TODO_LIST:
      if (todoSelIdx < totalTodos) {
        todoMenuSel = 0;
        gotoScreen(SCREEN_TODO_MENU);
      }
      break;
    case SCREEN_TODO_MENU:
      todoMenuSel = (todoMenuSel + 1) % 2;
      needsRedraw = true;
      break;
  }
}

// Unified nav handler (UP/DOWN/LEFT/RIGHT from MCP)
void handleNav(int dir) {
  beep();
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiPromptSel = (dir == NAV_DOWN || dir == NAV_RIGHT) ?
                      (wifiPromptSel + 1) % 2 : (wifiPromptSel + 1) % 2;
      needsRedraw = true;
      break;
    case SCREEN_APP_MENU:
      if (dir == NAV_DOWN)  appSelIdx = (appSelIdx + 1) % NUM_APPS;
      if (dir == NAV_UP)    appSelIdx = (appSelIdx - 1 + NUM_APPS) % NUM_APPS;
      needsRedraw = true;
      break;
    case SCREEN_TEXT_INPUT:
      if (dir == NAV_LEFT  && ti.cursor > 0)    { commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN); ti.cursor--; needsRedraw = true; }
      if (dir == NAV_RIGHT && ti.cursor < ti.len){ commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN); ti.cursor++; needsRedraw = true; }
      break;
    case SCREEN_WIFI_NETWORKS:
      if (dir == NAV_DOWN) wifiNetSel = (wifiNetSel + 1) % max(1, wifiNetCount);
      if (dir == NAV_UP)   wifiNetSel = (wifiNetSel - 1 + max(1, wifiNetCount)) % max(1, wifiNetCount);
      needsRedraw = true;
      break;
    case SCREEN_NOTE_LIST: {
      int n = totalNotes + 1; // +1 for create row
      if (dir == NAV_DOWN) noteSelIdx = (noteSelIdx + 1) % n;
      if (dir == NAV_UP)   noteSelIdx = (noteSelIdx - 1 + n) % n;
      needsRedraw = true;
      break;
    }
    case SCREEN_NOTE_EDIT:
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (dir == NAV_UP)    editCursorUp();
      if (dir == NAV_DOWN)  editCursorDown();
      if (dir == NAV_LEFT  && editCursor > 0)     editCursor--;
      if (dir == NAV_RIGHT && editCursor < editLen) editCursor++;
      needsRedraw = true;
      break;
    case SCREEN_NOTE_MENU:
      if (dir == NAV_DOWN) noteMenuSel = (noteMenuSel + 1) % 4;
      if (dir == NAV_UP)   noteMenuSel = (noteMenuSel - 1 + 4) % 4;
      needsRedraw = true;
      break;
    case SCREEN_SETTINGS:
      if (dir == NAV_DOWN) settingSel = (settingSel + 1) % numSettings;
      if (dir == NAV_UP)   settingSel = (settingSel - 1 + numSettings) % numSettings;
      needsRedraw = true;
      break;
    case SCREEN_TODO_LIST: {
      int n = totalTodos + 1;
      if (dir == NAV_DOWN) todoSelIdx = (todoSelIdx + 1) % n;
      if (dir == NAV_UP)   todoSelIdx = (todoSelIdx - 1 + n) % n;
      needsRedraw = true;
      break;
    }
    case SCREEN_TODO_MENU:
      if (dir == NAV_DOWN) todoMenuSel = (todoMenuSel + 1) % 2;
      if (dir == NAV_UP)   todoMenuSel = (todoMenuSel - 1 + 2) % 2;
      needsRedraw = true;
      break;
    default: break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BTN_BACK,  INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_MENU,  INPUT_PULLUP);
  pinMode(BUZZER,    OUTPUT);
  digitalWrite(BUZZER, LOW);

  // VSPI → MCP23S17
  SPI.begin(18, 19, 23, 22);
  if (!MCP.begin()) {
    Serial.println("MCP23S17 not found!");
    while (1) delay(1000);
  }
  Serial.println("MCP OK");

  // HSPI → TFT
  hspi.begin(14, 12, 13, 32);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  notesInit();

  // Seed sample todo
  todoCreate("Read the manual");

  // Init MCP hold-detection arrays
  for (int i = 0; i < 16; i++) {
    mcpPressTime[i] = 0;
    mcpHoldFired[i] = false;
  }

  gotoScreen(SCREEN_WIFI_PROMPT);
  redrawDisplay();
  needsRedraw = false;
  Serial.println("Ready.");
}

// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════
uint16_t     lastMcpState   = 0xFFFF;
unsigned long lastClockTick = 0;

void loop() {
  unsigned long now = millis();

  // ── Auto-commit pending T9 char ──────────────────────────────────
  if (pendingKey >= 0 && (now - lastKeyTime) >= COMMIT_DELAY_MS) {
    bool inEdit = (currentScreen == SCREEN_NOTE_EDIT);
    bool inTI   = (currentScreen == SCREEN_TEXT_INPUT);
    if (inEdit) {
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      needsRedraw = true;
    } else if (inTI) {
      commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
      needsRedraw = true;
    }
  }

  // ── Clock refresh on home (every 60 s) ──────────────────────────
  if (currentScreen == SCREEN_HOME && (now - lastClockTick) >= 60000) {
    lastClockTick = now;
    needsRedraw   = true;
  }

  // ── MCP buttons ──────────────────────────────────────────────────
  uint16_t mcpState = MCP.read16();

  // Hold detection
  for (int i = 0; i < 16; i++) {
    bool isPressed = !(mcpState & (1 << i));

    if (isPressed) {
      bool wasPressedBefore = !(lastMcpState & (1 << i));

      if (wasPressedBefore) {
        if (!mcpHoldFired[i] && (now - mcpPressTime[i]) >= MCP_HOLD_MS) {
          mcpHoldFired[i] = true;

          if (i == MCP_STAR_PIN) {
            bool inEdit = (currentScreen == SCREEN_NOTE_EDIT);
            bool inTI   = (currentScreen == SCREEN_TEXT_INPUT);

            if (inEdit || inTI) {
              if (shiftState == SHIFT_OFF)       shiftState = SHIFT_ONCE;
              else if (shiftState == SHIFT_ONCE) shiftState = SHIFT_CAPS;
              else                               shiftState = SHIFT_OFF;

              beep(60);
              needsRedraw = true;
            }
          }
        }
      } else {
        mcpPressTime[i] = now;
        mcpHoldFired[i] = false;
      }
    }
  }

  if (mcpState != lastMcpState) {
    for (int i = 0; i < 16; i++) {
      bool wasPressed = !(lastMcpState & (1 << i));
      bool isPressed  = !(mcpState & (1 << i));

      if (wasPressed && !isPressed) {

        if (mcpHoldFired[i]) {
          mcpHoldFired[i] = false;
          continue;
        }

        int keyIdx = mcpPinToKeyIndex(i);

        if (keyIdx >= 0) {
          bool inEdit = (currentScreen == SCREEN_NOTE_EDIT);
          bool inTI   = (currentScreen == SCREEN_TEXT_INPUT);

          if (inEdit) {
            beep();
            handleT9Key(keyIdx, editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
          } else if (inTI) {
            beep();
            handleT9Key(keyIdx, ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
          }
        } else {
          switch (i) {
            case NAV_DOWN:
            case NAV_UP:
            case NAV_LEFT:
            case NAV_RIGHT:
              handleNav(i);
              break;
          }
        }
      }
    }

    lastMcpState = mcpState;
  }

  // ── Direct buttons (released edge) ───────────────────────────────
  {
    bool cb = digitalRead(BTN_BACK);
    bool ce = digitalRead(BTN_ENTER);
    bool cm = digitalRead(BTN_MENU);
    if (btnBack.last  == LOW && cb == HIGH) handleBack();
    if (btnEnter.last == LOW && ce == HIGH) handleEnter();
    if (btnMenu.last  == LOW && cm == HIGH) handleMenu();
    btnBack.last  = cb;
    btnEnter.last = ce;
    btnMenu.last  = cm;
  }

  // ── Redraw only when dirty ────────────────────────────────────────
  if (needsRedraw) {
    redrawDisplay();
    needsRedraw = false;
  }

  delay(8);   // ~125 Hz poll rate
}
