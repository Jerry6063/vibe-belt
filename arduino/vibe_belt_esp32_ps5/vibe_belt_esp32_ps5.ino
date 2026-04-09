// ============================================================
//  Vibe Belt – ESP32 + PS5 DualSense (Bluepad32) + 14x DRV2605
// ============================================================

#include <Wire.h>
#include <Adafruit_DRV2605.h>
#include <Bluepad32.h>

// -------------------- Hardware --------------------
#define MUX_A 0x70
#define MUX_B 0x71
#define SDA_PIN 21
#define SCL_PIN 22

Adafruit_DRV2605 drv;
ControllerPtr myController = nullptr;

// -------------------- Motor Layout --------------------
// Physical grid (3 rows x 5 cols, one gap):
//   075  071  ---  173  172   <- row 0 (top)
//   076  072  070  174  171   <- row 1 (mid)
//   077  073  176  175  170   <- row 2 (bot)
//
// Each motor ID encodes: high digit = mux (0=0x70, 1=0x71), low digit = channel

struct Motor {
  uint8_t mux;   // MUX_A or MUX_B
  uint8_t ch;    // channel 0-7
};

// All 14 motors in a flat list for random/all access
const Motor ALL_MOTORS[] = {
  {MUX_A, 5}, {MUX_A, 1}, {MUX_B, 3}, {MUX_B, 2},  // row 0 (skip gap)
  {MUX_A, 6}, {MUX_A, 2}, {MUX_A, 0}, {MUX_B, 4}, {MUX_B, 1},  // row 1
  {MUX_A, 7}, {MUX_A, 3}, {MUX_B, 6}, {MUX_B, 5}, {MUX_B, 0},  // row 2
};
const uint8_t NUM_MOTORS = 14;

// Column groups for left-right waves
const uint8_t COL_COUNT = 5;
const uint8_t COL_SIZES[] = {3, 3, 1, 3, 4};
// col 0: 075,076,077
const Motor COL0[] = {{MUX_A,5},{MUX_A,6},{MUX_A,7}};
// col 1: 071,072,073
const Motor COL1[] = {{MUX_A,1},{MUX_A,2},{MUX_A,3}};
// col 2: 070
const Motor COL2[] = {{MUX_A,0}};
// col 3: 173,174,176
const Motor COL3[] = {{MUX_B,3},{MUX_B,4},{MUX_B,6}};
// col 4: 172,171,175,170
const Motor COL4[] = {{MUX_B,2},{MUX_B,1},{MUX_B,5},{MUX_B,0}};

const Motor* COLS[] = {COL0, COL1, COL2, COL3, COL4};

// Row groups for up-down waves
const uint8_t ROW_COUNT = 3;
const uint8_t ROW_SIZES[] = {4, 5, 5};
// row 0: 075,071,173,172
const Motor ROW0[] = {{MUX_A,5},{MUX_A,1},{MUX_B,3},{MUX_B,2}};
// row 1: 076,072,070,174,171
const Motor ROW1[] = {{MUX_A,6},{MUX_A,2},{MUX_A,0},{MUX_B,4},{MUX_B,1}};
// row 2: 077,073,176,175,170
const Motor ROW2[] = {{MUX_A,7},{MUX_A,3},{MUX_B,6},{MUX_B,5},{MUX_B,0}};

const Motor* ROWS[] = {ROW0, ROW1, ROW2};

// Ring order for L1/R1 rotation (clockwise around the perimeter)
const Motor RING[] = {
  {MUX_A,5},{MUX_A,1},{MUX_B,3},{MUX_B,2},  // top L→R
  {MUX_B,1},{MUX_B,0},                        // right top→bot
  {MUX_B,5},{MUX_B,6},{MUX_A,3},{MUX_A,7},  // bot R→L
  {MUX_A,6},                                   // left bot→top
};
const uint8_t RING_LEN = 11;

// -------------------- Timing --------------------
const int16_t STICK_DEADZONE = 80;
const unsigned long WAVE_BASE_MS = 150;  // base time per column step
const unsigned long HEARTBEAT_ON  = 200;
const unsigned long HEARTBEAT_OFF = 300;
const unsigned long SCATTER_MS = 80;
const unsigned long BREATHE_PERIOD = 2000;
const unsigned long ROTATION_MS = 60;

// -------------------- RTP helpers --------------------
void muxSelect(uint8_t muxAddr, uint8_t ch) {
  Wire.beginTransmission(muxAddr);
  Wire.write(1 << ch);
  Wire.endTransmission();
}

bool initDrvOnChannel() {
  if (!drv.begin()) return false;
  drv.setMode(DRV2605_MODE_REALTIME);
  drv.useERM();
  return true;
}

// Set RTP value (0-127) on one motor
void setMotorRTP(const Motor &m, uint8_t rtp) {
  muxSelect(m.mux, m.ch);
  initDrvOnChannel();
  drv.setRealtimeValue(rtp);
}

// Set RTP on a group of motors
void setGroupRTP(const Motor* group, uint8_t count, uint8_t rtp) {
  for (uint8_t i = 0; i < count; i++) {
    setMotorRTP(group[i], rtp);
  }
}

// Stop all motors
void stopAll() {
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    setMotorRTP(ALL_MOTORS[i], 0);
  }
}

// -------------------- Bluepad32 callbacks --------------------
void onConnected(ControllerPtr ctl) {
  if (myController == nullptr) {
    myController = ctl;
    Serial.println("PS5 controller connected!");
  }
}

void onDisconnected(ControllerPtr ctl) {
  if (myController == ctl) {
    myController = nullptr;
    stopAll();
    Serial.println("PS5 controller disconnected.");
  }
}

// -------------------- Wave state --------------------
struct WaveState {
  bool active;
  int8_t dir;            // +1 or -1
  bool horizontal;       // true = cols, false = rows
  uint8_t groupCount;
  float pos;             // fractional position (0.0 .. groupCount-1)
  unsigned long lastStep;
  unsigned long stepMs;
};

WaveState wave = {false, 0, true, 0, 0, 0, 0};

void startWave(bool horizontal, int8_t dir, unsigned long stepMs) {
  wave.active = true;
  wave.horizontal = horizontal;
  wave.dir = dir;
  wave.groupCount = horizontal ? COL_COUNT : ROW_COUNT;
  wave.pos = (dir > 0) ? 0.0 : (wave.groupCount - 1.0);
  wave.lastStep = millis();
  wave.stepMs = stepMs;
}

void stopWave() {
  wave.active = false;
  stopAll();
}

// Render sine-faded wave at current fractional position
void renderWave() {
  const Motor** groups = wave.horizontal ? COLS : ROWS;
  const uint8_t* sizes = wave.horizontal ? COL_SIZES : ROW_SIZES;

  for (uint8_t g = 0; g < wave.groupCount; g++) {
    float dist = fabs((float)g - wave.pos);
    uint8_t rtp = 0;
    if (dist < 1.5) {
      // sine fade: cos(dist * PI / 1.5) mapped to 0..RTP_MAX
      float fade = cos(dist * PI / 1.5);
      if (fade < 0) fade = 0;
      rtp = (uint8_t)(fade * 100);
    }
    setGroupRTP(groups[g], sizes[g], rtp);
  }
}

void tickWave() {
  if (!wave.active) return;
  unsigned long now = millis();
  if (now - wave.lastStep < wave.stepMs) return;
  wave.lastStep = now;

  wave.pos += wave.dir * 0.25;  // move 0.25 per tick for smoothness
  float maxPos = wave.groupCount - 1.0;

  // wrap around
  if (wave.pos > maxPos + 0.5) wave.pos = -0.5;
  if (wave.pos < -0.5) wave.pos = maxPos + 0.5;

  renderWave();
}

// -------------------- Rotation state (L1/R1) --------------------
struct RotationState {
  bool active;
  int8_t dir;
  float pos;
  unsigned long lastStep;
};
RotationState rotation = {false, 0, 0, 0};

void renderRotation() {
  for (uint8_t i = 0; i < RING_LEN; i++) {
    float dist = fabs((float)i - rotation.pos);
    // handle wrap distance
    float wrapDist = RING_LEN - dist;
    if (wrapDist < dist) dist = wrapDist;

    uint8_t rtp = 0;
    if (dist < 2.0) {
      float fade = cos(dist * PI / 2.0);
      if (fade < 0) fade = 0;
      rtp = (uint8_t)(fade * 100);
    }
    setMotorRTP(RING[i], rtp);
  }
  // also handle center motors not in ring (070, 174/176)
  setMotorRTP({MUX_A, 0}, 0);
  setMotorRTP({MUX_B, 4}, 0);
  setMotorRTP({MUX_B, 6}, 0);
}

void tickRotation() {
  if (!rotation.active) return;
  unsigned long now = millis();
  if (now - rotation.lastStep < ROTATION_MS) return;
  rotation.lastStep = now;

  rotation.pos += rotation.dir * 0.5;
  if (rotation.pos >= RING_LEN) rotation.pos -= RING_LEN;
  if (rotation.pos < 0) rotation.pos += RING_LEN;

  renderRotation();
}

// -------------------- Button mode states --------------------
bool btnX_active = false;      // heartbeat
bool btnO_active = false;      // sustained all
bool btnTri_active = false;    // scatter
bool btnSq_active = false;     // breathe

unsigned long heartbeatTimer = 0;
bool heartbeatPhase = false;   // true=on, false=off

unsigned long scatterTimer = 0;

unsigned long breatheStart = 0;

// -------------------- Process functions --------------------

void processLeftStick(int16_t lx, int16_t ly) {
  int16_t ax = abs(lx);
  int16_t ay = abs(ly);

  if (ax < STICK_DEADZONE && ay < STICK_DEADZONE) {
    if (wave.active) stopWave();
    return;
  }

  bool horizontal;
  int8_t dir;

  if (ax > ay) {
    horizontal = true;
    dir = (lx > 0) ? +1 : -1;
  } else {
    horizontal = false;
    dir = (ly > 0) ? +1 : -1;
  }

  // speed: bigger deflection → faster wave
  int16_t magnitude = max(ax, ay);
  unsigned long stepMs = map(magnitude, STICK_DEADZONE, 512, WAVE_BASE_MS, 30);
  stepMs = constrain(stepMs, 30, WAVE_BASE_MS);

  if (!wave.active || wave.horizontal != horizontal || wave.dir != dir) {
    startWave(horizontal, dir, stepMs);
  } else {
    wave.stepMs = stepMs;
  }
}

void processRightStick(int16_t rx, int16_t ry) {
  bool hasY = abs(ry) > STICK_DEADZONE;
  bool hasX = abs(rx) > STICK_DEADZONE;
  if (!hasY && !hasX) return;

  // Intensity: Y controls normal intensity, but extreme X (sharp turn) also triggers strong vibration
  float yIntensity = (float)abs(ry) / 512.0;  // 0.0 .. 1.0
  float xIntensity = (float)abs(rx) / 512.0;  // 0.0 .. 1.0

  // Sharp turn: Y near zero + X large → max intensity
  float intensity = max(yIntensity, xIntensity);
  uint8_t maxRtp = (uint8_t)(intensity * 127);
  if (maxRtp < 20) maxRtp = 20;

  // X axis maps to fractional column position (0.0 .. COL_COUNT-1)
  float pos = ((float)(constrain(rx, -512, 512) + 512) / 1024.0) * (COL_COUNT - 1);

  // Sine-fade across columns based on distance from pos
  for (uint8_t g = 0; g < COL_COUNT; g++) {
    float dist = fabs((float)g - pos);
    uint8_t rtp = 0;
    if (dist < 1.2) {
      float fade = cos(dist * PI / 1.2);
      if (fade < 0) fade = 0;
      rtp = (uint8_t)(fade * maxRtp);
    }
    setGroupRTP(COLS[g], COL_SIZES[g], rtp);
  }
}

void processHeartbeat() {
  if (!btnX_active) return;
  unsigned long now = millis();
  unsigned long dur = heartbeatPhase ? HEARTBEAT_ON : HEARTBEAT_OFF;
  if (now - heartbeatTimer < dur) return;
  heartbeatTimer = now;
  heartbeatPhase = !heartbeatPhase;

  uint8_t rtp = heartbeatPhase ? 100 : 0;
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    setMotorRTP(ALL_MOTORS[i], rtp);
  }
}

void processSustain() {
  // btnO: just keep all motors at steady vibration (set once on press)
}

void processScatter() {
  if (!btnTri_active) return;
  unsigned long now = millis();
  if (now - scatterTimer < SCATTER_MS) return;
  scatterTimer = now;

  // Turn off all, then random 3-4 motors on
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    setMotorRTP(ALL_MOTORS[i], 0);
  }
  uint8_t count = random(3, 6);
  for (uint8_t i = 0; i < count; i++) {
    uint8_t idx = random(0, NUM_MOTORS);
    setMotorRTP(ALL_MOTORS[idx], random(50, 100));
  }
}

void processBreathe() {
  if (!btnSq_active) return;
  unsigned long elapsed = millis() - breatheStart;
  float phase = (float)(elapsed % BREATHE_PERIOD) / BREATHE_PERIOD;
  // sine: 0→1→0 over one period
  float intensity = sin(phase * PI);
  uint8_t rtp = (uint8_t)(intensity * 100);
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    setMotorRTP(ALL_MOTORS[i], rtp);
  }
}

void processL2R2(uint16_t l2, uint16_t r2) {
  // analog trigger: 0-1023, map to RTP 0-100
  uint16_t triggerVal = max(l2, r2);
  if (triggerVal < 20) return;  // deadzone
  uint8_t rtp = map(triggerVal, 20, 1023, 10, 100);
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    setMotorRTP(ALL_MOTORS[i], rtp);
  }
}

void processDpad(bool up, bool down, bool left, bool right) {
  if (left) {
    if (!wave.active || wave.dir != -1 || !wave.horizontal)
      startWave(true, -1, 100);
  } else if (right) {
    if (!wave.active || wave.dir != +1 || !wave.horizontal)
      startWave(true, +1, 100);
  } else if (up) {
    if (!wave.active || wave.dir != -1 || wave.horizontal)
      startWave(false, -1, 100);
  } else if (down) {
    if (!wave.active || wave.dir != +1 || wave.horizontal)
      startWave(false, +1, 100);
  }
}

// -------------------- Main input handler --------------------
void processController() {
  if (myController == nullptr || !myController->isConnected()) return;

  int16_t lx = myController->axisX();
  int16_t ly = myController->axisY();
  int16_t rx = myController->axisRX();
  int16_t ry = myController->axisRY();
  uint16_t l2 = myController->brake();
  uint16_t r2 = myController->throttle();

  bool x_btn   = myController->x();       // Cross
  bool o_btn   = myController->a();       // Circle
  bool tri_btn = myController->y();       // Triangle
  bool sq_btn  = myController->b();       // Square
  bool l1_btn  = myController->l1();
  bool r1_btn  = myController->r1();
  uint8_t dpad = myController->dpad();
  bool dUp     = (dpad & DPAD_UP);
  bool dDown   = (dpad & DPAD_DOWN);
  bool dLeft   = (dpad & DPAD_LEFT);
  bool dRight  = (dpad & DPAD_RIGHT);

  // Determine which mode is active (priority: buttons > triggers > sticks)
  bool anyButton = x_btn || o_btn || tri_btn || sq_btn || l1_btn || r1_btn;
  bool anyDpad = dUp || dDown || dLeft || dRight;
  bool anyTrigger = (l2 > 20) || (r2 > 20);
  bool anyLeftStick = (abs(lx) > STICK_DEADZONE) || (abs(ly) > STICK_DEADZONE);
  bool anyRightStick = (abs(rx) > STICK_DEADZONE) || (abs(ry) > STICK_DEADZONE);

  // --- X: heartbeat ---
  if (x_btn && !btnX_active) {
    btnX_active = true;
    heartbeatTimer = millis();
    heartbeatPhase = true;
    // stop other modes
    btnO_active = btnTri_active = btnSq_active = false;
    wave.active = false;
    rotation.active = false;
  } else if (!x_btn && btnX_active) {
    btnX_active = false;
    stopAll();
  }

  // --- O: sustained all ---
  if (o_btn && !btnO_active) {
    btnO_active = true;
    btnX_active = btnTri_active = btnSq_active = false;
    wave.active = false;
    rotation.active = false;
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
      setMotorRTP(ALL_MOTORS[i], 90);
    }
  } else if (!o_btn && btnO_active) {
    btnO_active = false;
    stopAll();
  }

  // --- Triangle: scatter ---
  if (tri_btn && !btnTri_active) {
    btnTri_active = true;
    scatterTimer = millis();
    btnX_active = btnO_active = btnSq_active = false;
    wave.active = false;
    rotation.active = false;
  } else if (!tri_btn && btnTri_active) {
    btnTri_active = false;
    stopAll();
  }

  // --- Square: breathe ---
  if (sq_btn && !btnSq_active) {
    btnSq_active = true;
    breatheStart = millis();
    btnX_active = btnO_active = btnTri_active = false;
    wave.active = false;
    rotation.active = false;
  } else if (!sq_btn && btnSq_active) {
    btnSq_active = false;
    stopAll();
  }

  // --- L1: clockwise rotation ---
  if (l1_btn) {
    if (!rotation.active || rotation.dir != +1) {
      rotation.active = true;
      rotation.dir = +1;
      rotation.pos = 0;
      rotation.lastStep = millis();
      wave.active = false;
      btnX_active = btnO_active = btnTri_active = btnSq_active = false;
    }
  } else if (!l1_btn && rotation.active && rotation.dir == +1) {
    rotation.active = false;
    stopAll();
  }

  // --- R1: counter-clockwise rotation ---
  if (r1_btn) {
    if (!rotation.active || rotation.dir != -1) {
      rotation.active = true;
      rotation.dir = -1;
      rotation.pos = RING_LEN - 1;
      rotation.lastStep = millis();
      wave.active = false;
      btnX_active = btnO_active = btnTri_active = btnSq_active = false;
    }
  } else if (!r1_btn && rotation.active && rotation.dir == -1) {
    rotation.active = false;
    stopAll();
  }

  // --- L2/R2: analog trigger intensity ---
  if (anyTrigger && !anyButton && !rotation.active) {
    processL2R2(l2, r2);
  } else if (!anyTrigger && !anyButton && !anyDpad && !anyLeftStick && !anyRightStick && !rotation.active) {
    // nothing pressed → stop
    if (!wave.active) stopAll();
  }

  // --- D-pad: directional waves ---
  if (anyDpad && !anyButton && !rotation.active) {
    processDpad(dUp, dDown, dLeft, dRight);
  } else if (!anyDpad && wave.active && !anyLeftStick && !anyButton) {
    // d-pad released and no stick → stop wave
    stopWave();
  }

  // --- Left stick: directional wave (lower priority than buttons/dpad) ---
  if (!anyButton && !anyDpad && !rotation.active && !anyTrigger) {
    processLeftStick(lx, ly);
  }

  // --- Right stick: cursor (lowest priority) ---
  if (!anyButton && !anyDpad && !rotation.active && !anyTrigger && !anyLeftStick) {
    if (anyRightStick) {
      processRightStick(rx, ry);
    }
  }

  // Tick continuous modes
  processHeartbeat();
  processScatter();
  processBreathe();
  tickWave();
  tickRotation();
}

// -------------------- Setup & Loop --------------------
void setup() {
  Serial.begin(115200);
  Serial.println("Vibe Belt ESP32 + PS5 starting...");

  Wire.begin(SDA_PIN, SCL_PIN);

  BP32.setup(&onConnected, &onDisconnected);
  BP32.forgetBluetoothKeys();

  Serial.println("Ready. Put PS5 controller in pairing mode (PS + Create).");
}

void loop() {
  bool ok = BP32.update();
  if (ok) {
    processController();
  }
  delay(1);  // minimal yield for watchdog
}
