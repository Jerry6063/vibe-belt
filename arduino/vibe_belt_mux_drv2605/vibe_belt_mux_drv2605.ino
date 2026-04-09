#include <Wire.h>
#include <Adafruit_DRV2605.h>

Adafruit_DRV2605 drv;

// Two PCA9548A muxes
#define MUX_A 0x70
#define MUX_B 0x71

// Use 14 motors: 0x70 CH0..CH6 (7), 0x71 CH0..CH6 (7)
#define MOTORS_PER_MUX 7
#define NUM_MOTORS 14

// ---------- Timing ----------
const unsigned long FLOW_TICK_MS = 120;      // cursor flow speed (z)
const unsigned long WAVE_STEP_MS = 90;       // wave speed (w/a/d)
const unsigned long LONG_VIBE_MS = 900;      // for 's' long vibration (per motor)

// ---------- Effects ----------
const uint8_t EFFECT_STRONG = 1;   // strong click
const uint8_t EFFECT_WEAK   = 3;   // weaker click
const uint8_t EFFECT_LONG   = 118; // "Long buzz for programmatic stopping" (library effect)

// ---------- Flow state ----------
bool flowing = false;
int8_t flowDir = +1;               // only forward for now
unsigned long lastTick = 0;
uint8_t style = 1;                 // 0=single point, 1=point+trail
int cursor = 0;

// ---------- Wave orders (EDIT THESE TO MATCH YOUR HEX LAYOUT) ----------
// Default: treat motor index 0..13 as a 1D path.
// W bottom->top, A right->left use reverse, D left->right use forward.
const uint8_t WAVE_LEFT_TO_RIGHT[NUM_MOTORS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
const uint8_t WAVE_RIGHT_TO_LEFT[NUM_MOTORS] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
const uint8_t WAVE_BOTTOM_TO_TOP[NUM_MOTORS] = { 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

// ---------- MUX helpers ----------
void muxSelect(uint8_t muxAddr, uint8_t ch) {
  Wire.beginTransmission(muxAddr);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delay(2);
}

// Map motor index (0..13) -> (muxAddr, muxCh)
void motorMap(uint8_t idx, uint8_t &muxAddr, uint8_t &muxCh) {
  if (idx < MOTORS_PER_MUX) {
    muxAddr = MUX_A;
    muxCh = idx;                  // 0..6
  } else {
    muxAddr = MUX_B;
    muxCh = idx - MOTORS_PER_MUX; // 0..6
  }
}

// Init DRV on currently selected mux channel
bool initDRV() {
  bool ok = drv.begin();
  if (!ok) return false;
  drv.selectLibrary(1);
  drv.useERM();
  drv.setMode(DRV2605_MODE_INTTRIG);
  return true;
}

void playEffect(uint8_t effect) {
  drv.setWaveform(0, effect);
  drv.setWaveform(1, 0);
  drv.go();
}

void stopEffectNow() {
  drv.setWaveform(0, 0);
  drv.setWaveform(1, 0);
  drv.go();
}

// Vibrate one motor by index
void vibrateMotor(uint8_t idx, uint8_t effect) {
  uint8_t muxAddr, muxCh;
  motorMap(idx, muxAddr, muxCh);
  muxSelect(muxAddr, muxCh);
  if (!initDRV()) {
    Serial.print("Motor "); Serial.print(idx);
    Serial.println(" FAIL (DRV not found)");
    return;
  }
  playEffect(effect);
}

// Advance cursor along ring path
int advanceCursor(int c, int step) {
  int n = (c + step) % NUM_MOTORS;
  if (n < 0) n += NUM_MOTORS;
  return n;
}

// One tick of flow motion
void stepOnce(int stepDir) {
  int prev = cursor;
  cursor = advanceCursor(cursor, stepDir);

  vibrateMotor(cursor, EFFECT_STRONG);

  if (style == 1) {
    delay(50);
    vibrateMotor(prev, EFFECT_WEAK);
  }

  Serial.print("Cursor -> ");
  Serial.println(cursor);
}

// Generic wave runner
void runWave(const uint8_t *order, uint8_t len, uint8_t repeats) {
  for (uint8_t r = 0; r < repeats; r++) {
    for (uint8_t i = 0; i < len; i++) {
      vibrateMotor(order[i], EFFECT_STRONG);
      delay(WAVE_STEP_MS);
    }
  }
}

// 's' action: long vibration once (sequential; can't truly do all at the same instant)
void longVibeAllOnce() {
  Serial.println("S: long vibrate all motors once");
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    vibrateMotor(i, EFFECT_LONG);
    delay(LONG_VIBE_MS);
    stopEffectNow();
    delay(40);
  }
}

void printHelp() {
  Serial.println("\n=== Controls ===");
  Serial.println("z : start FLOW forward (cursor moves)");
  Serial.println("x : stop FLOW");
  Serial.println("0..13 : jump cursor to that motor index (pulse)");
  Serial.println("w : bottom->top wave x2");
  Serial.println("a : right->left wave x1");
  Serial.println("d : left->right wave x1");
  Serial.println("s : long vibrate all motors once");
  Serial.println("m : toggle flow style (0=point,1=point+trail)");
  Serial.println("================\n");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Wire.begin();
  printHelp();

  // quick roll call (optional)
  for (int i = 0; i < NUM_MOTORS; i++) {
    vibrateMotor(i, EFFECT_WEAK);
    delay(50);
  }
}

void loop() {
  // serial input
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      char c = line[0];

      if (c == 'z' || c == 'Z') {
        flowing = true;
        flowDir = +1;
        Serial.println("FLOW forward (z)");
      }
      else if (c == 'x' || c == 'X') {
        flowing = false;
        Serial.println("FLOW stop (x)");
      }
      else if (c == 'w' || c == 'W') {
        flowing = false;
        Serial.println("W: bottom->top wave x2");
        runWave(WAVE_BOTTOM_TO_TOP, NUM_MOTORS, 2);
      }
      else if (c == 'a' || c == 'A') {
        flowing = false;
        Serial.println("A: right->left wave");
        runWave(WAVE_RIGHT_TO_LEFT, NUM_MOTORS, 1);
      }
      else if (c == 'd' || c == 'D') {
        flowing = false;
        Serial.println("D: left->right wave");
        runWave(WAVE_LEFT_TO_RIGHT, NUM_MOTORS, 1);
      }
      else if (c == 's' || c == 'S') {
        flowing = false;
        longVibeAllOnce();
      }
      else if (c == 'm' || c == 'M') {
        style = (style == 0) ? 1 : 0;
        Serial.print("Style = "); Serial.println(style);
      }
      else {
        // number 0..13 => jump cursor
        int v = line.toInt();
        if (v >= 0 && v < NUM_MOTORS) {
          flowing = false;
          cursor = v;
          Serial.print("Cursor set to "); Serial.println(cursor);
          vibrateMotor(cursor, EFFECT_STRONG);
        } else {
          printHelp();
        }
      }
    }
  }

  // flow tick (non-blocking)
  if (flowing) {
    unsigned long now = millis();
    if (now - lastTick >= FLOW_TICK_MS) {
      lastTick = now;
      stepOnce(flowDir);
    }
  }
}

