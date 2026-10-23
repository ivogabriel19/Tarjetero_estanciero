# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

This repo holds two ESP32 projects that share the PN532 NFC reader code:

- `Terminal_Estanciero`: a "debit card" bank terminal for the board game Estanciero (an Argentine Monopoly), with a TFT touch display. It replaces the paper money.
- `Control_Acceso`: NFC access control with a 16x2 I2C LCD and a lock relay. It was started from scratch and replaces the earlier RC522 version, which is kept below.

Identifiers, comments and Serial messages are in Spanish; keep new code consistent with that.

### Control_Acceso

`Control_Acceso/Control_Acceso.ino` uses the same PN532 code as `Tests_HW/` and the LCD code from `Tests_HW/Prueba_LCD`. It compiles, but it has not been run on the hardware yet.

- There is no keypad. Users only tap cards or other NFC devices. More complex settings will be made from a phone over WiFi or Bluetooth, which is not chosen or built yet. Until then, Serial commands stand in for it: `lista`, `nombre N texto`, `borrar N`, `abrir` (the monitor must send "Nueva línea").
- Enabled cards are kept in NVS (`Preferences`, namespace `acceso`, key `datos`) as one `Datos` struct, saved after every change. Bump `VERSION_DATOS` whenever that struct changes.
- With no cards stored, the first card tapped becomes the master card (`ROL_MAESTRA`). An enabled card opens the relay for `TIEMPO_APERTURA`. The master card does not open: each tap cycles the mode NORMAL → ALTA (each new card tapped is enabled) → BAJA (each card tapped is disabled) → NORMAL. ALTA and BAJA return to NORMAL after `TIEMPO_ADMIN` without cards. The master cannot be removed with `borrar`.
- Holding the board's BOOT button (GPIO 0) for `TIEMPO_BORRADO` while it runs erases every card, master included. Holding BOOT at reset enters download mode instead.
- A 4-byte UID starting with 0x08 is random (ISO 14443-3). Phones and bank cards send one, and it changes on every read, so those are rejected and never stored. Supporting phones would need an Android HCE app answering an APDU, not the UID.
- `loop()` never blocks, so the phone server can be added later. The reader and the LCD are retried every `INTERVALO_RECONEXION` if they stop answering. `mostrar()` caches both LCD lines and rewrites only the ones that changed.
- Relay on GPIO 17, `RELE_ACTIVO` HIGH (many relay modules are active LOW). Not confirmed on the hardware.
- Names are at most 16 letters (one LCD line). `copiarNombre()` strips accents from UTF-8 input because the LCD shows ASCII only.

### Terminal_Estanciero

`Terminal_Estanciero/Terminal_Estanciero.ino` reuses the PN532 code and the display and touch settings from `Tests_HW/` unchanged. It compiles, but it has not been run on the hardware yet.

- Each card only identifies a player. Balances are kept in the ESP32's NVS (`Preferences`, namespace `estanciero`, key `partida`) as one `Partida` struct, which is saved after every change. Bump `VERSION_PARTIDA` whenever that struct changes. The bank has unlimited money.
- At boot, `elegirPartida()` asks whether to continue the saved game or start a new one. If there is no saved game, it goes straight to registration.
- `nuevaPartida()` registers the cards. Each new card opens an on-screen letter keyboard (`pedirNombre`, at most `LARGO_NOMBRE` letters, capitalized automatically). The name starts as the one that card had in the previous game, or as the color name, and the first key pressed replaces it. Tapping an already registered card again lets you rename it. Registration edits `partida` in memory and saves only on EMPEZAR. CANCELAR restores the previous game, and it appears only when there is a previous game to go back to.
- Each player's color comes from their slot (`FICHAS`: Rojo, Azul, ...), in the order their cards were registered. The name is stored in `Jugador.nombre`, and an empty name falls back to the color name.
- `loop()` is the main menu: two big buttons, COBRAR SALIDA and TRANSACCIONES, and two small ones, SALDOS (bottom left, which also has NUEVA PARTIDA) and DESHACER (bottom right). TRANSACCIONES opens SUMAR (the bank pays), QUITAR (the player pays the bank) and TRANSFERIR. Cancelling one of those goes back to the TRANSACCIONES screen, and finishing one goes back to the menu. Each operation is a blocking function: keypad (`pedirMonto`), then card (`esperarJugador`), then result screen. Tapping a card on the main menu shows its balance.
- A payment is rejected if the payer's balance is too low. The last `MAX_HISTORIAL` operations are kept so they can be undone one at a time, newest first.
- `tarjetaNueva()` reports a card only when it arrives, not on every poll while it rests on the reader. The card counts as removed after `LECTURAS_PARA_RETIRAR` polls in a row without it. `olvidarTarjeta()` makes a card that is already on the reader count again.
- `DINERO_INICIAL` and `MONTO_SALIDA` are placeholders until the user checks them against their edition's rulebook.

### Hardware tests

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
- `Tests_HW/Prueba_LCD/Prueba_LCD.ino`: 16x2 LCD test, verified on the hardware with the adapter at 5V through a level shifter. It lists the I2C bus, finds the LCD adapter, then reads the HD44780 cursor position back after writing each line (expected 0x10 and 0x50), which checks the adapter's pin mapping. After that it runs the visual tests (backlight blink, all pixels, characters, custom symbols) and then shows the uptime.
- `Tests_HW/Prueba_LCD_20x4/Prueba_LCD_20x4.ino`: 20x4 LCD test, not run on the hardware yet. It is kept separate from the 16x2 test and uses the same adapter, wiring and driver. Row start addresses in DDRAM are 0x00, 0x40, 0x14 and 0x54, because rows 3 and 4 continue rows 1 and 2. The cursor-position check expects 0x0A, 0x4A, 0x1E and 0x5E after 10 letters per row.
  - Then it runs the demos: a frame drawn with custom corners, the ROM character set (the E0 row tells ROM A00 from A02), 8 icons, 3x2 big digits, a 1-pixel-step progress bar, a 4-row equalizer that measures full-screen redraw time, two marquees, the hardware cursor, and a Pac-Man animation that redefines a CGRAM glyph in place. It ends on a big-digit uptime clock.
  - Each screen loads its own custom glyphs, because CGRAM holds only 8. Any screen that uses custom glyphs writes raw byte buffers through `escribirBytesEn()`, so slot 0 is usable there. The Arduino prototype generator fails on a `const uint8_t (*x)[8]` parameter, so `definirSimbolos()` takes `const uint8_t x[][8]` instead.
- `Tests_HW/Prueba_PN532_LCD/Prueba_PN532_LCD.ino`: the PN532 and the LCD sharing the I2C bus, not run on the hardware yet. It shows the card type (from the SAK), the UID length and the UID on the LCD, and it flags random UIDs.

### LCD 16x2 and 20x4 (Control_Acceso and the LCD tests)

- Hardware: a classic 1602 LCD (HD44780) with a PCF8574 I2C adapter, on the same bus as the PN532 (SDA→21, SCL→22). VCC→5V (VIN), GND→GND.
- It is driven directly with `Wire`, with no library, the same way as the PN532. The adapter pins are assumed to be the common mapping: P0=RS, P1=RW, P2=E, P3=backlight, P4–P7=D4–D7. Each `enviarLcd()` call sends up to 31 bytes in one I2C transaction.
- The address is found at boot from `DIRECCIONES_LCD`: 0x27 first (PCF8574), then 0x3F (PCF8574A), then the rest of both ranges. 0x24 is left out because it is the PN532.
- The bus stays at the 100 kHz default because the PCF8574 does not support more.
- Custom symbols live in CGRAM slots 1–4, because slot 0 would end a C string: `SIMBOLO_CERRADO`, `SIMBOLO_ABIERTO`, `SIMBOLO_SI`, `SIMBOLO_NO`. Concatenate them as separate string literals (`SIMBOLO_SI " Texto"`), because a hex escape followed by a hex letter would swallow it.
- The user's adapter is the common black "MH" board with a PCF8574T (not the A version). The A0–A2 pads are open, so it should be at 0x27. It has the standard pin mapping above, and a jumper that has to stay on for the backlight.
- Voltage: on that board, R8 and R9 (472 = 4.7 kΩ) should be the SDA/SCL pull-ups to the adapter's VCC. Not measured yet. With VCC at 5V they put 5V on ESP32 pins rated for 3.3V, and on the PN532 too. With the adapter at 3.3V the LCD works, but the text is very faint even after adjusting the contrast. So the LCD goes through a level shifter, with the adapter at 5V, and the PN532 stays wired straight to the ESP32 on the 3.3V side. The code does not change.
  - It worked on the hardware with a TXS0108E board (blue, "YF08E"). Its wiring: VA→3.3V, VB→5V, OE→3.3V (with OE floating, the outputs are off), A1/A2→GPIO 21/22, B1/B2→the adapter's SDA/SCL.
  - The user is switching to a 4-channel BSS138 board because it is smaller, and it is the better fit for I2C. Its wiring: LV→3.3V, HV→5V, GND→GND, LV1→GPIO 21, LV2→GPIO 22, HV1→the adapter's SDA, HV2→the adapter's SCL. Not tried yet.

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

`Control_acceso-ESP32/Control_acceso-ESP32.ino` is the earlier version, which uses an MFRC522 (RC522) reader and drives a door-lock relay. `Control_Acceso` replaces it and shares none of its code. The two "(RC522 sketch)" sections below describe it.

Wiring (RC522 → ESP32), from `Conexiones RC522-ESP32.jpg`: VCC→3.3V, GND→GND, RST→22, SDA/SS→21, MOSI→23, MISO→19, SCK→18. The relay is on GPIO 17 (HIGH = unlocked).

## Build / upload

There are no automated tests and no linter. The `Tests_HW/` sketches are run by hand on the hardware, and the user pastes the Serial Monitor output back. The project is developed in the Arduino IDE, with the ESP32 core `esp32:esp32` 3.3.x, Adafruit ILI9341 1.7.0 + Adafruit GFX 1.13.0, XPT2046_Touchscreen 1.4.0 and the MFRC522 library (miguelbalboa) 1.4.12. Adafruit_PN532 is no longer needed. The user works on two PCs. Arduino requires each `.ino` to sit in a folder with the same name. arduino-cli is not on PATH, but the IDE bundles it at `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`, or at `%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe` on a per-user install. To install into the same folders the IDE uses, pass `--config-file %USERPROFILE%\.arduinoIDE\arduino-cli.yaml`:

```
arduino-cli compile --fqbn esp32:esp32:esp32 Terminal_Estanciero
arduino-cli compile --fqbn esp32:esp32:esp32 Control_Acceso
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_LCD
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_LCD_20x4
arduino-cli compile --fqbn esp32:esp32:esp32 Tests_HW/Prueba_PN532_LCD
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
