#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "MCP23S17.h"

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

// ── Display / text state ───────────────────────────────
#define MAX_LEN       100
#define COMMIT_DELAY  600   // ms before character is committed
#define TEXT_X        4
#define TEXT_Y        4
#define CHAR_W        6     // default font width
#define CHAR_H        8     // default font height
#define LINE_H        10
#define SCREEN_W      128
#define SCREEN_H      160
#define CHARS_PER_LINE ((SCREEN_W - TEXT_X * 2) / CHAR_W)  // ~20

char    inputBuf[MAX_LEN + 1] = "";  // committed text
int     bufLen       = 0;
int     pendingKey   = -1;           // keyMap index of key being cycled
int     pendingIdx   = 0;            // which char in keyMap[pendingKey] is showing
unsigned long lastKeyTime = 0;

void beep(int ms = 30) {
  digitalWrite(BUZZER, HIGH);
  delay(ms);
  digitalWrite(BUZZER, LOW);
}

// ── Redraw full display ────────────────────────────────
void redrawDisplay() {
  tft.fillScreen(ST77XX_BLACK);

  // Title bar
  tft.fillRect(0, 0, SCREEN_H, 12, ST77XX_BLUE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(4, 2);
  tft.print("         Notepad");

  // Text area
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);

  // Print committed buffer
  int x = TEXT_X, y = 16;
  for (int i = 0; i < bufLen; i++) {
    if (inputBuf[i] == '\n') {
      x = TEXT_X;
      y += LINE_H;
    } else {
      tft.setCursor(x, y);
      tft.print(inputBuf[i]);
      x += CHAR_W;
      if (x + CHAR_W > SCREEN_W - TEXT_X) {
        x = TEXT_X;
        y += LINE_H;
      }
    }
  }

  // Print pending character (highlighted)
  if (pendingKey >= 0) {
    char pending = keyMap[pendingKey][pendingIdx];
    tft.fillRect(x, y, CHAR_W, CHAR_H, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(x, y);
    tft.print(pending);
    tft.setTextColor(ST77XX_WHITE);
    x += CHAR_W;
  }

  // Cursor
  tft.fillRect(x, y + CHAR_H - 1, CHAR_W, 1, ST77XX_GREEN);

  // Bottom hint bar
  tft.fillRect(0, SCREEN_W - 12, SCREEN_H, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, SCREEN_H - 10);
  tft.print("MENU=del BACK=clr");
}

// ── Commit pending char to buffer ─────────────────────
void commitPending() {
  if (pendingKey < 0) return;
  if (bufLen < MAX_LEN) {
    inputBuf[bufLen++] = keyMap[pendingKey][pendingIdx];
    inputBuf[bufLen]   = '\0';
  }
  pendingKey = -1;
  pendingIdx = 0;
}

// ── Handle a key press ────────────────────────────────
void handleKey(int keyIndex) {
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

  // Auto-commit if key held longer than COMMIT_DELAY
  if (pendingKey >= 0 && (now - lastKeyTime) >= COMMIT_DELAY) {
    commitPending();
    redrawDisplay();
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
        }
      }
    }
    lastMCPState = mcpState;
  }

  // ── Direct buttons (detect release) ──
  bool curBack  = digitalRead(BTN_BACK);
  bool curEnter = digitalRead(BTN_ENTER);
  bool curMenu  = digitalRead(BTN_MENU);

  if (lastMenu == LOW && curMenu == HIGH) {
    // MENU = delete last character
    beep(30);
    commitPending();
    if (bufLen > 0) {
      bufLen--;
      inputBuf[bufLen] = '\0';
    }
    redrawDisplay();
  }

  if (lastBack == LOW && curBack == HIGH) {
    // BACK = clear all
    beep(80);
    commitPending();
    bufLen = 0;
    inputBuf[0] = '\0';
    redrawDisplay();
  }

  if (lastEnter == LOW && curEnter == HIGH) {
    // ENTER = commit pending + newline (or your custom action)
    beep(50);
    commitPending();
    if (bufLen < MAX_LEN) {
      inputBuf[bufLen++] = '\n';
      // inputBuf[bufLen]   = '\0';
    }
    redrawDisplay();
  }

  lastBack  = curBack;
  lastEnter = curEnter;
  lastMenu  = curMenu;

  delay(10);
}