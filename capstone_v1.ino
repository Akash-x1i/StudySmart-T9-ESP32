#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "MCP23S17.h"
#include <WiFi.h>
#include <time.h>

// ── MCP23S17 ──────────────────────────────────────────
MCP23S17 MCP(22, 0x00, &SPI);

// ── TFT ───────────────────────────────────────────────
SPIClass hspi(HSPI);
#define TFT_CS  32
#define TFT_RST 33
#define TFT_DC  27
Adafruit_ST7735 tft = Adafruit_ST7735(&hspi, TFT_CS, TFT_DC, TFT_RST);

// ── Direct buttons ────────────────────────────────────
#define BTN_BACK   4
#define BTN_ENTER  16
#define BTN_MENU   17
#define BUZZER     25

// ── T9 keymap ─────────────────────────────────────────
const char* keyMap[] = {
  "1",        // key 1  (MCP pin index used as key id)
  "abc2",     // key 2
  "def3",     // key 3
  "ghi4",     // key 4
  "jkl5",     // key 5
  "mno6",     // key 6
  "pqrs7",    // key 7
  "tuv8",     // key 8
  "wxyz9",    // key 9
  " 0",       // key 0
  ".,!?*",    // key *
  "#@_-+",    // key #
};

// Maps button name → keyMap index
// mcpName order: GPA0=0, GPA1=#, GPA2=*, GPA3=9, GPA4=DOWN, GPA5=LEFT, GPA6=RIGHT, GPA7=UP
//                GPB0=1, GPB1=2, GPB2=3, GPB3=4, GPB4=5,    GPB5=6,    GPB6=7,     GPB7=8
// keyMap index:  0=1, 1=2, 2=3, 3=4, 4=5, 5=6, 6=7, 7=8, 8=9, 9=0, 10=*, 11=#

// Returns keyMap index for a given MCP pin, -1 if not a text key
int mcpPinToKeyIndex(int pin) {
  switch (pin) {
    case 0:  return 9;   // GPA0 = "0"  → " 0"
    case 1:  return 11;  // GPA1 = "#"  → "#@_-+"
    case 2:  return 10;  // GPA2 = "*"  → ".,!?*"
    case 3:  return 8;   // GPA3 = "9"  → "wxyz9"
    case 8:  return 0;   // GPB0 = "1"  → "1"
    case 9:  return 1;   // GPB1 = "2"  → "abc2"
    case 10: return 2;   // GPB2 = "3"  → "def3"
    case 11: return 3;   // GPB3 = "4"  → "ghi4"
    case 12: return 4;   // GPB4 = "5"  → "jkl5"
    case 13: return 5;   // GPB5 = "6"  → "mno6"
    case 14: return 6;   // GPB6 = "7"  → "pqrs7"
    case 15: return 7;   // GPB7 = "8"  → "tuv8"
    default: return -1;  // nav keys or unmapped
  }
}

// ── Screen states ────────────────────────────────────
enum ScreenState {
  WIFI_PROMPT,
  HOME_SCREEN,
  APP_MENU,
  WIFI_APP,
  WIFI_NETWORK_LIST,
  NOTEPAD_LIST,
  NOTEPAD_VIEW,
  NOTEPAD_EDIT,
  NOTEPAD_MENU
};

ScreenState currentScreen = WIFI_PROMPT;
int selectedApp = 0;  // for app menu selection

const char* WIFI_SSID = "Nue";
const char* WIFI_PASS = "aaaaaaaa";
bool wifiEnabled = false;
bool wifiConnected = false;
int wifiPromptOption = 0;  // 0 = yes, 1 = no

const int MAX_WIFI_NETWORKS = 10;
String wifiNetworkNames[MAX_WIFI_NETWORKS];
bool wifiNetworkOpen[MAX_WIFI_NETWORKS];
int wifiNetworkRssi[MAX_WIFI_NETWORKS];
int wifiNetworkCount = 0;
int wifiSelectedNetworkIdx = 0;
String wifiStatusMessage = "";

// ── Note storage ──────────────────────────────────────
#define MAX_NOTES     10
#define NOTE_NAME_LEN 20
#define NOTE_CONTENT_LEN 200

struct Note {
  char name[NOTE_NAME_LEN + 1];
  char content[NOTE_CONTENT_LEN + 1];
  int contentLen;
};

Note notes[MAX_NOTES];
int totalNotes = 0;
int selectedNoteIdx = 0;
int menuSelectedIdx = 0;  // for context menus
char editBuffer[NOTE_CONTENT_LEN + 1] = "";
int editBufLen = 0;
int editCursorPos = 0;

// ── Display / text state ───────────────────────────────
#define MAX_LEN       100
#define COMMIT_DELAY  600   // ms before character is committed
#define TEXT_X        4
#define TEXT_Y        4
#define CHAR_W        6     // default font width
#define CHAR_H        8     // default font height
#define LINE_H        10
#define SCREEN_W      160
#define SCREEN_H      128
#define CHARS_PER_LINE ((SCREEN_W - TEXT_X * 2) / CHAR_W)  // ~20

int     pendingKey   = -1;           // keyMap index of key being cycled
int     pendingIdx   = 0;            // which char in keyMap[pendingKey] is showing
unsigned long lastKeyTime = 0;
unsigned long lastClockUpdate = 0;

// ── Apps list ────────────────────────────────────────
const char* apps[] = {"Notepad", "WiFi"};
const int numApps = 2;

// ── Note management functions ─────────────────────────
void initializeNotes() {
  // Create a default note
  strcpy(notes[0].name, "Note 1");
  strcpy(notes[0].content, "Welcome to Notepad");
  notes[0].contentLen = strlen(notes[0].content);
  totalNotes = 1;
}

void createNewNote() {
  if (totalNotes >= MAX_NOTES) return;
  char newName[NOTE_NAME_LEN + 1];
  sprintf(newName, "Note %d", totalNotes + 1);
  strcpy(notes[totalNotes].name, newName);
  notes[totalNotes].content[0] = '\0';
  notes[totalNotes].contentLen = 0;
  totalNotes++;
}

void deleteNote(int idx) {
  if (idx < 0 || idx >= totalNotes) return;
  for (int i = idx; i < totalNotes - 1; i++) {
    strcpy(notes[i].name, notes[i + 1].name);
    strcpy(notes[i].content, notes[i + 1].content);
    notes[i].contentLen = notes[i + 1].contentLen;
  }
  totalNotes--;
  if (selectedNoteIdx >= totalNotes) selectedNoteIdx = totalNotes - 1;
}

void renameNote(int idx, const char* newName) {
  if (idx < 0 || idx >= totalNotes) return;
  strncpy(notes[idx].name, newName, NOTE_NAME_LEN);
  notes[idx].name[NOTE_NAME_LEN] = '\0';
}

void saveNote(int idx) {
  if (idx < 0 || idx >= totalNotes) return;
  strcpy(notes[idx].content, editBuffer);
  notes[idx].contentLen = editBufLen;
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (millis() - start < 10000) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "IST-5:30", 1);
  tzset();
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    return false;
  }
  return true;
}

bool getCurrentTimeStrings(char* dateStr, int dateLen, char* timeStr, int timeLen) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return false;
  strftime(dateStr, dateLen, "%d %b %Y", &timeinfo);
  strftime(timeStr, timeLen, "%H:%M", &timeinfo);
  return true;
}

void beep(int ms = 30) {
  digitalWrite(BUZZER, HIGH);
  delay(ms);
  digitalWrite(BUZZER, LOW);
}

// Cursor position helpers for edit buffer
int getEditCursorX(int pos) {
  int x = TEXT_X;
  int col = 0;
  for(int i = 0; i < pos && i < editBufLen; i++) {
    if(editBuffer[i] == '\n') {
      x = TEXT_X;
      col = 0;
    } else {
      x += CHAR_W;
      col++;
      if(col >= CHARS_PER_LINE) {
        x = TEXT_X;
        col = 0;
      }
    }
  }
  return x;
}

int getEditCursorY(int pos) {
  int y = 20;
  int col = 0;
  for(int i = 0; i < pos && i < editBufLen; i++) {
    if(editBuffer[i] == '\n') {
      y += LINE_H;
      col = 0;
    } else {
      col++;
      if(col >= CHARS_PER_LINE) {
        y += LINE_H;
        col = 0;
      }
    }
  }
  return y;
}

int getEditLineStart(int pos) {
  int start = 0;
  int col = 0;
  for(int i = 0; i < pos && i < editBufLen; i++) {
    if(editBuffer[i] == '\n') {
      start = i + 1;
      col = 0;
    } else {
      col++;
      if(col >= CHARS_PER_LINE) {
        start = i + 1;
        col = 0;
      }
    }
  }
  return start;
}

int getEditLineLength(int start) {
  int len = 0;
  int col = 0;
  for(int i = start; i < editBufLen && editBuffer[i] != '\n'; i++) {
    len++;
    col++;
    if(col >= CHARS_PER_LINE) break;
  }
  return len;
}

void moveEditCursorUp() {
  if (editCursorPos == 0) return;
  int currentLineStart = getEditLineStart(editCursorPos);
  if (currentLineStart == 0) return;
  int prevLineStart = getEditLineStart(currentLineStart - 1);
  int column = editCursorPos - currentLineStart;
  int prevLineLen = getEditLineLength(prevLineStart);
  editCursorPos = prevLineStart + min(column, prevLineLen);
}

void moveEditCursorDown() {
  int currentLineStart = getEditLineStart(editCursorPos);
  int currentLineLen = getEditLineLength(currentLineStart);
  int nextLineStart = currentLineStart + currentLineLen;
  if (nextLineStart >= editBufLen) return;
  if (editBuffer[nextLineStart] == '\n') nextLineStart++;
  int column = editCursorPos - currentLineStart;
  int nextLineLen = getEditLineLength(nextLineStart);
  editCursorPos = nextLineStart + min(column, nextLineLen);
}

// ── Redraw note list screen ─────────────────────────────
void redrawNoteList() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 12, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(62, 2);
  tft.print("Notes");
  
  // Draw create option
  int y = 18;
  tft.setTextSize(1);
  if (selectedNoteIdx == totalNotes) {
    tft.fillRect(2, y, SCREEN_W - 4, 10, ST77XX_CYAN);
    tft.setTextColor(ST77XX_BLACK);
  } else {
    tft.setTextColor(ST77XX_WHITE);
  }
  tft.setCursor(10, y + 1);
  tft.print("+ Create Note");
  tft.setTextColor(ST77XX_WHITE);
  
  // Draw notes list
  y = 32;
  for (int i = totalNotes-1; i >= 0; i--) {
    if (y + 12 > SCREEN_H - 12) break;
    if (i == selectedNoteIdx) {
      tft.fillRect(2, y, SCREEN_W - 4, 10, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(8, y + 1);
    tft.print(notes[i].name);
    tft.setTextColor(ST77XX_WHITE);
    y += 12;
  }
  
  // Bottom hint bar
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("More                  Home");
}

// ── Redraw note view screen ─────────────────────────────
void redrawNoteView() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Title bar with note name
  tft.fillRect(0, 0, SCREEN_W, 12, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(58, 2);
  tft.print(notes[selectedNoteIdx].name);
  
  // Display note content
  tft.setTextColor(ST77XX_WHITE);
  int x = TEXT_X, y = 18;
  for (int i = 0; i < notes[selectedNoteIdx].contentLen; i++) {
    if (notes[selectedNoteIdx].content[i] == '\n') {
      x = TEXT_X;
      y += LINE_H;
    } else {
      tft.setCursor(x, y);
      tft.print(notes[selectedNoteIdx].content[i]);
      x += CHAR_W;
      if (x + CHAR_W > SCREEN_W - TEXT_X) {
        x = TEXT_X;
        y += LINE_H;
      }
    }
  }
  
  // Bottom hint bar
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("More                  Back");
}

// ── Redraw note edit screen ─────────────────────────────
void redrawNoteEdit() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 12, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(44, 2);
  tft.print("Editing...");
  
  // Display edit buffer
  tft.setTextColor(ST77XX_WHITE);
  int x = TEXT_X, y = 18;
  for (int i = 0; i < editBufLen; i++) {
    if (editBuffer[i] == '\n') {
      x = TEXT_X;
      y += LINE_H;
    } else {
      tft.setCursor(x, y);
      tft.print(editBuffer[i]);
      x += CHAR_W;
      if (x + CHAR_W > SCREEN_W - TEXT_X) {
        x = TEXT_X;
        y += LINE_H;
      }
    }
  }
  
  // Cursor position
  int cursorX = getEditCursorX(editCursorPos);
  int cursorY = getEditCursorY(editCursorPos);
  
  // Print pending character (highlighted) at cursor
  if (pendingKey >= 0) {
    char pending = keyMap[pendingKey][pendingIdx];
    tft.fillRect(cursorX, cursorY, CHAR_W, CHAR_H, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(cursorX, cursorY);
    tft.print(pending);
    tft.setTextColor(ST77XX_WHITE);
    cursorX += CHAR_W;
  }
  
  // Cursor (vertical bar)
  tft.fillRect(cursorX, cursorY, 1, CHAR_H, ST77XX_GREEN);
  
  // Bottom hint bar
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("Save                  Back");
}

// ── Redraw note menu screen ─────────────────────────────
void redrawNoteMenu() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 12, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(54, 2);
  tft.print("Options");
  
  // Menu options
  const char* options[] = {"View", "Edit", "Rename", "Delete"};
  int numOptions = 4;
  int y = 30;
  
  for (int i = 0; i < numOptions; i++) {
    if (i == menuSelectedIdx) {
      tft.fillRect(5, y, SCREEN_W - 10, 12, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(20, y + 2);
    tft.print(options[i]);
    tft.setTextColor(ST77XX_WHITE);
    y += 16;
  }
  
  // Bottom hint bar
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("Select                Back");
}

// ── Redraw home screen ───────────────────────────────
void redrawHomeScreen() {
  tft.fillScreen(ST77XX_BLACK);
  
  // // Title bar
  // tft.fillRect(0, 0, SCREEN_W, 20, ST77XX_BLUE);
  // tft.setTextColor(ST77XX_WHITE);
  // tft.setTextSize(2);
  // tft.setCursor(20, 3);
  // tft.print("Device");

  char dateStr[20] = "Akash Bauri";
  char timeStr[20] = "Hello";
  if (wifiConnected) {
    if (!getCurrentTimeStrings(dateStr, sizeof(dateStr), timeStr, sizeof(timeStr))) {
      strcpy(dateStr, " sync fail");
      strcpy(timeStr, "Oops!");
    }
  }

  // Clock display
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(52, 36);
  tft.print(timeStr);

  // Date display
  tft.setTextSize(1);
  tft.setCursor(49, 62);
  tft.print(dateStr);

  // WiFi status
  if (wifiConnected) {
    tft.setCursor(53, 80);
    tft.print("Connected");
  } else if (wifiEnabled) {
    tft.setCursor(48, 80);
    tft.print("Connecting...");
  } else {
    tft.setCursor(25, 80);
    tft.print("any wifi available?");
  }
}

// ── Redraw WiFi prompt screen ────────────────────────
void redrawWifiPrompt() {
  tft.fillScreen(ST77XX_BLACK);

  tft.fillRect(0, 0, SCREEN_W, 20, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(12, 3);
  tft.print("WiFi Setup");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 35);
  tft.print("Enable WiFi?");

  int y = 60;
  const char* options[] = {"Yes", "No"};
  for (int i = 0; i < 2; i++) {
    if (wifiPromptOption == i) {
      tft.fillRect(10, y - 2, SCREEN_W - 20, 14, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(20, y);
    tft.print(options[i]);
    y += 18;
  }

  tft.setTextColor(ST77XX_CYAN);
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("Toggle                Skip");
}

void scanWifiNetworks() {
  wifiNetworkCount = 0;
  wifiSelectedNetworkIdx = 0;
  wifiStatusMessage = "Scanning...";
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    wifiStatusMessage = "No networks found";
    wifiNetworkCount = 0;
    return;
  }
  wifiNetworkCount = min(n, MAX_WIFI_NETWORKS);
  for (int i = 0; i < wifiNetworkCount; i++) {
    wifiNetworkNames[i] = WiFi.SSID(i);
    wifiNetworkOpen[i] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    wifiNetworkRssi[i] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  wifiStatusMessage = "Select network";
}

void connectToSelectedNetwork() {
  if (wifiSelectedNetworkIdx < 0 || wifiSelectedNetworkIdx >= wifiNetworkCount) {
    wifiStatusMessage = "No network selected";
    return;
  }
  String ssid = wifiNetworkNames[wifiSelectedNetworkIdx];
  bool open = wifiNetworkOpen[wifiSelectedNetworkIdx];
  if (!open && ssid != WIFI_SSID) {
    wifiStatusMessage = "Locked network";
    return;
  }
  wifiStatusMessage = "Connecting...";
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  if (open) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  unsigned long start = millis();
  while (millis() - start < 10000) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiEnabled = true;
    wifiStatusMessage = String("Connected ") + ssid;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "IST-5:30", 1);
    tzset();
  } else {
    wifiConnected = false;
    wifiStatusMessage = "Connect failed";
  }
}

void redrawWifiApp() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 20, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(24, 3);
  tft.print("WiFi Control");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 30);
  if (wifiConnected) {
    tft.print("Status: Connected");
  } else if (wifiEnabled) {
    tft.print("Status: Enabled");
  } else {
    tft.print("Status: Disabled");
  }

  tft.setCursor(10, 45);
  if (wifiConnected) {
    tft.print(WiFi.SSID());
  } else {
    tft.print(wifiStatusMessage);
  }

  tft.setCursor(10, 65);
  tft.print("ENTER=Toggle WiFi");
  tft.setCursor(10, 78);
  tft.print("MENU=Scan networks");
  tft.setCursor(10, 91);
  tft.print("BACK=Apps");
}

void redrawWifiNetworkList() {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 20, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 3);
  tft.print("WiFi Networks");

  if (wifiNetworkCount == 0) {
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 35);
    tft.print("No networks found");
  } else {
    int y = 30;
    for (int i = 0; i < wifiNetworkCount && y < SCREEN_H - 20; i++) {
      if (i == wifiSelectedNetworkIdx) {
        tft.fillRect(2, y - 2, SCREEN_W - 4, 12, ST77XX_CYAN);
        tft.setTextColor(ST77XX_BLACK);
      } else {
        tft.setTextColor(ST77XX_WHITE);
      }
      tft.setCursor(8, y);
      tft.print(wifiNetworkNames[i]);
      tft.setCursor(96, y);
      tft.print(wifiNetworkOpen[i] ? "OPEN" : "LOCK");
      y += 12;
    }
  }

  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("MENU=Rescan");
}

void redrawAppMenu() {
  tft.fillScreen(ST77XX_BLACK);
  
  // Title bar
  tft.fillRect(0, 0, SCREEN_W, 20, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(65, 5);
  tft.print("APPS");
  
  // Draw app items
  tft.setTextSize(1);
  for (int i = 0; i < numApps; i++) {
    int y = 40 + (i * 30);
    if (i == selectedApp) {
      tft.fillRect(5, y, SCREEN_W - 10, 20, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else {
      tft.setTextColor(ST77XX_WHITE);
    }
    tft.setCursor(20, y + 6);
    tft.print(apps[i]);
    tft.setTextColor(ST77XX_WHITE);
  }
  
  // Bottom hint bar
  tft.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("Open                  Home");
}

// ── Redraw full display ────────────────────────────────
void redrawDisplay() {
  switch(currentScreen) {
    case WIFI_PROMPT:
      redrawWifiPrompt();
      break;
    case HOME_SCREEN:
      redrawHomeScreen();
      break;
    case APP_MENU:
      redrawAppMenu();
      break;
    case WIFI_APP:
      redrawWifiApp();
      break;
    case WIFI_NETWORK_LIST:
      redrawWifiNetworkList();
      break;
    case NOTEPAD_LIST:
      redrawNoteList();
      break;
    case NOTEPAD_VIEW:
      redrawNoteView();
      break;
    case NOTEPAD_EDIT:
      redrawNoteEdit();
      break;
    case NOTEPAD_MENU:
      redrawNoteMenu();
      break;
  }
}

// ── Commit pending char to edit buffer ──────────────
void commitPending() {
  if (pendingKey < 0) return;
  if (editBufLen < NOTE_CONTENT_LEN) {
    memmove(&editBuffer[editCursorPos + 1], &editBuffer[editCursorPos], editBufLen - editCursorPos + 1);
    editBuffer[editCursorPos] = keyMap[pendingKey][pendingIdx];
    editBufLen++;
    editCursorPos++;
  }
  pendingKey = -1;
  pendingIdx = 0;
}

// ── Handle a key press ────────────────────────────────
void handleKey(int keyIndex) {
  // Only handle in edit mode
  if (currentScreen != NOTEPAD_EDIT) return;
  
  unsigned long now = millis();

  if (pendingKey == keyIndex && (now - lastKeyTime) < COMMIT_DELAY) {
    // Same key, fast enough → cycle to next char
    pendingIdx = (pendingIdx + 1) % strlen(keyMap[keyIndex]);
  } else {
    // Different key or too slow → commit previous, start new
    commitPending();
    pendingKey = keyIndex;
    pendingIdx = 0;
  }

  lastKeyTime = now;
  redrawDisplay();
}

// ── Setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_BACK,  INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_MENU,  INPUT_PULLUP);
  pinMode(BUZZER,    OUTPUT);

  // VSPI for MCP
  SPI.begin(18, 19, 23, 22);
  if (!MCP.begin()) {
    Serial.println("MCP23S17 not found!");
    while (1) delay(1000);
  }
  Serial.println("MCP23S17 OK");

  // HSPI for TFT
  hspi.begin(14, 12, 13, 32);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);

  // Initialize notes
  initializeNotes();

  lastClockUpdate = millis();

  // Start on WiFi prompt
  currentScreen = WIFI_PROMPT;
  redrawDisplay();
  Serial.println("Ready.");
}

// ── Main loop ─────────────────────────────────────────
uint16_t lastMCPState = 0xFFFF;
bool lastBack         = HIGH;
bool lastEnter        = HIGH;
bool lastMenu         = HIGH;

void loop() {
  unsigned long now = millis();

  // Auto-commit if key held longer than COMMIT_DELAY (only in edit mode)
  if (currentScreen == NOTEPAD_EDIT && pendingKey >= 0 && (now - lastKeyTime) >= COMMIT_DELAY) {
    commitPending();
    redrawDisplay();
  }

  // Update clock every minute on home screen
  if (currentScreen == HOME_SCREEN && (now - lastClockUpdate) >= 60000) {
    redrawHomeScreen();
    lastClockUpdate = now;
  }

  // ── MCP buttons (detect release) ──
  uint16_t mcpState = MCP.read16();
  if (mcpState != lastMCPState) {
    for (int i = 0; i < 16; i++) {
      bool wasPressed = !(lastMCPState & (1 << i));
      bool isPressed  = !(mcpState    & (1 << i));

      if (wasPressed && !isPressed) {
        // Released
        int keyIdx = mcpPinToKeyIndex(i);
        if (keyIdx >= 0) {
          beep();
          handleKey(keyIdx);
          Serial.print("Key: ");
          Serial.println(keyMap[keyIdx]);
        } else if (currentScreen == NOTEPAD_EDIT) {
          // Navigation keys (only in edit mode)
          beep();
          switch(i) {
            case 4: // GPA4 = DOWN
              moveEditCursorDown();
              break;
            case 5: // GPA5 = LEFT
              if (editCursorPos > 0) editCursorPos--;
              break;
            case 6: // GPA6 = RIGHT
              if (editCursorPos < editBufLen) editCursorPos++;
              break;
            case 7: // GPA7 = UP
              moveEditCursorUp();
              break;
          }
          redrawDisplay();
        } else if (currentScreen == APP_MENU) {
          // Navigation keys (only in app menu)
          beep();
          switch(i) {
            case 4: // GPA4 = DOWN
              selectedApp = (selectedApp + 1) % numApps;
              break;
            case 7: // GPA7 = UP
              selectedApp = (selectedApp - 1 + numApps) % numApps;
              break;
          }
          redrawDisplay();
        } else if (currentScreen == WIFI_NETWORK_LIST) {
          // Navigation keys in WiFi network list
          beep();
          switch(i) {
            case 4: // GPA4 = DOWN
              wifiSelectedNetworkIdx = (wifiSelectedNetworkIdx + 1) % wifiNetworkCount;
              break;
            case 7: // GPA7 = UP
              wifiSelectedNetworkIdx = (wifiSelectedNetworkIdx - 1 + wifiNetworkCount) % wifiNetworkCount;
              break;
          }
          redrawDisplay();
        } else if (currentScreen == NOTEPAD_LIST) {
          // Navigation keys in note list
          beep();
          switch(i) {
            case 4: // GPA4 = DOWN
              selectedNoteIdx = (selectedNoteIdx - 1) % (totalNotes + 1);
              if (selectedNoteIdx <0) selectedNoteIdx = totalNotes; 
              break;
            case 7: // GPA7 = UP
              selectedNoteIdx = (selectedNoteIdx + 1) % (totalNotes + 1);
              break;
          }
          redrawDisplay();
        } else if (currentScreen == NOTEPAD_MENU) {
          // Navigation keys in note menu
          beep();
          switch(i) {
            case 4: // GPA4 = DOWN
              menuSelectedIdx = (menuSelectedIdx + 1) % 4;
              break;
            case 7: // GPA7 = UP
              menuSelectedIdx = (menuSelectedIdx - 1 + 4) % 4;
              break;
          }
          redrawDisplay();
        }
      }
    }
    lastMCPState = mcpState;
  }

  // ── Direct buttons (detect release) ──
  bool curBack  = digitalRead(BTN_BACK);
  bool curEnter = digitalRead(BTN_ENTER);
  bool curMenu  = digitalRead(BTN_MENU);


  if (lastBack == LOW && curBack == HIGH) {
    beep(80);
    switch(currentScreen) {
      case WIFI_PROMPT:
        wifiEnabled = false;
        wifiConnected = false;
        currentScreen = HOME_SCREEN;
        redrawDisplay();
        break;
      case HOME_SCREEN:
        break;
      case APP_MENU:
        currentScreen = HOME_SCREEN;
        selectedApp = 0;
        redrawDisplay();
        break;
      case WIFI_APP:
        currentScreen = APP_MENU;
        redrawDisplay();
        break;
      case WIFI_NETWORK_LIST:
        currentScreen = WIFI_APP;
        redrawDisplay();
        break;
      case NOTEPAD_LIST:
        currentScreen = HOME_SCREEN;
        redrawDisplay();
        break;
      case NOTEPAD_VIEW:
        currentScreen = NOTEPAD_LIST;
        redrawDisplay();
        break;
      case NOTEPAD_EDIT:
        // Discard changes
        commitPending();
        if (editCursorPos > 0) {
          editCursorPos--;
          memmove(&editBuffer[editCursorPos], &editBuffer[editCursorPos + 1], editBufLen - editCursorPos);
          editBufLen--;
        }else{
          currentScreen = NOTEPAD_VIEW;
        }
        redrawDisplay();
        break;
      case NOTEPAD_MENU:
        currentScreen = NOTEPAD_LIST;
        menuSelectedIdx = 0;
        redrawDisplay();
        break;
    }
  }

  if (lastEnter == LOW && curEnter == HIGH) {
    beep(50);
    switch(currentScreen) {
      case WIFI_PROMPT:
        wifiEnabled = (wifiPromptOption == 0);
        if (wifiEnabled) {
          wifiConnected = connectWiFi();
        }
        currentScreen = HOME_SCREEN;
        redrawDisplay();
        break;
      case HOME_SCREEN:
        break;
      case APP_MENU:
        if (selectedApp == 0) {
          currentScreen = NOTEPAD_LIST;
          selectedNoteIdx = totalNotes;
        }else if(selectedApp == 1){
          currentScreen = WIFI_APP;
        }
        redrawDisplay();
        break;
      case WIFI_APP:
        // Toggle WiFi
        if (wifiEnabled) {
          WiFi.disconnect(true);
          wifiEnabled = false;
          wifiConnected = false;
          wifiStatusMessage = "Disabled";
        } else {
          wifiEnabled = true;
          wifiStatusMessage = "Enabled";
          // Try to connect to default network
          if (connectWiFi()) {
            wifiStatusMessage = "Connected";
          } else {
            wifiStatusMessage = "Connect failed";
          }
        }
        redrawDisplay();
        break;
      case WIFI_NETWORK_LIST:
        connectToSelectedNetwork();
        redrawDisplay();
        break;
      case NOTEPAD_LIST:
        if (selectedNoteIdx == totalNotes) {
          // Create new note
          createNewNote();
          strcpy(editBuffer, "");
          editBufLen = 0;
          editCursorPos = 0;
          currentScreen = NOTEPAD_EDIT;
        } else if (selectedNoteIdx >= 0 && selectedNoteIdx < totalNotes) {
          // Open note in view mode
          currentScreen = NOTEPAD_VIEW;
        }
        redrawDisplay();
        break;
      case NOTEPAD_VIEW:
        break;
      case NOTEPAD_EDIT:
        // ENTER = insert newline
        commitPending();
        if (editBufLen < NOTE_CONTENT_LEN) {
          memmove(&editBuffer[editCursorPos + 1], &editBuffer[editCursorPos], editBufLen - editCursorPos + 1);
          editBuffer[editCursorPos] = '\n';
          editBufLen++;
          editCursorPos++;
        }
        redrawDisplay();
        break;
      case NOTEPAD_MENU:
        // Execute selected menu option
        switch(menuSelectedIdx) {
          case 0: // View
            currentScreen = NOTEPAD_VIEW;
            break;
          case 1: // Edit
            strcpy(editBuffer, notes[selectedNoteIdx].content);
            editBufLen = notes[selectedNoteIdx].contentLen;
            editCursorPos = 0;
            pendingKey = -1;
            currentScreen = NOTEPAD_EDIT;
            break;
          case 2: // Rename (placeholder)
            currentScreen = NOTEPAD_LIST;
            break;
          case 3: // Delete
            deleteNote(selectedNoteIdx);
            currentScreen = NOTEPAD_LIST;
            break;
        }
        menuSelectedIdx = 0;
        redrawDisplay();
        break;
    }
  }

  if (lastMenu == LOW && curMenu == HIGH) {
    beep(30);
    switch(currentScreen) {
      case WIFI_PROMPT:
        wifiPromptOption = (wifiPromptOption + 1) % 2;
        redrawDisplay();
        break;
      case HOME_SCREEN:
        currentScreen = APP_MENU;
        selectedApp = 0;
        redrawDisplay();
        break;
      case APP_MENU:
        selectedApp = (selectedApp + 1) % numApps;
        redrawDisplay();
        break;
      case WIFI_APP:
        scanWifiNetworks();
        currentScreen = WIFI_NETWORK_LIST;
        redrawDisplay();
        break;
      case WIFI_NETWORK_LIST:
        scanWifiNetworks();
        redrawDisplay();
        break;
      case NOTEPAD_LIST:
        redrawDisplay();
        break;
      case NOTEPAD_VIEW:
        currentScreen = NOTEPAD_MENU;
        menuSelectedIdx = 0;
        redrawDisplay();
        break;
      case NOTEPAD_EDIT:
        // MENU = save note
        saveNote(selectedNoteIdx);
        editBufLen = 0;
        editCursorPos = 0;
        pendingKey = -1;
        currentScreen = NOTEPAD_VIEW;
        redrawDisplay();
        break;
      case NOTEPAD_MENU:
        menuSelectedIdx = (menuSelectedIdx + 1) % 4;
        redrawDisplay();
        break;
    }
  }

  lastBack  = curBack;
  lastEnter = curEnter;
  lastMenu  = curMenu;

  delay(10);
}

