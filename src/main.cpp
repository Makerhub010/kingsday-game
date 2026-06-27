#include <Arduino.h>

// ===== Kraak de Kluis / King's Day Heist - Raspberry Pi Pico =====
// Keypad (4x4) + AM312 PIR + passive piezo + 8x8 MAX7219 matrix (via MD_Parola)
// + 4 mini-game target sensors, each reveals one digit of the code.
//
// Targets:
//   Digit 1 -> SW-18010P vibration sensor (GP26)  - hit with ball
//   Digit 2 -> Limit switch              (GP27)  - press lever
//   Digit 3 -> IR proximity sensor       (GP28)  - bring object near
//   Digit 4 -> Light slot 10mm sensor    (GP18)  - drop ball through slot
//
// Matrix scheme:
//   Game start  -> 3 lives shown briefly (small X layout)
//   Red light   -> steady big X (alarm active, do not walk)
//   Green light -> off (move)
//   Caught      -> big X blinks 3x, then remaining lives as small X's
//   Mini-game hit -> shows that digit of the code (2s)
//   Game over   -> full blink, then "GAME OVER" scrolls
//   Win         -> checkmark
//
// Goal: hit the 4 targets to learn the code, enter code + DISARM to win,
//       before the red-light motion detector drains all your lives.

#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

// ---------- Keypad ----------
const int ROWS = 4, COLS = 4;
const int rowPins[ROWS] = {9, 2, 6, 14};
const int colPins[COLS] = {5, 7, 13, 11};
const char* keys[ROWS][COLS] = {
  { "1",   "2", "3",      "NONE"    },
  { "4",   "5", "6",      "NONE"    },
  { "7",   "8", "9",      "PROGRAM" },
  { "ARM", "0", "DISARM", "PANIC"   }
};
bool lastState[ROWS][COLS] = {false};

// ---------- PIR & Piezo ----------
const int PIR_PIN   = 15;
const int PIEZO_PIN = 22;

// ---------- Target sensors (one per mini-game, reveals one digit) ----------
// index 0 -> digit 1 (vibration), 1 -> digit 2 (limit), 2 -> digit 3 (IR prox), 3 -> digit 4 (light slot)
const int NUM_SENSORS = 4;
const int sensorPins[NUM_SENSORS] = {26, 27, 28, 18};
bool sensorLast[NUM_SENSORS] = {true, true, true, true};   // previous reading (HIGH = idle)
unsigned long lastHit[NUM_SENSORS] = {0, 0, 0, 0};
const unsigned long HIT_COOLDOWN_MS = 1500;

// ---------- LED Matrix (all via MD_Parola) ----------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 1
#define DIN_PIN 19
#define CLK_PIN 21
#define CS_PIN  20
MD_Parola P = MD_Parola(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
MD_MAX72XX *mx = nullptr;

// ---------- Secret code ----------
const char* SECRET_CODE = "1234";
char  entered[16];
int   enteredLen = 0;

// ---------- Game state ----------
enum GameState { IDLE, GREEN, RED, GAMEOVER, WIN };
GameState state = IDLE;
unsigned long phaseStart = 0;
unsigned long phaseDuration = 0;

int lives = 3;
const int START_LIVES = 3;
const unsigned long RED_GRACE_MS = 800;

// ---------- Bitmaps (column-format: one byte per column, LSB = top row) ----------
const uint8_t XMARK_COL[8] = {
  0b10000001,
  0b01000010,
  0b00100100,
  0b00011000,
  0b00011000,
  0b00100100,
  0b01000010,
  0b10000001
};

const uint8_t CHECK_COL[8] = {
  0b00000000,
  0b00000000,
  0b01000000,
  0b10000000,
  0b01000001,
  0b00100010,
  0b00010100,
  0b00001000
};

const uint8_t MINI_X_COL[3] = {
  0b101,
  0b010,
  0b101
};

// 5-wide x 7-tall digits, column-format (one byte per column, LSB = top row)
const uint8_t DIGIT_COL[10][5] = {
  {0b0111110,0b1000001,0b1000001,0b1000001,0b0111110}, // 0
  {0b0000000,0b0000010,0b1111111,0b0000000,0b0000000}, // 1
  {0b1000010,0b1100001,0b1010001,0b1001001,0b1000110}, // 2
  {0b0100001,0b1000001,0b1000101,0b1001011,0b0110001}, // 3
  {0b0011000,0b0010100,0b0010010,0b1111111,0b0010000}, // 4
  {0b0100111,0b1000101,0b1000101,0b1000101,0b0111001}, // 5
  {0b0111110,0b1001001,0b1001001,0b1001001,0b0110010}, // 6
  {0b0000001,0b1110001,0b0001001,0b0000101,0b0000011}, // 7
  {0b0110110,0b1001001,0b1001001,0b1001001,0b0110110}, // 8
  {0b0100110,0b1001001,0b1001001,0b1001001,0b0111110}  // 9
};

// ---------- Low-level matrix drawing through Parola's MD_MAX72XX ----------
void displayClearRaw() {
  mx->clear();
  mx->update();
}

void displayBitmapCol(const uint8_t col[8]) {
  mx->clear();
  for (uint8_t c = 0; c < 8; c++)
    mx->setColumn(c, col[c]);
  mx->update();
}

void displayAllOn() {
  mx->clear();
  for (uint8_t c = 0; c < 8; c++) mx->setColumn(c, 0xFF);
  mx->update();
}

void drawMiniXAt(int ox, int oy) {
  for (uint8_t c = 0; c < 3; c++)
    for (uint8_t r = 0; r < 3; r++)
      if ((MINI_X_COL[c] >> r) & 1) {
        int x = ox + c, y = oy + r;
        if (x >= 0 && x < 8 && y >= 0 && y < 8)
          mx->setPoint(y, x, true);
      }
}

void displayLives(int n) {
  mx->clear();
  const int posX[3] = {0, 3, 5};
  const int posY[3] = {0, 3, 5};
  for (int i = 0; i < n && i < 3; i++)
    drawMiniXAt(posX[i], posY[i]);
  mx->update();
}

void displayDigit(int d) {
  if (d < 0 || d > 9) return;
  mx->clear();
  for (uint8_t c = 0; c < 5; c++)
    mx->setColumn(c + 1, DIGIT_COL[d][c]);   // +1 nudges it right
  mx->update();
}

void blinkBigX(int times) {
  for (int i = 0; i < times; i++) {
    displayBitmapCol(XMARK_COL);
    delay(250);
    displayClearRaw();
    delay(200);
  }
}

void blinkFull(int times) {
  for (int i = 0; i < times; i++) {
    displayAllOn();
    delay(250);
    displayClearRaw();
    delay(200);
  }
}

void scrollGameOver() {
  P.displayClear();
  P.displayText("GAME OVER", PA_CENTER, 80, 800, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  while (!P.displayAnimate()) { /* scroll until done */ }
  P.displayReset();
  P.displayClear();
}

// ---------- Sound helpers ----------
void beep(int freq, int ms) { tone(PIEZO_PIN, freq, ms); delay(ms); noTone(PIEZO_PIN); }
void soundGreen()    { beep(880, 150); }
void soundRed()      { beep(300, 400); }
void soundLifeLost() { beep(200, 250); beep(150, 350); }
void soundGameOver() { for (int i = 0; i < 6; i++) { beep(1000, 150); beep(1500, 150); } }
void soundStart()    { beep(660, 100); beep(880, 100); beep(1100, 150); }
void soundWin()      { beep(880,120); beep(1100,120); beep(1320,120); beep(1760,300); }
void soundKey()      { beep(1200, 40); }
void soundBadCode()  { beep(180, 200); beep(140, 300); }
void soundReveal()   { beep(1320, 80); beep(1660, 120); }

// ---------- Keypad scan ----------
const char* scanKeypad() {
  const char* result = NULL;
  for (int r = 0; r < ROWS; r++) {
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(50);
    for (int c = 0; c < COLS; c++) {
      bool pressed = (digitalRead(colPins[c]) == LOW);
      if (pressed && !lastState[r][c]) result = keys[r][c];
      lastState[r][c] = pressed;
    }
    digitalWrite(rowPins[r], HIGH);
  }
  return result;
}

bool motionDetected() { return digitalRead(PIR_PIN) == HIGH; }

void printLives() {
  Serial.print("Lives: ");
  for (int i = 0; i < lives; i++) Serial.print("X ");
  Serial.println();
}

void resetCode() { enteredLen = 0; entered[0] = '\0'; }
bool isDigit(const char* k) { return strlen(k) == 1 && k[0] >= '0' && k[0] <= '9'; }

// ---------- Mini-game digit reveal ----------
void revealDigit(int idx) {
  if (idx < 0 || idx >= (int)strlen(SECRET_CODE)) return;
  char ch = SECRET_CODE[idx];
  int d = ch - '0';
  Serial.print(">>> TARGET ");
  Serial.print(idx + 1);
  Serial.print(" HIT - code digit ");
  Serial.print(idx + 1);
  Serial.print(" is: ");
  Serial.println(ch);

  soundReveal();
  displayDigit(d);
  delay(2000);                 // show the digit for 2s

  // Restore the display appropriate to the current state
  if (state == RED) displayBitmapCol(XMARK_COL);
  else              displayClearRaw();
}

// Edge-detected sensor check: fires once on the HIGH->LOW transition (active LOW),
// so sensors that rest in the triggered state (slot/proximity) reveal only once.
void checkSensors() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    bool reading = (digitalRead(sensorPins[i]) == LOW);   // true = triggered
    if (reading && !sensorLast[i]) {                      // new trigger edge
      if (millis() - lastHit[i] > HIT_COOLDOWN_MS) {
        lastHit[i] = millis();
        revealDigit(i);
      }
    }
    sensorLast[i] = reading;
  }
}

void setup() {
  Serial.begin(115200);
  for (int r = 0; r < ROWS; r++) { pinMode(rowPins[r], OUTPUT); digitalWrite(rowPins[r], HIGH); }
  for (int c = 0; c < COLS; c++) pinMode(colPins[c], INPUT_PULLUP);
  pinMode(PIR_PIN, INPUT);
  pinMode(PIEZO_PIN, OUTPUT);
  for (int i = 0; i < NUM_SENSORS; i++) pinMode(sensorPins[i], INPUT_PULLUP);

  P.begin();
  P.setIntensity(2);
  P.displayClear();
  mx = P.getGraphicObject();

  Serial.println("PIR warming up (~5s)... then press ARM to start.");
  delay(5000);
  Serial.println("Ready. Press ARM to start. Hit the 4 targets, then enter code + DISARM!");
}

void startGreen() {
  state = GREEN;
  phaseStart = millis();
  phaseDuration = random(2000, 5000);
  Serial.println(">>> GREEN LIGHT - move & play!");
  displayClearRaw();
  soundGreen();
}

void startRed() {
  state = RED;
  phaseStart = millis();
  phaseDuration = random(2500, 4500);
  Serial.println(">>> RED LIGHT - freeze! (alarm active)");
  displayBitmapCol(XMARK_COL);
  soundRed();
}

void startGame() {
  Serial.println("=== NEW GAME ===");
  lives = START_LIVES;
  resetCode();
  printLives();
  displayLives(lives);
  soundStart();
  // sync sensor baseline so a resting-triggered sensor doesn't fire immediately
  for (int i = 0; i < NUM_SENSORS; i++)
    sensorLast[i] = (digitalRead(sensorPins[i]) == LOW);
  delay(1500);
  startGreen();
}

void winGame() {
  Serial.println("=== CODE CORRECT - PLAYERS WIN! ===");
  state = WIN;
  displayBitmapCol(CHECK_COL);
  soundWin();
}

void gameOver() {
  Serial.println("=== GAME OVER - no lives left ===");
  state = GAMEOVER;
  blinkFull(4);
  soundGameOver();
  scrollGameOver();
  displayClearRaw();
}

void loop() {
  const char* key = scanKeypad();

  // Mini-game targets active during play
  if (state == GREEN || state == RED) checkSensors();

  if (key) {
    if (strcmp(key, "ARM") == 0 && (state == IDLE || state == GAMEOVER || state == WIN)) {
      startGame();
      return;
    }

    if (strcmp(key, "PANIC") == 0) {
      Serial.println("!!! MANUAL ALARM !!!");
      soundGameOver();
      return;
    }

    if (strcmp(key, "DISARM") == 0) {
      if (state == GREEN || state == RED) {
        if (strcmp(entered, SECRET_CODE) == 0) {
          winGame();
        } else {
          Serial.print("Wrong code submitted: "); Serial.println(entered);
          soundBadCode();
          resetCode();
        }
      } else {
        state = IDLE; noTone(PIEZO_PIN); resetCode();
        displayClearRaw();
        Serial.println("=== RESET to IDLE ===");
      }
      return;
    }

    if ((state == GREEN || state == RED) && isDigit(key)) {
      if (enteredLen < (int)sizeof(entered) - 1) {
        entered[enteredLen++] = key[0];
        entered[enteredLen] = '\0';
      }
      soundKey();
      Serial.print("Code so far: "); Serial.println(entered);
    }
  }

  switch (state) {
    case GREEN:
      if (millis() - phaseStart > phaseDuration) startRed();
      break;

    case RED: {
      unsigned long elapsed = millis() - phaseStart;
      if (elapsed > RED_GRACE_MS && motionDetected()) {
        lives--;
        Serial.println("!!! MOTION DURING RED LIGHT - CAUGHT !!!");
        blinkBigX(3);
        if (lives > 0) {
          Serial.print("Life lost! "); printLives();
          soundLifeLost();
          displayLives(lives);
          delay(1500);
          delay(500);
          startRed();
        } else {
          gameOver();
        }
      } else if (elapsed > phaseDuration) {
        startGreen();
      }
      break;
    }

    case GAMEOVER:
    case WIN:
    case IDLE:
      break;
  }

  delay(20);
}