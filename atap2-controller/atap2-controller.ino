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

const unsigned long OPEN_FULL_MS = 5000;
const unsigned long CLOSE_FULL_MS = 5000;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;

// =====================================================
// KODE RF
// =====================================================

// Segitiga ATAS (OPEN)
static const char RF_CODE_A_BITS[] =
"10000111100111100100001000011110011110011110010000111100100001000010000100001000011110011110010000111100111100111100100001000010000100001111001000010000100001111001000010000100";

// Segitiga BAWAH (CLOSE)
static const char RF_CODE_B_BITS[] =
"1000011110011110010000100001111001111001111001000011110010000100001000010000100001111001111001000011110011110010000111100100001000010000111100100001000010000111100100001000010000";

// Kunci LOCK (NOTHING)
static const char RF_CODE_C_BITS[] =
"10000111100111100100001000011110011110011110010000111100100001000010000100001000011110011110010000111100111100100001000010000111100100001111001000010000100001111001000010000100";

// Kotak STOP (PAUSE)
static const char RF_CODE_D_BITS[] =
"100001111001111001000010000111100111100111100100001111001000010000100001000010000111100111100100001111001111001000010000111100100001000011110010000100001000011110010000100001000";

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
  unsigned long t0 = millis();

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

  Serial.printf("[%lu ms] RF selesai dikirim (%lu ms)\n",
                millis(), millis() - t0);
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

  Serial.printf("[%lu ms] MQTT cmnd diterima\n", millis());

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

// Matikan modem sleep agar paket MQTT/HTTP langsung diproses,
// tanpa menunggu beacon berikutnya dari router.
void lowLatencyWiFi() {
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
}

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
</style></head><body><main>
)rawliteral";

  return html;
}

void handleRoot() {
  String html = pageHeader("Kontrol Atap");

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool mqttOk = mqtt.connected();

  // Header
  html += "<header><h1>Kontrol Atap</h1>"
          "<span id=\"conn\" class=\"badge ok\">Terhubung</span></header>";

  // Posisi
  html += "<section aria-live=\"polite\"><h2>Posisi atap</h2>";
  html += "<div class=\"pos\"><div class=\"pct\" id=\"pct\">";
  html += positionKnown ? String(roofPosition) + "<span class=\"u\">%</span>"
                        : "?";
  html += "</div><div id=\"mv\" class=\"mv";
  html += moving ? " on" : "";
  html += "\">" + htmlEscape(motionDirection) + "</div></div>";

  html += "<div class=\"scale\"><div class=\"track\">";
  html += "<div id=\"bar\" class=\"bar";
  html += moving ? " moving" : "";
  html += positionKnown ? "" : " unknown";
  html += "\" style=\"width:";
  html += positionKnown ? String(constrain(roofPosition, 0, 100)) : "100";
  html += "%\"></div></div>";
  html += "<div class=\"ticks\"><span>Tutup</span><span>25</span>"
          "<span>50</span><span>75</span><span>Buka</span></div></div>";

  html += "<p id=\"hint\" class=\"hint\"";
  html += positionKnown ? " hidden" : "";
  html += ">Posisi belum diketahui. Tekan Buka atau Tutup penuh sekali "
          "agar persentase bisa dipakai.</p>";
  html += "</section>";

  // Kontrol
  html += R"rawliteral(
<section><h2>Kontrol</h2>
<div class="grid2">
<button onclick="cmd('open')">Buka penuh</button>
<button onclick="cmd('close')">Tutup penuh</button>
</div>
<div class="grid3">
)rawliteral";

  const char *presets[] = {"25", "50", "75"};
  for (int i = 0; i < 3; i++) {
    html += "<button class=\"alt pre\" onclick=\"cmd('";
    html += presets[i];
    html += "')\"";
    html += positionKnown ? "" : " disabled";
    html += ">";
    html += presets[i];
    html += "%</button>";
  }

  html += R"rawliteral(
</div>
<button class="stop" onclick="cmd('pause')">Pause</button>
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
  html += "<dt>Alamat IP</dt><dd>" + WiFi.localIP().toString() + "</dd>";
  html += "<dt>Perintah terakhir</dt><dd id=\"lc\">" +
          htmlEscape(lastCommand) + "</dd>";
  html += "</dl></section>";

  html += R"rawliteral(
<section><h2>Pengaturan</h2>
<a class="link" href="/config"><span>Wi-Fi dan MQTT
<small>Jaringan, broker, dan topic</small></span></a>
<a class="link" href="/update"><span>Update firmware (OTA)
<small>Tanpa password, gunakan hanya di jaringan tepercaya</small></span></a>
</section>
<div id="toast" role="status"></div>
<script>
var $=function(i){return document.getElementById(i)};
var dir=$('mv').textContent,lastPos=null,busy=false,fails=0;

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
 var k=s.position_known;
 $('pct').innerHTML=k?s.position+'<span class="u">%</span>':'?';
 var b=$('bar');
 b.style.width=(k?s.position:100)+'%';
 b.className='bar'+(s.moving?' moving':'')+(k?'':' unknown');
 if(!s.moving){dir='Diam';}
 else if(k&&lastPos!==null&&s.position!==lastPos){
  dir=s.position>lastPos?'Membuka':'Menutup';
 }else if(dir==='Diam'){dir='Bergerak';}
 lastPos=s.position;
 var m=$('mv');m.textContent=dir;m.className='mv'+(s.moving?' on':'');
 $('hint').hidden=k;
 document.querySelectorAll('.pre').forEach(function(x){x.disabled=!k});
 badge('wifi',s.wifi);badge('mqtt',s.mqtt);
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
function cmd(c){
 if(busy)return;
 busy=true;
 fetch('/cmd?c='+encodeURIComponent(c))
 .then(function(r){return r.text()})
 .then(function(t){
  var p='Perintah diproses: ';
  if(t.indexOf(p)===0){$('lc').textContent=t.slice(p.length);}
  toast(t);poll();
 })
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
<p class="help">Tanpa spasi, tanda /, + atau #. Perintah dikirim ke
<b>base topic</b>/cmnd.</p>
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

  String page = pageHeader("Konfigurasi tersimpan");
  page += R"rawliteral(
<header><h1>Kontrol Atap</h1></header>
<section class="done">
<h2>Konfigurasi tersimpan</h2>
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
  lowLatencyWiFi();

  setupWeb();

  connectWiFi();

  wifiClient.setTimeout(1500);

  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(2);
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
  mqtt.loop();
  server.handleClient();

  // Reconnect Wi-Fi berkala.
  if (WiFi.status() != WL_CONNECTED &&
      strlen(cfg.wifiSSID) > 0 &&
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
