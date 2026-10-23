#include <Wire.h>
#include "esp_log.h"

// Prueba del display LCD 20x4 con adaptador I2C (PCF8574), sin librería
// Conexión: igual que el 16x2. SDA -> GPIO 21 y SCL -> GPIO 22 a través del conversor de nivel, VCC -> 5V, GND -> GND
// Busca el adaptador, comprueba que el LCD recibe bien los datos en las 4 filas y muestra de lo que es capaz:
// marco, juego de caracteres de la ROM, símbolos propios, números grandes, barras, texto que corre, cursor y
// animación. Al final queda mostrando un reloj grande con el tiempo desde el arranque

#define DIRECCION_PN532 0x24  // Esta prueba no usa el PN532, pero si está conectado lo reconoce en el bus

// Pines del PCF8574 en el adaptador: P0 = RS, P1 = RW, P2 = E, P3 = luz de fondo, P4 a P7 = D4 a D7 del LCD
#define LCD_RS  0x01
#define LCD_RW  0x02
#define LCD_E   0x04
#define LCD_LUZ 0x08

#define LCD_COLUMNAS 20
#define LCD_FILAS    4

// Comandos del controlador HD44780 del LCD
#define LCD_BORRAR          0x01
#define LCD_MODO_ENTRADA    0x06  // El cursor avanza a la derecha después de cada letra
#define LCD_APAGAR          0x08
#define LCD_ENCENDER        0x0C  // Display encendido, cursor oculto
#define LCD_CURSOR          0x0E  // Display encendido, cursor subrayado
#define LCD_CURSOR_PARPADEO 0x0F  // Display encendido, cursor subrayado y un bloque que parpadea
#define LCD_FUNCION         0x28  // Bus de 4 bits, 2 líneas (el de 4 filas son 2 líneas partidas), letras de 5x8 puntos
#define LCD_DIR_CGRAM       0x40  // + dirección: memoria de los símbolos propios (8 símbolos de 8 bytes)
#define LCD_DIR_DDRAM       0x80  // + dirección: memoria de lo que se ve

#define LLENO 0xFF  // Rectángulo lleno, de la ROM
#define VACIO ' '

// Dirección en la DDRAM del comienzo de cada fila: la fila 3 es la continuación de la 1, y la 4 la de la 2
const uint8_t FILAS_DDRAM[LCD_FILAS] = {0x00, 0x40, 0x14, 0x54};

// Direcciones posibles del adaptador: PCF8574 (0x20 a 0x27) o PCF8574A (0x38 a 0x3F), las más comunes primero.
// No está la 0x24, que es la del PN532
const uint8_t DIRECCIONES_LCD[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26,
                                   0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};

// ---------- Símbolos propios ----------
// El LCD guarda hasta 8 a la vez (posiciones 0 a 7), así que cada pantalla carga los suyos.
// Cada símbolo son 8 filas de 5 puntos, una por byte

// Marco: esquinas redondeadas, línea horizontal y línea vertical
enum { MARCO_SI, MARCO_SD, MARCO_II, MARCO_ID, MARCO_H, MARCO_V };
const uint8_t SIMBOLOS_MARCO[6][8] = {
  {0x00, 0x00, 0x00, 0x03, 0x04, 0x04, 0x04, 0x04},  // Esquina superior izquierda
  {0x00, 0x00, 0x00, 0x18, 0x04, 0x04, 0x04, 0x04},  // Esquina superior derecha
  {0x04, 0x04, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00},  // Esquina inferior izquierda
  {0x04, 0x04, 0x04, 0x18, 0x00, 0x00, 0x00, 0x00},  // Esquina inferior derecha
  {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x00},  // Horizontal
  {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // Vertical
};

// Íconos que le pueden servir al control de acceso
const char *NOMBRES_ICONOS[8] = {"candado cerrado", "candado abierto", "tilde", "cruz",
                                 "llave", "WiFi", "Bluetooth", "campana"};
const uint8_t SIMBOLOS_ICONOS[8][8] = {
  {0x0E, 0x11, 0x11, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},  // Candado cerrado
  {0x0E, 0x10, 0x10, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},  // Candado abierto
  {0x00, 0x01, 0x03, 0x16, 0x1C, 0x08, 0x00, 0x00},  // Tilde
  {0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00, 0x00},  // Cruz
  {0x0E, 0x11, 0x11, 0x0E, 0x04, 0x06, 0x04, 0x06},  // Llave
  {0x00, 0x0E, 0x11, 0x04, 0x0A, 0x00, 0x04, 0x00},  // WiFi
  {0x06, 0x05, 0x15, 0x0E, 0x0E, 0x15, 0x05, 0x06},  // Bluetooth
  {0x04, 0x0E, 0x0E, 0x0E, 0x1F, 0x00, 0x04, 0x00},  // Campana
};

// Piezas de los números grandes, que ocupan 3 columnas y 2 filas cada uno. SI, SD, II, ID: rectángulo lleno con
// esa esquina redondeada (superior/inferior, izquierda/derecha). AR, AB: barra arriba, barra abajo.
// AM: barra arriba y barra del medio. PT: punto de los dos puntos
enum { N_SI, N_AR, N_SD, N_II, N_AB, N_ID, N_AM, N_PT };
const uint8_t SIMBOLOS_NUMEROS[8][8] = {
  {0x07, 0x0F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},  // SI
  {0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00},  // AR
  {0x1C, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F},  // SD
  {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x0F, 0x07},  // II
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x1F, 0x1F},  // AB
  {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1E, 0x1C},  // ID
  {0x1F, 0x1F, 0x1F, 0x00, 0x00, 0x00, 0x1F, 0x1F},  // AM
  {0x00, 0x00, 0x00, 0x0E, 0x0E, 0x0E, 0x00, 0x00},  // PT
};
// Fila de arriba y fila de abajo de cada número
const uint8_t NUMEROS_GRANDES[10][2][3] = {
  {{N_SI, N_AR, N_SD},    {N_II, N_AB, N_ID}},    // 0
  {{N_AR, N_SD, VACIO},   {N_AB, LLENO, N_AB}},   // 1
  {{N_AM, N_AM, N_SD},    {N_II, N_AB, N_AB}},    // 2
  {{N_AM, N_AM, N_SD},    {N_AB, N_AB, N_ID}},    // 3
  {{N_II, N_AB, LLENO},   {VACIO, VACIO, LLENO}}, // 4
  {{N_SI, N_AM, N_AM},    {N_AB, N_AB, N_ID}},    // 5
  {{N_SI, N_AM, N_AM},    {N_II, N_AB, N_ID}},    // 6
  {{N_AR, N_AR, N_SD},    {VACIO, VACIO, LLENO}}, // 7
  {{N_SI, N_AM, N_SD},    {N_II, N_AB, N_ID}},    // 8
  {{N_SI, N_AM, N_SD},    {N_AB, N_AB, N_ID}},    // 9
};

// Animación
enum { PAC_MAN, FANTASMA, PUNTO };
const uint8_t PAC_ABIERTO[8]     = {0x0E, 0x1D, 0x1E, 0x1C, 0x1E, 0x1F, 0x0E, 0x00};
const uint8_t PAC_CERRADO[8]     = {0x0E, 0x1D, 0x1F, 0x1F, 0x1F, 0x1F, 0x0E, 0x00};
const uint8_t DIBUJO_FANTASMA[8] = {0x0E, 0x1F, 0x15, 0x1F, 0x1F, 0x1F, 0x15, 0x00};
const uint8_t DIBUJO_PUNTO[8]    = {0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x00};

uint8_t direccionLcd = 0;  // 0 = todavía no se encontró
bool luz = true;           // Luz de fondo
bool lcdResponde = true;
unsigned long ultimoMedioSegundo = 0;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Prueba de display LCD 20x4 con adaptador I2C");

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

  Serial.println("Prueba terminada. El LCD queda mostrando un reloj con el tiempo desde el arranque");
  iniciarReloj();
}

// Reloj grande con los minutos y segundos desde el arranque, y una barra que se llena en cada minuto
void loop() {
  unsigned long medioSegundo = millis() / 500;
  if (medioSegundo == ultimoMedioSegundo) {
    delay(20);
    return;
  }
  ultimoMedioSegundo = medioSegundo;
  unsigned long segundos = medioSegundo / 2;

  // Los dos puntos parpadean: se ven en la primera mitad de cada segundo
  char texto[16];
  snprintf(texto, sizeof(texto), "%lu %lu %c %lu %lu", (segundos / 600) % 10, (segundos / 60) % 10,
           medioSegundo % 2 == 0 ? ':' : ' ', (segundos % 60) / 10, segundos % 10);
  bool ok = dibujarGrande(texto, 0);

  // Una letra de la barra cada 3 segundos
  uint8_t linea[LCD_COLUMNAS];
  uint8_t llenas = (segundos % 60) / 3 + 1;
  for (uint8_t i = 0; i < LCD_COLUMNAS; i++) linea[i] = i < llenas ? LLENO : VACIO;
  ok = ok && escribirBytesEn(0, 3, linea, LCD_COLUMNAS);

  if (ok != lcdResponde) {
    lcdResponde = ok;
    Serial.println(ok ? "El LCD volvió a responder" : "El LCD dejó de responder");
    if (ok) {  // Si se cortó la alimentación, el LCD necesita arrancar de nuevo y perdió los símbolos propios
      iniciarLcd();
      iniciarReloj();
    }
  }
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

// Escribe 10 letras al principio de cada fila y lee del LCD dónde quedó el cursor: si coincide con lo esperado,
// el LCD recibe bien los datos y las filas están donde tienen que estar
void verificarLcd() {
  int leido[LCD_FILAS];
  bool fallo = false;
  bool todasOcupado = true;
  bool coinciden = true;
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) {
    char texto[11];
    snprintf(texto, sizeof(texto), "Fila %u ...", fila + 1);
    escribirEn(0, fila, texto);
    leido[fila] = leerPosicionLcd();
    if (leido[fila] < 0) fallo = true;
    if (!(leido[fila] & 0x80)) todasOcupado = false;
    if (leido[fila] != FILAS_DDRAM[fila] + 10) coinciden = false;
  }

  Serial.println("Verificación: se lee del LCD la posición del cursor después de escribir 10 letras en cada fila");
  if (fallo) {
    Serial.println("  No se pudo leer: el adaptador no contestó");
    return;
  }
  Serial.print("  Leído:");
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) Serial.printf(" 0x%02X", leido[fila]);
  Serial.print(" (tienen que ser");
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) Serial.printf(" 0x%02X", FILAS_DDRAM[fila] + 10);
  Serial.println(")");
  if (coinciden) {
    Serial.println("  OK: el LCD recibe bien los datos");
  } else if (todasOcupado) {
    Serial.println("  El LCD figura siempre ocupado: puede que no esté soldado al adaptador, que el adaptador");
    Serial.println("  no tenga conectado RW o que use otra asignación de pines. Si igual se ven las letras, está bien");
  } else {
    Serial.println("  No coincide: el adaptador puede usar otra asignación de pines");
  }
}

// ---------- Pantallas de prueba ----------

void pruebasVisuales() {
  Serial.println("Pantalla 1: marco con esquinas redondeadas, \"Prueba LCD 20x4\" y la dirección del adaptador");
  Serial.println("  Si la luz prende y no se ve nada, o se ven solo rectángulos, girá el preset de contraste");
  pantallaMarco();
  delay(5000);

  Serial.println("Pantalla 2: la luz de fondo parpadea 3 veces");
  for (int i = 0; i < 3; i++) {
    luzLcd(false);
    delay(400);
    luzLcd(true);
    delay(400);
  }

  Serial.println("Pantalla 3: los 80 rectángulos con todos los puntos encendidos");
  uint8_t lleno[LCD_COLUMNAS];
  memset(lleno, LLENO, LCD_COLUMNAS);
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) escribirBytesEn(0, fila, lleno, LCD_COLUMNAS);
  delay(3000);

  Serial.println("Pantalla 4: 80 letras distintas, en orden de arriba hacia abajo. Si las filas salen mezcladas, avisame");
  escribirEn(0, 0, "ABCDEFGHIJKLMNOPQRST");
  escribirEn(0, 1, "UVWXYZabcdefghijklmn");
  escribirEn(0, 2, "opqrstuvwxyz01234567");
  escribirEn(0, 3, "89!?#$%&()*+-/<=>@[]");
  delay(4000);

  Serial.println("Pantallas 5 a 7: el juego de caracteres de la ROM del LCD, 16 por fila con el código adelante");
  Serial.println("  Para saber qué ROM tiene, mirá la fila E0 de la pantalla 7:");
  Serial.println("  - ROM A00 (japonesa, la más común): letras griegas, ä, ñ y ö (la ñ es la anteúltima)");
  Serial.println("  - ROM A02 (europea): vocales con acento (à á â ã ä ...)");
  const uint8_t PAGINAS_ROM[3][LCD_FILAS] = {{0x20, 0x30, 0x40, 0x50}, {0x60, 0x70, 0xA0, 0xB0}, {0xC0, 0xD0, 0xE0, 0xF0}};
  for (uint8_t pagina = 0; pagina < 3; pagina++) {
    pantallaRom(PAGINAS_ROM[pagina]);
    delay(5000);
  }

  Serial.println("Pantalla 8: 8 símbolos propios, el máximo que guarda el LCD a la vez:");
  for (uint8_t i = 0; i < 8; i++) Serial.printf("  %u. %s\n", i + 1, NOMBRES_ICONOS[i]);
  pantallaIconos();
  delay(5000);

  Serial.println("Pantalla 9: los 10 números grandes, de 3 x 2 letras, armados con 8 símbolos propios");
  borrarLcd();
  definirSimbolos(SIMBOLOS_NUMEROS, 8);
  dibujarGrande("0 1 2 3 4", 0);
  dibujarGrande("5 6 7 8 9", 2);
  delay(5000);

  Serial.println("Pantalla 10: barra de progreso que avanza de a un punto (100 pasos en 20 letras)");
  demoBarra();

  Serial.println("Pantalla 11: ecualizador de 32 puntos de alto, en las 4 filas a la vez");
  demoEcualizador(6000);

  Serial.println("Pantalla 12: dos textos que corren en sentidos opuestos");
  demoMarquesina(6000);

  Serial.println("Pantalla 13: el cursor del LCD, subrayado y con parpadeo");
  demoCursor();

  Serial.println("Pantalla 14: animación. La boca se cierra cambiando el dibujo del símbolo, sin reescribir la fila");
  demoPacMan();
}

void pantallaMarco() {
  borrarLcd();
  definirSimbolos(SIMBOLOS_MARCO, 6);
  uint8_t linea[LCD_COLUMNAS];
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) {
    if (fila == 0 || fila == LCD_FILAS - 1) {
      memset(linea, MARCO_H, LCD_COLUMNAS);
      linea[0] = fila == 0 ? MARCO_SI : MARCO_II;
      linea[LCD_COLUMNAS - 1] = fila == 0 ? MARCO_SD : MARCO_ID;
    } else {
      char texto[LCD_COLUMNAS + 1];
      if (fila == 1) {
        strcpy(texto, "Prueba LCD 20x4");
      } else {
        snprintf(texto, sizeof(texto), "Direccion 0x%02X", direccionLcd);
      }
      centrar(texto, linea + 1, LCD_COLUMNAS - 2);
      linea[0] = MARCO_V;
      linea[LCD_COLUMNAS - 1] = MARCO_V;
    }
    escribirBytesEn(0, fila, linea, LCD_COLUMNAS);
  }
}

// Muestra 4 filas de 16 caracteres de la ROM, desde los códigos indicados, cada una con su código adelante
void pantallaRom(const uint8_t *inicios) {
  for (uint8_t fila = 0; fila < LCD_FILAS; fila++) {
    uint8_t linea[LCD_COLUMNAS];
    char codigo[3];
    snprintf(codigo, sizeof(codigo), "%02X", inicios[fila]);
    linea[0] = codigo[0];
    linea[1] = codigo[1];
    linea[2] = ' ';
    linea[3] = ' ';
    for (uint8_t i = 0; i < 16; i++) linea[4 + i] = inicios[fila] + i;
    escribirBytesEn(0, fila, linea, LCD_COLUMNAS);
  }
}

void pantallaIconos() {
  borrarLcd();
  definirSimbolos(SIMBOLOS_ICONOS, 8);
  escribirCentrado(0, "Simbolos propios");
  uint8_t linea[LCD_COLUMNAS];
  memset(linea, VACIO, LCD_COLUMNAS);
  for (uint8_t i = 0; i < 8; i++) linea[2 + i * 2] = i;
  escribirBytesEn(0, 2, linea, LCD_COLUMNAS);
  escribirCentrado(3, "8 como maximo");
}

void demoBarra() {
  borrarLcd();
  // Símbolos 0 a 4: de 1 a 5 columnas encendidas, dejando libres la fila de arriba y la de abajo
  for (uint8_t columnas = 1; columnas <= 5; columnas++) {
    uint8_t filas[8] = {0};
    for (uint8_t f = 1; f <= 6; f++) filas[f] = (0x1F << (5 - columnas)) & 0x1F;
    definirSimbolo(columnas - 1, filas);
  }
  escribirCentrado(0, "Barra de progreso");
  escribirCentrado(3, "100 pasos de 1 punto");

  for (int puntos = 0; puntos <= 100; puntos++) {
    dibujarBarra(1, puntos);
    delay(25);
  }
  delay(800);
  for (int puntos = 100; puntos >= 0; puntos--) {
    dibujarBarra(1, puntos);
    delay(5);
  }
  delay(500);
}

// Barra de 0 a 100 puntos en la fila indicada, con el porcentaje en la fila de abajo
void dibujarBarra(uint8_t fila, int puntos) {
  uint8_t linea[LCD_COLUMNAS];
  for (uint8_t i = 0; i < LCD_COLUMNAS; i++) {
    int resto = puntos - i * 5;  // Cada letra tiene 5 columnas de puntos
    linea[i] = resto >= 5 ? 4 : resto > 0 ? resto - 1 : VACIO;
  }
  escribirBytesEn(0, fila, linea, LCD_COLUMNAS);

  char texto[8];
  snprintf(texto, sizeof(texto), "%d %%", puntos);
  escribirCentrado(fila + 1, texto);
}

// Barras verticales que se mueven, de 0 a 32 puntos de alto. Mide cuánto tarda en escribir la pantalla entera
void demoEcualizador(unsigned long duracion) {
  borrarLcd();
  // Símbolos 0 a 6: de 1 a 7 filas encendidas desde abajo. La de 8 es el rectángulo lleno de la ROM
  for (uint8_t alto = 1; alto <= 7; alto++) {
    uint8_t filas[8] = {0};
    for (uint8_t f = 8 - alto; f < 8; f++) filas[f] = 0x1F;
    definirSimbolo(alto - 1, filas);
  }

  unsigned long inicio = millis();
  int cuadros = 0;
  while (millis() - inicio < duracion) {
    float t = (millis() - inicio) / 1000.0f;
    uint8_t lineas[LCD_FILAS][LCD_COLUMNAS];
    for (uint8_t columna = 0; columna < LCD_COLUMNAS; columna++) {
      int alto = (int)(16 + 7 * sinf(t * 5 + columna * 0.6f) + 7 * sinf(t * 2.3f - columna * 0.9f));  // 2 a 30
      for (uint8_t fila = 0; fila < LCD_FILAS; fila++) {
        int nivel = alto - (LCD_FILAS - 1 - fila) * 8;  // Puntos de la barra que caen en esta fila
        lineas[fila][columna] = nivel >= 8 ? LLENO : nivel > 0 ? nivel - 1 : VACIO;
      }
    }
    for (uint8_t fila = 0; fila < LCD_FILAS; fila++) escribirBytesEn(0, fila, lineas[fila], LCD_COLUMNAS);
    cuadros++;
  }
  Serial.printf("  %d pantallas completas en %lu ms: %lu ms cada una (unas %lu por segundo)\n",
                cuadros, duracion, duracion / cuadros, cuadros * 1000UL / duracion);
}

void demoMarquesina(unsigned long duracion) {
  borrarLcd();
  escribirCentrado(0, "Texto que corre");
  escribirCentrado(3, "en sentidos opuestos");
  const char *HACIA_IZQUIERDA = "Control de acceso con ESP32, PN532 y LCD 20x4   *   ";
  const char *HACIA_DERECHA = "   Acerque su tarjeta   >>>";
  uint8_t largoIzquierda = strlen(HACIA_IZQUIERDA);
  uint8_t largoDerecha = strlen(HACIA_DERECHA);

  unsigned long inicio = millis();
  for (unsigned long paso = 0; millis() - inicio < duracion; paso++) {
    uint8_t linea[LCD_COLUMNAS];
    for (uint8_t i = 0; i < LCD_COLUMNAS; i++) linea[i] = HACIA_IZQUIERDA[(paso + i) % largoIzquierda];
    escribirBytesEn(0, 1, linea, LCD_COLUMNAS);
    for (uint8_t i = 0; i < LCD_COLUMNAS; i++) {
      linea[i] = HACIA_DERECHA[(largoDerecha - paso % largoDerecha + i) % largoDerecha];
    }
    escribirBytesEn(0, 2, linea, LCD_COLUMNAS);
    delay(150);
  }
}

void demoCursor() {
  borrarLcd();
  escribirCentrado(0, "Cursor del LCD");
  escribirEn(0, 1, "Subrayado:");
  posicionar(11, 1);
  comandoLcd(LCD_CURSOR);
  delay(2500);

  escribirEn(0, 2, "Con parpadeo:");
  posicionar(14, 2);
  comandoLcd(LCD_CURSOR_PARPADEO);
  delay(2500);

  // Máquina de escribir: el cursor avanza solo después de cada letra
  posicionar(0, 3);
  for (const char *letra = "Escribiendo..."; *letra != '\0'; letra++) {
    enviarLcd((const uint8_t *)letra, 1, LCD_RS);
    delay(150);
  }
  delay(2000);
  comandoLcd(LCD_ENCENDER);  // Cursor oculto otra vez
}

// Pac-Man se come una fila de puntos, con un fantasma atrás
void demoPacMan() {
  borrarLcd();
  definirSimbolo(FANTASMA, DIBUJO_FANTASMA);
  definirSimbolo(PUNTO, DIBUJO_PUNTO);
  escribirCentrado(0, "Animacion");

  int puntaje = 0;
  for (int paso = 0; paso <= LCD_COLUMNAS + 3; paso++) {
    uint8_t linea[LCD_COLUMNAS];
    for (int columna = 0; columna < LCD_COLUMNAS; columna++) {
      if (columna == paso) linea[columna] = PAC_MAN;
      else if (columna == paso - 3) linea[columna] = FANTASMA;
      else if (columna > paso) linea[columna] = PUNTO;
      else linea[columna] = VACIO;
    }
    definirSimbolo(PAC_MAN, PAC_ABIERTO);
    escribirBytesEn(0, 2, linea, LCD_COLUMNAS);

    if (paso < LCD_COLUMNAS) puntaje += 10;
    char texto[LCD_COLUMNAS + 1];
    snprintf(texto, sizeof(texto), "Puntos: %d", puntaje);
    escribirCentrado(3, texto);
    delay(120);

    // La boca se cierra cambiando solo el dibujo: todo lo que muestra ese símbolo cambia al instante
    definirSimbolo(PAC_MAN, PAC_CERRADO);
    delay(120);
  }
  delay(1000);
}

void iniciarReloj() {
  borrarLcd();
  definirSimbolos(SIMBOLOS_NUMEROS, 8);
  escribirCentrado(2, "Tiempo encendido");
}

// Dibuja centrado en las filas "fila" y "fila" + 1 un texto de números grandes (3 columnas cada uno),
// espacios y dos puntos (1 columna cada uno)
bool dibujarGrande(const char *texto, uint8_t fila) {
  uint8_t ancho = 0;
  for (const char *c = texto; *c != '\0'; c++) ancho += (*c >= '0' && *c <= '9') ? 3 : 1;

  uint8_t lineas[2][LCD_COLUMNAS];
  memset(lineas, VACIO, sizeof(lineas));
  uint8_t columna = ancho < LCD_COLUMNAS ? (LCD_COLUMNAS - ancho) / 2 : 0;
  for (const char *c = texto; *c != '\0' && columna < LCD_COLUMNAS; c++) {
    if (*c >= '0' && *c <= '9') {
      for (uint8_t i = 0; i < 3 && columna < LCD_COLUMNAS; i++, columna++) {
        lineas[0][columna] = NUMEROS_GRANDES[*c - '0'][0][i];
        lineas[1][columna] = NUMEROS_GRANDES[*c - '0'][1][i];
      }
    } else {
      if (*c == ':') {
        lineas[0][columna] = N_PT;
        lineas[1][columna] = N_PT;
      }
      columna++;
    }
  }
  return escribirBytesEn(0, fila, lineas[0], LCD_COLUMNAS) && escribirBytesEn(0, fila + 1, lineas[1], LCD_COLUMNAS);
}

// Copia el texto centrado en "linea", de "ancho" letras, completando con espacios
void centrar(const char *texto, uint8_t *linea, uint8_t ancho) {
  uint8_t largo = strlen(texto);
  if (largo > ancho) largo = ancho;
  memset(linea, VACIO, ancho);
  memcpy(linea + (ancho - largo) / 2, texto, largo);
}

bool escribirCentrado(uint8_t fila, const char *texto) {
  uint8_t linea[LCD_COLUMNAS];
  centrar(texto, linea, LCD_COLUMNAS);
  return escribirBytesEn(0, fila, linea, LCD_COLUMNAS);
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
  ok &= borrarLcd();
  ok &= comandoLcd(LCD_MODO_ENTRADA);
  ok &= comandoLcd(LCD_ENCENDER);
  return ok;
}

bool borrarLcd() {
  bool ok = comandoLcd(LCD_BORRAR);
  delay(2);  // Borrar tarda 1,5 ms
  return ok;
}

// Lleva el cursor a una columna (0 a 19) y fila (0 a 3)
bool posicionar(uint8_t columna, uint8_t fila) {
  return comandoLcd(LCD_DIR_DDRAM | (FILAS_DDRAM[fila] + columna));
}

// Escribe bytes desde una posición: letras de la ROM o símbolos propios (0 a 7), hasta 31 seguidos
bool escribirBytesEn(uint8_t columna, uint8_t fila, const uint8_t *bytes, uint8_t cantidad) {
  return posicionar(columna, fila) && enviarLcd(bytes, cantidad, LCD_RS);
}

bool escribirEn(uint8_t columna, uint8_t fila, const char *texto) {
  return escribirBytesEn(columna, fila, (const uint8_t *)texto, strlen(texto));
}

bool comandoLcd(uint8_t comando) {
  return enviarLcd(&comando, 1, 0);
}

bool definirSimbolo(uint8_t posicion, const uint8_t *filas) {
  return comandoLcd(LCD_DIR_CGRAM | (posicion << 3)) && enviarLcd(filas, 8, LCD_RS);
}

// Define los símbolos 0 a cantidad - 1 de una vez (la dirección en la CGRAM avanza sola)
bool definirSimbolos(const uint8_t simbolos[][8], uint8_t cantidad) {
  bool ok = comandoLcd(LCD_DIR_CGRAM);
  for (uint8_t i = 0; i < cantidad; i++) ok &= enviarLcd(simbolos[i], 8, LCD_RS);
  return ok;
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
