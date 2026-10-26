/* ESP32-S3 N16R8 Wi-Fi stereo radio + 4 buttons + OLED + REST API
   Station list and settings are persisted in NVS (Preferences).
   Sleep mode is a Wi-Fi-connected software sleep: audio and OLED stop while
   the web server remains available for /api/wake.
*/
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Update.h>
#include "Audio.h"

// Defaults are used only on the first boot, or after all saved stations are removed.
struct Station {
  String name;
  String url;
};

const char* DEFAULT_NAMES[] = {
  "Radio ROKS", "KISS FM Ukraine", "Lux FM", "Hit FM", "Lofi Beats"
};
const char* DEFAULT_URLS[] = {
  "http://online.radioroks.ua/RadioROKS",
  "https://online.kissfm.ua/KissFM_Ukr_HD",
  "https://lux.radio.tvstitch.com/lux_lviv_adv_sd",
  "http://online.hitfm.ua/HitFM",
  "http://das-sa39.cdnstream1.com/5582_128"
};
constexpr uint8_t DEFAULT_STATION_COUNT = sizeof(DEFAULT_URLS) / sizeof(DEFAULT_URLS[0]);
constexpr uint8_t MAX_STATIONS = 32;
Station stations[MAX_STATIONS];
uint8_t stationCount = 0;
uint8_t currentStation = 0;
Preferences preferences;

// MAX98357A I2S bus.
constexpr int I2S_DOUT = 7;
constexpr int I2S_BCLK = 15;
constexpr int I2S_LRC = 16;
// Set to a real amplifier enable pin if your hardware has one. -1 means unavailable.
constexpr int AUDIO_ENABLE_PIN = -1;

// Buttons to common GND; INPUT_PULLUP means pressed = LOW.
constexpr int BUTTON_PREV = 8;
constexpr int BUTTON_NEXT = 9;
constexpr int BUTTON_VOLUME_DOWN = 10;
constexpr int BUTTON_VOLUME_UP = 11;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 800;

// OLED SSD1306 128x64 I2C.
constexpr int OLED_SDA = 1;
constexpr int OLED_SCL = 2;
constexpr uint8_t OLED_ADDRESS = 0x3C;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledReady = false;
uint32_t lastDisplayUpdate = 0;
String streamTitle;

Audio audio;
WebServer webServer(80);
uint8_t volume = 10;
bool radioPlaying = false;
bool sleepMode = false;
bool resumeAfterSleep = false;
bool inConfigPortal = false;
bool volumeSavePending = false;
bool otaUploadActive = false;
bool otaUploadAccepted = false;
bool otaUploadFailed = false;

// OTA is intentionally unauthenticated, but only private RFC1918 LAN clients may upload.
const char* FIRMWARE_VERSION = "2.7.0-ota";
uint32_t volumeChangedAt = 0;
constexpr uint32_t VOLUME_SAVE_DELAY_MS = 3000;

void toggleRadio();
void changeStation(int direction);
void startStation();
void updateDisplay(bool force = false);
void pollButtons();
void configModeCallback(WiFiManager *myWiFiManager);
void connectWiFi();
void printStations();
void printHelp();
void setVolumeFromControl(int delta);
bool isPrivateLanClient();
void handleOtaUpload();
void handleOtaFinish();

const char RADIO_PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="uk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Radio</title><style>
:root{color-scheme:dark}body{font:16px system-ui,Arial;max-width:560px;margin:0 auto;padding:18px;background:#111;color:#eee}
.card{background:#222;border-radius:14px;padding:18px;margin-bottom:14px;box-shadow:0 2px 12px #000}h1{font-size:22px;margin:0 0 8px}
#status{color:#aaa;min-height:24px}.row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
button,input{font:inherit;border:0;border-radius:9px;padding:10px}button{background:#1683d8;color:#fff;cursor:pointer}button.secondary{background:#555}button.danger{background:#a83232}
input{box-sizing:border-box;background:#333;color:#fff;width:100%;margin:4px 0}form{display:grid;gap:4px}.station{display:flex;gap:8px;align-items:center;border-top:1px solid #444;padding:9px 0}.station span{flex:1;overflow:hidden}.station small{display:block;color:#999;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.station button{min-width:42px}.station button.danger{font-size:22px;padding:5px 12px}label{font-size:13px;color:#bbb;margin-top:4px}
</style></head><body><div class="card"><h1>ESP32 Internet Radio</h1><div id="status">Завантаження…</div>
<div class="row"><button onclick="cmd('/api/toggle')">Start / Stop</button><button class="secondary" onclick="cmd('/api/prev')">◀ Previous</button><button onclick="cmd('/api/next')">Next ▶</button></div>
<div class="row"><button class="secondary" onclick="cmd('/api/vdown')">Volume −</button><button onclick="cmd('/api/vup')">Volume +</button></div>
<div class="row"><button class="secondary" onclick="cmd('/api/sleep')">Sleep</button><button onclick="cmd('/api/wake')">Wake</button></div></div>
<div class="card"><h2>Firmware Update</h2><p>Current version: 2.7.0-ota</p><form id="otaForm" enctype="multipart/form-data"><label for="firmware">Select firmware.bin</label><input id="firmware" name="firmware" type="file" accept=".bin,application/octet-stream" required><button type="submit">Upload and reboot</button></form></div>
<div class="card"><h2>Radio Stations</h2><div id="stations"></div><h3>Add a station</h3><form id="addStationForm" onsubmit="addStation(event)"><label for="stationName">Station name</label><input id="stationName" required maxlength="48" autocomplete="off" placeholder="For example: My Radio"><label for="stationUrl">Audio stream URL</label><input id="stationUrl" required type="url" autocomplete="off" placeholder="https://example.com/stream.mp3"><button type="submit">Save station</button></form></div>
<script>
const api=async(u,o)=>{let r=await fetch(u,o);if(!r.ok){let t=await r.text();throw Error(t||('HTTP '+r.status))}return r};
const cmd=async(u)=>{try{await api(u);await update()}catch(e){alert('Error: '+e.message)}};
const removeStation=async(i,label)=>{if(confirm('Delete station \"'+label+'\"?'))await cmd('/api/stations/delete?id='+i)};
const addStation=async(e)=>{e.preventDefault();let nameInput=document.getElementById('stationName'),urlInput=document.getElementById('stationUrl');try{await api('/api/stations/add',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({name:nameInput.value.trim(),url:urlInput.value.trim()})});nameInput.value='';urlInput.value='';await update()}catch(x){alert('Could not save station: '+x.message)}};
const update=async()=>{try{let s=await (await api('/api/status')).json(),statusEl=document.getElementById('status'),stationsEl=document.getElementById('stations');statusEl.textContent=(s.sleeping?'Sleeping | ':'')+(s.playing?'Playing: ':'Stopped | ')+s.station+' | Volume '+s.volume+'/21 | IP '+s.ip;stationsEl.innerHTML='';s.stations.forEach((x,i)=>{let d=document.createElement('div');d.className='station';let info=document.createElement('span'),title=document.createElement('b'),urlText=document.createElement('small'),play=document.createElement('button'),remove=document.createElement('button');title.textContent=x.name;urlText.textContent=x.url;info.append(title,urlText);play.textContent='▶';play.onclick=()=>cmd('/api/station/'+i);remove.textContent='×';remove.title='Delete station';remove.className='danger';remove.onclick=()=>removeStation(i,x.name);d.append(info,play,remove);stationsEl.appendChild(d)})}catch(e){document.getElementById('status').textContent='Connection error'}};
update();setInterval(update,3000);
document.getElementById('otaForm').onsubmit=async e=>{e.preventDefault();let f=document.getElementById('firmware').files[0];if(!f||!confirm('Upload firmware and reboot the radio?'))return;let b=document.querySelector('#otaForm button');b.disabled=true;b.textContent='Uploading...';try{let r=await fetch('/update',{method:'POST',body:new FormData(document.getElementById('otaForm'))});let x=await r.json();alert(x.ok?'Firmware uploaded. Radio is rebooting.':'OTA failed: '+(x.error||'unknown error'));}catch(x){alert('OTA request failed: '+x.message)}finally{b.disabled=false;b.textContent='Upload and reboot'}};
</script></body></html>
)rawliteral";

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

void saveSettings() {
  preferences.begin("radio", false);
  preferences.putUChar("count", stationCount);
  preferences.putUChar("current", currentStation);
  preferences.putUChar("volume", volume);
  for (uint8_t i = 0; i < MAX_STATIONS; i++) {
    String nameKey = "n" + String(i);
    String urlKey = "u" + String(i);
    if (i < stationCount) {
      preferences.putString(nameKey.c_str(), stations[i].name);
      preferences.putString(urlKey.c_str(), stations[i].url);
    } else {
      preferences.remove(nameKey.c_str());
      preferences.remove(urlKey.c_str());
    }
  }
  preferences.end();
}

void loadSettings() {
  preferences.begin("radio", true);
  uint8_t savedCount = preferences.getUChar("count", 0);
  volume = min<uint8_t>(preferences.getUChar("volume", 10), 21);
  currentStation = preferences.getUChar("current", 0);
  if (savedCount == 0 || savedCount > MAX_STATIONS) {
    stationCount = DEFAULT_STATION_COUNT;
    for (uint8_t i = 0; i < stationCount; i++) {
      stations[i].name = DEFAULT_NAMES[i];
      stations[i].url = DEFAULT_URLS[i];
    }
    currentStation = 0;
    preferences.end();
    saveSettings();
    return;
  }
  stationCount = savedCount;
  for (uint8_t i = 0; i < stationCount; i++) {
    stations[i].name = preferences.getString(("n" + String(i)).c_str(), "Station");
    stations[i].url = preferences.getString(("u" + String(i)).c_str(), "");
  }
  preferences.end();
  if (currentStation >= stationCount) currentStation = 0;
}

void sendStatus() {
  String json = "{\"playing\":" + String(radioPlaying ? "true" : "false") +
    ",\"sleeping\":" + String(sleepMode ? "true" : "false") +
    ",\"volume\":" + String(volume) +
    ",\"stationIndex\":" + String(currentStation) +
    ",\"station\":\"" + jsonEscape(stations[currentStation].name) +
    "\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"stations\":[";
  for (uint8_t i = 0; i < stationCount; i++) {
    if (i) json += ",";
    json += "{\"name\":\"" + jsonEscape(stations[i].name) +
      "\",\"url\":\"" + jsonEscape(stations[i].url) + "\"}";
  }
  json += "]}";
  webServer.send(200, "application/json; charset=utf-8", json);
}

void sendError(int code, const char* message) {
  webServer.send(code, "application/json", String("{\"error\":\"") + jsonEscape(message) + "\"}");
}

bool validStreamUrl(const String& url) {
  return (url.startsWith("http://") || url.startsWith("https://")) && url.length() <= 240;
}

void apiAddStation() {
  if (stationCount >= MAX_STATIONS) { sendError(409, "Station limit reached"); return; }
  String name = webServer.arg("name");
  String url = webServer.arg("url");
  name.trim(); url.trim();
  if (name.length() == 0 || name.length() > 48 || !validStreamUrl(url)) {
    sendError(400, "Name or stream URL is invalid"); return;
  }
  stations[stationCount].name = name;
  stations[stationCount].url = url;
  stationCount++;
  saveSettings();
  webServer.send(201, "application/json", "{\"ok\":true}");
}

void apiDeleteStation() {
  if (!webServer.hasArg("id")) { sendError(400, "Missing station id"); return; }
  int index = webServer.arg("id").toInt();
  if (index < 0 || index >= stationCount) { sendError(404, "Station not found"); return; }
  if (stationCount == 1) { sendError(409, "At least one station must remain"); return; }
  bool deletedCurrent = currentStation == index;
  for (int i = index; i < stationCount - 1; i++) stations[i] = stations[i + 1];
  stationCount--;
  if (currentStation > index || deletedCurrent) currentStation = min<uint8_t>(currentStation, stationCount - 1);
  saveSettings();
  if (deletedCurrent && radioPlaying) startStation();
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void apiStationByPath(const String& path) {
  int index = path.substring(String("/api/station/").length()).toInt();
  if (index < 0 || index >= stationCount) { sendError(404, "Station not found"); return; }
  currentStation = index;
  saveSettings();
  if (!sleepMode && WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; }
  webServer.send(200, "text/plain", "OK");
  updateDisplay(true);
}

void sleepRadio() {
  if (sleepMode) return;
  resumeAfterSleep = radioPlaying;
  audio.stopSong();
  radioPlaying = false;
  sleepMode = true;
  if (AUDIO_ENABLE_PIN >= 0) digitalWrite(AUDIO_ENABLE_PIN, LOW);
  if (oledReady) display.ssd1306_command(SSD1306_DISPLAYOFF);
  Serial.println("Radio: sleep mode (Wi-Fi remains connected)");
}

void wakeRadio() {
  if (!sleepMode) return;
  sleepMode = false;
  if (AUDIO_ENABLE_PIN >= 0) digitalWrite(AUDIO_ENABLE_PIN, HIGH);
  if (oledReady) display.ssd1306_command(SSD1306_DISPLAYON);
  audio.setVolume(volume);
  if (resumeAfterSleep && WiFi.status() == WL_CONNECTED) {
    startStation();
    radioPlaying = true;
  }
  resumeAfterSleep = false;
  updateDisplay(true);
  Serial.println("Radio: awake");
}

void setVolumeFromControl(int delta) {
  if (sleepMode) return;
  int nextVolume = constrain((int)volume + delta, 0, 21);
  if (nextVolume == volume) return;
  volume = (uint8_t)nextVolume;
  // Apply immediately; do not write NVS in the request/button/audio path.
  audio.setVolume(volume);
  volumeSavePending = true;
  volumeChangedAt = millis();
}

void handleWebCommand(const String& command) {
  if (command == "toggle" && !sleepMode) toggleRadio();
  else if (command == "next" && !sleepMode) changeStation(+1);
  else if (command == "prev" && !sleepMode) changeStation(-1);
  else if (command == "vup") setVolumeFromControl(+1);
  else if (command == "vdown") setVolumeFromControl(-1);
  else if (command == "sleep") sleepRadio();
  else if (command == "wake") wakeRadio();
  webServer.send(200, "text/plain", "OK");
  updateDisplay(true);
}

void handleNotFound() {
  String path = webServer.uri();
  if (path.startsWith("/api/station/") && webServer.method() == HTTP_GET) {
    apiStationByPath(path); return;
  }
  sendError(404, "Not found");
}

bool isPrivateLanClient() {
  IPAddress ip = webServer.client().remoteIP();
  uint8_t a = ip[0], b = ip[1];
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

void handleOtaUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaUploadActive = false;
    otaUploadAccepted = isPrivateLanClient() && upload.filename.endsWith(".bin");
    otaUploadFailed = !otaUploadAccepted;
    if (!otaUploadAccepted) return;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { Update.printError(Serial); otaUploadFailed = true; return; }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaUploadAccepted && !otaUploadFailed && Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); otaUploadFailed = true; }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaUploadAccepted && !otaUploadFailed && Update.end(true)) otaUploadActive = true;
    else { Update.printError(Serial); otaUploadFailed = true; }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaUploadFailed = true;
  }
}

void handleOtaFinish() {
  if (!isPrivateLanClient() || !otaUploadActive) {
    webServer.send(403, "application/json", "{\"ok\":false,\"error\":\"LAN OTA upload failed\"}");
    return;
  }
  webServer.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  delay(250);
  ESP.restart();
}

void startWebServer() {
  webServer.on("/", HTTP_GET, []() { webServer.send_P(200, "text/html; charset=utf-8", RADIO_PAGE); });
  webServer.on("/api/status", HTTP_GET, sendStatus);
  webServer.on("/api/toggle", HTTP_GET, []() { handleWebCommand("toggle"); });
  webServer.on("/api/next", HTTP_GET, []() { handleWebCommand("next"); });
  webServer.on("/api/prev", HTTP_GET, []() { handleWebCommand("prev"); });
  webServer.on("/api/vup", HTTP_GET, []() { handleWebCommand("vup"); });
  webServer.on("/api/vdown", HTTP_GET, []() { handleWebCommand("vdown"); });
  webServer.on("/api/sleep", HTTP_GET, []() { handleWebCommand("sleep"); });
  webServer.on("/api/wake", HTTP_GET, []() { handleWebCommand("wake"); });
  webServer.on("/api/stations/add", HTTP_POST, apiAddStation);
  webServer.on("/api/stations/add", HTTP_GET, apiAddStation);
  webServer.on("/api/stations/delete", HTTP_POST, apiDeleteStation);
  webServer.on("/api/stations/delete", HTTP_GET, apiDeleteStation);
  webServer.on("/update", HTTP_POST, handleOtaFinish, handleOtaUpload);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  Serial.print("Web control: http://"); Serial.print(WiFi.localIP()); Serial.println("/");
}

struct ButtonState { int pin; bool stableState; bool lastReading; uint32_t lastChange; uint32_t pressedAt; bool longPressHandled; };
ButtonState buttons[] = {
  {BUTTON_PREV, HIGH, HIGH, 0, 0, false}, {BUTTON_NEXT, HIGH, HIGH, 0, 0, false},
  {BUTTON_VOLUME_DOWN, HIGH, HIGH, 0, 0, false}, {BUTTON_VOLUME_UP, HIGH, HIGH, 0, 0, false}
};

void showSetupScreen(const char* apName, const char* ipAddr) {
  if (!oledReady) return;
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor(18, 4); display.print("WiFi Setup Mode"); display.drawFastHLine(0, 16, 128, SSD1306_WHITE);
  display.setCursor(0, 22); display.print("Connect to Wi-Fi AP:"); display.setCursor(4, 34); display.printf("> %s", apName);
  display.setCursor(0, 48); display.print("Then open browser:"); display.setCursor(4, 56); display.printf("http://%s", ipAddr); display.display();
}

void updateDisplay(bool force) {
  if (inConfigPortal || sleepMode || !oledReady || (!force && millis() - lastDisplayUpdate < 100)) return;
  lastDisplayUpdate = millis(); display.clearDisplay(); display.setTextColor(SSD1306_WHITE); display.setTextSize(2);
  String name = stations[currentStation].name, line = ""; int16_t y = 2, x1, y1; uint16_t w, h; int wordStart = 0;
  while (wordStart < (int)name.length()) {
    while (wordStart < (int)name.length() && name[wordStart] == ' ') wordStart++;
    if (wordStart >= (int)name.length()) break;
    int wordEnd = name.indexOf(' ', wordStart); if (wordEnd < 0) wordEnd = name.length();
    String word = name.substring(wordStart, wordEnd), candidate = line.length() ? line + " " + word : word;
    display.getTextBounds(candidate.c_str(), 0, 0, &x1, &y1, &w, &h);
    if (w > 126 && line.length()) { display.getTextBounds(line.c_str(), 0, 0, &x1, &y1, &w, &h); display.setCursor((128 - (int)w) / 2, y); display.print(line); y += 17; line = word; }
    else line = candidate;
    wordStart = wordEnd + 1;
  }
  if (line.length() && y < 36) { display.getTextBounds(line.c_str(), 0, 0, &x1, &y1, &w, &h); display.setCursor((128 - (int)w) / 2, y); display.print(line); }
  constexpr uint8_t BAR_COUNT = 8, BAR_WIDTH = 9, BAR_GAP = 6, EQ_BASELINE = 63, EQ_MAX_HEIGHT = 24;
  int eqWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP, eqX = (128 - eqWidth) / 2; uint32_t t = millis() / 70;
  for (uint8_t i = 0; i < BAR_COUNT; i++) { uint8_t barHeight = radioPlaying ? 4 + (((t + i * 3) % 13 * (i + 3) + i * 5) % (EQ_MAX_HEIGHT - 3)) : 2; display.fillRect(eqX + i * (BAR_WIDTH + BAR_GAP), EQ_BASELINE - barHeight + 1, BAR_WIDTH, barHeight, SSD1306_WHITE); }
  display.display();
}

void changeStation(int direction) {
  int next = (int)currentStation + direction;
  if (next < 0) next = stationCount - 1; if (next >= stationCount) next = 0;
  currentStation = next; saveSettings();
  if (!sleepMode && WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; }
  updateDisplay(true);
}

void toggleRadio() {
  if (sleepMode) return;
  if (radioPlaying) { audio.stopSong(); radioPlaying = false; Serial.println("Radio: stopped"); }
  else if (WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; Serial.println("Radio: playing"); }
}

void handleShortPress(uint8_t index) {
  if (sleepMode) return;
  switch (index) {
    case 0: changeStation(-1); break; case 1: changeStation(+1); break;
    case 2: setVolumeFromControl(-1); break;
    case 3: setVolumeFromControl(+1); break;
  }
  updateDisplay(true);
}

void handleLongPress(uint8_t index) { if (index == 0) toggleRadio(); updateDisplay(true); }

void pollButtons() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    bool reading = digitalRead(buttons[i].pin);
    if (reading != buttons[i].lastReading) { buttons[i].lastChange = now; buttons[i].lastReading = reading; }
    if (now - buttons[i].lastChange >= BUTTON_DEBOUNCE_MS && reading != buttons[i].stableState) {
      buttons[i].stableState = reading;
      if (reading == LOW) { buttons[i].pressedAt = now; buttons[i].longPressHandled = false; }
      else if (!buttons[i].longPressHandled) handleShortPress(i);
    }
    if (buttons[i].stableState == LOW && !buttons[i].longPressHandled && now - buttons[i].pressedAt >= BUTTON_LONG_PRESS_MS) { buttons[i].longPressHandled = true; handleLongPress(i); }
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  inConfigPortal = true; Serial.println("Entered WiFi Config Portal mode"); Serial.print("Connect to AP: "); Serial.println(myWiFiManager->getConfigPortalSSID()); Serial.print("Portal IP: "); Serial.println(WiFi.softAPIP());
  showSetupScreen(myWiFiManager->getConfigPortalSSID().c_str(), WiFi.softAPIP().toString().c_str());
}

void connectWiFi() {
  WiFiManager wm; wm.setAPCallback(configModeCallback); wm.setConfigPortalTimeout(180);
  Serial.println("Attempting Wi-Fi connection..."); bool connected = wm.autoConnect("ESP32-Radio-Setup"); inConfigPortal = false;
  if (connected) { Serial.print("Wi-Fi connected! IP address: "); Serial.println(WiFi.localIP()); startWebServer(); }
  else Serial.println("Failed to connect or portal timed out.");
  updateDisplay(true);
}

void startStation() {
  audio.stopSong(); Serial.printf("Station %u/%u: %s\n", currentStation + 1, stationCount, stations[currentStation].name.c_str()); Serial.print("Opening stream: "); Serial.println(stations[currentStation].url);
  if (!audio.connecttohost(stations[currentStation].url.c_str())) Serial.println("Could not open stream.");
  streamTitle = ""; updateDisplay(true);
}

void printStations() { Serial.println("Stations:"); for (uint8_t i = 0; i < stationCount; i++) Serial.printf("  %u: %s (%s)\n", i + 1, stations[i].name.c_str(), stations[i].url.c_str()); }
void printHelp() { Serial.println("Buttons: 1=prev, 2=next, 3=vol-, 4=vol+; long 1=play/pause"); Serial.println("Web API: /api/status, /api/stations/add, /api/stations/delete, /api/sleep, /api/wake"); printStations(); }

void setup() {
  Serial.begin(115200); delay(1000); Serial.println("\nESP32-S3 Stereo Wi-Fi Internet Radio");
  pinMode(BUTTON_PREV, INPUT_PULLUP); pinMode(BUTTON_NEXT, INPUT_PULLUP); pinMode(BUTTON_VOLUME_DOWN, INPUT_PULLUP); pinMode(BUTTON_VOLUME_UP, INPUT_PULLUP);
  if (AUDIO_ENABLE_PIN >= 0) { pinMode(AUDIO_ENABLE_PIN, OUTPUT); digitalWrite(AUDIO_ENABLE_PIN, HIGH); }
  loadSettings(); Wire.begin(OLED_SDA, OLED_SCL); oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!oledReady) Serial.println("OLED not found at I2C address 0x3C"); else { display.clearDisplay(); display.display(); }
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT); audio.setVolume(volume);
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED && stationCount > 0) { startStation(); radioPlaying = true; }
  printHelp(); updateDisplay(true);
}

void loop() {
  if (!sleepMode) { audio.loop(); updateDisplay(); }
  pollButtons(); webServer.handleClient();
  if (volumeSavePending && millis() - volumeChangedAt >= VOLUME_SAVE_DELAY_MS) {
    saveSettings();
    volumeSavePending = false;
  }
  if (Serial.available()) {
    char command = Serial.read();
    if (command >= '1' && command <= '0' + stationCount) { currentStation = command - '1'; if (!sleepMode && WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; } saveSettings(); }
    else switch (command) {
      case '+': setVolumeFromControl(+1); break; case '-': setVolumeFromControl(-1); break;
      case 's': if (!sleepMode) { audio.stopSong(); radioPlaying = false; } break; case 'r': if (!sleepMode && WiFi.status() == WL_CONNECTED) { startStation(); radioPlaying = true; } break;
      case 'n': if (!sleepMode) changeStation(+1); break; case 'p': if (!sleepMode) changeStation(-1); break; case 'l': printStations(); break; case 'h': printHelp(); break;
      default: break;
    }
    updateDisplay(true);
  }
}

void audio_info(const char* info) { Serial.print("audio: "); Serial.println(info); }
void audio_showstation(const char* info) { Serial.print("station: "); Serial.println(info); }
void audio_showstreamtitle(const char* info) { streamTitle = info; Serial.print("title: "); Serial.println(info); updateDisplay(true); }
