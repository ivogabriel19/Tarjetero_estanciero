# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RFID access-control firmware for an ESP32. The project is being restarted around a PN532 NFC reader and a TFT touch display. Identifiers, comments and Serial messages are in Spanish; keep new code consistent with that.

The hardware test sketches live in `Tests_HW/`:

- `Tests_HW/Prueba_PN532/Prueba_PN532.ino`: basic PN532 test over I2C, verified on the hardware (it reads a 4-byte MIFARE keyfob). It prints the reader's firmware version and the UID of each card. Wiring: VCC→3.3V, GND→GND, SDA→21, SCL→22, with the module's DIP switches set to I2C (1 ON, 2 OFF). IRQ and RSTO are not connected. The user chose I2C over SPI, so keep the PN532 on I2C.
  - It speaks the PN532 frame protocol directly with `Wire`, with no library. Adafruit_PN532 1.3.4 over I2C found the chip once and then failed every time on this setup. The library polls a 1-byte status and immediately does a second read for the frame. This sketch reads the status and the frame in one transaction and leaves 5 ms between transactions.
  - The ESP32 core 3.x I2C driver logs an error line for every NACK. The PN532 NACKs while busy, so the sketches silence the `i2c.master` log tag.
  - From the factory, the PN532 searches for a card forever (MxRtyPassiveActivation 0xFF) and won't answer anything else until it finds one. Resetting only the ESP32 leaves it in that state, so the sketch limits the retries and sends an ACK frame at boot to abort any leftover command.
- `Tests_HW/Escaner_I2C/Escaner_I2C.ino`: I2C bus scanner. It also reports the idle SDA/SCL levels. The PN532 appears at 0x24.
- `Tests_HW/Prueba_Display/Prueba_Display.ino`: display and touch test, verified on the hardware.
  - At boot it prints the ILI9341 diagnostic registers and runs the visual tests once: colors, bars, text, shapes, the 4 rotations and fill speed.
  - Then it stays in a touch test: you draw with your finger, and a "BORRAR" button clears the screen.
- `Tests_HW/Prueba_PN532_Display/Prueba_PN532_Display.ino`: the PN532 code plus the display and touch together. It shows the reader status and each card's UID. Touching the screen draws a dot and shows the coordinates in a bottom bar.
  - The reader and the display are verified on the hardware. The touch was added afterwards, with the same settings that were verified in `Prueba_Display`.

### Display and touch (both display sketches)

- Hardware: a red-PCB "TFT SPI 240x320 V1.2" with an ILI9341 controller and an XPT2046 touch chip.
- Wiring: display CS→5, DC→4, RESET→15, SDI/MOSI→23, SCK→18, SDO/MISO→19, VCC and LED→3.3V. The touch shares the SPI bus: T_CLK→18, T_DIN→23, T_DO→19, T_CS→16. T_IRQ is not connected, so the library polls the pressure over SPI. The SD pins are not connected.
- SPI runs at 10 MHz because the library's ESP32 default (40 MHz) is too fast for breadboard wires.
- T_CS is driven HIGH before `tft.begin()` so the touch chip stays off the bus while the display is set up.
- Orientation, confirmed on the hardware: as the display is mounted, image rotation 3 (landscape) reads upright and rotation 1 is upside down. This is the `ROTACION` constant.
- The touch panel is mounted 180° from what the XPT2046 library assumes, so the touch uses `ROTACION_TOUCH = (ROTACION + 2) % 4`, which is 1.
- Raw-to-screen mapping uses `TOUCH_X/Y_MIN/MAX` (200/3700/240/3800, for touch rotation 1). These values work on this board. To fine-tune them, use the "Touch crudo" values printed over Serial.
- The built-in GFX font has no UTF-8 support, so on-screen text has no accents.

### Earlier version (RC522)

`Control_acceso-ESP32/Control_acceso-ESP32.ino` is the earlier version, which uses an MFRC522 (RC522) reader and drives a door-lock relay. The two "(RC522 sketch)" sections below describe it.

Wiring (RC522 → ESP32), from `Conexiones RC522-ESP32.jpg`: VCC→3.3V, GND→GND, RST→22, SDA/SS→21, MOSI→23, MISO→19, SCK→18. The relay is on GPIO 17 (HIGH = unlocked).

## Build / upload

There are no automated tests and no linter. The `Tests_HW/` sketches are run by hand on the hardware, and the user pastes the Serial Monitor output back. The project is developed in the Arduino IDE, with the ESP32 core `esp32:esp32` 3.3.x, Adafruit ILI9341 1.7.0 + Adafruit GFX 1.13.0, XPT2046_Touchscreen 1.4.0, Adafruit_PN532 1.3.4 (installed but no longer used) and the MFRC522 library (miguelbalboa) 1.4.12. Arduino requires each `.ino` to sit in a folder with the same name. arduino-cli is not on PATH, but the IDE bundles it at `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`:

```
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_PN532
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_PN532_Display
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_Display
arduino-cli compile --fqbn esp32:esp32:esp32 Control_acceso-ESP32
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COMx Control_acceso-ESP32
arduino-cli monitor -p COMx -c baudrate=115200
```

## How it works (RC522 sketch)

- `loop()` is a small state machine driven by card taps. Tapping a master card (`altaMaster` or `bajaMaster`, hard-coded 4-byte UIDs) arms a one-shot mode (`modoAlta` or `modoBaja`). The *next* card tapped is registered or removed, and then the mode clears. Any other card opens the lock if it is registered.
- Registered cards are stored in EEPROM in 4-byte slots across addresses 0–511 (128 slots). A slot whose first byte is `0xFF` is free, and deleting a card writes `0xFF` back.
- `abrirCerradura()` blocks for 5 s with `delay(5000)` while the relay is on, so no cards are read during that time.

## Known pitfalls (RC522 sketch)

- On the ESP32, EEPROM is emulated in flash. It needs `EEPROM.begin(size)` in `setup()` and `EEPROM.commit()` after writes. The sketch currently calls neither. Without `begin()`, the ESP32 core's `EEPROM.read()` returns 0 and `EEPROM.write()` does nothing, so registering a card always reports "No hay espacio" and nothing is saved.
- The ESP32 `SPI.begin()` arguments are `(SCK, MISO, MOSI, SS)`. The inline comment in `setup()` lists them in the wrong order, but the pin values themselves are correct.
- The storage and comparison logic assumes 4-byte UIDs. `compararUID` checks only 4 bytes, but the EEPROM read/write loops use `rfid.uid.size`, so a card with a 7-byte UID would spill into the next slot.
- `bajaMaster` is still the placeholder `FF FF FF FF` and must be set to a real card UID.
