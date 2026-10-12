#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>

// =====================================================
// KONFIGURASI
// =====================================================

#define RF_PIN D5
#define RF_BIT_US 300
#define RF_GAP_US 10836
#define RF_FRAME_COUNT 5          // jumlah entri frame per kode (array di bawah)

// Perbaikan kestabilan RF:
#define RF_SEND_COUNT 8           // total frame dikirim per perintah (asli: 5)
#define RF_MIN_INTERVAL_MS 150    // jeda minimum antar-pengiriman RF
#define RF_LOCK_INTERRUPTS 1      // 1 = matikan interrupt saat 1 frame dikirim

#define ROOF_COUNT 2

#define EEPROM_SIZE 1024
#define CONFIG_MAGIC 0xA71B

const char *AP_SSID = "ATAP-SETUP";
const char *AP_PASSWORD = "";

// Durasi gerak penuh (ms) per atap: {Atap 1, Atap 2}
const unsigned long OPEN_FULL_MS[ROOF_COUNT]  = {5000, 5000};
const unsigned long CLOSE_FULL_MS[ROOF_COUNT] = {5000, 5000};

const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;

// Access point otomatis:
//  - menyala jika Wi-Fi belum diatur, gagal saat boot, atau putus
//    lebih dari AP_FALLBACK_MS
//  - mati lagi jika Wi-Fi sudah stabil AP_STOP_DELAY_MS dan tidak ada
//    perangkat yang sedang tersambung ke AP
const unsigned long AP_FALLBACK_MS = 20000;
const unsigned long AP_STOP_DELAY_MS = 30000;

static const char *const ROOF_NAME[ROOF_COUNT] = {"Atap 1", "Atap 2"};
// Struktur topic atap: <base>/Data/<ROOF_ID>/stat dan .../cmnd
// (topic MQTT peka huruf besar/kecil)
static const char *const DATA_SEGMENT = "Data";
static const char *const ROOF_ID[ROOF_COUNT] = {"atap1", "atap2"};
static const char *const DEFAULT_BASE_TOPIC = "ATAP";

// =====================================================
// KODE RF
// =====================================================

// ---------- ATAP 1 ----------

static const char RF1_CODE_A_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000111101000010000100001000";

static const char RF1_CODE_B_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000100001000011110100";

static const char RF1_CODE_C_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000111101000010000100";

static const char RF1_CODE_D_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000100001000011110100001000";

static const char *RF1_CODE_A[RF_FRAME_COUNT] = {
  RF1_CODE_A_BITS, RF1_CODE_A_BITS, RF1_CODE_A_BITS,
  RF1_CODE_A_BITS, RF1_CODE_A_BITS
};

static const char *RF1_CODE_B[RF_FRAME_COUNT] = {
  RF1_CODE_B_BITS, RF1_CODE_B_BITS, RF1_CODE_B_BITS,
  RF1_CODE_B_BITS, RF1_CODE_B_BITS
};

static const char *RF1_CODE_C[RF_FRAME_COUNT] = {
  RF1_CODE_C_BITS, RF1_CODE_C_BITS, RF1_CODE_C_BITS,
  RF1_CODE_C_BITS, RF1_CODE_C_BITS
};

static const char *RF1_CODE_D[RF_FRAME_COUNT] = {
  RF1_CODE_D_BITS, RF1_CODE_D_BITS, RF1_CODE_D_BITS,
  RF1_CODE_D_BITS, RF1_CODE_D_BITS
};

// ---------- ATAP 2 ----------

// Segitiga ATAS (OPEN)
static const char RF2_CODE_A_BITS[] =
"10000111100111100100001000011110011110011110010000111100100001000010000100001000011110011110010000111100111100111100100001000010000100001111001000010000100001111001000010000100";

// Segitiga BAWAH (CLOSE)
static const char RF2_CODE_B_BITS[] =
"1000011110011110010000100001111001111001111001000011110010000100001000010000100001111001111001000011110011110010000111100100001000010000111100100001000010000111100100001000010000";

// Kunci LOCK (NOTHING)
static const char RF2_CODE_C_BITS[] =
"10000111100111100100001000011110011110011110010000111100100001000010000100001000011110011110010000111100111100100001000010000111100100001111001000010000100001111001000010000100";

// Kotak STOP (PAUSE)
static const char RF2_CODE_D_BITS[] =
"100001111001111001000010000111100111100111100100001111001000010000100001000010000111100111100100001111001111001000010000111100100001000011110010000100001000011110010000100001000";

static const char *RF2_CODE_A[RF_FRAME_COUNT] = {
  RF2_CODE_A_BITS, RF2_CODE_A_BITS, RF2_CODE_A_BITS,
  RF2_CODE_A_BITS, RF2_CODE_A_BITS
};

static const char *RF2_CODE_B[RF_FRAME_COUNT] = {
  RF2_CODE_B_BITS, RF2_CODE_B_BITS, RF2_CODE_B_BITS,
  RF2_CODE_B_BITS, RF2_CODE_B_BITS
};

static const char *RF2_CODE_C[RF_FRAME_COUNT] = {
  RF2_CODE_C_BITS, RF2_CODE_C_BITS, RF2_CODE_C_BITS,
  RF2_CODE_C_BITS, RF2_CODE_C_BITS
};

static const char *RF2_CODE_D[RF_FRAME_COUNT] = {
  RF2_CODE_D_BITS, RF2_CODE_D_BITS, RF2_CODE_D_BITS,
  RF2_CODE_D_BITS, RF2_CODE_D_BITS
};

// ---------- Tabel kode RF per atap ----------

struct RfSet {
  const char *const *open;     // A
  const char *const *close;    // B
  const char *const *nothing;  // C
  const char *const *pause;    // D
};

static const RfSet RF_SET[ROOF_COUNT] = {
  { RF1_CODE_A, RF1_CODE_B, RF1_CODE_C, RF1_CODE_D },
  { RF2_CODE_A, RF2_CODE_B, RF2_CODE_C, RF2_CODE_D }
};

// =====================================================
// KONFIGURASI TERSIMPAN
// =====================================================

// Urutan field jangan diubah supaya konfigurasi lama di EEPROM
// (Wi-Fi, MQTT, base topic) tetap terbaca.
struct Settings {
  uint16_t magic;
  char wifiSSID[33];
  char wifiPassword[65];
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPassword[65];
  char baseTopic[64];
};

Settings cfg;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// State tiap atap
struct RoofState {
  bool moving = false;
  bool positionKnown = false;

  int position = 0;
  int startPosition = 0;
  int target = 0;

  unsigned long startTime = 0;
  unsigned long duration = 0;

  String lastCommand = "Belum ada perintah";
  String direction = "Diam";
};

RoofState roof[ROOF_COUNT];

unsigned long lastMQTTAttempt = 0;
unsigned long lastWiFiAttempt = 0;

bool apActive = false;
bool wifiLastState = false;
unsigned long wifiStateSince = 0;

unsigned long lastRfEnd = 0;
bool rfEverSent = false;

// =====================================================
// UTILITAS DAN EEPROM
// =====================================================

void copyText(char *dst, size_t size, const String &src) {
  src.toCharArray(dst, size);
  dst[size - 1] = '\0';
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

// Base topic: tidak kosong, muat di buffer, tanpa spasi, /, + atau #.
bool validTopic(const String &t) {
  return t.length() > 0 &&
         t.length() < sizeof(cfg.baseTopic) &&
         t.indexOf(' ') < 0 &&
         t.indexOf('+') < 0 &&
         t.indexOf('#') < 0 &&
         t.indexOf('/') < 0;
}

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));

  cfg.magic = CONFIG_MAGIC;
  cfg.mqttPort = 1883;

  copyText(cfg.mqttHost, sizeof(cfg.mqttHost), "192.168.1.100");
  copyText(cfg.baseTopic, sizeof(cfg.baseTopic), DEFAULT_BASE_TOPIC);
}

void saveSettings() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  if (cfg.magic != CONFIG_MAGIC) {
    setDefaults();
    return;
  }

  // Migrasi: firmware lama memakai base topic per atap (ATAP1 / ATAP2).
  // Sekarang base topic satu untuk semua atap.
  if (strcmp(cfg.baseTopic, "ATAP1") == 0 ||
      strcmp(cfg.baseTopic, "ATAP2") == 0) {
    copyText(cfg.baseTopic, sizeof(cfg.baseTopic), DEFAULT_BASE_TOPIC);
    saveSettings();
  }
}

// =====================================================
// TOPIC MQTT
// =====================================================

String topicLWT() {
  return String(cfg.baseTopic) + "/LWT";
}

String topicIP() {
  return String(cfg.baseTopic) + "/IP";
}

// <base>/Data/atap1
String topicRoof(int r) {
  return String(cfg.baseTopic) + "/" + DATA_SEGMENT + "/" + ROOF_ID[r];
}

// <base>/Data/atap1/stat
String topicData(int r) {
  return topicRoof(r) + "/stat";
}

// <base>/Data/atap1/cmnd
String topicCommand(int r) {
  return topicRoof(r) + "/cmnd";
}

// =====================================================
// RF TRANSMITTER
// =====================================================

// Kirim satu frame. Waktu tiap bit memakai target absolut dari cycle
// counter CPU (bukan delayMicroseconds berurutan), jadi:
//  - tidak ada drift kumulatif dari overhead digitalWrite,
//  - bit yang tertunda interrupt Wi-Fi langsung dikejar oleh bit berikutnya.
static void sendRFFrame(const char *bits) {
  const uint32_t mask = 1UL << RF_PIN;
  const uint32_t bitCycles = (uint32_t)ESP.getCpuFreqMHz() * RF_BIT_US;

#if RF_LOCK_INTERRUPTS
  noInterrupts();
#endif

  uint32_t next = ESP.getCycleCount();

  for (size_t i = 0; bits[i] != '\0'; i++) {
    if (bits[i] == '1') {
      GPOS = mask;
    } else {
      GPOC = mask;
    }

    next += bitCycles;
    while ((int32_t)(ESP.getCycleCount() - next) < 0) {
    }
  }

  GPOC = mask;

#if RF_LOCK_INTERRUPTS
  interrupts();
#endif
}

void sendRF(const char *const frames[]) {
  // Beri jeda dari pengiriman sebelumnya (mis. PAUSE lalu langsung OPEN)
  // supaya receiver tidak menganggapnya satu sinyal yang sama.
  if (rfEverSent) {
    while (millis() - lastRfEnd < RF_MIN_INTERVAL_MS) {
      delay(1);
    }
  }

  unsigned long t0 = millis();

  for (int n = 0; n < RF_SEND_COUNT; n++) {
    sendRFFrame(frames[n % RF_FRAME_COUNT]);

    if (n < RF_SEND_COUNT - 1) {
      delayMicroseconds(RF_GAP_US);
    }
  }

  digitalWrite(RF_PIN, LOW);

  lastRfEnd = millis();
  rfEverSent = true;

  Serial.printf("[%lu ms] RF selesai dikirim (%lu ms)\n",
                millis(), millis() - t0);
}

void sendOpenRF(int r) {
  Serial.printf("%s: RF A - OPEN\n", ROOF_NAME[r]);
  sendRF(RF_SET[r].open);
}

void sendCloseRF(int r) {
  Serial.printf("%s: RF B - CLOSE\n", ROOF_NAME[r]);
  sendRF(RF_SET[r].close);
}

void sendNothingRF(int r) {
  Serial.printf("%s: RF C - NOTHING\n", ROOF_NAME[r]);
  sendRF(RF_SET[r].nothing);
}

void sendPauseRF(int r) {
  Serial.printf("%s: RF D - PAUSE\n", ROOF_NAME[r]);
  sendRF(RF_SET[r].pause);
}

// =====================================================
// STATUS MQTT
// =====================================================

void publishStatus(int r) {
  if (!mqtt.connected()) return;

  RoofState &s = roof[r];

  String payload = "{";
  payload += "\"position\":" + String(s.position);
  payload += ",\"position_known\":";
  payload += s.positionKnown ? "true" : "false";
  payload += ",\"moving\":";
  payload += s.moving ? "true" : "false";
  payload += ",\"direction\":\"" + s.direction + "\"";
  payload += ",\"command\":\"" + s.lastCommand + "\"";
  payload += "}";

  String topic = topicData(r);
  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

void publishIP() {
  if (!mqtt.connected()) return;

  String topic = topicIP();
  String ip = WiFi.localIP().toString();

  mqtt.publish(topic.c_str(), ip.c_str(), true);
}

// =====================================================
// PERGERAKAN ATAP BERDASARKAN WAKTU
// =====================================================

void pauseRoof(int r) {
  RoofState &s = roof[r];

  if (s.moving && s.positionKnown && s.duration > 0) {
    unsigned long elapsed = millis() - s.startTime;

    float ratio = min(1.0f,
      (float)elapsed / (float)s.duration);

    int delta = s.target - s.startPosition;

    s.position = constrain(
      s.startPosition + (int)(delta * ratio),
      0, 100
    );
  }

  // Kirim kode pause meskipun posisi belum diketahui.
  sendPauseRF(r);

  s.moving = false;
  s.direction = "Diam";
  s.lastCommand = "pause";

  publishStatus(r);
}

void moveToPosition(int r, int target) {
  RoofState &s = roof[r];

  target = constrain(target, 0, 100);

  if (s.moving) {
    pauseRoof(r);
  }

  // Posisi parsial memerlukan referensi posisi yang diketahui.
  if (!s.positionKnown && target != 0 && target != 100) {
    s.lastCommand =
      "Posisi belum diketahui. Jalankan OPEN/CLOSE penuh dahulu.";
    publishStatus(r);
    return;
  }

  if (s.positionKnown && target == s.position) {
    s.lastCommand = "Posisi sudah sesuai";
    publishStatus(r);
    return;
  }

  s.startPosition = s.position;
  s.target = target;
  s.startTime = millis();

  if (target == 100) {
    s.direction = "Membuka";

    s.duration = s.positionKnown
      ? (unsigned long)(OPEN_FULL_MS[r] *
          (100.0f - s.position) / 100.0f)
      : OPEN_FULL_MS[r];

    sendOpenRF(r);
    s.lastCommand = "open";
  } else if (target == 0) {
    s.direction = "Menutup";

    s.duration = s.positionKnown
      ? (unsigned long)(CLOSE_FULL_MS[r] *
          s.position / 100.0f)
      : CLOSE_FULL_MS[r];

    sendCloseRF(r);
    s.lastCommand = "close";
  } else if (target > s.position) {
    s.direction = "Membuka";

    s.duration = (unsigned long)(
      OPEN_FULL_MS[r] * (target - s.position) / 100.0f
    );

    sendOpenRF(r);
    s.lastCommand = String(target);
  } else {
    s.direction = "Menutup";

    s.duration = (unsigned long)(
      CLOSE_FULL_MS[r] * (s.position - target) / 100.0f
    );

    sendCloseRF(r);
    s.lastCommand = String(target);
  }

  if (s.duration == 0) {
    sendPauseRF(r);
    s.position = target;
    s.positionKnown = true;
    s.moving = false;
    s.direction = "Diam";
  } else {
    s.moving = true;
  }

  publishStatus(r);
}

void updatePosition() {
  for (int r = 0; r < ROOF_COUNT; r++) {
    RoofState &s = roof[r];

    if (!s.moving) continue;

    unsigned long elapsed = millis() - s.startTime;

    if (elapsed >= s.duration) {
      sendPauseRF(r);

      s.position = s.target;
      s.positionKnown = true;
      s.moving = false;
      s.direction = "Diam";

      Serial.printf("%s posisi estimasi: %d%%\n",
                    ROOF_NAME[r], s.position);

      publishStatus(r);
      continue;
    }

    if (s.positionKnown && s.duration > 0) {
      float ratio = (float)elapsed / (float)s.duration;
      int delta = s.target - s.startPosition;

      s.position = constrain(
        s.startPosition + (int)(delta * ratio),
        0, 100
      );
    }
  }
}

// =====================================================
// PEMROSESAN PERINTAH
// =====================================================

void handleCommand(int r, String command) {
  command.trim();
  command.toLowerCase();

  Serial.printf("%s perintah: %s\n", ROOF_NAME[r], command.c_str());

  if (command == "on" || command == "1" ||
      command == "open") {
    moveToPosition(r, 100);
  } else if (command == "off" || command == "0" ||
             command == "close") {
    moveToPosition(r, 0);
  } else if (command == "pause" || command == "stop") {
    pauseRoof(r);
  } else if (command == "25") {
    moveToPosition(r, 25);
  } else if (command == "50") {
    moveToPosition(r, 50);
  } else if (command == "75") {
    moveToPosition(r, 75);
  } else if (command == "nothing" || command == "c") {
    sendNothingRF(r);
    roof[r].lastCommand = "nothing";
    publishStatus(r);
  } else {
    Serial.println(F("Perintah tidak dikenal"));
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  Serial.printf("[%lu ms] MQTT cmnd diterima\n", millis());

  String t(topic);
  int r = -1;

  for (int i = 0; i < ROOF_COUNT; i++) {
    if (t == topicCommand(i)) {
      r = i;
      break;
    }
  }

  if (r < 0) return;

  String command;
  command.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

  handleCommand(r, command);
}

// =====================================================
// WI-FI
// =====================================================

// Matikan modem sleep agar paket MQTT/HTTP langsung diproses,
// tanpa menunggu beacon berikutnya dari router.
void lowLatencyWiFi() {
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
}

void startAccessPoint() {
  if (apActive) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  lowLatencyWiFi();

  apActive = true;

  Serial.print(F("AP setup AKTIF: "));
  Serial.println(AP_SSID);
  Serial.print(F("Buka: http://"));
  Serial.println(WiFi.softAPIP());
}

void stopAccessPoint() {
  if (!apActive) return;

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  lowLatencyWiFi();

  apActive = false;

  Serial.println(F("AP setup dimatikan (Wi-Fi router stabil)"));
}

// Ada perangkat yang sedang tersambung ke AP (sedang konfigurasi).
bool apHasClients() {
  return apActive && WiFi.softAPgetStationNum() > 0;
}

void connectWiFi() {
  if (strlen(cfg.wifiSSID) == 0) return;

  WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);
  lowLatencyWiFi();

  Serial.print(F("Menghubungkan Wi-Fi"));

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 15000) {
    delay(250);
    server.handleClient();
    Serial.print('.');
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP Wemos: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("Wi-Fi belum terhubung"));
  }
}

// Nyalakan / matikan AP otomatis sesuai kondisi Wi-Fi.
void manageAccessPoint() {
  unsigned long now = millis();

  // Wi-Fi belum diatur: AP harus selalu tersedia.
  if (strlen(cfg.wifiSSID) == 0) {
    startAccessPoint();
    return;
  }

  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected != wifiLastState) {
    wifiLastState = connected;
    wifiStateSince = now;
  }

  if (!connected) {
    if (!apActive && now - wifiStateSince >= AP_FALLBACK_MS) {
      Serial.println(F("Wi-Fi tidak tersedia, menyalakan AP setup"));
      startAccessPoint();
    }
  } else if (apActive &&
             now - wifiStateSince >= AP_STOP_DELAY_MS &&
             !apHasClients()) {
    stopAccessPoint();
  }
}

// =====================================================
// MQTT RECONNECT
// =====================================================

bool connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqtt.connected()) return true;

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);

  String clientID =
    "AtapWemos-" + String(ESP.getChipId(), HEX);

  // Satu perangkat = satu LWT untuk kedua atap.
  String lwt = topicLWT();

  bool connected;

  if (strlen(cfg.mqttUser) > 0) {
    connected = mqtt.connect(
      clientID.c_str(),
      cfg.mqttUser,
      cfg.mqttPassword,
      lwt.c_str(),
      1,
      true,
      "Offline"
    );
  } else {
    connected = mqtt.connect(
      clientID.c_str(),
      lwt.c_str(),
      1,
      true,
      "Offline"
    );
  }

  if (!connected) {
    Serial.print(F("MQTT gagal, state="));
    Serial.println(mqtt.state());
    return false;
  }

  // Kirim paket kecil (PUBACK, ping, status) langsung tanpa ditunda.
  wifiClient.setNoDelay(true);

  for (int r = 0; r < ROOF_COUNT; r++) {
    String cmd = topicCommand(r);
    mqtt.subscribe(cmd.c_str(), 1);
  }

  mqtt.publish(lwt.c_str(), "Online", true);
  publishIP();

  for (int r = 0; r < ROOF_COUNT; r++) {
    publishStatus(r);
  }

  Serial.println(F("MQTT tersambung"));
  Serial.println("LWT: " + lwt);
  Serial.println("IP: " + topicIP());

  for (int r = 0; r < ROOF_COUNT; r++) {
    Serial.printf("%s -> DATA: %s | CMND: %s\n",
                  ROOF_NAME[r],
                  topicData(r).c_str(),
                  topicCommand(r).c_str());
  }

  return true;
}

// =====================================================
// WEB DASHBOARD
// =====================================================

String pageHeader(String title) {
  String html = R"rawliteral(
<!DOCTYPE html><html lang="id"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>)rawliteral";

  html += title;

  html += R"rawliteral(</title>
<style>
:root{
 --bg:#eef1f5;--panel:#fff;--ink:#17202b;--muted:#5d6b7c;
 --line:#d5dce5;--track:#e1e6ed;--accent:#0a5fd0;--accent-ink:#fff;
 --danger:#c8283d;--ok:#12794a;--ok-bg:#dff3e8;
 --bad:#a61d31;--bad-bg:#fbe3e6;--warn:#8a5300;--warn-bg:#fdecc8;
 --r:12px
}
@media(prefers-color-scheme:dark){:root{
 --bg:#0e141b;--panel:#18212c;--ink:#e8edf3;--muted:#9aa8b8;
 --line:#2b3745;--track:#2b3745;--accent:#4b93f5;--accent-ink:#06121f;
 --danger:#ef5a6e;--ok:#5fd39b;--ok-bg:#12372a;
 --bad:#ff8a99;--bad-bg:#43161f;--warn:#f3be5b;--warn-bg:#40300f
}}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
background:var(--bg);color:var(--ink);margin:0;padding:16px;
line-height:1.45;-webkit-text-size-adjust:100%}
main{max-width:520px;margin:0 auto;padding-bottom:40px}
header{display:flex;align-items:center;justify-content:space-between;
gap:12px;margin:4px 0 14px}
h1{font-size:22px;margin:0;font-weight:700}
h2{font-size:16px;margin:0 0 12px;font-weight:700}
section{background:var(--panel);border:1px solid var(--line);
border-radius:var(--r);padding:16px;margin:0 0 12px}
p{margin:0 0 10px}
a{color:var(--accent)}
.muted{color:var(--muted);font-size:13px}
.badge{display:inline-block;padding:3px 10px;border-radius:99px;
font-size:13px;font-weight:600;white-space:nowrap}
.badge.ok{background:var(--ok-bg);color:var(--ok)}
.badge.bad{background:var(--bad-bg);color:var(--bad)}
.badge.warn{background:var(--warn-bg);color:var(--warn)}

.pos{display:flex;align-items:baseline;justify-content:space-between;
gap:10px;margin-bottom:12px}
.pct{font-size:56px;font-weight:700;line-height:1;
font-variant-numeric:tabular-nums;letter-spacing:-1px}
.pct .u{font-size:24px;font-weight:600;margin-left:2px;color:var(--muted);
letter-spacing:0}
.mv{font-size:15px;font-weight:600;color:var(--muted);text-align:right}
.mv.on{color:var(--accent)}
.scale{position:relative;margin:6px 0 4px}
.track{height:18px;border-radius:9px;background:var(--track);overflow:hidden}
.bar{height:100%;width:0;border-radius:9px;background:var(--accent);
transition:width .8s linear}
.bar.moving{background-image:repeating-linear-gradient(
 135deg,rgba(255,255,255,.28) 0 10px,transparent 10px 20px);
 background-size:28px 28px;animation:slide 1s linear infinite}
.bar.unknown{background:repeating-linear-gradient(
 135deg,var(--track) 0 10px,var(--line) 10px 20px);opacity:.9}
.bar.unknown.moving{background-color:var(--accent)}
@keyframes slide{to{background-position:28px 0}}
.ticks{display:flex;justify-content:space-between;font-size:12px;
color:var(--muted);margin-top:4px;font-variant-numeric:tabular-nums}
.hint{background:var(--warn-bg);color:var(--warn);border-radius:8px;
padding:10px 12px;font-size:14px;margin:12px 0 0}
.hint[hidden]{display:none}

dl{margin:0;display:grid;grid-template-columns:auto 1fr;gap:9px 14px;
align-items:center}
dt{color:var(--muted);font-size:14px}
dd{margin:0;text-align:right;font-size:14px;word-break:break-word}

.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:10px}
button,.btn{font:inherit;font-weight:600;border:1px solid transparent;
border-radius:10px;min-height:52px;padding:10px 12px;cursor:pointer;
background:var(--accent);color:var(--accent-ink);
transition:transform .08s,opacity .15s}
button:active,.btn:active{transform:scale(.97)}
button:focus-visible,a:focus-visible,input:focus-visible{
outline:3px solid var(--accent);outline-offset:2px}
button.alt{background:transparent;color:var(--accent);
border-color:var(--accent)}
button.stop{background:var(--danger);color:#fff;width:100%;
margin-top:10px;min-height:56px;font-size:17px}
button:disabled{opacity:.4;cursor:not-allowed;transform:none}
.btn{display:block;text-align:center;text-decoration:none;
line-height:30px;background:transparent;color:var(--accent);
border-color:var(--accent)}

.link{display:flex;align-items:center;justify-content:space-between;
gap:10px;text-decoration:none;color:var(--ink);padding:12px 0;
border-top:1px solid var(--line)}
.link:first-of-type{border-top:0;padding-top:0}
.link:last-child{padding-bottom:0}
.link span{font-weight:600}
.link small{display:block;color:var(--muted);font-weight:400;font-size:13px}

label{display:block;font-size:14px;font-weight:600;margin:0 0 5px}
input{font:inherit;width:100%;min-height:46px;padding:10px 12px;
margin:0 0 14px;background:var(--bg);color:var(--ink);
border:1px solid var(--line);border-radius:9px}
.field{position:relative}
.field input{padding-right:84px}
.field .show{position:absolute;right:5px;top:5px;min-height:36px;
padding:4px 12px;font-size:14px;background:transparent;
color:var(--accent);border:0}
.help{color:var(--muted);font-size:13px;margin:-8px 0 14px}
form button[type=submit]{width:100%;min-height:54px;font-size:16px}
.done{text-align:center;padding:12px 0}
.done h2{font-size:20px}

#toast{position:fixed;left:50%;bottom:22px;transform:translate(-50%,30px);
max-width:90%;background:var(--ink);color:var(--bg);padding:12px 18px;
border-radius:10px;font-size:14px;opacity:0;pointer-events:none;
transition:opacity .2s,transform .2s;z-index:9}
#toast.show{opacity:1;transform:translate(-50%,0)}
#toast.err{background:var(--danger);color:#fff}
@media(prefers-reduced-motion:reduce){
 .bar,.bar.moving{transition:none;animation:none}
 #toast,button{transition:none}
}
.roof h2{font-size:18px}
.roof .pct{font-size:52px}
</style></head><body><main>
)rawliteral";

  return html;
}

// Kartu satu atap: posisi, progress bar, dan tombol kontrol.
String roofCard(int r) {
  RoofState &s = roof[r];

  String i = String(r);      // untuk id elemen
  String n = String(r + 1);  // untuk parameter ?r=

  String h;
  h.reserve(2200);

  h += "<section class=\"roof\" aria-live=\"polite\"><h2>";
  h += ROOF_NAME[r];
  h += "</h2>";

  h += "<div class=\"pos\"><div class=\"pct\" id=\"pct" + i + "\">";
  h += s.positionKnown ? String(s.position) + "<span class=\"u\">%</span>"
                       : "?";
  h += "</div><div id=\"mv" + i + "\" class=\"mv";
  h += s.moving ? " on" : "";
  h += "\">" + htmlEscape(s.direction) + "</div></div>";

  h += "<div class=\"scale\"><div class=\"track\">";
  h += "<div id=\"bar" + i + "\" class=\"bar";
  h += s.moving ? " moving" : "";
  h += s.positionKnown ? "" : " unknown";
  h += "\" style=\"width:";
  h += s.positionKnown ? String(constrain(s.position, 0, 100)) : "100";
  h += "%\"></div></div>";
  h += "<div class=\"ticks\"><span>Tutup</span><span>25</span>"
       "<span>50</span><span>75</span><span>Buka</span></div></div>";

  h += "<p id=\"hint" + i + "\" class=\"hint\"";
  h += s.positionKnown ? " hidden" : "";
  h += ">Posisi belum diketahui. Tekan Buka atau Tutup penuh sekali "
       "agar persentase bisa dipakai.</p>";

  h += "<div class=\"grid2\" style=\"margin-top:14px\">";
  h += "<button onclick=\"cmd('" + n + "','open')\">Buka penuh</button>";
  h += "<button onclick=\"cmd('" + n + "','close')\">Tutup penuh</button>";
  h += "</div><div class=\"grid3\">";

  const char *presets[] = {"25", "50", "75"};
  for (int p = 0; p < 3; p++) {
    h += "<button class=\"alt pre pre" + i + "\" onclick=\"cmd('" + n + "','";
    h += presets[p];
    h += "')\"";
    h += s.positionKnown ? "" : " disabled";
    h += ">";
    h += presets[p];
    h += "%</button>";
  }

  h += "</div><button class=\"stop\" onclick=\"cmd('" + n +
       "','pause')\">Pause " + String(ROOF_NAME[r]) + "</button>";
  h += "</section>";

  return h;
}

void handleRoot() {
  String html = pageHeader("Kontrol Atap");
  html.reserve(html.length() + 9000);

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool mqttOk = mqtt.connected();

  // Header
  html += "<header><h1>Kontrol Atap</h1>"
          "<span id=\"conn\" class=\"badge ok\">Terhubung</span></header>";

  // Kartu tiap atap
  for (int r = 0; r < ROOF_COUNT; r++) {
    html += roofCard(r);
  }

  // Pause semua
  html += R"rawliteral(
<section><h2>Semua atap</h2>
<button class="stop" style="margin-top:0" onclick="cmd('all','pause')">Pause semua atap</button>
<p class="muted" style="margin:12px 0 0">Persentase dihitung dari waktu
gerak, bukan dari sensor posisi.</p>
</section>
)rawliteral";

  // Status
  html += "<section><h2>Status perangkat</h2><dl>";
  html += "<dt>Wi-Fi</dt><dd><span id=\"wifi\" class=\"badge ";
  html += wifiOk ? "ok\">Terhubung" : "bad\">Terputus";
  html += "</span></dd>";
  html += "<dt>MQTT</dt><dd><span id=\"mqtt\" class=\"badge ";
  html += mqttOk ? "ok\">Terhubung" : "bad\">Terputus";
  html += "</span></dd>";
  html += "<dt>Access point</dt><dd><span id=\"ap\" class=\"badge ";
  html += apActive ? "warn\">Aktif" : "ok\">Mati";
  html += "</span></dd>";
  html += "<dt>Alamat IP</dt><dd>" + WiFi.localIP().toString() + "</dd>";

  for (int r = 0; r < ROOF_COUNT; r++) {
    html += "<dt>Topic ";
    html += ROOF_NAME[r];
    html += "</dt><dd>" + htmlEscape(topicRoof(r)) + "</dd>";
  }

  for (int r = 0; r < ROOF_COUNT; r++) {
    html += "<dt>Perintah terakhir ";
    html += ROOF_NAME[r];
    html += "</dt><dd id=\"lc" + String(r) + "\">" +
            htmlEscape(roof[r].lastCommand) + "</dd>";
  }

  html += "</dl></section>";

  html += R"rawliteral(
<section><h2>Pengaturan</h2>
<a class="link" href="/config"><span>Wi-Fi dan MQTT
<small>Jaringan, broker, dan base topic</small></span></a>
<a class="link" href="/update"><span>Update firmware (OTA)
<small>Tanpa password, gunakan hanya di jaringan tepercaya</small></span></a>
</section>
<div id="toast" role="status"></div>
<script>
var $=function(i){return document.getElementById(i)};
var busy=false,fails=0;

function toast(m,err){
 var t=$('toast');t.textContent=m;t.className='show'+(err?' err':'');
 clearTimeout(window._t);
 window._t=setTimeout(function(){t.className=''},3200);
}
function badge(id,ok){
 var e=$(id);e.textContent=ok?'Terhubung':'Terputus';
 e.className='badge '+(ok?'ok':'bad');
}
function render(s){
 for(var i=0;i<s.roofs.length;i++){
  var r=s.roofs[i],k=r.position_known;
  $('pct'+i).innerHTML=k?r.position+'<span class="u">%</span>':'?';
  var b=$('bar'+i);
  b.style.width=(k?r.position:100)+'%';
  b.className='bar'+(r.moving?' moving':'')+(k?'':' unknown');
  var m=$('mv'+i);m.textContent=r.direction;m.className='mv'+(r.moving?' on':'');
  $('hint'+i).hidden=k;
  document.querySelectorAll('.pre'+i).forEach(function(x){x.disabled=!k});
  $('lc'+i).textContent=r.command;
 }
 badge('wifi',s.wifi);badge('mqtt',s.mqtt);
 var a=$('ap');a.textContent=s.ap?'Aktif':'Mati';a.className='badge '+(s.ap?'warn':'ok');
 var c=$('conn');c.textContent='Terhubung';c.className='badge ok';
}
function poll(){
 if(document.hidden)return;
 fetch('/status').then(function(r){return r.json()}).then(function(s){
  fails=0;render(s);
 }).catch(function(){
  if(++fails>=2){
   var c=$('conn');c.textContent='Tidak terjangkau';c.className='badge bad';
  }
 });
}
function cmd(r,c){
 if(busy)return;
 busy=true;
 fetch('/cmd?r='+encodeURIComponent(r)+'&c='+encodeURIComponent(c))
 .then(function(x){return x.text()})
 .then(function(t){toast(t);poll();})
 .catch(function(){toast('Perintah gagal terkirim. Periksa koneksi ke perangkat.',true)})
 .then(function(){busy=false});
}
setInterval(poll,1500);
document.addEventListener('visibilitychange',poll);
</script>
</main></body></html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = pageHeader("Konfigurasi");

  html += R"rawliteral(
<header><h1>Konfigurasi</h1></header>
<form method="POST" action="/save" onsubmit="var b=this.querySelector('button[type=submit]');b.disabled=true;b.textContent='Menyimpan...'">
<section><h2>Wi-Fi</h2>
<label for="ssid">Nama Wi-Fi (SSID) router</label>
<input id="ssid" name="ssid" maxlength="32" autocomplete="off" autocapitalize="off" value=")rawliteral";
  html += htmlEscape(cfg.wifiSSID);

  html += R"rawliteral(">
<label for="wpass">Password Wi-Fi</label>
<div class="field">
<input id="wpass" name="wpass" type="password" maxlength="64"
autocomplete="new-password"
placeholder="Kosongkan untuk mempertahankan password">
<button type="button" class="show" onclick="tg('wpass',this)">Tampilkan</button>
</div>
</section>
<section><h2>MQTT</h2>
<label for="host">Host atau IP broker</label>
<input id="host" name="host" maxlength="63" autocomplete="off" autocapitalize="off" value=")rawliteral";
  html += htmlEscape(cfg.mqttHost);

  html += R"rawliteral(">
<label for="port">Port</label>
<input id="port" name="port" type="number" inputmode="numeric" min="1" max="65535" value=")rawliteral";
  html += String(cfg.mqttPort);

  html += R"rawliteral(">
<label for="user">Username</label>
<input id="user" name="user" maxlength="32" autocomplete="off" autocapitalize="off" value=")rawliteral";
  html += htmlEscape(cfg.mqttUser);

  html += R"rawliteral(">
<label for="mpass">Password MQTT</label>
<div class="field">
<input id="mpass" name="mpass" type="password" maxlength="64"
autocomplete="new-password"
placeholder="Kosongkan untuk mempertahankan password">
<button type="button" class="show" onclick="tg('mpass',this)">Tampilkan</button>
</div>
<label for="base">Base topic</label>
<input id="base" name="base" maxlength="63" autocomplete="off" autocapitalize="off" value=")rawliteral";
  html += htmlEscape(cfg.baseTopic);

  html += R"rawliteral(">
<p class="help">Tanpa spasi, tanda /, + atau #. Topic yang dipakai:
<b>base</b>/LWT, <b>base</b>/IP, <b>base</b>/Data/atap1/stat,
<b>base</b>/Data/atap1/cmnd, <b>base</b>/Data/atap2/stat, dan
<b>base</b>/Data/atap2/cmnd.</p>
</section>
<button type="submit">Simpan konfigurasi</button>
</form>
<p style="margin-top:16px"><a href="/">Kembali ke dashboard</a></p>
<script>
function tg(id,b){
 var i=document.getElementById(id),h=i.type==='password';
 i.type=h?'text':'password';
 b.textContent=h?'Sembunyikan':'Tampilkan';
}
</script>
</main></body></html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid")) {
    copyText(cfg.wifiSSID, sizeof(cfg.wifiSSID), server.arg("ssid"));
  }

  if (server.hasArg("wpass") && server.arg("wpass").length() > 0) {
    copyText(cfg.wifiPassword, sizeof(cfg.wifiPassword), server.arg("wpass"));
  }

  if (server.hasArg("host")) {
    copyText(cfg.mqttHost, sizeof(cfg.mqttHost), server.arg("host"));
  }

  if (server.hasArg("port")) {
    int port = server.arg("port").toInt();
    if (port >= 1 && port <= 65535) cfg.mqttPort = port;
  }

  if (server.hasArg("user")) {
    copyText(cfg.mqttUser, sizeof(cfg.mqttUser), server.arg("user"));
  }

  if (server.hasArg("mpass") && server.arg("mpass").length() > 0) {
    copyText(cfg.mqttPassword, sizeof(cfg.mqttPassword), server.arg("mpass"));
  }

  bool topicRejected = false;

  if (server.hasArg("base")) {
    String base = server.arg("base");
    base.trim();

    if (validTopic(base)) {
      copyText(cfg.baseTopic, sizeof(cfg.baseTopic), base);
    } else {
      topicRejected = true;
    }
  }

  saveSettings();

  mqtt.disconnect();

  String page = pageHeader("Konfigurasi tersimpan");
  page += R"rawliteral(
<header><h1>Kontrol Atap</h1></header>
<section class="done">
<h2>Konfigurasi tersimpan</h2>
)rawliteral";

  if (topicRejected) {
    page += "<p class=\"hint\">Base topic tidak diubah: tidak boleh "
            "kosong dan tanpa spasi, /, + atau #.</p>";
  }

  page += R"rawliteral(
<p>Wemos sedang menghubungkan Wi-Fi dan MQTT dengan pengaturan baru.
Jika alamat IP berubah, cek alamat barunya di topic IP atau serial monitor.</p>
<p><a class="btn" href="/">Kembali ke dashboard</a></p>
</section>
</main></body></html>
)rawliteral";

  server.send(200, "text/html", page);

  WiFi.disconnect();
  delay(200);
  connectWiFi();

  lastMQTTAttempt = millis();
}

void handleCommandWeb() {
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "Parameter c diperlukan");
    return;
  }

  // r = 1 / 2 / all. Tanpa r dianggap Atap 1 (kompatibel dengan versi lama).
  String rs = server.hasArg("r") ? server.arg("r") : String("1");
  rs.trim();
  rs.toLowerCase();

  String c = server.arg("c");

  if (rs == "all") {
    for (int r = 0; r < ROOF_COUNT; r++) {
      handleCommand(r, c);
    }

    server.send(200, "text/plain", "Semua atap: " + c);
    return;
  }

  int r = rs.toInt() - 1;

  if (r < 0 || r >= ROOF_COUNT) {
    server.send(400, "text/plain", "Parameter r harus 1, 2, atau all");
    return;
  }

  handleCommand(r, c);
  server.send(200, "text/plain",
              String(ROOF_NAME[r]) + ": " + roof[r].lastCommand);
}

void handleStatusWeb() {
  String json = "{";
  json += "\"wifi\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"mqtt\":";
  json += mqtt.connected() ? "true" : "false";
  json += ",\"ap\":";
  json += apActive ? "true" : "false";
  json += ",\"roofs\":[";

  for (int r = 0; r < ROOF_COUNT; r++) {
    RoofState &s = roof[r];

    if (r > 0) json += ",";

    json += "{\"position\":" + String(s.position);
    json += ",\"position_known\":";
    json += s.positionKnown ? "true" : "false";
    json += ",\"moving\":";
    json += s.moving ? "true" : "false";
    json += ",\"direction\":\"" + s.direction + "\"";
    json += ",\"command\":\"" + s.lastCommand + "\"}";
  }

  json += "]}";

  server.send(200, "application/json", json);
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/cmd", HTTP_GET, handleCommandWeb);
  server.on("/status", HTTP_GET, handleStatusWeb);

  // OTA tanpa username dan password.
  httpUpdater.setup(&server, "/update");

  server.begin();
  Serial.println(F("Web server berjalan"));
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RF_PIN, OUTPUT);
  digitalWrite(RF_PIN, LOW);

  loadSettings();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // Mulai sebagai station saja. AP setup hanya menyala jika Wi-Fi belum
  // diatur atau tidak bisa terhubung (lihat manageAccessPoint).
  WiFi.mode(WIFI_STA);
  lowLatencyWiFi();

  setupWeb();

  connectWiFi();

  wifiLastState = WiFi.status() == WL_CONNECTED;
  wifiStateSince = millis();

  if (!wifiLastState) {
    startAccessPoint();
  }

  wifiClient.setTimeout(1500);

  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);
  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);

  connectMQTT();

  Serial.println();
  Serial.println(F("=== ATAP CONTROLLER (2 ATAP) READY ==="));
  if (apActive) {
    Serial.println("Setup AP: http://192.168.4.1");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Dashboard router: http://"));
    Serial.println(WiFi.localIP());
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  mqtt.loop();
  server.handleClient();

  manageAccessPoint();

  // Reconnect Wi-Fi berkala. Selama ada perangkat yang sedang
  // konfigurasi lewat AP, jangan scan/reconnect karena itu memindahkan
  // channel radio dan memutus koneksi mereka.
  if (WiFi.status() != WL_CONNECTED &&
      strlen(cfg.wifiSSID) > 0 &&
      !apHasClients() &&
      millis() - lastWiFiAttempt >= WIFI_RETRY_MS) {

    lastWiFiAttempt = millis();

    Serial.println(F("Wi-Fi putus, mencoba reconnect..."));
    WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);
    lowLatencyWiFi();
  }

  // Reconnect MQTT berkala.
  if (WiFi.status() == WL_CONNECTED &&
      !mqtt.connected() &&
      millis() - lastMQTTAttempt >= MQTT_RETRY_MS) {

    lastMQTTAttempt = millis();

    Serial.println(F("MQTT putus, mencoba reconnect..."));
    connectMQTT();
  }

  mqtt.loop();
  updatePosition();
}
