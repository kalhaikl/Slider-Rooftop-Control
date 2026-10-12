
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
#define RF_FRAME_COUNT 5

#define EEPROM_SIZE 1024
#define CONFIG_MAGIC 0xA71B

const char *AP_SSID = "ATAP-SETUP";
const char *AP_PASSWORD = "";

const unsigned long OPEN_FULL_MS = 20000;
const unsigned long CLOSE_FULL_MS = 20000;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;

// =====================================================
// KODE RF
// =====================================================

static const char RF_CODE_A_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000111101000010000100001000";

static const char RF_CODE_B_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000100001000011110100";

static const char RF_CODE_C_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000111101000010000100";

static const char RF_CODE_D_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000100001000011110100001000";

static const char *RF_CODE_A[RF_FRAME_COUNT] = {
  RF_CODE_A_BITS, RF_CODE_A_BITS, RF_CODE_A_BITS,
  RF_CODE_A_BITS, RF_CODE_A_BITS
};

static const char *RF_CODE_B[RF_FRAME_COUNT] = {
  RF_CODE_B_BITS, RF_CODE_B_BITS, RF_CODE_B_BITS,
  RF_CODE_B_BITS, RF_CODE_B_BITS
};

static const char *RF_CODE_C[RF_FRAME_COUNT] = {
  RF_CODE_C_BITS, RF_CODE_C_BITS, RF_CODE_C_BITS,
  RF_CODE_C_BITS, RF_CODE_C_BITS
};

static const char *RF_CODE_D[RF_FRAME_COUNT] = {
  RF_CODE_D_BITS, RF_CODE_D_BITS, RF_CODE_D_BITS,
  RF_CODE_D_BITS, RF_CODE_D_BITS
};

// =====================================================
// KONFIGURASI TERSIMPAN
// =====================================================

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

bool moving = false;
bool positionKnown = false;

int roofPosition = 0;
int motionStartPosition = 0;
int motionTarget = 0;

unsigned long motionStartTime = 0;
unsigned long motionDuration = 0;
unsigned long lastMQTTAttempt = 0;
unsigned long lastWiFiAttempt = 0;

String lastCommand = "Belum ada perintah";
String motionDirection = "Diam";

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

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));

  cfg.magic = CONFIG_MAGIC;
  cfg.mqttPort = 1883;

  copyText(cfg.mqttHost, sizeof(cfg.mqttHost), "192.168.1.100");
  copyText(cfg.baseTopic, sizeof(cfg.baseTopic), "ATAP1");
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  if (cfg.magic != CONFIG_MAGIC) {
    setDefaults();
  }
}

void saveSettings() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// =====================================================
// TOPIC MQTT
// =====================================================

String topicLWT() {
  return String(cfg.baseTopic) + "/lwt";
}

String topicIP() {
  return String(cfg.baseTopic) + "/IP";
}

String topicCommand() {
  return String(cfg.baseTopic) + "/cmnd";
}

String topicStatus() {
  return String(cfg.baseTopic) + "/status";
}

// =====================================================
// RF TRANSMITTER
// =====================================================

void sendRF(const char *const frames[]) {
  for (int frame = 0; frame < RF_FRAME_COUNT; frame++) {
    const char *bits = frames[frame];

    for (size_t i = 0; bits[i] != '\0'; i++) {
      digitalWrite(RF_PIN, bits[i] == '1' ? HIGH : LOW);
      delayMicroseconds(RF_BIT_US);
    }

    digitalWrite(RF_PIN, LOW);

    if (frame < RF_FRAME_COUNT - 1) {
      delayMicroseconds(RF_GAP_US);
    }
  }

  digitalWrite(RF_PIN, LOW);
}

void sendOpenRF() {
  Serial.println(F("RF A - OPEN"));
  sendRF(RF_CODE_A);
}

void sendCloseRF() {
  Serial.println(F("RF B - CLOSE"));
  sendRF(RF_CODE_B);
}

void sendNothingRF() {
  Serial.println(F("RF C - NOTHING"));
  sendRF(RF_CODE_C);
}

void sendPauseRF() {
  Serial.println(F("RF D - PAUSE"));
  sendRF(RF_CODE_D);
}

// =====================================================
// STATUS MQTT
// =====================================================

void publishStatus() {
  if (!mqtt.connected()) return;

  String payload = "{";
  payload += "\"position\":" + String(roofPosition);
  payload += ",\"position_known\":";
  payload += positionKnown ? "true" : "false";
  payload += ",\"moving\":";
  payload += moving ? "true" : "false";
  payload += ",\"direction\":\"" + motionDirection + "\"";
  payload += ",\"command\":\"" + lastCommand + "\"";
  payload += "}";

  String topic = topicStatus();
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

void pauseRoof() {
  if (moving && positionKnown && motionDuration > 0) {
    unsigned long elapsed = millis() - motionStartTime;

    float ratio = min(1.0f,
      (float)elapsed / (float)motionDuration);

    int delta = motionTarget - motionStartPosition;

    roofPosition = constrain(
      motionStartPosition + (int)(delta * ratio),
      0, 100
    );
  }

  // Kirim kode pause meskipun posisi belum diketahui.
  sendPauseRF();

  moving = false;
  motionDirection = "Diam";
  lastCommand = "pause";

  publishStatus();
}

void moveToPosition(int target) {
  target = constrain(target, 0, 100);

  if (moving) {
    pauseRoof();
  }

  // Posisi parsial memerlukan referensi posisi yang diketahui.
  if (!positionKnown && target != 0 && target != 100) {
    lastCommand =
      "Posisi belum diketahui. Jalankan OPEN/CLOSE penuh dahulu.";
    publishStatus();
    return;
  }

  if (positionKnown && target == roofPosition) {
    lastCommand = "Posisi sudah sesuai";
    publishStatus();
    return;
  }

  motionStartPosition = roofPosition;
  motionTarget = target;
  motionStartTime = millis();

  if (target == 100) {
    motionDirection = "Membuka";

    motionDuration = positionKnown
      ? (unsigned long)(OPEN_FULL_MS *
          (100.0f - roofPosition) / 100.0f)
      : OPEN_FULL_MS;

    sendOpenRF();
    lastCommand = "open";
  } else if (target == 0) {
    motionDirection = "Menutup";

    motionDuration = positionKnown
      ? (unsigned long)(CLOSE_FULL_MS *
          roofPosition / 100.0f)
      : CLOSE_FULL_MS;

    sendCloseRF();
    lastCommand = "close";
  } else if (target > roofPosition) {
    motionDirection = "Membuka";

    motionDuration = (unsigned long)(
      OPEN_FULL_MS * (target - roofPosition) / 100.0f
    );

    sendOpenRF();
    lastCommand = String(target);
  } else {
    motionDirection = "Menutup";

    motionDuration = (unsigned long)(
      CLOSE_FULL_MS * (roofPosition - target) / 100.0f
    );

    sendCloseRF();
    lastCommand = String(target);
  }

  if (motionDuration == 0) {
    sendPauseRF();
    roofPosition = target;
    positionKnown = true;
    moving = false;
    motionDirection = "Diam";
  } else {
    moving = true;
  }

  publishStatus();
}

void updatePosition() {
  if (!moving) return;

  unsigned long elapsed = millis() - motionStartTime;

  if (elapsed >= motionDuration) {
    sendPauseRF();

    roofPosition = motionTarget;
    positionKnown = true;
    moving = false;
    motionDirection = "Diam";

    Serial.printf("Posisi estimasi: %d%%\n", roofPosition);

    publishStatus();
    return;
  }

  if (positionKnown && motionDuration > 0) {
    float ratio = (float)elapsed / (float)motionDuration;
    int delta = motionTarget - motionStartPosition;

    roofPosition = constrain(
      motionStartPosition + (int)(delta * ratio),
      0, 100
    );
  }
}

// =====================================================
// PEMROSESAN PERINTAH
// =====================================================

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();

  Serial.print(F("Perintah: "));
  Serial.println(command);

  if (command == "on" || command == "1" ||
      command == "open") {
    moveToPosition(100);
  } else if (command == "off" || command == "0" ||
             command == "close") {
    moveToPosition(0);
  } else if (command == "pause" || command == "stop") {
    pauseRoof();
  } else if (command == "25") {
    moveToPosition(25);
  } else if (command == "50") {
    moveToPosition(50);
  } else if (command == "75") {
    moveToPosition(75);
  } else if (command == "nothing" || command == "c") {
    sendNothingRF();
    lastCommand = "nothing";
    publishStatus();
  } else {
    Serial.println(F("Perintah tidak dikenal"));
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != topicCommand()) return;

  String command;
  command.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

  handleCommand(command);
}

// =====================================================
// WI-FI
// =====================================================

void startAccessPoint() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print(F("AP setup: "));
  Serial.println(AP_SSID);
  Serial.print(F("Buka: http://"));
  Serial.println(WiFi.softAPIP());
}

void connectWiFi() {
  if (strlen(cfg.wifiSSID) == 0) return;

  WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);

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

// =====================================================
// MQTT RECONNECT
// =====================================================

bool connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqtt.connected()) return true;

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);

  String clientID =
    "AtapWemos-" + String(ESP.getChipId(), HEX);

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

  String cmd = topicCommand();
  mqtt.subscribe(cmd.c_str(), 1);

  mqtt.publish(lwt.c_str(), "Online", true);
  publishIP();
  publishStatus();

  Serial.println(F("MQTT tersambung"));
  Serial.println("LWT: " + lwt);
  Serial.println("IP: " + topicIP());
  Serial.println("CMND: " + cmd);

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
<title>)rawliteral";

  html += title;

  html += R"rawliteral(</title>
<style>
body{font-family:Arial,sans-serif;background:#eef3fd;color:#14213d;
margin:0;padding:18px}
main{max-width:540px;margin:auto}
section{background:white;padding:18px;margin:12px 0;
border-radius:14px;box-shadow:0 2px 8px #ccd5e5}
h1{color:#1557e6;font-size:24px}
button{border:0;border-radius:9px;padding:13px;margin:4px;
background:#1557e6;color:white;font-weight:bold;cursor:pointer}
.stop{background:#d92d43}
input{box-sizing:border-box;width:100%;padding:11px;margin:5px 0 12px;
border:1px solid #cbd5e1;border-radius:8px}
a{color:#1557e6}
small{color:#5a6a8a}
</style></head><body><main>
)rawliteral";

  return html;
}

void handleRoot() {
  String html = pageHeader("Kontrol Atap");

  html += "<h1>Kontrol Atap</h1><section><h3>Status</h3>";
  html += "<p>Wi-Fi: ";
  html += WiFi.status() == WL_CONNECTED ? "Terhubung" : "Terputus";
  html += "<br>IP: " + WiFi.localIP().toString();
  html += "<br>MQTT: ";
  html += mqtt.connected() ? "Terhubung" : "Terputus";
  html += "<br>Posisi: ";
  html += positionKnown ? String(roofPosition) + "%" : "Belum diketahui";
  html += "<br>Gerakan: " + motionDirection;
  html += "<br>Perintah: " + htmlEscape(lastCommand);
  html += "</p></section>";

  html += R"rawliteral(
<section><h3>Kontrol Atap</h3>
<p><button onclick="cmd('open')">Buka 100%</button>
<button onclick="cmd('close')">Tutup 100%</button></p>
<p><button onclick="cmd('25')">25%</button>
<button onclick="cmd('50')">50%</button>
<button onclick="cmd('75')">75%</button></p>
<p><button class="stop" onclick="cmd('pause')">PAUSE</button></p>
<small>Persentase berdasarkan estimasi waktu, bukan sensor posisi.</small>
</section>
<section><h3>Konfigurasi</h3>
<p><a href="/config">Pengaturan Wi-Fi dan MQTT</a></p>
<p><a href="/update">OTA Firmware (tanpa password)</a></p>
</section>
<script>
function cmd(c){
 fetch('/cmd?c='+encodeURIComponent(c))
 .then(r=>r.text()).then(alert);
}
setTimeout(()=>location.reload(),10000);
</script>
</main></body></html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = pageHeader("Konfigurasi");

  html += R"rawliteral(
<h1>Konfigurasi</h1><section>
<form method="POST" action="/save">
<label>Wi-Fi SSID router</label>
<input name="ssid" maxlength="32" value=")rawliteral";
  html += htmlEscape(cfg.wifiSSID);

  html += R"rawliteral(">
<label>Password Wi-Fi</label>
<input name="wpass" type="password" maxlength="64"
placeholder="Kosongkan untuk mempertahankan password">
<label>MQTT Host/IP</label>
<input name="host" maxlength="63" value=")rawliteral";
  html += htmlEscape(cfg.mqttHost);

  html += R"rawliteral(">
<label>MQTT Port</label>
<input name="port" type="number" min="1" max="65535" value=")rawliteral";
  html += String(cfg.mqttPort);

  html += R"rawliteral(">
<label>MQTT Username</label>
<input name="user" maxlength="32" value=")rawliteral";
  html += htmlEscape(cfg.mqttUser);

  html += R"rawliteral(">
<label>Password MQTT</label>
<input name="mpass" type="password" maxlength="64"
placeholder="Kosongkan untuk mempertahankan password">
<label>MQTT Base Topic</label>
<input name="base" maxlength="63" value=")rawliteral";
  html += htmlEscape(cfg.baseTopic);

  html += R"rawliteral(">
<button type="submit">Simpan konfigurasi</button>
</form></section><p><a href="/">Kembali ke dashboard</a></p>
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

  if (server.hasArg("base")) {
    String base = server.arg("base");
    base.trim();

    if (base.length() > 0 &&
        base.length() < sizeof(cfg.baseTopic) &&
        base.indexOf(' ') < 0 &&
        base.indexOf('+') < 0 &&
        base.indexOf('#') < 0 &&
        base.indexOf('/') < 0) {
      copyText(cfg.baseTopic, sizeof(cfg.baseTopic), base);
    }
  }

  saveSettings();

  mqtt.disconnect();

  server.send(200, "text/html",
    "<h2>Konfigurasi tersimpan</h2>"
    "<p>Wemos mencoba menghubungkan Wi-Fi dan MQTT.</p>"
    "<p><a href='/'>Kembali ke dashboard</a></p>");

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

  handleCommand(server.arg("c"));
  server.send(200, "text/plain", "Perintah diproses: " + lastCommand);
}

void handleStatusWeb() {
  String json = "{";
  json += "\"position\":" + String(roofPosition);
  json += ",\"position_known\":";
  json += positionKnown ? "true" : "false";
  json += ",\"moving\":";
  json += moving ? "true" : "false";
  json += ",\"wifi\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"mqtt\":";
  json += mqtt.connected() ? "true" : "false";
  json += "}";

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

  // AP konfigurasi tetap tersedia.
  startAccessPoint();

  setupWeb();

  connectWiFi();

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);

  connectMQTT();

  Serial.println();
  Serial.println(F("=== ATAP CONTROLLER READY ==="));
  Serial.println("Setup AP: http://192.168.4.1");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("Dashboard router: http://"));
    Serial.println(WiFi.localIP());
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  server.handleClient();

  // Reconnect Wi-Fi berkala.
  if (WiFi.status() != WL_CONNECTED &&
      strlen(cfg.wifiSSID) > 0 &&
      millis() - lastWiFiAttempt >= WIFI_RETRY_MS) {

    lastWiFiAttempt = millis();

    Serial.println(F("Wi-Fi putus, mencoba reconnect..."));
    WiFi.begin(cfg.wifiSSID, cfg.wifiPassword);
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