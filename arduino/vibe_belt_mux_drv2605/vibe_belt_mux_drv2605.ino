/*
 *  Vibe Belt — Leonardo / Pro Micro + 2×PCA9548A + 14×DRV2605
 *  Serial controlled, RTP mode (real-time proportional intensity)
 *
 *  Motor layout matches ESP32 version:
 *    075  071  ---  173  172    row 0  (idx 0-3, gap at col 2)
 *    076  072  070  174  171    row 1  (idx 4-8)
 *    077  073  176  175  170    row 2  (idx 9-13)
 *
 *  Protocol (115200 baud, line-based):
 *    PING                  → PONG
 *    STOP                  → OK
 *    HIT <0-13> [rtp]      → OK <idx>         single motor, optional intensity 0-127
 *    MULTI <bitmask> [rtp] → OK                fire multiple motors at once
 *    RTP <idx> <0-127>     → OK                set one motor to sustained RTP level
 *    ALLOFF                → OK                zero all motors
 */

#include <Wire.h>
#include <Adafruit_DRV2605.h>

Adafruit_DRV2605 drv;

/* ═══ Hardware ═══ */
#define MUX_A  0x70
#define MUX_B  0x71

struct Motor {
  uint8_t mux;
  uint8_t ch;
};

// Physical layout — order matches grid (row-major, skip gap)
static const Motor MOTORS[] = {
  {MUX_A, 5}, {MUX_A, 1},                {MUX_B, 3}, {MUX_B, 2},  // row 0 (4 motors)
  {MUX_A, 6}, {MUX_A, 2}, {MUX_A, 0},   {MUX_B, 4}, {MUX_B, 1},  // row 1 (5 motors)
  {MUX_A, 7}, {MUX_A, 3}, {MUX_B, 6},   {MUX_B, 5}, {MUX_B, 0},  // row 2 (5 motors)
};
static const uint8_t N = 14;

// Grid position for each motor index (row, col) — for reference
// idx:  0     1     2     3     4     5     6     7     8     9    10    11    12    13
// pos: 0,0  0,1  0,3  0,4  1,0  1,1  1,2  1,3  1,4  2,0  2,1  2,2  2,3  2,4

/* ═══ Low-level ═══ */

static void muxSelect(uint8_t addr, uint8_t ch) {
  Wire.beginTransmission(addr);
  Wire.write(1 << ch);
  Wire.endTransmission();
}

static bool drvInit() {
  if (!drv.begin()) return false;
  drv.setMode(DRV2605_MODE_REALTIME);
  drv.useERM();
  return true;
}

static bool setMotorRTP(uint8_t idx, uint8_t rtp) {
  if (idx >= N) return false;
  muxSelect(MOTORS[idx].mux, MOTORS[idx].ch);
  if (!drvInit()) return false;
  drv.setRealtimeValue(rtp);
  return true;
}

static void allOff() {
  for (uint8_t i = 0; i < N; i++) {
    setMotorRTP(i, 0);
  }
}

/* ═══ Command handler ═══ */

static void handleCmd(String line) {
  line.trim();
  if (line.length() == 0) return;

  // PING
  if (line.equalsIgnoreCase("PING")) {
    Serial.println("PONG");
    return;
  }

  // STOP / ALLOFF
  if (line.equalsIgnoreCase("STOP") || line.equalsIgnoreCase("ALLOFF")) {
    allOff();
    Serial.println("OK");
    return;
  }

  // HIT <idx> [rtp]   — default rtp=100
  if (line.startsWith("HIT ") || line.startsWith("hit ")) {
    String args = line.substring(4);
    args.trim();
    int sp = args.indexOf(' ');
    int idx;
    uint8_t rtp = 100;
    if (sp > 0) {
      idx = args.substring(0, sp).toInt();
      rtp = constrain(args.substring(sp + 1).toInt(), 0, 127);
    } else {
      idx = args.toInt();
    }
    if (idx < 0 || idx >= N) { Serial.println("ERR"); return; }
    setMotorRTP(idx, rtp);
    Serial.print("OK "); Serial.println(idx);
    return;
  }

  // RTP <idx> <value>
  if (line.startsWith("RTP ") || line.startsWith("rtp ")) {
    String args = line.substring(4);
    args.trim();
    int sp = args.indexOf(' ');
    if (sp <= 0) { Serial.println("ERR"); return; }
    int idx = args.substring(0, sp).toInt();
    int val = args.substring(sp + 1).toInt();
    if (idx < 0 || idx >= N) { Serial.println("ERR"); return; }
    setMotorRTP(idx, constrain(val, 0, 127));
    Serial.print("OK "); Serial.println(idx);
    return;
  }

  // MULTI <bitmask> [rtp]  — bitmask is 14-bit, each bit = one motor
  if (line.startsWith("MULTI ") || line.startsWith("multi ")) {
    String args = line.substring(6);
    args.trim();
    int sp = args.indexOf(' ');
    uint16_t mask;
    uint8_t rtp = 100;
    if (sp > 0) {
      mask = (uint16_t)strtoul(args.substring(0, sp).c_str(), NULL, 10);
      rtp  = constrain(args.substring(sp + 1).toInt(), 0, 127);
    } else {
      mask = (uint16_t)strtoul(args.c_str(), NULL, 10);
    }
    for (uint8_t i = 0; i < N; i++) {
      if (mask & (1 << i)) {
        setMotorRTP(i, rtp);
      }
    }
    Serial.println("OK");
    return;
  }

  Serial.print("ERR "); Serial.println(line);
}

/* ═══ Arduino entry ═══ */

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Wire.begin();
  delay(500);
  Serial.println("READY");
}

void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCmd(line);
  }
}
