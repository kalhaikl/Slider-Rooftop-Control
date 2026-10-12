#include <Arduino.h>

// =====================================================
// RF RECEIVER (XY-MK-5V) - WEMOS D1 MINI, PIN D5
//
// Membaca sinyal RF 433 MHz, mengubahnya menjadi deretan
// bit (1 bit = 300 us) seperti RF_CODE_x_BITS di firmware
// atap, lalu mencocokkannya dengan kode A, B, C, dan D.
//
// Wiring:
//   XY-MK-5V VCC  -> 3V3 (atau 5V, lihat catatan)
//   XY-MK-5V DATA -> D5  (salah satu pin DATA, keduanya sama)
//   XY-MK-5V GND  -> G
//
// Catatan: bila VCC receiver dari 5V, output DATA bisa
// mencapai 5V dan GPIO ESP8266 hanya aman 3.3V. Pasang
// pembagi tegangan: DATA -> 10k -> D5 -> 20k -> GND.
//
// Serial Monitor: 115200 baud.
// =====================================================

#define RX_PIN D5

#define BIT_US 300          // lebar 1 bit
#define GAP_MIN_US 6000     // LOW selama ini = jeda antar frame
#define PULSE_MAX_US 4000   // pulsa lebih panjang = bukan frame valid
#define PULSE_TOL_US 130    // toleransi lebar pulsa terhadap kelipatan 300 us
#define MAX_BAD_PULSES 2    // frame ditolak bila pulsa menyimpang lebih dari ini
#define MIN_PULSES 20
#define MAX_PULSES 300
#define BITS_MAX 400
#define RING_SIZE 512
#define REPEAT_WINDOW_MS 600
#define STATUS_MS 3000

#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR
#endif

// =====================================================
// KODE YANG DIKENAL (sama dengan firmware atap)
// =====================================================

static const char RF_CODE_A_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000111101000010000100001000";

static const char RF_CODE_B_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000100001000011110100";

static const char RF_CODE_C_BITS[] =
"111101000011110100001000011110100001111010000100001111010000100001000010000100001000010000100001000010000111101000010000100";

static const char RF_CODE_D_BITS[] =
"1111010000111101000010000111101000011110100001000011110100001000010000100001000010000100001000010000100001000011110100001000";

struct KnownCode {
  const char *name;
  const char *bits;
};

static const KnownCode KNOWN[] = {
  {"A (OPEN)",    RF_CODE_A_BITS},
  {"B (CLOSE)",   RF_CODE_B_BITS},
  {"C (NOTHING)", RF_CODE_C_BITS},
  {"D (PAUSE)",   RF_CODE_D_BITS}
};

static const int KNOWN_COUNT = sizeof(KNOWN) / sizeof(KNOWN[0]);

// =====================================================
// PENANGKAP PULSA (INTERRUPT)
// =====================================================

volatile uint32_t ringDur[RING_SIZE];
volatile uint8_t ringLvl[RING_SIZE];
volatile uint16_t ringHead = 0;
volatile uint16_t ringTail = 0;
volatile bool ringOverflow = false;
volatile uint32_t lastEdgeUs = 0;
volatile uint32_t edgeCount = 0;

void IRAM_ATTR onEdge() {
  uint32_t now = micros();
  uint32_t dur = now - lastEdgeUs;
  lastEdgeUs = now;
  edgeCount++;

  // Level pulsa yang baru saja berakhir = kebalikan level sekarang.
  uint8_t endedLevel = digitalRead(RX_PIN) ? 0 : 1;

  uint16_t next = (ringHead + 1) % RING_SIZE;
  if (next == ringTail) {
    ringOverflow = true;
    return;
  }

  ringDur[ringHead] = dur;
  ringLvl[ringHead] = endedLevel;
  ringHead = next;
}

// =====================================================
// DEKODER FRAME
// =====================================================

uint16_t frameDur[MAX_PULSES];
uint8_t frameLvl[MAX_PULSES];
uint16_t frameCount = 0;

bool collecting = false;
bool frameValid = true;

uint32_t lastGapUs = 0;
uint32_t frameNumber = 0;
uint32_t rejectedFrames = 0;
unsigned long lastFrameMs = 0;
unsigned long lastStatusMs = 0;

char lastBits[BITS_MAX];
int lastBitsLen = 0;
int repeatCount = 0;

int trimmedLen(const char *s) {
  int l = strlen(s);
  while (l > 0 && s[l - 1] == '0') l--;
  return l;
}

void handleFrame() {
  if (frameCount < MIN_PULSES) return;

  char bits[BITS_MAX];
  int len = 0;
  int bad = 0;

  for (uint16_t i = 0; i < frameCount; i++) {
    uint32_t d = frameDur[i];
    uint32_t n = (d + BIT_US / 2) / BIT_US;
    if (n < 1) n = 1;

    int err = (int)d - (int)(n * BIT_US);
    if (err < 0) err = -err;
    if (err > PULSE_TOL_US) bad++;

    for (uint32_t k = 0; k < n; k++) {
      if (len >= BITS_MAX - 1) {
        rejectedFrames++;
        return;
      }
      bits[len++] = frameLvl[i] ? '1' : '0';
    }
  }

  bits[len] = '\0';

  if (bad > MAX_BAD_PULSES) {
    rejectedFrames++;
    return;
  }

  // Cocokkan dengan kode yang dikenal. Nol di akhir kode asli
  // menyatu dengan jeda sehingga tidak terbaca receiver.
  int bestIdx = -1;
  int bestDiff = 9999;

  for (int c = 0; c < KNOWN_COUNT; c++) {
    int kl = trimmedLen(KNOWN[c].bits);
    if (kl != len) continue;

    int diff = 0;
    for (int i = 0; i < len; i++) {
      if (bits[i] != KNOWN[c].bits[i]) diff++;
    }

    if (diff < bestDiff) {
      bestDiff = diff;
      bestIdx = c;
    }
  }

  frameNumber++;

  // Frame identik yang datang beruntun = pengulangan dari 1 kali tekan.
  static unsigned long prevFrameMs = 0;
  unsigned long now = millis();
  lastFrameMs = now;

  bool isRepeat = (len == lastBitsLen) &&
                  (memcmp(bits, lastBits, len) == 0) &&
                  (now - prevFrameMs < REPEAT_WINDOW_MS);
  prevFrameMs = now;

  if (isRepeat) {
    repeatCount++;
    Serial.printf("  ulangan ke-%d sama (%d bit, pulsa buruk: %d)\n",
                  repeatCount + 1, len, bad);
    return;
  }

  repeatCount = 0;
  memcpy(lastBits, bits, len + 1);
  lastBitsLen = len;

  Serial.println();
  Serial.printf("Frame #%lu | %d bit | %u pulsa | jeda sebelumnya %lu us | pulsa buruk: %d\n",
                frameNumber, len, frameCount, lastGapUs, bad);

  if (bestIdx >= 0 && bestDiff == 0) {
    Serial.printf("COCOK: kode %s\n", KNOWN[bestIdx].name);
  } else if (bestIdx >= 0 && bestDiff <= 3) {
    Serial.printf("HAMPIR COCOK: kode %s (beda %d bit)\n",
                  KNOWN[bestIdx].name, bestDiff);
  } else {
    Serial.println("Kode BARU / tidak dikenal. Salin untuk dipakai:");
  }

  Serial.println("static const char RF_CODE_X_BITS[] =");
  Serial.print('"');
  Serial.print(bits);
  Serial.println("\";");
}

void processPulse(uint8_t level, uint32_t dur) {
  // LOW yang sangat panjang = jeda antar frame.
  if (level == 0 && dur >= GAP_MIN_US) {
    if (collecting && frameValid) handleFrame();

    collecting = true;
    frameValid = true;
    frameCount = 0;
    lastGapUs = dur;
    return;
  }

  if (!collecting || !frameValid) return;

  // Frame selalu diawali pulsa HIGH setelah jeda.
  if (frameCount == 0 && level == 0) {
    frameValid = false;
    return;
  }

  if (dur > PULSE_MAX_US || frameCount >= MAX_PULSES) {
    frameValid = false;
    return;
  }

  frameDur[frameCount] = (uint16_t)dur;
  frameLvl[frameCount] = level;
  frameCount++;
}

// =====================================================
// SETUP DAN LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println(F("=== RF RECEIVER READY (pin D5) ==="));
  Serial.println(F("Tekan tombol remote atau nyalakan transmitter."));

  pinMode(RX_PIN, INPUT);
  lastEdgeUs = micros();
  attachInterrupt(digitalPinToInterrupt(RX_PIN), onEdge, CHANGE);
}

void loop() {
  if (ringOverflow) {
    ringOverflow = false;
    collecting = false;
  }

  while (ringTail != ringHead) {
    uint8_t level = ringLvl[ringTail];
    uint32_t dur = ringDur[ringTail];

    ringTail = (ringTail + 1) % RING_SIZE;

    processPulse(level, dur);
  }

  if (millis() - lastStatusMs >= STATUS_MS) {
    lastStatusMs = millis();

    noInterrupts();
    uint32_t edges = edgeCount;
    edgeCount = 0;
    interrupts();

    if (millis() - lastFrameMs >= STATUS_MS) {
      Serial.printf("Menunggu sinyal... edge/%d detik: %lu, frame ditolak: %lu\n",
                    STATUS_MS / 1000, edges, rejectedFrames);

      if (edges == 0) {
        Serial.println(F("  Tidak ada aktivitas sama sekali: cek kabel VCC, DATA, GND."));
      }
    }
  }

  yield();
}
