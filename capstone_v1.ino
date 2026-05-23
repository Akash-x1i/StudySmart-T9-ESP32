// ════════════════════════════════════════════════════════════════════
//  StudySmart ESP32  –  Ask? + Quiz  (StudySmart API v2)
//  Optimised for low RAM: one page / one question in RAM at a time.
//  Streaming JSON parse, JSON filter, no large String response buffers.
// ════════════════════════════════════════════════════════════════════

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "MCP23S17.h"
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_mac.h>

struct QuizResult {
  bool ok;
  int percent;
  char grade[4];
};

// ── Hardware ──────────────────────────────────────────────────────
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

// ── Display constants ────────────────────────────────────────────
#define SCREEN_W        160
#define SCREEN_H        128
#define CHAR_W          6
#define CHAR_H          8
#define LINE_H          10
#define TEXT_X          4
#define TEXT_Y          4
#define CHARS_PER_LINE  ((SCREEN_W - TEXT_X * 2) / CHAR_W)   // 25
#define TITLE_H         12
#define FOOTER_H        12
#define LIST_ITEM_H     12
#define LIST_Y_START    (TITLE_H + 4)
#define LIST_VISIBLE    ((SCREEN_H - TITLE_H - FOOTER_H - 4) / LIST_ITEM_H)

// ── T9 keymap ─────────────────────────────────────────────────────
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
    case 0:  return 9;
    case 1:  return 11;
    case 2:  return 10;
    case 3:  return 8;
    case 8:  return 0;
    case 9:  return 1;
    case 10: return 2;
    case 11: return 3;
    case 12: return 4;
    case 13: return 5;
    case 14: return 6;
    case 15: return 7;
    default: return -1;
  }
}

#define NAV_DOWN   4
#define NAV_LEFT   5
#define NAV_RIGHT  6
#define NAV_UP     7

// ── Shift / caps state ───────────────────────────────────────────
enum ShiftState { SHIFT_OFF, SHIFT_ONCE, SHIFT_CAPS };
ShiftState shiftState = SHIFT_OFF;

char applyShift(char c, bool consume = true) {
  if (shiftState == SHIFT_OFF) return c;
  char out = (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (consume && shiftState == SHIFT_ONCE) shiftState = SHIFT_OFF;
  return out;
}

// ── MCP hold detection ───────────────────────────────────────────
#define MCP_HOLD_MS  400
#define MCP_STAR_PIN 2
unsigned long mcpPressTime[16];
bool          mcpHoldFired[16];

// ── T9 pending-key state ─────────────────────────────────────────
#define COMMIT_DELAY_MS 600
int           pendingKey  = -1;
int           pendingIdx  = 0;
unsigned long lastKeyTime = 0;

// ════════════════════════════════════════════════════════════════════
//  SCREEN STATE MACHINE
// ════════════════════════════════════════════════════════════════════
enum ScreenState {
  SCREEN_WIFI_PROMPT,
  SCREEN_HOME,
  SCREEN_APP_MENU,
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
  // ── Study AI ──────────────────────────
  SCREEN_STUDY_HOME,    // "Ask?" / "Quiz" / "Settings"
  SCREEN_STUDY_ASK,     // text input for question
  SCREEN_STUDY_ANSWER,  // paginated answer display
  SCREEN_STUDY_QUIZ,    // text input for quiz topic
  SCREEN_STUDY_QUESTION,// one question at a time
  SCREEN_STUDY_RESULT,  // final score
  SCREEN_STUDY_HEALTH,  // API health result
  SCREEN_STUDY_LOADING, // spinner
  SCREEN_STUDY_SETTINGS,// API base editor
};

ScreenState currentScreen = SCREEN_WIFI_PROMPT;
bool        needsRedraw   = true;

#define gotoScreen(s)  do { currentScreen = (s); needsRedraw = true; } while(0)

// ════════════════════════════════════════════════════════════════════
//  GENERIC TEXT INPUT
// ════════════════════════════════════════════════════════════════════
#define TI_MAX_LEN 80

struct TextInputCtx {
  char   title[24];
  char   buf[TI_MAX_LEN + 1];
  int    len;
  int    cursor;
  bool   password;
  void (*onCommit)(const char* text);
  void (*onCancel)();
};
TextInputCtx ti;

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
  pendingKey  = -1;
  pendingIdx  = 0;
  lastKeyTime = 0;
  shiftState  = SHIFT_OFF;
}

// ── T9 helpers ───────────────────────────────────────────────────
void commitPendingTo(char* buf, int& len, int& cursor, int maxLen) {
  if (pendingKey < 0) return;
  if (len < maxLen) {
    memmove(&buf[cursor + 1], &buf[cursor], len - cursor + 1);
    buf[cursor] = applyShift(KEY_MAP[pendingKey][pendingIdx], true);
    len++; cursor++;
  }
  pendingKey = -1; pendingIdx = 0;
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

bool backspaceAt(char* buf, int& len, int& cursor) {
  if (cursor == 0) return false;
  memmove(&buf[cursor - 1], &buf[cursor], len - cursor + 1);
  len--; cursor--;
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  BUTTON HANDLER
// ════════════════════════════════════════════════════════════════════
struct Btn { int pin; bool last = HIGH; };
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
char   wifiPassword[65] = "";
int    wifiPromptSel = 0;

String  wifiNetNames[MAX_WIFI_NETWORKS];
bool    wifiNetOpen [MAX_WIFI_NETWORKS];
int     wifiNetRssi [MAX_WIFI_NETWORKS];
int     wifiNetCount = 0;
int     wifiNetSel   = 0;
int     wifiNetScroll= 0;
String  wifiStatus   = "";

// Device ID derived from MAC
char deviceId[18] = "";

void initDeviceId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceId, sizeof(deviceId),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

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
  wifiNetCount = 0; wifiNetSel = 0; wifiNetScroll = 0;
  wifiStatus = "Scanning...";
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

// ── HTTP JSON helpers ─────────────────────────────────────────────
//  Returns http status code, -1 on connection failure.
//  Caller provides a DeserializationOption::Filter to keep RAM low.
int httpPostJsonStream(const char* url,
                       const String& payload,
                       JsonDocument& doc,
                       const JsonDocument* filter = nullptr) {
  if (!wifiConnected) return -100;
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    return -101;
  }

  HTTPClient http;
  Serial.print("POST ");
  Serial.println(url);
  http.begin(url);
  http.setTimeout(60000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", "StudySmartESP32/1.0");

  int code = http.POST(payload);
  if (code <= 0) {
    Serial.print("HTTP POST error: ");
    Serial.println(http.errorToString(code));
    http.end();
    return code;
  }

  String response = http.getString();
  if (code < 200 || code > 299) {
    Serial.print("HTTP POST status: ");
    Serial.println(code);
    Serial.print("Raw response: ");
    Serial.println(response.substring(0, 180));
    http.end();
    return code;
  }

  DeserializationError err;
  if (filter) {
    err = deserializeJson(doc, response,
                          DeserializationOption::Filter(*filter));
  } else {
    err = deserializeJson(doc, response);
  }

  http.end();
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    Serial.print("Raw response: ");
    Serial.println(response.substring(0, 180));
    return -2;
  }
  return code;
}

int httpGetJsonStream(const char* url,
                      JsonDocument& doc,
                      const JsonDocument* filter = nullptr) {
  if (!wifiConnected) return -100;
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    return -101;
  }

  HTTPClient http;
  Serial.print("GET ");
  Serial.println(url);
  http.begin(url);
  http.setTimeout(15000);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", "StudySmartESP32/1.0");

  int code = http.GET();
  if (code <= 0) {
    Serial.print("HTTP GET error: ");
    Serial.println(http.errorToString(code));
    http.end();
    return code;
  }

  String response = http.getString();
  if (code < 200 || code > 299) {
    Serial.print("HTTP GET status: ");
    Serial.println(code);
    Serial.print("Raw response: ");
    Serial.println(response.substring(0, 180));
    http.end();
    return code;
  }

  DeserializationError err;
  if (filter) {
    err = deserializeJson(doc, response,
                          DeserializationOption::Filter(*filter));
  } else {
    err = deserializeJson(doc, response);
  }

  http.end();
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    Serial.print("Raw response: ");
    Serial.println(response.substring(0, 180));
    return -2;
  }
  return code;
}

// ════════════════════════════════════════════════════════════════════
//  STUDY AI STATE  –  minimal RAM footprint
// ════════════════════════════════════════════════════════════════════
char studyApiBase[96] = "https://studysmart-api-kray.onrender.com";

// ── Ask? state ───────────────────────────────────────────────────
#define ASK_PAGE_LEN  240   // chars per page (server enforces ~220)
#define ASK_ID_LEN    24

char askAnswerId[ASK_ID_LEN]  = "";
int  askTotalPages            = 0;
int  askCurrentPage           = 0;
char askPageText[ASK_PAGE_LEN]= "";
bool askHasMore               = false;

// Scroll within the current page (lines)
int  askLineScroll            = 0;
#define ASK_VISIBLE_LINES  ((SCREEN_H - TITLE_H - FOOTER_H - 4) / LINE_H)

// ── Quiz state ───────────────────────────────────────────────────
#define QUIZ_ID_LEN  24

char quizId[QUIZ_ID_LEN]        = "";
int  quizTotal                  = 0;
int  quizIndex                  = 0;
int  quizScore                  = 0;
int  quizSelected               = 0;    // highlighted option 0-3
bool quizAnswered               = false;// true after ENTER pressed
bool quizWasCorrect             = false;
char quizTopic[TI_MAX_LEN + 1]  = "";

char quizQuestionText[128]      = "";
char quizOptions[4][48]         = {};
int  quizCorrectIndex           = 0;
char quizExplanation[128]       = "";

// Study home menu
int  studyHomeSel               = 0;    // 0=Ask? 1=Quiz 2=Settings
int  studySettingSel            = 0;

// Error message (shown briefly on SCREEN_STUDY_LOADING on failure)
char studyError[48]             = "";
char studyHealthText[80]        = "";
int  studyHealthCode            = 0;

void buildStudyUrl(char* out, size_t outLen, const char* path) {
  size_t baseLen = strlen(studyApiBase);
  while (baseLen > 0 && studyApiBase[baseLen - 1] == '/') baseLen--;
  if (path[0] == '/') path++;
  snprintf(out, outLen, "%.*s/%s", (int)baseLen, studyApiBase, path);
}

void setStudyHttpError(const char* action, int code) {
  if (code == -100) {
    strlcpy(studyError, "WiFi flag is off.", sizeof(studyError));
  } else if (code == -101) {
    strlcpy(studyError, "WiFi dropped.", sizeof(studyError));
  } else if (code == -2) {
    strlcpy(studyError, "Bad JSON from API.", sizeof(studyError));
  } else if (code < 0) {
    snprintf(studyError, sizeof(studyError), "%s HTTP %d", action, code);
  } else {
    snprintf(studyError, sizeof(studyError), "%s HTTP %d", action, code);
  }
  Serial.print(action);
  Serial.print(" failed, code=");
  Serial.print(code);
  Serial.print(", api=");
  Serial.println(studyApiBase);
}

bool isRetryableHttpCode(int code) {
  return code == 502 || code == 503 || code == 504;
}

bool studyHealthCheck() {
  char url[120];
  buildStudyUrl(url, sizeof(url), "/api/health");

  StaticJsonDocument<256> doc;
  int code = httpGetJsonStream(url, doc);
  studyHealthCode = code;

  if (code < 200 || code > 299) {
    setStudyHttpError("Health", code);
    snprintf(studyHealthText, sizeof(studyHealthText), "%s", studyError);
    return false;
  }

  bool hasOk = !doc["ok"].isNull();
  bool hasVersion = !doc["v"].isNull();
  bool hasGemini = !doc["gemini"].isNull();

  bool ok = doc["ok"] | false;
  int version = doc["v"] | 0;
  bool gemini = doc["gemini"] | false;
  if (!hasOk && !hasVersion && !hasGemini) {
    snprintf(studyHealthText, sizeof(studyHealthText), "HTTP OK, no fields");
    Serial.println("Health reached API, but expected JSON fields were missing.");
    return true;
  }

  snprintf(studyHealthText, sizeof(studyHealthText),
           "OK:%s v:%d Gemini:%s",
           ok ? "yes" : "no",
           version,
           gemini ? "yes" : "no");
  Serial.print("Health OK: ");
  Serial.println(studyHealthText);
  return ok;
}

// ════════════════════════════════════════════════════════════════════
//  STUDY API CALLS
// ════════════════════════════════════════════════════════════════════

// -- Ask: POST /api/ask  → stores answerId + page 1 text -----------
bool studyAsk(const char* question) {
  char url[120];
  buildStudyUrl(url, sizeof(url), "/api/ask");

  // Build payload
  StaticJsonDocument<256> req;
  req["question"] = question;
  req["deviceId"] = deviceId;
  String payload;
  serializeJson(req, payload);

  // Filter: only keep fields we use
  StaticJsonDocument<128> filter;
  filter["answerId"]    = true;
  filter["totalPages"]  = true;
  filter["page"]        = true;
  filter["text"]        = true;
  filter["hasMore"]     = true;

  StaticJsonDocument<512> doc;
  int code = httpPostJsonStream(url, payload, doc, &filter);
  if (isRetryableHttpCode(code)) {
    strlcpy(studyError, "Server waking. Retry...", sizeof(studyError));
    redrawDisplay();
    delay(2500);
    doc.clear();
    code = httpPostJsonStream(url, payload, doc, &filter);
  }

  if (code < 200 || code > 299) {
    setStudyHttpError("Ask", code);
    return false;
  }

  strlcpy(askAnswerId,  doc["answerId"] | "", ASK_ID_LEN);
  askTotalPages  = doc["totalPages"] | 1;
  askCurrentPage = doc["page"]       | 1;
  strlcpy(askPageText, doc["text"]   | "", ASK_PAGE_LEN);
  askHasMore     = doc["hasMore"]    | false;
  askLineScroll  = 0;

  return true;
}

// -- Ask: GET /api/ask/:id/page/:n  → update page text ------------
bool studyAskPage(int pageNum) {
  char url[160];
  char path[80];
  snprintf(path, sizeof(path), "/api/ask/%s/page/%d", askAnswerId, pageNum);
  buildStudyUrl(url, sizeof(url), path);

  StaticJsonDocument<128> filter;
  filter["page"]       = true;
  filter["text"]       = true;
  filter["hasMore"]    = true;
  filter["totalPages"] = true;

  StaticJsonDocument<512> doc;
  int code = httpGetJsonStream(url, doc, &filter);

  if (code < 200 || code > 299) {
    setStudyHttpError("Page", code);
    return false;
  }

  askCurrentPage = doc["page"]       | pageNum;
  strlcpy(askPageText, doc["text"]   | "", ASK_PAGE_LEN);
  askHasMore     = doc["hasMore"]    | false;
  askTotalPages  = doc["totalPages"] | askTotalPages;
  askLineScroll  = 0;

  return true;
}

// -- Quiz: POST /api/quiz/generate  → stores quizId + question 0 --
bool studyQuizGenerate(const char* topic, int count) {
  char url[120];
  buildStudyUrl(url, sizeof(url), "/api/quiz/generate");

  StaticJsonDocument<256> req;
  req["topic"]    = topic;
  req["count"]    = count;
  req["deviceId"] = deviceId;
  String payload;
  serializeJson(req, payload);

  // Filter keeps only the fields we actually read
  StaticJsonDocument<256> filter;
  filter["quizId"]            = true;
  filter["totalQuestions"]    = true;
  filter["question"]["index"] = true;
  filter["question"]["text"]  = true;
  filter["question"]["options"][0] = true;
  filter["question"]["correctIndex"] = true;
  filter["question"]["explanation"]  = true;

  StaticJsonDocument<1024> doc;
  int code = httpPostJsonStream(url, payload, doc, &filter);
  if (isRetryableHttpCode(code)) {
    strlcpy(studyError, "Server waking. Retry...", sizeof(studyError));
    redrawDisplay();
    delay(2500);
    doc.clear();
    code = httpPostJsonStream(url, payload, doc, &filter);
  }

  if (code < 200 || code > 299) {
    setStudyHttpError("Quiz gen", code);
    return false;
  }

  strlcpy(quizId, doc["quizId"] | "", QUIZ_ID_LEN);
  quizTotal    = doc["totalQuestions"] | count;
  quizIndex    = 0;
  quizScore    = 0;
  quizSelected = 0;
  quizAnswered = false;
  strlcpy(quizTopic, topic, sizeof(quizTopic));

  JsonObject q = doc["question"];
  strlcpy(quizQuestionText, q["text"] | "", sizeof(quizQuestionText));
  JsonArray opts = q["options"];
  for (int i = 0; i < 4; i++)
    strlcpy(quizOptions[i], opts[i] | "", sizeof(quizOptions[i]));
  quizCorrectIndex = q["correctIndex"] | 0;
  strlcpy(quizExplanation, q["explanation"] | "", sizeof(quizExplanation));

  return true;
}

// -- Quiz: GET /api/quiz/:id/question/:n --------------------------
bool studyQuizQuestion(int index) {
  char url[160];
  char path[80];
  snprintf(path, sizeof(path), "/api/quiz/%s/question/%d", quizId, index);
  buildStudyUrl(url, sizeof(url), path);

  StaticJsonDocument<256> filter;
  filter["question"]["index"]       = true;
  filter["question"]["text"]        = true;
  filter["question"]["options"][0]  = true;
  filter["question"]["correctIndex"]= true;
  filter["question"]["explanation"] = true;

  StaticJsonDocument<1024> doc;
  int code = httpGetJsonStream(url, doc, &filter);

  if (code < 200 || code > 299) {
    setStudyHttpError("Question", code);
    return false;
  }

  JsonObject q = doc["question"];
  strlcpy(quizQuestionText, q["text"] | "", sizeof(quizQuestionText));
  JsonArray opts = q["options"];
  for (int i = 0; i < 4; i++)
    strlcpy(quizOptions[i], opts[i] | "", sizeof(quizOptions[i]));
  quizCorrectIndex = q["correctIndex"] | 0;
  strlcpy(quizExplanation, q["explanation"] | "", sizeof(quizExplanation));

  quizSelected = 0;
  quizAnswered = false;

  return true;
}

// -- Quiz: POST /api/quiz/:id/submit ------------------------------
QuizResult studyQuizSubmit() {
  QuizResult res = { false, 0, "?" };

  char url[160];
  char path[80];
  snprintf(path, sizeof(path), "/api/quiz/%s/submit", quizId);
  buildStudyUrl(url, sizeof(url), path);

  StaticJsonDocument<256> req;
  req["deviceId"]       = deviceId;
  req["score"]          = quizScore;
  req["totalQuestions"] = quizTotal;
  req["topic"]          = quizTopic;
  String payload;
  serializeJson(req, payload);

  StaticJsonDocument<64> filter;
  filter["ok"]      = true;
  filter["percent"] = true;
  filter["grade"]   = true;

  StaticJsonDocument<128> doc;
  int code = httpPostJsonStream(url, payload, doc, &filter);

  if (code >= 200 && code < 300) {
    res.ok      = doc["ok"] | false;
    res.percent = doc["percent"] | 0;
    strlcpy(res.grade, doc["grade"] | "?", sizeof(res.grade));
  }
  return res;
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
int  totalNotes  = 0;
int  noteSelIdx  = 0;
int  noteScroll  = 0;
int  noteMenuSel = 0;

char editBuf[NOTE_CONTENT_LEN + 1] = "";
int  editLen    = 0;
int  editCursor = 0;

void notesInit() {
  strcpy(notes[0].name,    "Welcome");
  strcpy(notes[0].content, "Welcome to StudySmart!\nUse Menu for options.");
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
  int cur = editLineStart(editCursor);
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
//  TODO
// ════════════════════════════════════════════════════════════════════
#define MAX_TODOS  20
#define TODO_LEN   40

struct Todo { char text[TODO_LEN + 1]; bool done; };
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
struct Settings { char timezone[32]; bool autoWifi; bool soundEnabled; };
Settings cfg = { "IST-5:30", false, true };
const int numSettings = 3;
int       settingSel  = 0;

// ════════════════════════════════════════════════════════════════════
//  APP REGISTRY
// ════════════════════════════════════════════════════════════════════
struct AppEntry { const char* name; ScreenState screen; };
const AppEntry APPS[] = {
  { "Study AI", SCREEN_STUDY_HOME },
  { "Notepad",  SCREEN_NOTE_LIST  },
  { "Todo",     SCREEN_TODO_LIST  },
  { "WiFi",     SCREEN_WIFI_APP   },
  { "Settings", SCREEN_SETTINGS   },
};
const int NUM_APPS  = 5;
int       appSelIdx = 0;
int       appScroll = 0;

// ════════════════════════════════════════════════════════════════════
//  DRAW HELPERS
// ════════════════════════════════════════════════════════════════════
void beep(int ms = 30) {
  if (!cfg.soundEnabled) return;
  digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW);
}

void drawTitleBar(const char* title, uint16_t bg = ST77XX_BLUE) {
  tft.fillRect(0, 0, SCREEN_W, TITLE_H, bg);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  int len = strlen(title);
  int x   = max(0, (SCREEN_W - len * CHAR_W) / 2);
  tft.setCursor(x, 2);
  tft.print(title);
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

void drawList(const char** items, int n, int selected, int& scroll,
              bool showCreate = false) {
  if (selected < scroll) scroll = selected;
  if (selected >= scroll + LIST_VISIBLE) scroll = selected - LIST_VISIBLE + 1;
  tft.setTextSize(1);
  int y = LIST_Y_START;
  if (showCreate) {
    if (selected == n) {
      tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else { tft.setTextColor(0x07E0); }
    tft.setCursor(6, y + 1); tft.print("+ New");
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  for (int i = scroll; i < n && y < SCREEN_H - FOOTER_H; i++) {
    bool sel = (i == selected);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(6, y + 1);
    char tmp[24]; strncpy(tmp, items[i], 23); tmp[23] = '\0';
    tft.print(tmp);
    tft.setTextColor(ST77XX_WHITE);
    y += LIST_ITEM_H;
  }
}

// Wrap and draw plain text, starting at visual line lineOffset
// Returns number of visual lines drawn.
int drawWrappedText(const char* text, int startX, int startY,
                    int maxWidth, int maxY,
                    uint16_t colour, int lineOffset = 0) {
  tft.setTextColor(colour);
  tft.setTextSize(1);
  int charsPerLine = maxWidth / CHAR_W;
  int x = startX, y = startY;
  int line = 0;

  const char* p = text;
  while (*p) {
    // Scan to end of word
    const char* word = p;
    while (*p && *p != ' ' && *p != '\n') p++;

    int wordLen = p - word;
    int curCol  = (x - startX) / CHAR_W;

    // Wrap if needed
    if (curCol + wordLen > charsPerLine && curCol > 0) {
      line++;
      if (line >= lineOffset) {
        y += LINE_H;
        if (y >= maxY) return line;
      }
      x = startX;
    }

    // Print characters of this word
    for (int i = 0; i < wordLen; i++) {
      if (line >= lineOffset && y < maxY) {
        tft.setCursor(x, y);
        tft.print(word[i]);
      }
      x += CHAR_W;
      if ((x - startX) / CHAR_W >= charsPerLine) {
        line++;
        if (line >= lineOffset) {
          y += LINE_H;
          if (y >= maxY) return line;
        }
        x = startX;
      }
    }

    // Space or newline
    if (*p == '\n') {
      line++;
      if (line >= lineOffset) {
        y += LINE_H;
        if (y >= maxY) return line;
      }
      x = startX; p++;
    } else if (*p == ' ') {
      x += CHAR_W;
      if ((x - startX) / CHAR_W >= charsPerLine) {
        line++;
        if (line >= lineOffset) {
          y += LINE_H;
          if (y >= maxY) return line;
        }
        x = startX;
      }
      p++;
    }
  }
  return line;
}

// ════════════════════════════════════════════════════════════════════
//  SCREEN DRAW FUNCTIONS
// ════════════════════════════════════════════════════════════════════

void drawWifiPrompt() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("WiFi Setup");
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(10, 32); tft.print("Enable WiFi?");
  const char* opts[] = { "Yes", "No" };
  int y = 52;
  for (int i = 0; i < 2; i++) {
    if (wifiPromptSel == i) { tft.fillRect(10, y - 2, SCREEN_W - 20, 14, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(20, y); tft.print(opts[i]); y += 18;
  }
  drawFooter("Toggle", "Skip");
}

void drawHome() {
  tft.fillScreen(ST77XX_BLACK);
  char date[20] = "StudySmart"; char time_[20] = "";
  if (wifiConnected) {
    if (!getCurrentTime(date, sizeof(date), time_, sizeof(time_))) {
      strcpy(date, "Sync failed"); strcpy(time_, "");
    }
  }
  tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE);
  if (time_[0]) {
    int tx = (SCREEN_W - strlen(time_) * 12) / 2;
    tft.setCursor(tx, 36); tft.print(time_);
  }
  tft.setTextSize(1);
  int dx = (SCREEN_W - strlen(date) * CHAR_W) / 2;
  tft.setCursor(dx, 62); tft.print(date);
  if (wifiConnected)    { tft.setTextColor(0x07E0); tft.setCursor(28, 84); tft.print("WiFi connected"); }
  else if (wifiEnabled) { tft.setTextColor(ST77XX_YELLOW); tft.setCursor(34, 84); tft.print("WiFi enabled"); }
  else                  { tft.setTextColor(0xF800); tft.setCursor(58, 84); tft.print("WiFi off"); }
  drawFooter("Apps", "");
}

void drawAppMenu() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Apps");
  const char* names[NUM_APPS];
  for (int i = 0; i < NUM_APPS; i++) names[i] = APPS[i].name;
  drawList(names, NUM_APPS, appSelIdx, appScroll);
  drawFooter("Open", "Home");
}

void drawTextInput() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar(ti.title);
  tft.drawRect(TEXT_X, TITLE_H + 4, SCREEN_W - TEXT_X * 2, CHAR_H + 6, ST77XX_CYAN);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  int x = TEXT_X + 3, y = TITLE_H + 7;
  int boxChars = (SCREEN_W - TEXT_X * 2 - 6) / CHAR_W;
  int slots    = (pendingKey >= 0) ? boxChars - 1 : boxChars;
  int startChar = 0;
  if (ti.cursor >= slots) startChar = ti.cursor - slots + 1;
  for (int i = startChar; i < ti.len && x < SCREEN_W - TEXT_X - 3; i++) {
    tft.setCursor(x, y); tft.print(ti.password ? '*' : ti.buf[i]); x += CHAR_W;
  }
  if (pendingKey >= 0) {
    tft.fillRect(x, y - 1, CHAR_W, CHAR_H + 1, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK); tft.setCursor(x, y);
    tft.print(applyShift(KEY_MAP[pendingKey][pendingIdx], false));
    tft.setTextColor(ST77XX_WHITE); x += CHAR_W;
  }
  tft.fillRect(x, y - 1, 1, CHAR_H + 1, ST77XX_GREEN);
  tft.setTextColor(0x7BEF);
  tft.setCursor(TEXT_X, TITLE_H + 26); tft.print("T9: tap=cycle  wait=commit");
  tft.setCursor(TEXT_X, TITLE_H + 36); tft.print("BACK=del  MENU=save");
  drawFooter("Save", "Cancel");
}

// ── Study AI screens ─────────────────────────────────────────────
void drawStudyHome() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Study AI");
  const char* opts[] = { "Ask?", "Quiz", "Health Check", "Settings" };
  int y = 24;
  for (int i = 0; i < 4; i++) {
    if (i == studyHomeSel) {
      tft.fillRect(5, y - 2, SCREEN_W - 10, 14, ST77XX_CYAN);
      tft.setTextColor(ST77XX_BLACK);
    } else { tft.setTextColor(ST77XX_WHITE); }
    tft.setTextSize(1); tft.setCursor(10, y); tft.print(opts[i]); y += 18;
  }
  tft.setTextColor(ST77XX_WHITE);
  drawFooter("Select", "Back");
}

void drawStudyLoading() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Please Wait");
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  tft.setCursor(10, 55); tft.print("Loading...");
  if (studyError[0]) {
    tft.setTextColor(0xF800);
    tft.setCursor(4, 75); tft.print(studyError);
  }
  drawFooter("", "");
}

// Answer screen: shows one page of text with scroll indicator
void drawStudyAnswer() {
  tft.fillScreen(ST77XX_BLACK);

  // Title with page indicator
  char title[24];
  snprintf(title, sizeof(title), "Answer %d/%d", askCurrentPage, askTotalPages);
  drawTitleBar(title);

  int textAreaY  = TITLE_H + 3;
  int textAreaBot = SCREEN_H - FOOTER_H;

  drawWrappedText(askPageText,
                  TEXT_X, textAreaY,
                  SCREEN_W - TEXT_X * 2,
                  textAreaBot,
                  ST77XX_WHITE,
                  askLineScroll);

  // Scroll hint arrows
  if (askLineScroll > 0) {
    tft.setTextColor(ST77XX_CYAN); tft.setCursor(SCREEN_W - 10, textAreaY);
    tft.print("^");
  }

  // Footer: left=prev page, right=next page
  char leftLabel[12]  = "";
  char rightLabel[12] = "";
  if (askCurrentPage > 1)             strncpy(leftLabel,  "< Prev", 11);
  if (askHasMore || askCurrentPage < askTotalPages) strncpy(rightLabel, "Next >", 11);
  drawFooter(leftLabel, rightLabel);
}

// Question screen: question + 4 options, or explanation after answering
void drawStudyQuestion() {
  tft.fillScreen(ST77XX_BLACK);

  char title[24];
  snprintf(title, sizeof(title), "Q %d/%d", quizIndex + 1, quizTotal);
  drawTitleBar(title);

  // Question text (wrapped, max 3 lines)
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  int qLines = 0, x = TEXT_X, y = TITLE_H + 3;
  const char* p = quizQuestionText;
  while (*p && qLines < 3) {
    int col = 0;
    tft.setCursor(x, y);
    while (*p && col < CHARS_PER_LINE) {
      tft.print(*p); p++; col++;
    }
    y += LINE_H; qLines++;
  }

  // Divider
  tft.drawFastHLine(0, y, SCREEN_W, ST77XX_CYAN); y += 3;

  if (!quizAnswered) {
    // Show options
    for (int i = 0; i < 4 && y < SCREEN_H - FOOTER_H; i++) {
      if (i == quizSelected) {
        tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN);
        tft.setTextColor(ST77XX_BLACK);
      } else { tft.setTextColor(ST77XX_WHITE); }
      tft.setCursor(4, y + 1);
      char tmp[44]; strncpy(tmp, quizOptions[i], 43); tmp[43] = '\0';
      tft.print(tmp);
      tft.setTextColor(ST77XX_WHITE);
      y += LIST_ITEM_H;
    }
    drawFooter("Answer", "Quit");
  } else {
    // Show correct / wrong + explanation
    bool correct = (quizSelected == quizCorrectIndex);
    uint16_t resultCol = correct ? 0x07E0 : 0xF800;
    tft.setTextColor(resultCol);
    tft.setCursor(TEXT_X, y); y += LINE_H;
    tft.print(correct ? "Correct!" : "Wrong!");

    tft.setTextColor(0x7BEF);
    // Show correct answer if wrong
    if (!correct) {
      tft.setCursor(TEXT_X, y); y += LINE_H;
      tft.print("Ans: "); tft.print(quizOptions[quizCorrectIndex]);
    }
    // Explanation (one line)
    if (quizExplanation[0] && y < SCREEN_H - FOOTER_H) {
      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(TEXT_X, y);
      char exp[CHARS_PER_LINE + 1];
      strncpy(exp, quizExplanation, CHARS_PER_LINE); exp[CHARS_PER_LINE] = '\0';
      tft.print(exp);
    }
    bool isLast = (quizIndex + 1 >= quizTotal);
    drawFooter(isLast ? "Finish" : "Next", "Quit");
  }
}

// Result screen shown after quiz submission
QuizResult lastResult = { false, 0, "?" };
void drawStudyResult() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Quiz Result");
  tft.setTextSize(2);
  uint16_t col = lastResult.percent >= 60 ? 0x07E0 : 0xF800;
  tft.setTextColor(col);
  tft.setCursor(40, 40); tft.print(lastResult.percent); tft.print("%");
  tft.setTextSize(1); tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(50, 70);
  tft.print("Grade: "); tft.print(lastResult.grade);
  tft.setCursor(20, 86);
  tft.print(quizScore); tft.print("/"); tft.print(quizTotal);
  tft.print(" correct");
  drawFooter("", "Back");
}

void drawStudyHealth() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("API Health");
  uint16_t col = (studyHealthCode >= 200 && studyHealthCode < 300) ? 0x07E0 : 0xF800;
  tft.setTextColor(col);
  tft.setTextSize(1);
  tft.setCursor(6, 24);
  tft.print(studyHealthText);
  tft.setTextColor(0x7BEF);
  tft.setCursor(6, 44);
  tft.print("Code: ");
  tft.print(studyHealthCode);
  tft.setCursor(6, 60);
  tft.print(studyApiBase);
  drawFooter("Retry", "Back");
}

void drawStudySettings() {
  tft.fillScreen(ST77XX_BLACK);
  drawTitleBar("Study Settings");
  bool sel = (studySettingSel == 0);
  int y = 30;
  if (sel) { tft.fillRect(5, y - 2, SCREEN_W - 10, 14, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
  else     { tft.setTextColor(ST77XX_WHITE); }
  tft.setTextSize(1); tft.setCursor(10, y); tft.print("API Base:");
  tft.setTextColor(ST77XX_WHITE); tft.setCursor(10, y + 12); tft.print(studyApiBase);
  drawFooter("Edit", "Back");
}

// ── WiFi screens ──────────────────────────────────────────────────
void drawWifiApp() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("WiFi Control");
  tft.setTextSize(1);
  uint16_t col = wifiConnected ? 0x07E0 : (wifiEnabled ? ST77XX_YELLOW : 0xF800);
  tft.setTextColor(col); tft.setCursor(6, 24);
  tft.print(wifiConnected ? "Connected" : (wifiEnabled ? "Enabled" : "Disabled"));
  tft.setTextColor(ST77XX_WHITE);
  if (wifiConnected) {
    tft.setCursor(6, 36); tft.print(WiFi.SSID());
    tft.setCursor(6, 48); tft.print(WiFi.localIP().toString());
  } else { tft.setCursor(6, 36); tft.print(wifiStatus); }
  tft.setTextColor(0x7BEF);
  tft.setCursor(6, 70); tft.print("ENTER = Toggle WiFi");
  tft.setCursor(6, 80); tft.print("MENU  = Scan networks");
  drawFooter("Scan", "Back");
}

void drawWifiNetworks() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Networks");
  if (wifiNetCount == 0) {
    tft.setTextColor(ST77XX_WHITE); tft.setCursor(6, 30); tft.print(wifiStatus);
  } else {
    int y = LIST_Y_START;
    if (wifiNetSel < wifiNetScroll) wifiNetScroll = wifiNetSel;
    if (wifiNetSel >= wifiNetScroll + LIST_VISIBLE) wifiNetScroll = wifiNetSel - LIST_VISIBLE + 1;
    for (int i = wifiNetScroll; i < wifiNetCount && y < SCREEN_H - FOOTER_H; i++) {
      if (i == wifiNetSel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
      else { tft.setTextColor(ST77XX_WHITE); }
      tft.setCursor(4, y + 1);
      char nm[16]; strncpy(nm, wifiNetNames[i].c_str(), 15); nm[15] = '\0';
      tft.print(nm);
      tft.setCursor(SCREEN_W - 36, y + 1); tft.print(wifiNetOpen[i] ? "OPEN" : "LOCK");
      tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
    }
  }
  drawFooter("Connect", "Back");
}

// ── Notepad screens ───────────────────────────────────────────────
void drawNoteList() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Notes");
  const char* names[MAX_NOTES];
  for (int i = 0; i < totalNotes; i++) names[i] = notes[i].name;
  int sel = (noteSelIdx == totalNotes) ? totalNotes : noteSelIdx;
  drawList(names, totalNotes, sel, noteScroll, true);
  drawFooter("Open", "Home");
}

void drawNoteView() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar(notes[noteSelIdx].name);
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  int x = TEXT_X, y = TITLE_H + 4, col = 0;
  for (int i = 0; i < notes[noteSelIdx].contentLen; i++) {
    char c = notes[noteSelIdx].content[i];
    if (c == '\n') { x = TEXT_X; y += LINE_H; col = 0; }
    else { tft.setCursor(x, y); tft.print(c); x += CHAR_W; if (++col >= CHARS_PER_LINE) { x = TEXT_X; y += LINE_H; col = 0; } }
    if (y >= SCREEN_H - FOOTER_H) break;
  }
  drawFooter("Options", "Back");
}

void drawNoteEdit() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Edit Note");
  tft.setTextColor(ST77XX_WHITE); tft.setTextSize(1);
  int x = TEXT_X, y = TITLE_H + 4, col = 0;
  for (int i = 0; i < editLen; i++) {
    if (editBuf[i] == '\n') { x = TEXT_X; y += LINE_H; col = 0; }
    else { tft.setCursor(x, y); tft.print(editBuf[i]); x += CHAR_W; if (++col >= CHARS_PER_LINE) { x = TEXT_X; y += LINE_H; col = 0; } }
  }
  int cx = editCursorX(editCursor), cy = editCursorY(editCursor);
  if (pendingKey >= 0) {
    tft.fillRect(cx, cy, CHAR_W, CHAR_H, ST77XX_WHITE);
    tft.setTextColor(ST77XX_BLACK); tft.setCursor(cx, cy);
    tft.print(applyShift(KEY_MAP[pendingKey][pendingIdx], false));
    tft.setTextColor(ST77XX_WHITE); cx += CHAR_W;
  }
  tft.fillRect(cx, cy, 1, CHAR_H, ST77XX_GREEN);
  drawFooter("Save", "Back");
}

void drawNoteMenu() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Note Options");
  const char* opts[] = { "View", "Edit", "Rename", "Delete" };
  int y = 28;
  for (int i = 0; i < 4; i++) {
    if (i == noteMenuSel) { tft.fillRect(5, y, SCREEN_W - 10, 14, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else { tft.setTextColor(ST77XX_WHITE); }
    tft.setTextSize(1); tft.setCursor(20, y + 3); tft.print(opts[i]);
    tft.setTextColor(ST77XX_WHITE); y += 18;
  }
  drawFooter("Select", "Back");
}

// ── Todo screens ──────────────────────────────────────────────────
void drawTodoList() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Todo");
  if (todoSelIdx < todoScroll) todoScroll = todoSelIdx;
  if (todoSelIdx >= todoScroll + LIST_VISIBLE) todoScroll = todoSelIdx - LIST_VISIBLE + 1;
  int y = LIST_Y_START;
  { bool sel = (todoSelIdx == totalTodos);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else     { tft.setTextColor(0x07E0); }
    tft.setTextSize(1); tft.setCursor(6, y + 1); tft.print("+ New task");
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  for (int i = todoScroll; i < totalTodos && y < SCREEN_H - FOOTER_H; i++) {
    bool sel = (i == todoSelIdx);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1); tft.print(todos[i].done ? "[x]" : "[ ]");
    tft.setCursor(26, y + 1);
    char tmp[20]; strncpy(tmp, todos[i].text, 19); tmp[19] = '\0'; tft.print(tmp);
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  }
  drawFooter("Check", "Home");
}

void drawTodoMenu() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Task Options");
  const char* opts[] = { "Toggle done", "Delete" };
  int y = 36;
  for (int i = 0; i < 2; i++) {
    if (i == todoMenuSel) { tft.fillRect(5, y, SCREEN_W - 10, 14, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else { tft.setTextColor(ST77XX_WHITE); }
    tft.setTextSize(1); tft.setCursor(20, y + 3); tft.print(opts[i]);
    tft.setTextColor(ST77XX_WHITE); y += 20;
  }
  drawFooter("Select", "Back");
}

// ── Settings screen ───────────────────────────────────────────────
void drawSettings() {
  tft.fillScreen(ST77XX_BLACK); drawTitleBar("Settings");
  tft.setTextSize(1); int y = LIST_Y_START;
  auto row = [&](int idx, const char* label, const char* val) {
    bool sel = (settingSel == idx);
    if (sel) { tft.fillRect(2, y, SCREEN_W - 4, LIST_ITEM_H - 1, ST77XX_CYAN); tft.setTextColor(ST77XX_BLACK); }
    else { tft.setTextColor(ST77XX_WHITE); }
    tft.setCursor(4, y + 1); tft.print(label);
    tft.setCursor(80, y + 1); tft.print(val);
    tft.setTextColor(ST77XX_WHITE); y += LIST_ITEM_H;
  };
  row(0, "Timezone", cfg.timezone);
  row(1, "Auto WiFi", cfg.autoWifi ? "ON" : "OFF");
  row(2, "Sound",     cfg.soundEnabled ? "ON" : "OFF");
  drawFooter("Edit", "Back");
}

// ════════════════════════════════════════════════════════════════════
//  MASTER REDRAW DISPATCHER
// ════════════════════════════════════════════════════════════════════
void redrawDisplay() {
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:    drawWifiPrompt();    break;
    case SCREEN_HOME:           drawHome();          break;
    case SCREEN_APP_MENU:       drawAppMenu();       break;
    case SCREEN_TEXT_INPUT:     drawTextInput();     break;
    case SCREEN_WIFI_APP:       drawWifiApp();       break;
    case SCREEN_WIFI_NETWORKS:  drawWifiNetworks();  break;
    case SCREEN_NOTE_LIST:      drawNoteList();      break;
    case SCREEN_NOTE_VIEW:      drawNoteView();      break;
    case SCREEN_NOTE_EDIT:      drawNoteEdit();      break;
    case SCREEN_NOTE_MENU:      drawNoteMenu();      break;
    case SCREEN_SETTINGS:       drawSettings();      break;
    case SCREEN_TODO_LIST:      drawTodoList();      break;
    case SCREEN_TODO_MENU:      drawTodoMenu();      break;
    case SCREEN_STUDY_HOME:     drawStudyHome();     break;
    case SCREEN_STUDY_ANSWER:   drawStudyAnswer();   break;
    case SCREEN_STUDY_QUESTION: drawStudyQuestion(); break;
    case SCREEN_STUDY_RESULT:   drawStudyResult();   break;
    case SCREEN_STUDY_HEALTH:   drawStudyHealth();   break;
    case SCREEN_STUDY_SETTINGS: drawStudySettings(); break;
    case SCREEN_STUDY_LOADING:  drawStudyLoading();  break;
    default: break;
  }
}

// ════════════════════════════════════════════════════════════════════
//  TEXT INPUT CALLBACKS
// ════════════════════════════════════════════════════════════════════

// -- Rename note --
void onNoteRenameCommit(const char* text) {
  strncpy(notes[noteSelIdx].name, text, NOTE_NAME_LEN);
  notes[noteSelIdx].name[NOTE_NAME_LEN] = '\0';
  gotoScreen(SCREEN_NOTE_LIST);
}
void onNoteRenameCancel() { gotoScreen(SCREEN_NOTE_LIST); }

// -- WiFi password --
void onWifiPassCommit(const char* text) {
  strncpy(wifiPassword, text, 64); wifiPassword[64] = '\0';
  wifiStatus = "Connecting..."; gotoScreen(SCREEN_WIFI_APP); redrawDisplay();
  if (connectWiFi(wifiNetNames[wifiNetSel].c_str(), wifiPassword)) {
    wifiConnected = true; wifiEnabled = true; wifiStatus = "Connected";
  } else { wifiConnected = false; wifiStatus = "Failed"; }
  needsRedraw = true;
}
void onWifiPassCancel() { gotoScreen(SCREEN_WIFI_NETWORKS); }

// -- Timezone --
void onTimezoneCommit(const char* text) {
  strncpy(cfg.timezone, text, 31); cfg.timezone[31] = '\0';
  setenv("TZ", cfg.timezone, 1); tzset(); gotoScreen(SCREEN_SETTINGS);
}
void onTimezoneCancel() { gotoScreen(SCREEN_SETTINGS); }

// -- New todo --
void onTodoCommit(const char* text) { todoCreate(text); gotoScreen(SCREEN_TODO_LIST); }
void onTodoCancel() { gotoScreen(SCREEN_TODO_LIST); }

// -- Study API base --
void onStudyApiCommit(const char* text) {
  strlcpy(studyApiBase, text, sizeof(studyApiBase));
  gotoScreen(SCREEN_STUDY_SETTINGS);
}
void onStudyApiCancel() { gotoScreen(SCREEN_STUDY_SETTINGS); }

// ── Study Ask? callbacks ─────────────────────────────────────────
void onAskCommit(const char* question) {
  studyError[0] = '\0';
  gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();

  if (studyAsk(question)) {
    gotoScreen(SCREEN_STUDY_ANSWER);
  } else {
    // Show error for 2s then back
    needsRedraw = true; redrawDisplay(); delay(2000);
    gotoScreen(SCREEN_STUDY_HOME);
  }
}
void onAskCancel() { gotoScreen(SCREEN_STUDY_HOME); }

// ── Study Quiz callbacks ─────────────────────────────────────────
void onQuizTopicCommit(const char* topic) {
  studyError[0] = '\0';
  gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();

  if (studyQuizGenerate(topic, 20)) {
    gotoScreen(SCREEN_STUDY_QUESTION);
  } else {
    needsRedraw = true; redrawDisplay(); delay(2000);
    gotoScreen(SCREEN_STUDY_HOME);
  }
}
void onQuizTopicCancel() { gotoScreen(SCREEN_STUDY_HOME); }

// ════════════════════════════════════════════════════════════════════
//  INPUT HANDLERS
// ════════════════════════════════════════════════════════════════════

void handleBack() {
  beep(80);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiEnabled = wifiConnected = false; gotoScreen(SCREEN_HOME); break;
    case SCREEN_HOME: break;
    case SCREEN_APP_MENU:    appSelIdx = 0; gotoScreen(SCREEN_HOME); break;
    case SCREEN_TEXT_INPUT:
      if (pendingKey >= 0) {
        commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
        backspaceAt(ti.buf, ti.len, ti.cursor); needsRedraw = true;
      } else if (ti.len > 0) {
        backspaceAt(ti.buf, ti.len, ti.cursor); needsRedraw = true;
      } else { if (ti.onCancel) ti.onCancel(); }
      break;
    case SCREEN_WIFI_APP:      gotoScreen(SCREEN_APP_MENU);      break;
    case SCREEN_WIFI_NETWORKS: gotoScreen(SCREEN_WIFI_APP);      break;
    case SCREEN_NOTE_LIST:     gotoScreen(SCREEN_HOME);          break;
    case SCREEN_NOTE_VIEW:     gotoScreen(SCREEN_NOTE_LIST);     break;
    case SCREEN_NOTE_EDIT:
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (!backspaceAt(editBuf, editLen, editCursor)) gotoScreen(SCREEN_NOTE_VIEW);
      needsRedraw = true; break;
    case SCREEN_NOTE_MENU:     noteMenuSel = 0; gotoScreen(SCREEN_NOTE_LIST); break;
    case SCREEN_SETTINGS:      gotoScreen(SCREEN_APP_MENU); break;
    case SCREEN_TODO_LIST:     gotoScreen(SCREEN_HOME);     break;
    case SCREEN_TODO_MENU:     gotoScreen(SCREEN_TODO_LIST); break;
    // Study AI
    case SCREEN_STUDY_HOME:     gotoScreen(SCREEN_APP_MENU);      break;
    case SCREEN_STUDY_ANSWER:
      // Previous page or back to home
      if (askCurrentPage > 1) {
        gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
        if (studyAskPage(askCurrentPage - 1)) gotoScreen(SCREEN_STUDY_ANSWER);
        else { delay(1500); gotoScreen(SCREEN_STUDY_HOME); }
      } else { gotoScreen(SCREEN_STUDY_HOME); }
      break;
    case SCREEN_STUDY_QUESTION:
      // Quit quiz early
      gotoScreen(SCREEN_STUDY_HOME); break;
    case SCREEN_STUDY_RESULT:   gotoScreen(SCREEN_STUDY_HOME);    break;
    case SCREEN_STUDY_HEALTH:   gotoScreen(SCREEN_STUDY_HOME);    break;
    case SCREEN_STUDY_SETTINGS: gotoScreen(SCREEN_STUDY_HOME);    break;
    default: break;
  }
}

void handleEnter() {
  beep(50);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT:
      wifiEnabled = (wifiPromptSel == 0);
      if (wifiEnabled) wifiConnected = connectWiFi(DEFAULT_SSID, DEFAULT_PASS);
      gotoScreen(SCREEN_HOME); break;
    case SCREEN_HOME: break;
    case SCREEN_APP_MENU:
      if (APPS[appSelIdx].screen == SCREEN_NOTE_LIST) noteSelIdx = totalNotes;
      if (APPS[appSelIdx].screen == SCREEN_STUDY_HOME) studyHomeSel = 0;
      gotoScreen(APPS[appSelIdx].screen); break;
    case SCREEN_TEXT_INPUT:
      commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
      if (ti.onCommit) ti.onCommit(ti.buf); break;
    case SCREEN_WIFI_APP:
      if (wifiEnabled) { WiFi.disconnect(true); wifiEnabled = wifiConnected = false; wifiStatus = "Disabled"; }
      else { wifiEnabled = true; wifiConnected = connectWiFi(DEFAULT_SSID, DEFAULT_PASS); wifiStatus = wifiConnected ? "Connected" : "Failed"; }
      needsRedraw = true; break;
    case SCREEN_WIFI_NETWORKS:
      if (wifiNetCount > 0) {
        if (wifiNetOpen[wifiNetSel]) {
          wifiStatus = "Connecting..."; needsRedraw = true; redrawDisplay();
          wifiConnected = connectWiFi(wifiNetNames[wifiNetSel].c_str(), "");
          wifiEnabled   = wifiConnected; wifiStatus = wifiConnected ? "Connected" : "Failed";
          gotoScreen(SCREEN_WIFI_APP);
        } else { tiInit("WiFi Password", "", true, onWifiPassCommit, onWifiPassCancel); gotoScreen(SCREEN_TEXT_INPUT); }
      } break;
    case SCREEN_NOTE_LIST:
      if (noteSelIdx == totalNotes) {
        noteCreate(); noteSelIdx = totalNotes - 1;
        editBuf[0] = '\0'; editLen = 0; editCursor = 0; pendingKey = -1;
        gotoScreen(SCREEN_NOTE_EDIT);
      } else { gotoScreen(SCREEN_NOTE_VIEW); }
      break;
    case SCREEN_NOTE_VIEW: break;
    case SCREEN_NOTE_EDIT:
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (editLen < NOTE_CONTENT_LEN) {
        memmove(&editBuf[editCursor + 1], &editBuf[editCursor], editLen - editCursor + 1);
        editBuf[editCursor] = '\n'; editLen++; editCursor++;
      }
      needsRedraw = true; break;
    case SCREEN_NOTE_MENU:
      switch (noteMenuSel) {
        case 0: gotoScreen(SCREEN_NOTE_VIEW); break;
        case 1:
          strncpy(editBuf, notes[noteSelIdx].content, NOTE_CONTENT_LEN);
          editLen = notes[noteSelIdx].contentLen; editCursor = 0; pendingKey = -1;
          gotoScreen(SCREEN_NOTE_EDIT); break;
        case 2:
          tiInit("Rename Note", notes[noteSelIdx].name, false, onNoteRenameCommit, onNoteRenameCancel);
          gotoScreen(SCREEN_TEXT_INPUT); break;
        case 3: noteDelete(noteSelIdx); gotoScreen(SCREEN_NOTE_LIST); break;
      }
      noteMenuSel = 0; break;
    case SCREEN_SETTINGS:
      switch (settingSel) {
        case 0: tiInit("Timezone", cfg.timezone, false, onTimezoneCommit, onTimezoneCancel); gotoScreen(SCREEN_TEXT_INPUT); break;
        case 1: cfg.autoWifi     = !cfg.autoWifi;     needsRedraw = true; break;
        case 2: cfg.soundEnabled = !cfg.soundEnabled; needsRedraw = true; break;
      } break;
    case SCREEN_TODO_LIST:
      if (todoSelIdx == totalTodos) { tiInit("New Task", "", false, onTodoCommit, onTodoCancel); gotoScreen(SCREEN_TEXT_INPUT); }
      else { todos[todoSelIdx].done = !todos[todoSelIdx].done; needsRedraw = true; }
      break;
    case SCREEN_TODO_MENU:
      switch (todoMenuSel) {
        case 0: todos[todoSelIdx].done = !todos[todoSelIdx].done; break;
        case 1: todoDelete(todoSelIdx); break;
      }
      todoMenuSel = 0; gotoScreen(SCREEN_TODO_LIST); break;

    // ── Study AI ──────────────────────────────────────────────────
    case SCREEN_STUDY_HOME:
      switch (studyHomeSel) {
        case 0: // Ask?
          tiInit("Ask a question", "", false, onAskCommit, onAskCancel);
          gotoScreen(SCREEN_TEXT_INPUT); break;
        case 1: // Quiz
          tiInit("Quiz topic", "", false, onQuizTopicCommit, onQuizTopicCancel);
          gotoScreen(SCREEN_TEXT_INPUT); break;
        case 2: // Health Check
          studyError[0] = '\0';
          strlcpy(studyHealthText, "Checking...", sizeof(studyHealthText));
          studyHealthCode = 0;
          gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
          studyHealthCheck();
          gotoScreen(SCREEN_STUDY_HEALTH); break;
        case 3: // Settings
          studySettingSel = 0; gotoScreen(SCREEN_STUDY_SETTINGS); break;
      } break;

    case SCREEN_STUDY_ANSWER:
      // ENTER = next page
      if (askHasMore || askCurrentPage < askTotalPages) {
        gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
        if (studyAskPage(askCurrentPage + 1)) gotoScreen(SCREEN_STUDY_ANSWER);
        else { delay(1500); gotoScreen(SCREEN_STUDY_HOME); }
      } break;

    case SCREEN_STUDY_QUESTION:
      if (!quizAnswered) {
        // Submit answer
        quizAnswered = true;
        quizWasCorrect = (quizSelected == quizCorrectIndex);
        if (quizWasCorrect) quizScore++;
        needsRedraw = true;
      } else {
        // Advance to next question or finish
        quizIndex++;
        if (quizIndex >= quizTotal) {
          // Submit and show result
          gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
          lastResult = studyQuizSubmit();
          gotoScreen(SCREEN_STUDY_RESULT);
        } else {
          gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
          if (studyQuizQuestion(quizIndex)) gotoScreen(SCREEN_STUDY_QUESTION);
          else { delay(1500); gotoScreen(SCREEN_STUDY_HOME); }
        }
      } break;

    case SCREEN_STUDY_RESULT:  gotoScreen(SCREEN_STUDY_HOME); break;
    case SCREEN_STUDY_HEALTH:
      studyError[0] = '\0';
      strlcpy(studyHealthText, "Checking...", sizeof(studyHealthText));
      studyHealthCode = 0;
      gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
      studyHealthCheck();
      gotoScreen(SCREEN_STUDY_HEALTH); break;
    case SCREEN_STUDY_SETTINGS:
      if (studySettingSel == 0) {
        tiInit("API Base", studyApiBase, false, onStudyApiCommit, onStudyApiCancel);
        gotoScreen(SCREEN_TEXT_INPUT);
      } break;
    default: break;
  }
}

void handleMenu() {
  beep(30);
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT: wifiPromptSel = (wifiPromptSel + 1) % 2; needsRedraw = true; break;
    case SCREEN_HOME: appSelIdx = 0; gotoScreen(SCREEN_APP_MENU); break;
    case SCREEN_APP_MENU: appSelIdx = (appSelIdx + 1) % NUM_APPS; needsRedraw = true; break;
    case SCREEN_TEXT_INPUT:
      commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN);
      if (ti.onCommit) ti.onCommit(ti.buf); break;
    case SCREEN_WIFI_APP: scanWifi(); gotoScreen(SCREEN_WIFI_NETWORKS); break;
    case SCREEN_WIFI_NETWORKS: scanWifi(); needsRedraw = true; break;
    case SCREEN_NOTE_VIEW: noteMenuSel = 0; gotoScreen(SCREEN_NOTE_MENU); break;
    case SCREEN_NOTE_EDIT:
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      noteSave(noteSelIdx); editLen = 0; editCursor = 0; pendingKey = -1;
      gotoScreen(SCREEN_NOTE_VIEW); break;
    case SCREEN_NOTE_MENU: noteMenuSel = (noteMenuSel + 1) % 4; needsRedraw = true; break;
    case SCREEN_SETTINGS: settingSel = (settingSel + 1) % numSettings; needsRedraw = true; break;
    case SCREEN_TODO_LIST:
      if (todoSelIdx < totalTodos) { todoMenuSel = 0; gotoScreen(SCREEN_TODO_MENU); } break;
    case SCREEN_TODO_MENU: todoMenuSel = (todoMenuSel + 1) % 2; needsRedraw = true; break;
    // Study: MENU scrolls within a page on answer screen
    case SCREEN_STUDY_ANSWER: askLineScroll++; needsRedraw = true; break;
    default: break;
  }
}

void handleNav(int dir) {
  beep();
  switch (currentScreen) {
    case SCREEN_WIFI_PROMPT: wifiPromptSel = (wifiPromptSel + 1) % 2; needsRedraw = true; break;
    case SCREEN_APP_MENU:
      if (dir == NAV_DOWN) appSelIdx = (appSelIdx + 1) % NUM_APPS;
      if (dir == NAV_UP)   appSelIdx = (appSelIdx - 1 + NUM_APPS) % NUM_APPS;
      needsRedraw = true; break;
    case SCREEN_TEXT_INPUT:
      if (dir == NAV_LEFT  && ti.cursor > 0)     { commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN); ti.cursor--; needsRedraw = true; }
      if (dir == NAV_RIGHT && ti.cursor < ti.len) { commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN); ti.cursor++; needsRedraw = true; }
      break;
    case SCREEN_WIFI_NETWORKS:
      if (dir == NAV_DOWN) wifiNetSel = (wifiNetSel + 1) % max(1, wifiNetCount);
      if (dir == NAV_UP)   wifiNetSel = (wifiNetSel - 1 + max(1, wifiNetCount)) % max(1, wifiNetCount);
      needsRedraw = true; break;
    case SCREEN_NOTE_LIST: {
      int n = totalNotes + 1;
      if (dir == NAV_DOWN) noteSelIdx = (noteSelIdx + 1) % n;
      if (dir == NAV_UP)   noteSelIdx = (noteSelIdx - 1 + n) % n;
      needsRedraw = true; break;
    }
    case SCREEN_NOTE_EDIT:
      commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN);
      if (dir == NAV_UP)    editCursorUp();
      if (dir == NAV_DOWN)  editCursorDown();
      if (dir == NAV_LEFT  && editCursor > 0)      editCursor--;
      if (dir == NAV_RIGHT && editCursor < editLen) editCursor++;
      needsRedraw = true; break;
    case SCREEN_NOTE_MENU:
      if (dir == NAV_DOWN) noteMenuSel = (noteMenuSel + 1) % 4;
      if (dir == NAV_UP)   noteMenuSel = (noteMenuSel - 1 + 4) % 4;
      needsRedraw = true; break;
    case SCREEN_SETTINGS:
      if (dir == NAV_DOWN) settingSel = (settingSel + 1) % numSettings;
      if (dir == NAV_UP)   settingSel = (settingSel - 1 + numSettings) % numSettings;
      needsRedraw = true; break;
    case SCREEN_TODO_LIST: {
      int n = totalTodos + 1;
      if (dir == NAV_DOWN) todoSelIdx = (todoSelIdx + 1) % n;
      if (dir == NAV_UP)   todoSelIdx = (todoSelIdx - 1 + n) % n;
      needsRedraw = true; break;
    }
    case SCREEN_TODO_MENU:
      if (dir == NAV_DOWN) todoMenuSel = (todoMenuSel + 1) % 2;
      if (dir == NAV_UP)   todoMenuSel = (todoMenuSel - 1 + 2) % 2;
      needsRedraw = true; break;
    case SCREEN_STUDY_HOME:
      if (dir == NAV_DOWN) studyHomeSel = (studyHomeSel + 1) % 4;
      if (dir == NAV_UP)   studyHomeSel = (studyHomeSel - 1 + 4) % 4;
      needsRedraw = true; break;
    // Answer: UP/DOWN scrolls within page; LEFT/RIGHT flips page
    case SCREEN_STUDY_ANSWER:
      if (dir == NAV_DOWN) { askLineScroll++; needsRedraw = true; }
      if (dir == NAV_UP)   { if (askLineScroll > 0) askLineScroll--; needsRedraw = true; }
      if (dir == NAV_RIGHT && (askHasMore || askCurrentPage < askTotalPages)) {
        gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
        if (studyAskPage(askCurrentPage + 1)) gotoScreen(SCREEN_STUDY_ANSWER);
        else { delay(1500); gotoScreen(SCREEN_STUDY_HOME); }
      }
      if (dir == NAV_LEFT && askCurrentPage > 1) {
        gotoScreen(SCREEN_STUDY_LOADING); redrawDisplay();
        if (studyAskPage(askCurrentPage - 1)) gotoScreen(SCREEN_STUDY_ANSWER);
        else { delay(1500); gotoScreen(SCREEN_STUDY_HOME); }
      }
      break;
    // Quiz: UP/DOWN changes selected option
    case SCREEN_STUDY_QUESTION:
      if (!quizAnswered) {
        if (dir == NAV_DOWN) quizSelected = (quizSelected + 1) % 4;
        if (dir == NAV_UP)   quizSelected = (quizSelected - 1 + 4) % 4;
        needsRedraw = true;
      } break;
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
  if (!MCP.begin()) { Serial.println("MCP23S17 not found!"); while (1) delay(1000); }
  Serial.println("MCP OK");

  // HSPI → TFT
  hspi.begin(14, 12, 13, 32);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  initDeviceId();
  Serial.print("Device ID: "); Serial.println(deviceId);

  notesInit();
  todoCreate("Study hard!");

  for (int i = 0; i < 16; i++) { mcpPressTime[i] = 0; mcpHoldFired[i] = false; }

  gotoScreen(SCREEN_WIFI_PROMPT);
  redrawDisplay();
  needsRedraw = false;
  Serial.println("Ready.");
}

// ════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════
uint16_t      lastMcpState   = 0xFFFF;
unsigned long lastClockTick  = 0;

void loop() {
  unsigned long now = millis();

  // Auto-commit pending T9 char
  if (pendingKey >= 0 && (now - lastKeyTime) >= COMMIT_DELAY_MS) {
    bool inEdit = (currentScreen == SCREEN_NOTE_EDIT);
    bool inTI   = (currentScreen == SCREEN_TEXT_INPUT);
    if (inEdit) { commitPendingTo(editBuf, editLen, editCursor, NOTE_CONTENT_LEN); needsRedraw = true; }
    else if (inTI) { commitPendingTo(ti.buf, ti.len, ti.cursor, TI_MAX_LEN); needsRedraw = true; }
  }

  // Clock refresh on home
  if (currentScreen == SCREEN_HOME && (now - lastClockTick) >= 60000) {
    lastClockTick = now; needsRedraw = true;
  }

  // MCP buttons
  uint16_t mcpState = MCP.read16();

  // Hold detection (for shift toggle on star key)
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
              beep(60); needsRedraw = true;
            }
          }
        }
      } else { mcpPressTime[i] = now; mcpHoldFired[i] = false; }
    }
  }

  if (mcpState != lastMcpState) {
    for (int i = 0; i < 16; i++) {
      bool wasPressed = !(lastMcpState & (1 << i));
      bool isPressed  = !(mcpState     & (1 << i));
      if (wasPressed && !isPressed) {
        if (mcpHoldFired[i]) { mcpHoldFired[i] = false; continue; }
        int keyIdx = mcpPinToKeyIndex(i);
        if (keyIdx >= 0) {
          bool inEdit = (currentScreen == SCREEN_NOTE_EDIT);
          bool inTI   = (currentScreen == SCREEN_TEXT_INPUT);
          if (inEdit) { beep(); handleT9Key(keyIdx, editBuf, editLen, editCursor, NOTE_CONTENT_LEN); }
          else if (inTI) { beep(); handleT9Key(keyIdx, ti.buf, ti.len, ti.cursor, TI_MAX_LEN); }
        } else {
          switch (i) {
            case NAV_DOWN: case NAV_UP: case NAV_LEFT: case NAV_RIGHT: handleNav(i); break;
          }
        }
      }
    }
    lastMcpState = mcpState;
  }

  // Direct buttons (released edge)
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

  // Redraw only when dirty
  if (needsRedraw) { redrawDisplay(); needsRedraw = false; }

  delay(8);
}
