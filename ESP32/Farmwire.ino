#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <NewPing.h>
#include <Preferences.h>
#include <RTClib.h>
#include <Update.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Wire.h>


// ================= WIFI & SERVER CONFIG =================
// Default Server IP (will be overwritten by WiFiManager if entered)
char websockets_server_host[40] = "homepump.espserver.site";
const uint16_t websockets_server_port = 443; // Server Port (WSS)

// ================= OTA CONFIG =================
const char *firmwareUrl = "https://github.com/shohidmax/home_pump/releases/download/tank/Home_tank.ino.bin";
const char *versionUrl = "https://raw.githubusercontent.com/shohidmax/home_pump/refs/heads/main/version.txt";
const char *currentFirmwareVersion = "1.1.0";
const unsigned long updateCheckInterval = 5 * 60 * 1000; // 5 minutes
unsigned long lastUpdateCheck = 0;

// ================= CONFIGURATION =================
// These defaults will be overwritten by values from Preferences if available
int TANK_HEIGHT_CM = 100;
const int SENSOR_GAP_CM = 5;
int PUMP_ON_LEVEL = 20;  // Default, can be changed via Dashboard
int PUMP_OFF_LEVEL = 90; // Default, can be changed via Dashboard
bool schedulesEnabled = true;
int PRE_SCHEDULE_LIMIT = 65; // Limit before schedule
int RECOVERY_TRIGGER = 70;   // Trigger for missed schedule recovery

// ================= PINS =================
#define PIN_RELAY_PUMP 32
#define PIN_DHT 4
#define PIN_BUZZER 13
#define PIN_US_SIG 5
#define PIN_SW_RESET 33
#define I2C_SDA 21
#define I2C_SCL 22
#define DF_RX 16
#define DF_TX 17

// ================= OBJECTS =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
RTC_DS3231 rtc;
DHT dht(PIN_DHT, DHT11);
NewPing sonar(PIN_US_SIG, PIN_US_SIG, 400);
HardwareSerial dfSerial(1);

WebSocketsClient webSocket;
Preferences preferences;
WebServer server(80);

// ================= VARIABLES =================
int waterLevelPercent = 0;
bool pumpStatus = false;
bool manualMode =
    false; // true = controlled by web, false = automatic sensor logic
// --- Advanced Logic Variables ---
bool recoveryPending = false;
int lastScheduleDay = -1;
int lastScheduleHour = -1;

unsigned long lastReadTime = 0;
unsigned long lastSendTime = 0;
unsigned long resetBtnTimer = 0;
bool resetBtnActive = false;
float temperature = 0.0;

// ================= FUNCTION PROTOTYPES =================
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void sendDataToServer();
void handleResetButton();
void readSensors();
void controlPump();
void updateDisplay();
void updateDisplay();
void beep(int times, int duration);
void loadSettings();
void saveSettings();
void handleRoot();
void handleRoot();
void handleSaveHeight();
// OTA Functions
void checkForFirmwareUpdate();
String fetchLatestVersion();
void downloadAndApplyFirmware();
bool startOTAUpdate(WiFiClient *client, int contentLength);

void setup() {
  Serial.begin(115200);

  // 1. Initialize Pins
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_SW_RESET, INPUT_PULLUP);
  digitalWrite(PIN_RELAY_PUMP, LOW);

  // Load settings from NVS
  loadSettings();

  // 2. Initialize Display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 Failed"));
    for (;;)
      ;
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(F("Booting System..."));
  display.display();
  delay(1000);

  // 3. Initialize Sensors
  dht.begin();
  if (!rtc.begin()) {
    Serial.println("No RTC");
  }
  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);

  // 4. Connect to WiFi using WiFiManager
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.println("If stuck, connect to:");
  display.println("AP: Farmwire_AP");
  display.display();

  WiFiManager wm;

  // Custom Parameter for Server IP
  WiFiManagerParameter custom_server_ip("server", "Server IP",
                                        websockets_server_host, 40);
  wm.addParameter(&custom_server_ip);
  wm.setConnectTimeout(60); // Timeout if cannot connect

  // AutoConnect
  bool res = wm.autoConnect("Farmwire_AP"); // AP Name

  if (!res) {
    Serial.println("Failed to connect");
    display.println("WiFi Failed");
    // ESP.restart(); // Optional: restart if failed
  } else {
    Serial.println("WiFi Connected");
    display.println("WiFi OK");
    display.println(WiFi.localIP());

    // Read updated Server IP
    strcpy(websockets_server_host, custom_server_ip.getValue());
    Serial.print("Server IP: ");
    Serial.println(websockets_server_host);

    // Check for updates immediately on boot
    checkForFirmwareUpdate();
  }
  display.display();
  delay(1000);

  // 5. Connect WebSocket (WSS)
  webSocket.beginSSL(websockets_server_host, websockets_server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  // 6. Start Web Server for Local Config
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSaveHeight);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  webSocket.loop();      // Handle network traffic
  server.handleClient(); // Handle local web server
  handleResetButton();

  // OTA Update Check (Every 5 mins)
  if (millis() - lastUpdateCheck >= updateCheckInterval) {
    lastUpdateCheck = millis();
    if (WiFi.status() == WL_CONNECTED) {
      checkForFirmwareUpdate();
    }
  }

  // Read sensors every 2 seconds
  if (millis() - lastReadTime > 2000) {
    readSensors();
    controlPump(); // Logic handles Auto vs Manual inside
    updateDisplay();
    sendDataToServer();
    lastReadTime = millis();
  }
}

// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WSc] Disconnected!");
    break;
  case WStype_CONNECTED:
    Serial.println("[WSc] Connected to server");
    break;
  case WStype_TEXT:
    Serial.printf("[WSc] Got text: %s\n", payload);

    // Parse JSON Command
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      if (!doc.containsKey("command"))
        return;
      const char *command = doc["command"];
      if (strcmp(command, "PUMP_ON") == 0) {
        manualMode = true;
        pumpStatus = true;
        digitalWrite(PIN_RELAY_PUMP, HIGH);
        beep(1, 100);
      } else if (strcmp(command, "PUMP_OFF") == 0) {
        manualMode = true;
        pumpStatus = false;
        digitalWrite(PIN_RELAY_PUMP, LOW);
        beep(1, 100);
      } else if (strcmp(command, "AUTO") == 0) {
        manualMode = false;
        beep(2, 50);
      } else if (strcmp(command, "SETTINGS") == 0) {
        // Parse settings
        if (doc.containsKey("min"))
          PUMP_ON_LEVEL = doc["min"];
        if (doc.containsKey("max"))
          PUMP_OFF_LEVEL = doc["max"];
        if (doc.containsKey("sched"))
          schedulesEnabled = doc["sched"];
        if (doc.containsKey("pre"))
          PRE_SCHEDULE_LIMIT = doc["pre"];
        if (doc.containsKey("rec"))
          RECOVERY_TRIGGER = doc["rec"];

        saveSettings(); // Save to NVS
        beep(3, 100);   // Confirm settings update
      }
    }
    break;
  }
}

void sendDataToServer() {
  if (WiFi.status() == WL_CONNECTED) {
    StaticJsonDocument<200> doc;
    doc["level"] = waterLevelPercent;
    doc["pump"] = pumpStatus;
    doc["temp"] = temperature;
    doc["mode"] = manualMode ? "MANUAL" : "AUTO";

    // Broadcast current settings so Dashboard stays in sync
    JsonObject settings = doc.createNestedObject("settings");
    settings["min"] = PUMP_ON_LEVEL;
    settings["max"] = PUMP_OFF_LEVEL;
    settings["sched"] = schedulesEnabled;
    settings["pre"] = PRE_SCHEDULE_LIMIT;
    settings["rec"] = RECOVERY_TRIGGER;
    // Note: TANK_HEIGHT_CM is available but not edited via websocket currently
    settings["h_cm"] = TANK_HEIGHT_CM;

    String jsonString;
    serializeJson(doc, jsonString);
    webSocket.sendTXT(jsonString);
  }
}

// --- Standard Logic ---

void handleResetButton() {
  if (digitalRead(PIN_SW_RESET) == LOW) {
    if (!resetBtnActive) {
      resetBtnActive = true;
      resetBtnTimer = millis();
    }
    if (millis() - resetBtnTimer > 10000) {
      display.clearDisplay();
      display.setCursor(10, 20);
      display.println("RESETTING");
      display.display();
      beep(3, 100);
      ESP.restart();
    }
  } else {
    resetBtnActive = false;
  }
}

void readSensors() {
  unsigned int uS = sonar.ping_median(5);
  int distanceCM = sonar.convert_cm(uS);

  if (distanceCM > 0) {
    int rawLevel =
        map(distanceCM, TANK_HEIGHT_CM + SENSOR_GAP_CM, SENSOR_GAP_CM, 0, 100);
    waterLevelPercent = constrain(rawLevel, 0, 100);
  }

  float t = dht.readTemperature();
  if (!isnan(t))
    temperature = t;
}

void controlPump() {
  if (manualMode)
    return;

  DateTime now = rtc.now();
  int currentHour = now.hour();
  int currentDay = now.day();

  // Reset tracker on new day
  if (currentDay != lastScheduleDay) {
    lastScheduleDay = currentDay;
    lastScheduleHour = -1;
  }

  // --- 1. Schedule & Recovery Detection ---
  if (schedulesEnabled) {
    int slots[] = {8, 14, 20}; // Schedule times: 8am, 2pm, 8pm

    for (int slot : slots) {
      // A. Check if currently IN schedule window (first 10 mins)
      if (currentHour == slot && now.minute() < 10) {
        // Avoid re-triggering if already handled
        if (lastScheduleHour != slot) {
          if (waterLevelPercent <= 85) {
            // Trigger Schedule Start
            if (!pumpStatus) {
              pumpStatus = true;
              digitalWrite(PIN_RELAY_PUMP, HIGH);
              beep(1, 500);
            }
            lastScheduleHour = slot; // Mark done
          } else {
            // Tank full enough, skip schedule
            lastScheduleHour = slot; // Mark skipped/done
          }
        }
      }
      // B. Check if we MISSED this slot (e.g. power outage during 8am)
      else if (currentHour > slot && lastScheduleHour < slot) {
        // We differ from the last run slot, so we missed it.
        // (Simple check: if we haven't recorded running this slot, and it's
        // past time) BUT avoid marking if we simply skipped it earlier. Logic
        // deficiency: lastScheduleHour tracks *successful/skipped* run. If it
        // is -1, and hour is 9, we missed 8. Correct.
        recoveryPending = true;
      }
    }
  }

  // --- 2. Start Logic (Non-Scheduled) ---
  if (!pumpStatus) {
    // Normal Low Level Start
    if (waterLevelPercent <= PUMP_ON_LEVEL) {
      pumpStatus = true;
      digitalWrite(PIN_RELAY_PUMP, HIGH);
      beep(1, 500);
    }
    // Recovery Start (Missed Schedule)
    else if (recoveryPending && waterLevelPercent <= RECOVERY_TRIGGER) {
      pumpStatus = true;
      digitalWrite(PIN_RELAY_PUMP, HIGH);
      beep(3, 200);
    }
  }

  // --- 3. Stop Logic ---
  if (pumpStatus) {
    int limit = PUMP_OFF_LEVEL; // Default 90%

    // Pre-Schedule Throttling (1 hour before schedules)
    if (currentHour == 7 || currentHour == 13 || currentHour == 19) {
      limit = PRE_SCHEDULE_LIMIT;
    }

    if (waterLevelPercent >= limit) {
      pumpStatus = false;
      digitalWrite(PIN_RELAY_PUMP, LOW);
      beep(2, 200);

      // If we topped up fully (>=90), clear recovery
      if (waterLevelPercent >= 90) {
        recoveryPending = false;
      }
    }
  }
}

void updateDisplay() {
  DateTime now = rtc.now();
  display.clearDisplay();

  // Time & Temp
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(now.hour());
  display.print(':');
  if (now.minute() < 10)
    display.print('0');
  display.print(now.minute());

  display.setCursor(80, 0);
  display.print((int)temperature);
  display.print("C");
  if (WiFi.status() == WL_CONNECTED)
    display.print(" (W)");

  // Level
  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print("Lvl: ");
  display.print(waterLevelPercent);
  display.print("%");

  // Bar Graph
  display.drawRect(0, 35, 128, 10, WHITE);
  int barWidth = map(waterLevelPercent, 0, 100, 0, 126);
  display.fillRect(2, 37, barWidth, 6, WHITE);

  // Pump Status
  display.setTextSize(1);
  display.setCursor(0, 50);
  if (manualMode)
    display.print("MANUAL: ");
  else
    display.print("AUTO: ");

  if (pumpStatus)
    display.print("ON");
  else
    display.print("OFF");

  display.display();
}

// ================= PERSISTENCE & LOCAL SERVER =================
void loadSettings() {
  preferences.begin("farmwire", true); // Read-only
  PUMP_ON_LEVEL = preferences.getInt("on", 20);
  PUMP_OFF_LEVEL = preferences.getInt("off", 90);
  PRE_SCHEDULE_LIMIT = preferences.getInt("pre", 65);
  RECOVERY_TRIGGER = preferences.getInt("rec", 70);
  schedulesEnabled = preferences.getBool("sched", true);
  TANK_HEIGHT_CM = preferences.getInt("h_cm", 100);
  preferences.end();
  Serial.println("Settings loaded from NVS");
}

void saveSettings() {
  preferences.begin("farmwire", false); // Read-write
  preferences.putInt("on", PUMP_ON_LEVEL);
  preferences.putInt("off", PUMP_OFF_LEVEL);
  preferences.putInt("pre", PRE_SCHEDULE_LIMIT);
  preferences.putInt("rec", RECOVERY_TRIGGER);
  preferences.putBool("sched", schedulesEnabled);
  // TANK_HEIGHT_CM is saved separately when updated via web
  preferences.end();
  Serial.println("Settings saved to NVS");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name=\"viewport\" "
                "content=\"width=device-width, initial-scale=1\">";
  html += "<style>body{font-family:sans-serif;margin:20px;text-align:center;} "
          "input{padding:10px;font-size:1.2rem;} button{padding:10px "
          "20px;font-size:1.2rem;background:#007bff;color:white;border:none;}</"
          "style>";
  html += "</head><body><h1>Tank Configuration</h1>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<label>Total Tank Height (cm):</label><br><br>";
  html += "<input type=\"number\" name=\"height\" value=\"" +
          String(TANK_HEIGHT_CM) + "\"><br><br>";
  html += "<button type=\"submit\">Save Configuration</button>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveHeight() {
  if (server.hasArg("height")) {
    int h = server.arg("height").toInt();
    if (h > 50 && h < 500) {
      TANK_HEIGHT_CM = h;
      preferences.begin("farmwire", false);
      preferences.putInt("h_cm", TANK_HEIGHT_CM);
      preferences.end();

      server.send(200, "text/html",
                  "<h1>Saved!</h1><p>New Tank Height: " + String(h) +
                      " cm</p><a href=\"/\">Back</a>");
      beep(2, 200);
    } else {
      server.send(400, "text/plain", "Invalid Height");
    }
  } else {
    server.send(400, "text/plain", "Missing Argument");
  }
}

if (times > 1)
  delay(100);
}
}

// ================= OTA FUNCTIONS =================
void checkForFirmwareUpdate() {
  Serial.println("Checking for firmware update...");

  String latestVersion = fetchLatestVersion();

  if (latestVersion == "") {
    Serial.println("Could not verify latest version.");
    return;
  }

  Serial.println("Current Version: " + String(currentFirmwareVersion));
  Serial.println("Latest Version: " + latestVersion);

  if (latestVersion != currentFirmwareVersion) {
    Serial.println("Update available. Starting OTA...");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Updating Firmware...");
    display.println("Do not turn off!");
    display.display();
    downloadAndApplyFirmware();
  } else {
    Serial.println("Device is up to date.");
  }
}

String fetchLatestVersion() {
  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation for GitHub

  HTTPClient http;
  if (!http.begin(client, versionUrl)) {
    Serial.println("Unable to connect to Version URL");
    return "";
  }

  int httpCode = http.GET();
  String latestVersion = "";

  if (httpCode == HTTP_CODE_OK) {
    latestVersion = http.getString();
    latestVersion.trim();
  } else {
    Serial.printf("Failed to fetch version. HTTP code: %d\n", httpCode);
  }

  http.end();
  return latestVersion;
}

void downloadAndApplyFirmware() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, firmwareUrl)) {
    Serial.println("Unable to connect to Firmware URL");
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    if (contentLength > 0) {
      WiFiClient *stream = http.getStreamPtr();
      if (startOTAUpdate(stream, contentLength)) {
        Serial.println("OTA Success! Restarting...");
        display.clearDisplay();
        display.println("Update Success!");
        display.println("Restarting...");
        display.display();
        delay(1000);
        ESP.restart();
      } else {
        Serial.println("OTA Failed.");
      }
    }
  }
  http.end();
}

bool startOTAUpdate(WiFiClient *client, int contentLength) {
  if (!Update.begin(contentLength))
    return false;

  size_t written = 0;
  uint8_t buffer[1024];
  unsigned long lastDataTime = millis();

  while (written < contentLength) {
    if (client->available()) {
      int bytesRead = client->read(buffer, sizeof(buffer));
      if (bytesRead > 0) {
        Update.write(buffer, bytesRead);
        written += bytesRead;
        lastDataTime = millis();
      }
    }
    if (millis() - lastDataTime > 20000) {
      Update.abort();
      return false;
    }
    yield();
  }
  return Update.end();
}