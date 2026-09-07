/* ESP32-S3 N16R8 Wi-Fi stereo radio + 4 buttons + 0.96 OLED
   Two MAX98357A modules share the I2S bus; channel selection is hardware-configured.
   Includes WiFiManager captive portal for 1-click configuration on any network.
*/
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include "Audio.h"

// Station list: you can freely add more stations here, STATION_COUNT adapts automatically.
const char* stationNames[] = {
  "Radio ROKS",
  "KISS FM Ukraine",
  "Lux FM",
  "Hit FM",
  "Lofi Beats"
};

const char* stationUrls[] = {
  "http://online.radioroks.ua/RadioROKS",
  "https://online.kissfm.ua/KissFM_Ukr_HD",
  "https://lux.radio.tvstitch.com/lux_lviv_adv_sd",
  "http://online.hitfm.ua/HitFM",
  "http://das-sa39.cdnstream1.com/5582_128"
};

constexpr uint8_t STATION_COUNT = sizeof(stationUrls) / sizeof(stationUrls[0]);
uint8_t currentStation = 0;

// MAX98357A I2S bus (connect these three signals to BOTH modules).
constexpr int I2S_DOUT = 7;
constexpr int I2S_BCLK = 15;
constexpr int I2S_LRC = 16;

// Buttons to common GND; INPUT_PULLUP means pressed = LOW.
constexpr int BUTTON_PREV = 8;        // Button 1: short prev, long play/pause
constexpr int BUTTON_NEXT = 9;        // Button 2: short next
constexpr int BUTTON_VOLUME_DOWN = 10;// Button 3: short vol-
constexpr int BUTTON_VOLUME_UP = 11;  // Button 4: short vol+
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 800;

// OLED SSD1306 128x64 I2C. Change these only if your wiring differs.
constexpr int OLED_SDA = 1;
constexpr int OLED_SCL = 2;
constexpr uint8_t OLED_ADDRESS = 0x3C;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledReady = false;
String streamTitle;
uint32_t lastDisplayUpdate = 0;

Audio audio;
WebServer webServer(80);
uint8_t volume = 10;
bool radioPlaying = false;
bool inConfigPortal = false;

void toggleRadio();
void changeStation(int direction);
void startStation();
void updateDisplay(bool force = false);

const char RADIO_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP32 Radio</title><style>
body{font-family:Arial,sans-serif;max-width:480px;margin:30px auto;padding:0 18px;background:#111;color:#eee}
.card{background:#222;border-radius:14px;padding:20px;text-align:center;box-shadow:0 2px 12px #000}
h1{font-size:22px;margin:0 0 8px}p{color:#aaa}.row{display:flex;gap:10px;justify-content:center;flex-wrap:wrap}
button{font-size:17px;padding:12px 17px;border:0;border-radius:10px;background:#1683d8;color:white;cursor:pointer}
button:active{transform:scale(.97)}.secondary{background:#555}.stations{margin-top:18px;display:grid;gap:8px}
</style></head><body><div class='card'><h1>ESP32 Internet Radio</h1>
<p id='status'>Loading...</p><div class='row'><button onclick='cmd("toggle")'>Start / Stop</button><button onclick='cmd("prev")' class='secondary'>Previous</button><button onclick='cmd("next")'>Next</button></div>
<div class='row' style='margin-top:10px'><button onclick='cmd("vdown")' class='secondary'>Volume -</button><button onclick='cmd("vup")'>Volume +</button></div>
<div class='stations' id='stations'></div></div>
<script>
const cmd = async (c) => { await fetch('/api/' + c); update(); };
const update = async () => { let s = await (await fetch('/api/status')).json(); document.getElementById('status').textContent = (s.playing ? 'Playing: ' : 'Stopped: ') + s.station + ' | Volume ' + s.volume + '/21'; let d = document.getElementById('stations'); d.innerHTML = ''; s.stations.forEach((n, i) => { let b = document.createElement('button'); b.textContent = n; b.onclick = () => { fetch('/api/station/' + i).then(update); }; d.appendChild(b); }); };
update(); setInterval(update, 3000);
</script></body></html>
)rawliteral";

void sendStatus() {
  String json = "{\"playing\":" + String(radioPlaying ? "true" : "false") +
    ",\"station\":\"" + String(stationNames[currentStation]) +
    "\",\"volume\":" + String(volume) + ",\"stations\":[";
  for (uint8_t i = 0; i < STATION_COUNT; i++) {
    if (i) json += ",";
    json += "\"" + String(stationNames[i]) + "\"";
  }
  json += "]}";
  webServer.send(200, "application/json", json);
}

void handleWebCommand(const String& command) {
  if (command == "toggle") toggleRadio();
  else if (command == "next") changeStation(+1);
  else if (command == "prev") changeStation(-1);
  else if (command == "vup") { if (volume < 21) volume++; audio.setVolume(volume); }
  else if (command == "vdown") { if (volume > 0) volume--; audio.setVolume(volume); }
  webServer.send(200, "text/plain", "OK");
  updateDisplay(true);
}

void startWebServer() {
  webServer.on("/", HTTP_GET, []() { webServer.send_P(200, "text/html", RADIO_PAGE); });
  webServer.on("/api/status", HTTP_GET, sendStatus);
  webServer.on("/api/toggle", HTTP_GET, []() { handleWebCommand("toggle"); });
  webServer.on("/api/next", HTTP_GET, []() { handleWebCommand("next"); });
  webServer.on("/api/prev", HTTP_GET, []() { handleWebCommand("prev"); });
  webServer.on("/api/vup", HTTP_GET, []() { handleWebCommand("vup"); });
  webServer.on("/api/vdown", HTTP_GET, []() { handleWebCommand("vdown"); });
  for (uint8_t i = 0; i < STATION_COUNT; i++) {
    webServer.on(String("/api/station/") + i, HTTP_GET, [i]() {
      currentStation = i;
      startStation();
      radioPlaying = true;
      webServer.send(200, "text/plain", "OK");
      updateDisplay(true);
    });
  }
  webServer.begin();
  Serial.println("Web control: http://<ESP32-IP>/");
}

struct ButtonState {
  int pin;
  bool stableState;
  bool lastReading;
  uint32_t lastChange;
  uint32_t pressedAt;
  bool longPressHandled;
};
ButtonState buttons[] = {
  {BUTTON_PREV, HIGH, HIGH, 0, 0, false},
  {BUTTON_NEXT, HIGH, HIGH, 0, 0, false},
  {BUTTON_VOLUME_DOWN, HIGH, HIGH, 0, 0, false},
  {BUTTON_VOLUME_UP, HIGH, HIGH, 0, 0, false}
};

void showSetupScreen(const char* apName, const char* ipAddr) {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(18, 4);
  display.print("WiFi Setup Mode");
  display.drawFastHLine(0, 16, 128, SSD1306_WHITE);

  display.setCursor(0, 22);
  display.print("Connect to Wi-Fi AP:");
  display.setCursor(4, 34);
  display.setTextSize(1);
  display.printf("> %s", apName);

  display.setCursor(0, 48);
  display.print("Then open browser:");
  display.setCursor(4, 56);
  display.printf("http://%s", ipAddr);

  display.display();
}

void updateDisplay(bool force) {
  if (inConfigPortal) return;
  // Redraw often enough for a smooth-looking mini equalizer animation.
  if (!oledReady || (!force && millis() - lastDisplayUpdate < 100)) return;
  lastDisplayUpdate = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Station name only. Wrap at spaces, never in the middle of a word.
  display.setTextSize(2);
  String name = stationNames[currentStation];
  String line = "";
  int16_t y = 2;
  int16_t x1, y1;
  uint16_t textWidth, textHeight;
  int wordStart = 0;
  while (wordStart < name.length()) {
    while (wordStart < name.length() && name[wordStart] == ' ') wordStart++;
    if (wordStart >= name.length()) break;
    int wordEnd = name.indexOf(' ', wordStart);
    if (wordEnd < 0) wordEnd = name.length();
    String word = name.substring(wordStart, wordEnd);
    String candidate = line.length() ? line + " " + word : word;
    display.getTextBounds(candidate.c_str(), 0, 0, &x1, &y1, &textWidth, &textHeight);
    if (textWidth > 126 && line.length() > 0) {
      display.getTextBounds(line.c_str(), 0, 0, &x1, &y1, &textWidth, &textHeight);
      display.setCursor((128 - static_cast<int>(textWidth)) / 2, y);
      display.print(line);
      y += 17;
      line = word;
    } else {
      line = candidate;
    }
    wordStart = wordEnd + 1;
  }
  if (line.length() > 0 && y < 36) {
    display.getTextBounds(line.c_str(), 0, 0, &x1, &y1, &textWidth, &textHeight);
    display.setCursor((128 - static_cast<int>(textWidth)) / 2, y);
    display.print(line);
  }

  // Eight-bar mini equalizer. It is an activity animation, not an audio FFT.
  constexpr uint8_t BAR_COUNT = 8;
  constexpr uint8_t BAR_WIDTH = 9;
  constexpr uint8_t BAR_GAP = 6;
  constexpr uint8_t EQ_BASELINE = 63;
  constexpr uint8_t EQ_MAX_HEIGHT = 24;
  const int eqWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int eqX = (128 - eqWidth) / 2;
  uint32_t t = millis() / 70;

  for (uint8_t i = 0; i < BAR_COUNT; i++) {
    uint8_t height = 2;
    if (radioPlaying) {
      uint32_t wave = (t + i * 3) % 13;
      height = 4 + ((wave * (i + 3) + i * 5) % (EQ_MAX_HEIGHT - 3));
    }
    int x = eqX + i * (BAR_WIDTH + BAR_GAP);
    int y = EQ_BASELINE - height + 1;
    display.fillRect(x, y, BAR_WIDTH, height, SSD1306_WHITE);
  }

  display.display();
}

void startStation();

// Cycles through stations in a circle: wrap-around at both ends
void changeStation(int direction) {
  int next = static_cast<int>(currentStation) + direction;
  if (next < 0) next = STATION_COUNT - 1;
  if (next >= STATION_COUNT) next = 0;
  currentStation = static_cast<uint8_t>(next);
  if (WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; }
  updateDisplay(true);
}

void toggleRadio() {
  if (radioPlaying) {
    audio.stopSong();
    radioPlaying = false;
    Serial.println("Radio: stopped");
  } else if (WiFi.status() == WL_CONNECTED) {
    startStation();
    radioPlaying = true;
    Serial.println("Radio: playing");
  }
}

void handleShortPress(uint8_t index) {
  switch (index) {
    case 0: // Button 1: Previous station
      if (WiFi.status() == WL_CONNECTED) {
        changeStation(-1);
        radioPlaying = true;
      }
      break;
    case 1: // Button 2: Next station
      if (WiFi.status() == WL_CONNECTED) {
        changeStation(+1);
        radioPlaying = true;
      }
      break;
    case 2: // Button 3: Volume down
      if (volume > 0) volume--;
      audio.setVolume(volume);
      Serial.printf("Volume: %u/21\n", volume);
      break;
    case 3: // Button 4: Volume up
      if (volume < 21) volume++;
      audio.setVolume(volume);
      Serial.printf("Volume: %u/21\n", volume);
      break;
    default:
      break;
  }
  updateDisplay(true);
}

void handleLongPress(uint8_t index) {
  switch (index) {
    case 0: // Button 1: Long press -> Play / Pause (toggle)
      toggleRadio();
      break;
    case 1: // Button 2: Long press -> Reserved for custom action
      // Add your custom long-press action here (e.g. mute, favorite station)
      Serial.println("Button 2 long press");
      break;
    case 2: // Button 3: Long press -> Reserved for custom action
      // Add your custom long-press action here
      Serial.println("Button 3 long press");
      break;
    case 3: // Button 4: Long press -> Reserved for custom action
      // Add your custom long-press action here
      Serial.println("Button 4 long press");
      break;
    default:
      break;
  }
  updateDisplay(true);
}

void pollButtons() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    bool reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastReading) {
      buttons[i].lastChange = now;
      buttons[i].lastReading = reading;
    }
    if (now - buttons[i].lastChange >= BUTTON_DEBOUNCE_MS && reading != buttons[i].stableState) {
      buttons[i].stableState = reading;
      if (reading == LOW) {
        buttons[i].pressedAt = now;
        buttons[i].longPressHandled = false;
      } else if (!buttons[i].longPressHandled) {
        handleShortPress(i);
      }
    }
    if (buttons[i].stableState == LOW && !buttons[i].longPressHandled &&
        now - buttons[i].pressedAt >= BUTTON_LONG_PRESS_MS) {
      buttons[i].longPressHandled = true;
      handleLongPress(i);
    }
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  inConfigPortal = true;
  Serial.println("Entered WiFi Config Portal mode");
  Serial.print("Connect to AP: "); Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("Portal IP: "); Serial.println(WiFi.softAPIP());
  showSetupScreen(myWiFiManager->getConfigPortalSSID().c_str(), WiFi.softAPIP().toString().c_str());
}

void connectWiFi() {
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180); // 3 minutes timeout if unattended

  Serial.println("Attempting Wi-Fi connection...");
  bool connected = wm.autoConnect("ESP32-Radio-Setup");

  inConfigPortal = false;

  if (connected) {
    Serial.print("Wi-Fi connected! IP address: ");
    Serial.println(WiFi.localIP());
    startWebServer();
  } else {
    Serial.println("Failed to connect or portal timed out.");
  }
  updateDisplay(true);
}

void startStation() {
  audio.stopSong();
  Serial.printf("Station %u/%u: %s\n", currentStation + 1, STATION_COUNT, stationNames[currentStation]);
  Serial.print("Opening stream: "); Serial.println(stationUrls[currentStation]);
  if (!audio.connecttohost(stationUrls[currentStation])) Serial.println("Could not open stream.");
  streamTitle = "";
  updateDisplay(true);
}

void printStations() {
  Serial.println("Stations:");
  for (uint8_t i = 0; i < STATION_COUNT; i++) Serial.printf("  %u: %s\n", i + 1, stationNames[i]);
}

void printHelp() {
  Serial.println("Buttons: 1=prev stn (long: play/pause); 2=next stn; 3=vol-; 4=vol+");
  Serial.println("Long press threshold: 800 ms");
  Serial.printf("Serial: 1..%u station, n next, p previous, +/- volume, s stop, r restart, l list, h help\n", STATION_COUNT);
  printStations();
}

void setup() {
  Serial.begin(115200); delay(1000);
  Serial.println("\nESP32-S3 Stereo Wi-Fi Internet Radio");

  pinMode(BUTTON_PREV, INPUT_PULLUP);
  pinMode(BUTTON_NEXT, INPUT_PULLUP);
  pinMode(BUTTON_VOLUME_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_VOLUME_UP, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!oledReady) Serial.println("OLED not found at I2C address 0x3C");
  else { display.clearDisplay(); display.display(); }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; }
  printHelp(); updateDisplay(true);
}

void loop() {
  audio.loop();
  pollButtons();
  updateDisplay();
  webServer.handleClient();

  if (Serial.available()) {
    char command = Serial.read();
    if (command >= '1' && command <= '0' + STATION_COUNT) {
      currentStation = static_cast<uint8_t>(command - '1');
      if (WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; }
    } else {
      switch (command) {
        case '+': if (volume < 21) volume++; audio.setVolume(volume); break;
        case '-': if (volume > 0) volume--; audio.setVolume(volume); break;
        case 's': audio.stopSong(); radioPlaying = false; Serial.println("Stopped"); break;
        case 'r': if (WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; } break;
        case 'n': if (WiFi.status() == WL_CONNECTED) changeStation(+1); break;
        case 'p': if (WiFi.status() == WL_CONNECTED) changeStation(-1); break;
        case 'l': printStations(); break;
        case 'h': printHelp(); break;
        default: break;
      }
    }
    if (command == '+' || command == '-') Serial.printf("Volume: %u/21\n", volume);
    updateDisplay(true);
  }
}

void audio_info(const char* info) { Serial.print("audio: "); Serial.println(info); }
void audio_showstation(const char* info) { Serial.print("station: "); Serial.println(info); }
void audio_showstreamtitle(const char* info) { streamTitle = info; Serial.print("title: "); Serial.println(info); updateDisplay(true); }
