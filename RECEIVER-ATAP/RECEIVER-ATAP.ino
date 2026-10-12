
#include <Arduino.h>

#define RF_RX_PIN D5

const uint16_t MAX_PULSES = 500;
const uint32_t FRAME_GAP_US = 5000;

volatile uint32_t pulseDuration[MAX_PULSES];
volatile uint8_t pulseLevel[MAX_PULSES];
volatile uint16_t pulseCount = 0;
volatile uint32_t lastEdgeUs = 0;
volatile bool frameReady = false;

void IRAM_ATTR rfISR() {
  uint32_t now = micros();
  uint32_t duration = now - lastEdgeUs;
  uint8_t level = digitalRead(RF_RX_PIN);

  if (lastEdgeUs != 0 && duration >= FRAME_GAP_US) {
    if (pulseCount > 0) {
      frameReady = true;
    }
    lastEdgeUs = now;
    return;
  }

  if (lastEdgeUs != 0 && !frameReady && duration > 0 &&
      pulseCount < MAX_PULSES) {
    pulseDuration[pulseCount] = duration;
    pulseLevel[pulseCount] = !level;
    pulseCount++;
  }

  lastEdgeUs = now;
}

void setup() {
  Serial.begin(115200);
  pinMode(RF_RX_PIN, INPUT);

  attachInterrupt(
    digitalPinToInterrupt(RF_RX_PIN),
    rfISR,
    CHANGE
  );

  Serial.println();
  Serial.println("XY-MK-5V RF Receiver Test");
  Serial.println("Pin DATA: D5");
  Serial.println("Menunggu sinyal FS1000A...");
}

void loop() {
  if (!frameReady) return;

  uint32_t durations[MAX_PULSES];
  uint8_t levels[MAX_PULSES];
  uint16_t count;

  noInterrupts();
  count = pulseCount;

  for (uint16_t i = 0; i < count; i++) {
    durations[i] = pulseDuration[i];
    levels[i] = pulseLevel[i];
  }

  pulseCount = 0;
  frameReady = false;
  interrupts();

  Serial.printf("\nFrame diterima: %u pulsa\n", count);
  Serial.println("Level,Durasi(us)");

  for (uint16_t i = 0; i < count; i++) {
    Serial.printf("%u,%lu\n",
                  levels[i],
                  (unsigned long)durations[i]);
  }

  Serial.println("--- akhir frame ---");
}