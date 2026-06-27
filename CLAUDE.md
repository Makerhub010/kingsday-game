# Project: Kraak de Kluis / King's Day Heist - Raspberry Pi Pico

## Overview
A physical multiplayer "Red Light, Green Light" game built on a Raspberry Pi Pico
using PlatformIO (Arduino framework, mbed core). Salvaged parts from a Dunway
security alarm (keypad + PIR) plus new peripherals. The main code is in src/main.cpp
and is already working. Please read it first.

## Hardware & Pin Map
Controller: Raspberry Pi Pico

Keypad: 4x4 matrix (salvaged Dunway alarm membrane), discovered layout:
  Row pins (GP): 9, 2, 6, 14
  Col pins (GP): 5, 7, 13, 11
  Layout:
    1 2 3 NONE
    4 5 6 NONE
    7 8 9 PROGRAM
    ARM 0 DISARM PANIC

PIR motion sensor (AM312-style, 3.3V): GP15
Passive piezo buzzer: GP22
LED matrix: 8x8 MAX7219, driven via MD_Parola library
  DIN=GP19, CS=GP20, CLK=GP21, VCC=5V(VBUS)
  All drawing goes through P.getGraphicObject() (single MD_MAX72XX object) to avoid
  SPI handoff issues. Bitmaps are COLUMN-format (one byte per column, LSB = top row).

Four mini-game target sensors, each reveals ONE digit of the secret code (active LOW,
edge-detected so they fire once per action):
  Digit 1 -> SW-18010P vibration sensor  -> GP26 (2-pin: GPIO + GND)
  Digit 2 -> Limit switch                -> GP27 (2-pin: C->GND, NO->GP27)
  Digit 3 -> IR proximity sensor         -> GP28 (3-pin: VCC/GND/OUT)
  Digit 4 -> Light slot 10mm sensor      -> GP18 (3-pin: VCC/GND/OUT)

## Game Logic
- Press ARM to start a game (3 lives).
- Light alternates GREEN (move, matrix off) and RED (freeze, steady big X on matrix),
  random durations.
- During RED, motion on the PIR (after an 800ms grace period) costs a life: big X
  blinks 3x, then remaining lives shown as small X glyphs. 0 lives -> GAME OVER
  (full blink, then "GAME OVER" scrolls via Parola).
- Players hit the 4 physical targets to reveal each digit of the code (digit shows
  on matrix ~2s + Serial). SECRET_CODE maps position->target (default "1234": target 1
  reveals digit 1, etc.).
- To win: enter the code on the keypad, then press DISARM. Correct -> WIN (checkmark).
  Wrong -> buzz + clear, keep playing.
- PANIC = manual alarm anytime. DISARM outside play = reset to idle.

## Matrix Display Scheme
- Game start: 3 lives (small X layout) ~1.5s
- Green: off
- Red: steady big X
- Caught: big X blinks 3x, then lives as small X's
- Target hit: that digit (2s)
- Game over: full blink x4, then scrolling "GAME OVER"
- Win: checkmark

## Libraries (platformio.ini)
  majicdesigns/MD_MAX72XX
  majicdesigns/MD_Parola

## Known Notes / Constraints
- mbed Pico core rejects old B10101010 binary-literal macros (this is why LedControl
  failed and we moved to MD_MAX72XX/MD_Parola). Avoid libraries using those.
- Piezo is on GP22 (moved from GP16).
- Several reveal/lose sequences use blocking delay(), which pauses PIR watching during
  that window. This is currently acceptable but could be made non-blocking.

## Open Design Decisions (TODO)
1. Win screen: replace the checkmark bitmap with a scrolling "WIN" + smiley face,
   to match the "GAME OVER" treatment. (Checkmark bitmap is rough in column format.)
2. Optionally make digit-reveal and life-loss sequences non-blocking so red-light
   danger continues during them.
3. Verify IR proximity / light slot output polarity on the actual modules; flip per-
   sensor logic if either reads active HIGH instead of active LOW.

## First Task
Please read src/main.cpp, confirm your understanding of the current state, and then
[describe whatever you want to do next here].