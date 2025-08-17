/*
  ESP32_MusicPlayer.ino
  - ESP32-WROOM + ST7735 1.44" (SPI)
  - MAX98357A I2S amp (I2S_NUM_0)
  - Bluetooth A2DP Sink -> I2S (phone streams audio to ESP32)
  - Two modes:
      * Bluetooth only (play audio from phone via A2DP)
      * Spotify mode (choose WiFi or SIM, connect network, fetch /now.json & /cover.rgb565 from bridge, then wait for BT)
  - 3 buttons: LEFT / RIGHT / OK (INPUT_PULLUP)
  - TinyGSM configured for SIM7600 family by default (change if needed)
  - All UI strings are plain ASCII (no accented characters)
*/

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <BluetoothA2DPSink.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>

/////////////////////
// Optional flags  //
/////////////////////
#define USE_A2DP_METADATA 0   // set 1 if your A2DP library supports set_avrc_metadata_callback
#define USE_A2DP_VOLUME   0   // set 1 if your A2DP library supports volume callback registration

/////////////////////
// Pin definitions //
/////////////////////
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4
#define BTN_OK    15
#define BTN_LEFT  32
#define BTN_RIGHT 33
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  22
#define SIM_RX    16
#define SIM_TX    17
#define SIM_BAUD  115200
#define DEFAULT_BRIDGE "http://192.168.4.1:5000"

/////////////////////
// Globals & Libs  //
/////////////////////
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
BluetoothA2DPSink a2dp;
Preferences prefs;
HardwareSerial SerialAT(2);
TinyGsm modem(SerialAT);
TinyGsmClient simClient(modem);
bool simReady = false;

/////////////////////
// App enums       //
/////////////////////
enum BootMode { MODE_BT_ONLY = 0, MODE_SPOTIFY = 1 };
enum NetChoice { NET_WIFI = 0, NET_SIM = 1 };
enum AppState { ST_PICK_MODE, ST_PICK_NET, ST_WIFI_SCAN, ST_WIFI_KEYBOARD, ST_NET_READY, ST_WAIT_BT, ST_PLAYER };

BootMode chosenMode = MODE_BT_ONLY;
NetChoice chosenNet = NET_WIFI;
AppState appState = ST_PICK_MODE;

/////////////////////
// WiFi scan store //
/////////////////////
static const int MAX_AP = 16;
String ap_ssid[MAX_AP];
int ap_rssi[MAX_AP];
int ap_count = 0;
int ap_sel = 0;

/////////////////////
// Metadata / UI   //
/////////////////////
String currentTitle = "";
String currentArtist = "";
uint8_t currentBars = 3; // 1..5
volatile bool remoteMuted = false;
volatile uint8_t remoteVolume = 127;
unsigned long lastDeb = 0;

/////////////////////
// Keyboard layout //
/////////////////////
const char kb_up[4][12] = {
  {'0','1','2','3','4','5','6','7','8','9','!','@'},
  {'#','$','%','^','&','*','(',')','-','_','=','+'},
  {'A','B','C','D','E','F','G','H','I','J','K','L'},
  {'M','N','O','P','Q','R','S','T','U','V','W',' '}
};
const char kb_low[4][12] = {
  {'a','b','c','d','e','f','g','h','i','j','k','l'},
  {'m','n','o','p','q','r','s','t','u','v','w','x'},
  {'y','z','0','1','2','3','4','5','6','7','8','9'},
  {'!','@','#','$','%','^','&','*','(',')','-',' '}
};

/////////////////////
// Prototypes      //
/////////////////////
void drawPickMode();
void drawPickNet();
void doWifiScan();
void drawWifiList();
void drawKeyboardScreen(const String &preview, int selX, int selY, bool lower);
bool keyboardInput(String &outPassword, const String &ssid);
bool wifiConnectAndSave(const String &ssid, const String &pass, uint8_t &barsOut);
void initI2S();
bool simInit(const char* apn); // Fixed: Corrected prototype
bool simHttpGetText(const String &url, String &out);
bool simHttpGetRawToTFT(const String &url);
bool fetchNowJSONWiFi(String &outTitle, String &outArtist, uint8_t &bars);
bool fetchCoverWiFiToTFT();
void drawPlayerScreen(bool forceFetch);
void drawFallbackThumb();
void drawWifiBars(int x, int y, int strength);
bool checkNetwork();

/////////////////////
// Bluetooth AVRCP //
/////////////////////
#if USE_A2DP_METADATA
void avrc_metadata_cb(uint8_t id, const uint8_t *text) {
  String s = String((char*)text);
  if (id == 1) currentTitle = s;
  else if (id == 2) currentArtist = s;
}
#endif

#if USE_A2DP_VOLUME
void avrc_volume_cb(uint8_t vol) {
  remoteVolume = vol;
  remoteMuted = (vol == 0);
  if (remoteMuted) tft.fillRect(100,2,24,24, ST77XX_WHITE);
  else tft.fillRect(100,2,24,24, ST77XX_BLACK);
}
#endif

/////////////////////
// TinyGSM / SIM   //
/////////////////////
bool simInit(const char* apn) {
  SerialAT.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
  delay(200);
  for (int i = 0; i < 3; i++) {
    if (modem.init()) break;
    Serial.println("modem.init() failed, retrying...");
    if (modem.restart()) break;
    delay(1000);
  }
  if (!modem.isNetworkConnected()) {
    Serial.println("Waiting for network registration (60s)...");
    if (!modem.waitForNetwork(60000L)) {
      Serial.println("Network registration failed");
      return false;
    }
  }
  Serial.println("Network registered, connecting GPRS...");
  if (!modem.gprsConnect(apn, "", "")) {
    Serial.println("gprsConnect failed");
    return false;
  }
  simReady = modem.isGprsConnected();
  Serial.printf("SIM ready: %d\n", simReady);
  return simReady;
}

bool simHttpGetText(const String &url, String &out) {
  out = "";
  if (!simReady) return false;
  if (!url.startsWith("http://")) return false;
  String u = url.substring(7);
  int slash = u.indexOf('/');
  String host = (slash == -1) ? u : u.substring(0, slash);
  String path = (slash == -1) ? "/" : u.substring(slash);
  int port = 80;
  int colon = host.indexOf(':');
  if (colon != -1) {
    port = host.substring(colon+1).toInt();
    host = host.substring(0, colon);
  }
  if (!simClient.connect(host.c_str(), port)) return false;
  String req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
  simClient.print(req);
  unsigned long t0 = millis();
  bool headersPassed = false;
  String payload = "";
  while (millis() - t0 < 8000) {
    while (simClient.available()) {
      String line = simClient.readStringUntil('\n');
      if (!headersPassed) {
        if (line == "\r") headersPassed = true;
      } else {
        payload += line + "\n";
      }
      t0 = millis();
    }
    if (!simClient.connected()) break;
  }
  simClient.stop();
  if (payload.length() == 0) return false;
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;
  out = doc["title"] | "";
  currentArtist = doc["artist"] | "";
  currentBars = constrain(doc["strength"] | 3, 1, 5);
  return out.length() > 0;
}

bool simHttpGetRawToTFT(const String &url) {
  if (!simReady) return false;
  if (!url.startsWith("http://")) return false;
  String u = url.substring(7);
  int slash = u.indexOf('/');
  String host = (slash == -1) ? u : u.substring(0, slash);
  String path = (slash == -1) ? "/" : u.substring(slash);
  int port = 80;
  int colon = host.indexOf(':');
  if (colon != -1) {
    port = host.substring(colon+1).toInt();
    host = host.substring(0, colon);
  }
  if (!simClient.connect(host.c_str(), port)) return false;
  String req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
  simClient.print(req);
  unsigned long t0 = millis();
  String headers = "";
  while (millis() - t0 < 8000) {
    if (simClient.available()) {
      char c = simClient.read();
      headers += c;
      if (headers.indexOf("\r\n\r\n") != -1) break;
    }
  }
  if (headers.indexOf("\r\n\r\n") == -1) { simClient.stop(); return false; }
  size_t need = 128UL * 128UL * 2UL; // 128x128 pixels, 2 bytes per pixel (RGB565)
  tft.startWrite();
  tft.setAddrWindow(0, 0, 128, 128);
  const int CH = 512;
  static uint8_t buf[CH];
  int x = 0, y = 0;
  while (need > 0 && (millis() - t0 < 15000)) {
    if (simClient.available()) {
      int r = simClient.readBytes(buf, min((size_t)CH, need));
      if (r > 0) {
        for (int i = 0; i < r - 1; i += 2) { // Process 2 bytes at a time (RGB565)
          uint16_t color = (buf[i] << 8) | buf[i + 1]; // Combine high and low bytes
          tft.pushColor(color);
          x++;
          if (x >= 128) {
            x = 0;
            y++;
            if (y >= 128) break; // Ensure we don't exceed display bounds
          }
        }
        need -= r;
        t0 = millis();
      }
    }
  }
  tft.endWrite();
  simClient.stop();
  if (need > 0) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(6, 56);
    tft.setTextColor(ST77XX_RED);
    tft.print("Image fetch failed");
    delay(1000);
    return false;
  }
  return true;
}

/////////////////////
// WiFi / Bridge   //
/////////////////////
bool fetchNowJSONWiFi(String &outTitle, String &outArtist, uint8_t &bars) {
  outTitle = ""; outArtist = ""; bars = 3;
  String bridge = prefs.getString("bridge", DEFAULT_BRIDGE);
  String url = bridge + "/now.json";
  HTTPClient http;
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;
  outTitle = doc["title"] | "";
  outArtist = doc["artist"] | "";
  bars = constrain(doc["strength"] | 3, 1, 5);
  return true;
}

bool fetchCoverWiFiToTFT() {
  String bridge = prefs.getString("bridge", DEFAULT_BRIDGE);
  String url = bridge + "/cover.rgb565";
  HTTPClient http;
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  WiFiClient *stream = http.getStreamPtr();
  tft.startWrite();
  tft.setAddrWindow(0, 0, 128, 128);
  const int CH = 1024;
  static uint8_t buf[CH];
  size_t need = 128UL * 128UL * 2UL; // 128x128 pixels, 2 bytes per pixel (RGB565)
  int x = 0, y = 0;
  unsigned long t0 = millis();
  while (need > 0 && (millis() - t0 < 15000)) {
    size_t n = stream->readBytes(buf, min((size_t)CH, need));
    if (n == 0) break;
    for (int i = 0; i < n - 1; i += 2) { // Process 2 bytes at a time (RGB565)
      uint16_t color = (buf[i] << 8) | buf[i + 1]; // Combine high and low bytes
      tft.pushColor(color);
      x++;
      if (x >= 128) {
        x = 0;
        y++;
        if (y >= 128) break; // Ensure we don't exceed display bounds
      }
    }
    need -= n;
  }
  tft.endWrite();
  http.end();
  if (need > 0) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(6, 56);
    tft.setTextColor(ST77XX_RED);
    tft.print("Image fetch failed");
    delay(1000);
    return false;
  }
  return true;
}

/////////////////////
// UI helpers      //
/////////////////////
void drawWifiBars(int x, int y, int strength) {
  strength = constrain(strength, 0, 5);
  for (int i = 0; i < 5; i++) {
    int h = 3 + i * 3;
    int w = 3;
    int bx = x + i * 6;
    int by = y - h;
    uint16_t col = (i < strength) ? ST77XX_WHITE : ST77XX_BLACK;
    tft.fillRect(bx, by, w, h, col);
  }
  if (strength == 0) {
    tft.drawLine(x, y - 10, x + 20, y, ST77XX_RED);
    tft.drawLine(x, y, x + 20, y - 10, ST77XX_RED);
  }
}

void drawFallbackThumb() {
  tft.fillRect(0, 0, 128, 96, ST77XX_BLUE);
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(46, 34);
  tft.print("♪");
}

void drawPickMode() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 12);
  tft.print("Choose mode:");
  tft.setCursor(12, 40);
  if (chosenMode == MODE_BT_ONLY) {
    tft.setTextColor(ST77XX_WHITE); tft.print("[*] Bluetooth only");
    tft.setCursor(12, 60); tft.setTextColor(ST77XX_GREEN); tft.print("[ ] Spotify mode");
  } else {
    tft.setTextColor(ST77XX_GREEN); tft.print("[ ] Bluetooth only");
    tft.setCursor(12, 60); tft.setTextColor(ST77XX_WHITE); tft.print("[*] Spotify mode");
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(6, 100);
  tft.print("LEFT/RIGHT to choose, OK to enter");
}

void drawPickNet() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 18);
  tft.print("Choose network for Spotify:");
  tft.setCursor(12, 48);
  if (chosenNet == NET_WIFI) {
    tft.setTextColor(ST77XX_WHITE); tft.print("[*] Wi-Fi");
    tft.setCursor(12, 68); tft.setTextColor(ST77XX_GREEN); tft.print("[ ] SIM 4G");
  } else {
    tft.setTextColor(ST77XX_GREEN); tft.print("[ ] Wi-Fi");
    tft.setCursor(12, 68); tft.setTextColor(ST77XX_WHITE); tft.print("[*] SIM 4G");
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(6, 100);
  tft.print("LEFT/RIGHT to choose, OK to select");
}

void doWifiScan() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(6, 6); tft.setTextColor(ST77XX_WHITE); tft.print("Scanning Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(120);
  ap_count = WiFi.scanNetworks();
  if (ap_count < 1) {
    ap_count = 1;
    ap_ssid[0] = "(no networks)";
    ap_rssi[0] = -100;
  } else if (ap_count > MAX_AP) ap_count = MAX_AP;
  for (int i = 0; i < ap_count; i++) {
    ap_ssid[i] = WiFi.SSID(i);
    ap_rssi[i] = WiFi.RSSI(i);
  }
  ap_sel = 0;
}

void drawWifiList() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  int show = min(ap_count, 7);
  for (int i = 0; i < show; i++) {
    int y = 6 + i * 17;
    bool on = (i == ap_sel);
    if (on) { tft.fillRect(0, y - 2, 128, 14, ST77XX_WHITE); tft.setTextColor(ST77XX_BLACK); }
    else tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(4, y);
    tft.print(ap_ssid[i]);
    int s = map(ap_rssi[i], -90, -30, 1, 5);
    s = constrain(s, 0, 5);
    drawWifiBars(128 - 36, y + 12, s);
  }
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 128 - 12);
  tft.print("OK = select, LEFT/RIGHT = navigate");
}

void drawKeyboardScreen(const String &preview, int selX, int selY, bool lower) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 4); tft.print("Enter Wi-Fi password:");
  tft.fillRect(4, 16, 120, 12, ST77XX_WHITE);
  tft.setCursor(6, 18); tft.setTextColor(ST77XX_BLACK); tft.print(preview);
  int startY = 34;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 12; c++) {
      int x = 6 + c * 9;
      int y = startY + r * 12;
      char ch = lower ? kb_low[r][c] : kb_up[r][c];
      bool on = (selX == c && selY == r);
      if (on) tft.fillRect(x - 2, y - 2, 10, 10, ST77XX_WHITE);
      tft.setTextColor(on ? ST77XX_BLACK : ST77XX_GREEN);
      tft.setCursor(x, y);
      tft.print(ch);
    }
  }
  int y2 = startY + 4 * 12 + 6;
  tft.fillRect(10, y2, 44, 12, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(22, y2 + 2); tft.print("DEL");
  tft.fillRect(74, y2, 44, 12, ST77XX_BLACK);
  tft.setCursor(82, y2 + 2); tft.print("DONE");
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(4, 128 - 10);
  tft.print("Hold OK to toggle case");
}

bool keyboardInput(String &outPassword, const String &ssid) {
  int selX = 0, selY = 0;
  bool lower = false;
  String cur = "";
  unsigned long last = 0;
  unsigned long okHoldStart = 0;
  while (true) {
    drawKeyboardScreen(cur, selX, selY, lower);
    if (millis() - last < 150) { delay(5); continue; }
    if (digitalRead(BTN_RIGHT) == LOW) {
      if (selY < 4) selX = (selX + 1) % 12;
      else selX = (selX + 1) % 2;
      last = millis(); continue;
    }
    if (digitalRead(BTN_LEFT) == LOW) {
      if (selY < 4) selX = (selX - 1 < 0) ? 11 : selX - 1;
      else selX = (selX - 1 < 0) ? 1 : selX - 1;
      last = millis(); continue;
    }
    if (digitalRead(BTN_OK) == LOW) {
      if (okHoldStart == 0) okHoldStart = millis();
      if (millis() - okHoldStart > 700) {
        lower = !lower; okHoldStart = 0; last = millis(); continue;
      }
      if (selY == 4) {
        if (selX == 0) { if (cur.length() > 0) cur.remove(cur.length() - 1); }
        else { outPassword = cur; return true; }
      } else {
        char ch = lower ? kb_low[selY][selX] : kb_up[selY][selX];
        cur += ch;
      }
      delay(150);
      okHoldStart = 0;
      last = millis();
      continue;
    } else okHoldStart = 0;
  }
}

bool wifiConnectAndSave(const String &ssid, const String &pass, uint8_t &barsOut) {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(6, 56); tft.setTextColor(ST77XX_WHITE);
  tft.print("Connecting to: ");
  tft.print(ssid);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) return false;
  int rssi = WiFi.RSSI();
  barsOut = (uint8_t)constrain(map(rssi, -90, -30, 1, 5), 1, 5);
  prefs.putString("ssid", ssid);
  prefs.putString("wpass", pass);
  return true;
}

void initI2S() {
  const i2s_port_t i2s_num = I2S_NUM_0;
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = 6,
    .dma_buf_len = 60,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = -1
  };
  i2s_set_pin(i2s_num, &pin_config);
}

void drawPlayerScreen(bool forceFetch) {
  bool covered = false;
  String connStatus = "No network";
  if (chosenMode == MODE_SPOTIFY) {
    if (chosenNet == NET_WIFI && WiFi.status() == WL_CONNECTED) {
      connStatus = "WiFi";
      if (forceFetch) {
        if (!fetchCoverWiFiToTFT()) drawFallbackThumb();
        else covered = true;
      } else {
        String t, a; uint8_t bars;
        if (fetchNowJSONWiFi(t, a, bars)) {
          if (t.length()) currentTitle = t;
          if (a.length()) currentArtist = a;
          currentBars = bars;
        }
        drawFallbackThumb();
      }
    } else if (chosenNet == NET_SIM && simReady) {
      connStatus = "SIM";
      String j;
      if (simHttpGetText(prefs.getString("bridge", DEFAULT_BRIDGE) + "/now.json", j)) {
        if (j.length()) currentTitle = j;
      }
      if (!simHttpGetRawToTFT(prefs.getString("bridge", DEFAULT_BRIDGE) + "/cover.rgb565")) drawFallbackThumb();
      else covered = true;
    } else {
      drawFallbackThumb();
    }
  } else {
    connStatus = "Bluetooth";
    drawFallbackThumb();
  }

  tft.fillRect(0, 96, 128, 32, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(2, 98);
  String t = currentTitle.length() ? currentTitle : "(Unknown)";
  String a = currentArtist.length() ? currentArtist : "(Unknown)";
  if (t.length() > 20) t = t.substring(0, 20);
  if (a.length() > 18) a = a.substring(0, 18);
  tft.print("Title: ");
  tft.print(t);
  tft.setCursor(2, 110);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("Artist: ");
  tft.print(a);
  tft.setCursor(2, 122);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(connStatus);
  drawWifiBars(128 - 36, 126, currentBars);

  if (remoteMuted) tft.fillRect(100, 2, 24, 24, ST77XX_WHITE);
  else tft.fillRect(100, 2, 24, 24, ST77XX_BLACK);
}

bool checkNetwork() {
  if (chosenMode != MODE_SPOTIFY) return true;
  if (chosenNet == NET_WIFI) {
    if (WiFi.status() != WL_CONNECTED) {
      String ssid = prefs.getString("ssid", "");
      String pass = prefs.getString("wpass", "");
      if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
      }
      return WiFi.status() == WL_CONNECTED;
    }
    return true;
  } else if (chosenNet == NET_SIM) {
    if (!simReady || !modem.isGprsConnected()) {
      simReady = simInit("internet");
      return simReady;
    }
    return true;
  }
  return false;
}

void setupPins() {
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  setupPins();

  tft.initR(INITR_144GREENTAB);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 56);
  tft.print("Initializing...");

  prefs.begin("app", false);

  initI2S();
  tft.setCursor(6, 68);
  tft.print("I2S ready");

  a2dp.start("ESP32-MusicPlayer");
  #if USE_A2DP_METADATA
  a2dp.set_avrc_metadata_callback(avrc_metadata_cb);
  #endif
  #if USE_A2DP_VOLUME
  a2dp.set_avrc_volume_callback(avrc_volume_cb);
  #endif
  tft.setCursor(6, 80);
  tft.print("Bluetooth ready");

  SerialAT.begin(SIM_BAUD, SERIAL_8N1, SIM_RX, SIM_TX);
  if (!modem.init()) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(6, 56);
    tft.setTextColor(ST77XX_RED);
    tft.print("SIM init failed");
    delay(2000);
  }

  drawPickMode();
  lastDeb = millis();
}

void loop() {
  switch (appState) {
    case ST_PICK_MODE:
      if (millis() - lastDeb > 150) {
        if (digitalRead(BTN_LEFT) == LOW || digitalRead(BTN_RIGHT) == LOW) {
          chosenMode = (chosenMode == MODE_BT_ONLY) ? MODE_SPOTIFY : MODE_BT_ONLY;
          drawPickMode();
          lastDeb = millis();
        } else if (digitalRead(BTN_OK) == LOW) {
          if (chosenMode == MODE_BT_ONLY) {
            appState = ST_WAIT_BT;
            tft.fillScreen(ST77XX_BLACK);
            tft.setCursor(10, 54); tft.setTextColor(ST77XX_WHITE);
            tft.print("Waiting for Bluetooth...");
            drawPlayerScreen(false);
          } else {
            appState = ST_PICK_NET;
            drawPickNet();
          }
          lastDeb = millis();
        }
      }
      break;

    case ST_PICK_NET:
      if (millis() - lastDeb > 150) {
        if (digitalRead(BTN_LEFT) == LOW || digitalRead(BTN_RIGHT) == LOW) {
          chosenNet = (chosenNet == NET_WIFI) ? NET_SIM : NET_WIFI;
          drawPickNet();
          lastDeb = millis();
        } else if (digitalRead(BTN_OK) == LOW) {
          if (chosenNet == NET_WIFI) {
            appState = ST_WIFI_SCAN;
            doWifiScan();
            drawWifiList();
          } else {
            tft.fillScreen(ST77XX_BLACK);
            tft.setCursor(8, 56); tft.setTextColor(ST77XX_WHITE);
            tft.print("Starting SIM...");
            if (simInit("internet")) {
              appState = ST_WAIT_BT;
              tft.fillScreen(ST77XX_BLACK);
              tft.setCursor(8, 56); tft.setTextColor(ST77XX_WHITE);
              tft.print("SIM ready. Waiting for BT...");
              drawPlayerScreen(true);
            } else {
              tft.fillScreen(ST77XX_BLACK);
              tft.setCursor(8, 56); tft.setTextColor(ST77XX_RED);
              tft.print("SIM failed");
              delay(1200);
              appState = ST_PICK_NET;
              drawPickNet();
            }
          }
          lastDeb = millis();
        }
      }
      break;

    case ST_WIFI_SCAN:
      if (millis() - lastDeb > 150) {
        if (digitalRead(BTN_RIGHT) == LOW) {
          ap_sel = (ap_sel + 1) % max(1, ap_count);
          drawWifiList(); lastDeb = millis();
        } else if (digitalRead(BTN_LEFT) == LOW) {
          ap_sel = (ap_sel - 1 < 0) ? max(1, ap_count) - 1 : ap_sel - 1;
          drawWifiList(); lastDeb = millis();
        } else if (digitalRead(BTN_OK) == LOW) {
          if (ap_ssid[ap_sel] == "(no networks)") {
            doWifiScan();
            drawWifiList();
          } else {
            String chosen = ap_ssid[ap_sel];
            String pass;
            bool ok = keyboardInput(pass, chosen);
            if (ok) {
              uint8_t bars = 3;
              if (wifiConnectAndSave(chosen, pass, bars)) {
                currentBars = bars;
                appState = ST_WAIT_BT;
                tft.fillScreen(ST77XX_BLACK);
                tft.setCursor(8, 56); tft.setTextColor(ST77XX_WHITE);
                tft.print("WiFi ready. Waiting for BT...");
                drawPlayerScreen(true);
              } else {
                tft.fillScreen(ST77XX_BLACK);
                tft.setCursor(8, 56); tft.setTextColor(ST77XX_RED);
                tft.print("WiFi failed");
                delay(1200);
                doWifiScan(); drawWifiList();
              }
            } else {
              drawWifiList();
            }
          }
          lastDeb = millis();
        }
      }
      break;

    case ST_WAIT_BT:
      {
        static unsigned long last = 0;
        if (millis() - last > 1000) {
          checkNetwork();
          drawPlayerScreen(false);
          last = millis();
        }
        if (digitalRead(BTN_OK) == LOW && millis() - lastDeb > 150) {
          appState = ST_PLAYER;
          drawPlayerScreen(false);
          lastDeb = millis();
        }
      }
      break;

    case ST_PLAYER:
      if (millis() - lastDeb > 150) {
        if (digitalRead(BTN_OK) == LOW) { a2dp.play(); drawPlayerScreen(false); lastDeb = millis(); }
        else if (digitalRead(BTN_LEFT) == LOW) { a2dp.previous(); drawPlayerScreen(false); lastDeb = millis(); }
        else if (digitalRead(BTN_RIGHT) == LOW) { a2dp.next(); drawPlayerScreen(false); lastDeb = millis(); }
      }
      static unsigned long lastUpd = 0;
      if (millis() - lastUpd > 3000) {
        if (checkNetwork() && chosenMode == MODE_SPOTIFY) {
          if (chosenNet == NET_WIFI && WiFi.status() == WL_CONNECTED) {
            String t, a; uint8_t bars;
            if (fetchNowJSONWiFi(t, a, bars)) {
              if (t.length()) currentTitle = t;
              if (a.length()) currentArtist = a;
              currentBars = bars;
            }
          } else if (chosenNet == NET_SIM && simReady) {
            String j;
            if (simHttpGetText(prefs.getString("bridge", DEFAULT_BRIDGE) + "/now.json", j)) {
              if (j.length()) currentTitle = j;
            }
          }
        }
        drawPlayerScreen(false);
        lastUpd = millis();
      }
      break;
  }
  delay(5);
}