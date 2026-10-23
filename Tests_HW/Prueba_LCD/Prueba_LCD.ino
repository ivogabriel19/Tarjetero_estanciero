#include <Wire.h>
#include "esp_log.h"

// Prueba del display LCD 16x2 con adaptador I2C (PCF8574), sin librería
// Conexión: SDA -> GPIO 21, SCL -> GPIO 22 (el mismo bus que el PN532), VCC -> 5V (VIN), GND -> GND
// Busca el adaptador en el bus, inicia el LCD, comprueba que el LCD recibe bien los datos y muestra unas
// pantallas de prueba. Si la luz prende pero no se ven letras, hay que girar el preset de contraste del adaptador

#define DIRECCION_PN532 0x24  // Esta prueba no usa el PN532, pero si está conectado lo reconoce en el bus

// Pines del PCF8574 en el adaptador: P0 = RS, P1 = RW, P2 = E, P3 = luz de fondo, P4 a P7 = D4 a D7 del LCD
#define LCD_RS  0x01
#define LCD_RW  0x02
#define LCD_E   0x04
#define LCD_LUZ 0x08

#define LCD_COLUMNAS 16

// Comandos del controlador HD44780 del LCD
#define LCD_BORRAR       0x01
#define LCD_MODO_ENTRADA 0x06  // El cursor avanza a la derecha después de cada letra
#define LCD_APAGAR       0x08
#define LCD_ENCENDER     0x0C  // Display encendido, cursor oculto
#define LCD_FUNCION      0x28  // Bus de 4 bits, 2 líneas, letras de 5x8 puntos
#define LCD_DIR_CGRAM    0x40  // + dirección: memoria de los símbolos propios
#define LCD_DIR_DDRAM    0x80  // + dirección: memoria de lo que se ve (línea 1 desde 0x00, línea 2 desde 0x40)

// Símbolos propios: se guardan en las posiciones 1 a 4 (la 0 no se puede poner dentro de un texto)
#define SIMBOLO_CERRADO "\x01"
#define SIMBOLO_ABIERTO "\x02"
#define SIMBOLO_SI      "\x03"
#define SIMBOLO_NO      "\x04"
const uint8_t SIMBOLOS[4][8] = {
  {0x0E, 0x11, 0x11, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},  // Candado cerrado
  {0x0E, 0x10, 0x10, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},  // Candado abierto
  {0x00, 0x01, 0x03, 0x16, 0x1C, 0x08, 0x00, 0x00},  // Tilde
  {0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00, 0x00},  // Cruz
};

// Direcciones posibles del adaptador: PCF8574 (0x20 a 0x27) o PCF8574A (0x38 a 0x3F), las más comunes primero.
// No está la 0x24, que es la del PN532
const uint8_t DIRECCIONES_LCD[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26,
                                   0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};

uint8_t direccionLcd = 0;  // 0 = todavía no se encontró
bool luz = true;           // Luz de fondo
bool lcdResponde = true;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Prueba de display LCD 16x2 con adaptador I2C");

  Wire.begin();  // SDA 21, SCL 22, a 100 kHz (el PCF8574 no admite más)
  delay(50);     // El LCD necesita 40 ms desde que se alimenta antes de recibir comandos

  while ((direccionLcd = buscarLcd()) == 0) {
    Serial.println("No se encontró el adaptador del LCD. Revisá SDA, SCL, VCC y GND. Reintentando...");
    delay(2000);
  }
  Serial.printf("Adaptador del LCD en 0x%02X\n", direccionLcd);

  if (!iniciarLcd()) Serial.println("El adaptador no aceptó algún comando de arranque");
  verificarLcd();
  pruebasVisuales();

  Serial.println("Prueba terminada. El LCD queda mostrando el tiempo desde el arranque");
}

void loop() {
  char texto[LCD_COLUMNAS + 1];
  snprintf(texto, sizeof(texto), "Tiempo: %lu s", millis() / 1000);
  bool ok = escribirLcd(0, SIMBOLO_SI " LCD funciona") && escribirLcd(1, texto);
  if (ok != lcdResponde) {
    lcdResponde = ok;
    Serial.println(ok ? "El LCD volvió a responder" : "El LCD dejó de responder");
    if (ok) iniciarLcd();  // Si se cortó la alimentación, el LCD necesita arrancar de nuevo
  }
  delay(1000);
}

// Recorre el bus I2C mostrando lo que encuentra, y devuelve la dirección del adaptador del LCD (0 si no está)
uint8_t buscarLcd() {
  Serial.println("Dispositivos en el bus I2C:");
  bool hayAlguno = false;
  for (uint8_t direccion = 1; direccion < 127; direccion++) {
    if (!respondeI2C(direccion)) continue;
    hayAlguno = true;
    Serial.printf("  0x%02X", direccion);
    if (direccion == DIRECCION_PN532) {
      Serial.print(" (PN532)");
    } else if ((direccion >= 0x20 && direccion <= 0x27) || (direccion >= 0x38 && direccion <= 0x3F)) {
      Serial.print(" (posible adaptador de LCD)");
    }
    Serial.println();
  }
  if (!hayAlguno) Serial.println("  Ninguno");

  for (uint8_t direccion : DIRECCIONES_LCD) {
    if (respondeI2C(direccion)) return direccion;
  }
  return 0;
}

bool respondeI2C(uint8_t direccion) {
  Wire.beginTransmission(direccion);
  return Wire.endTransmission() == 0;
}

// Escribe en las dos líneas y lee del LCD dónde quedó el cursor: si coincide con lo esperado, el LCD
// recibe bien los datos
void verificarLcd() {
  escribirLcd(0, "Prueba LCD 16x2");
  int linea1 = leerPosicionLcd();
  escribirLcd(1, "Verificando...");
  int linea2 = leerPosicionLcd();

  Serial.println("Verificación: se lee del LCD la posición del cursor después de escribir cada línea");
  if (linea1 < 0 || linea2 < 0) {
    Serial.println("  No se pudo leer: el adaptador no contestó");
    return;
  }
  Serial.printf("  Leído: 0x%02X y 0x%02X (tienen que ser 0x10 y 0x50)\n", linea1, linea2);
  if (linea1 == 0x10 && linea2 == 0x50) {
    Serial.println("  OK: el LCD recibe bien los datos");
  } else if ((linea1 & 0x80) && (linea2 & 0x80)) {
    Serial.println("  El LCD figura siempre ocupado: puede que no esté soldado al adaptador, que el adaptador");
    Serial.println("  no tenga conectado RW o que use otra asignación de pines. Si igual se ven las letras, está bien");
  } else {
    Serial.println("  No coincide: el adaptador puede usar otra asignación de pines");
  }
}

void pruebasVisuales() {
  char texto[LCD_COLUMNAS + 1];

  Serial.println("Pantalla 1: \"Prueba LCD 16x2\" y la dirección del adaptador");
  Serial.println("  Si la luz prende y no se ve nada, o se ven solo rectángulos, girá el preset de contraste");
  escribirLcd(0, "Prueba LCD 16x2");
  snprintf(texto, sizeof(texto), "Direccion 0x%02X", direccionLcd);
  escribirLcd(1, texto);
  delay(5000);

  Serial.println("Pantalla 2: la luz de fondo parpadea 3 veces");
  escribirLcd(0, "Luz de fondo");
  escribirLcd(1, "parpadeando...");
  for (int i = 0; i < 3; i++) {
    luzLcd(false);
    delay(400);
    luzLcd(true);
    delay(400);
  }

  Serial.println("Pantalla 3: los 32 rectángulos con todos los puntos encendidos");
  memset(texto, 0xFF, LCD_COLUMNAS);  // 0xFF es el rectángulo lleno
  texto[LCD_COLUMNAS] = '\0';
  escribirLcd(0, texto);
  escribirLcd(1, texto);
  delay(3000);

  Serial.println("Pantalla 4: letras y números en las 32 posiciones");
  escribirLcd(0, "ABCDEFGHIJKLMNOP");
  escribirLcd(1, "0123456789!?+-*/");
  delay(3000);

  Serial.println("Pantalla 5: símbolos propios (candado cerrado, candado abierto, tilde y cruz)");
  escribirLcd(0, "Simbolos propios");
  escribirLcd(1, "  " SIMBOLO_CERRADO "   " SIMBOLO_ABIERTO "   " SIMBOLO_SI "   " SIMBOLO_NO);
  delay(5000);
}

// ---------- LCD ----------

// Secuencia de arranque del HD44780 en modo de 4 bits
bool iniciarLcd() {
  bool ok = escribirAdaptador(luz ? LCD_LUZ : 0);
  // Tres veces "bus de 8 bits", para salir de cualquier estado en que haya quedado, y después "bus de 4 bits"
  ok &= enviarMitad(0x03);
  delay(5);
  ok &= enviarMitad(0x03);
  delay(1);
  ok &= enviarMitad(0x03);
  delay(1);
  ok &= enviarMitad(0x02);
  delay(1);
  ok &= comandoLcd(LCD_FUNCION);
  ok &= comandoLcd(LCD_APAGAR);
  ok &= comandoLcd(LCD_BORRAR);
  delay(2);  // Borrar tarda 1,5 ms
  ok &= comandoLcd(LCD_MODO_ENTRADA);
  for (uint8_t i = 0; i < 4; i++) ok &= definirSimbolo(i + 1, SIMBOLOS[i]);
  ok &= comandoLcd(LCD_ENCENDER);
  return ok;
}

// Escribe una línea entera (fila 0 o 1), completando con espacios lo que falte
bool escribirLcd(uint8_t fila, const char *texto) {
  uint8_t linea[LCD_COLUMNAS];
  uint8_t i = 0;
  for (; i < LCD_COLUMNAS && texto[i] != '\0'; i++) linea[i] = texto[i];
  for (; i < LCD_COLUMNAS; i++) linea[i] = ' ';
  return comandoLcd(LCD_DIR_DDRAM | (fila == 0 ? 0x00 : 0x40)) && enviarLcd(linea, LCD_COLUMNAS, LCD_RS);
}

bool comandoLcd(uint8_t comando) {
  return enviarLcd(&comando, 1, 0);
}

bool definirSimbolo(uint8_t posicion, const uint8_t *filas) {
  return comandoLcd(LCD_DIR_CGRAM | (posicion << 3)) && enviarLcd(filas, 8, LCD_RS);
}

bool luzLcd(bool encendida) {
  luz = encendida;
  return escribirAdaptador(luz ? LCD_LUZ : 0);
}

// Manda bytes al LCD en una sola transacción I2C: comandos (rs = 0) o letras (rs = LCD_RS), hasta 31 bytes.
// Cada byte va en dos mitades de 4 bits, y el LCD toma cada mitad cuando E baja
bool enviarLcd(const uint8_t *bytes, uint8_t cantidad, uint8_t rs) {
  if (direccionLcd == 0) return false;
  uint8_t base = rs | (luz ? LCD_LUZ : 0);
  Wire.beginTransmission(direccionLcd);
  Wire.write(base);  // RS queda fijo antes del primer pulso de E
  for (uint8_t i = 0; i < cantidad; i++) {
    uint8_t alta = (bytes[i] & 0xF0) | base;
    uint8_t baja = (bytes[i] << 4) | base;
    Wire.write(alta | LCD_E);
    Wire.write(alta);
    Wire.write(baja | LCD_E);
    Wire.write(baja);
  }
  return Wire.endTransmission() == 0;
}

// Manda solo la mitad alta de un byte (al arrancar, el LCD todavía espera un bus de 8 bits)
bool enviarMitad(uint8_t mitad) {
  if (direccionLcd == 0) return false;
  uint8_t valor = (mitad << 4) | (luz ? LCD_LUZ : 0);
  Wire.beginTransmission(direccionLcd);
  Wire.write(valor | LCD_E);
  Wire.write(valor);
  return Wire.endTransmission() == 0;
}

// Pone las 8 salidas del PCF8574
bool escribirAdaptador(uint8_t valor) {
  if (direccionLcd == 0) return false;
  Wire.beginTransmission(direccionLcd);
  Wire.write(valor);
  return Wire.endTransmission() == 0;
}

// Lee del LCD el bit de ocupado (bit 7) y la posición del cursor (bits 0 a 6), o -1 si el adaptador no contesta.
// Para leer, RW va en alto y D4 a D7 quedan en alto en el PCF8574, así el LCD las puede manejar
int leerPosicionLcd() {
  uint8_t base = 0xF0 | LCD_RW | (luz ? LCD_LUZ : 0);
  int valor = 0;
  for (int intento = 0; intento < 10; intento++) {
    uint8_t mitades[2];
    for (int i = 0; i < 2; i++) {
      Wire.beginTransmission(direccionLcd);
      Wire.write(base);
      Wire.write(base | LCD_E);
      if (Wire.endTransmission() != 0) return -1;
      if (Wire.requestFrom(direccionLcd, (uint8_t)1) != 1) return -1;
      mitades[i] = Wire.read() & 0xF0;
    }
    escribirAdaptador(luz ? LCD_LUZ : 0);  // E y RW otra vez en bajo
    valor = mitades[0] | (mitades[1] >> 4);
    if (!(valor & 0x80)) break;  // Si está ocupado, espera y vuelve a leer
    delay(1);
  }
  return valor;
}
