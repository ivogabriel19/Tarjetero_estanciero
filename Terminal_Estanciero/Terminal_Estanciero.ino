#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include "esp_log.h"

// Terminal de tarjetas de débito para el Estanciero (sirve también para el Monopoly): reemplaza los billetes.
// Cada jugador tiene una tarjeta NFC que solo lo identifica: los saldos se guardan en la memoria del ESP32,
// así que sobreviven a un reinicio o a un corte de luz. El banco tiene dinero ilimitado.

// ---------- Reglas ----------
// Confirmar los montos con el reglamento de la edición del juego. Para el Monopoly: 1500 y 200
#define DINERO_INICIAL 35000     // Lo que recibe cada jugador al empezar la partida
#define MONTO_SALIDA   5000      // Lo que cobra un jugador al pasar por la salida
#define MONTO_MAXIMO   99999999  // Máximo que se puede ingresar con el teclado
#define MAX_JUGADORES  8
#define MAX_HISTORIAL  30        // Cuántas operaciones se pueden deshacer, de la última para atrás

// ---------- Hardware ----------
// PN532 por I2C: SDA -> GPIO 21, SCL -> GPIO 22 (pines I2C por defecto del ESP32)
// Se habla con el PN532 directamente con Wire, sin librería, dejando una pausa entre transacciones I2C
#define DIRECCION_PN532 0x24

// Display TFT ILI9341 240x320 por SPI: SCK -> GPIO 18, SDI (MOSI) -> GPIO 23, SDO (MISO) -> GPIO 19
#define TFT_CS  5
#define TFT_DC  4
#define TFT_RST 15
#define VELOCIDAD_SPI_TFT 10000000  // 10 MHz, seguro con cables de protoboard (la librería usa 40 MHz por defecto)
#define ROTACION 3  // Horizontal (320x240), derecha según cómo está montado el display (la 1 se ve cabeza abajo)

// Touch XPT2046: comparte el bus SPI con el display (T_CLK -> 18, T_DIN -> 23, T_DO -> 19)
// T_IRQ no se usa: la librería consulta la presión por SPI
#define TOUCH_CS 16
#define ROTACION_TOUCH ((ROTACION + 2) % 4)  // En este display el touch está girado 180° respecto de la imagen

// Valores crudos del touch (0 a 4095) en los bordes de la pantalla, para ROTACION_TOUCH 1 (los mismos que en Prueba_Display)
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define PAUSA_BUS             5     // ms de pausa entre transacciones I2C
#define TIMEOUT_TRAMA         1000  // ms máximos de espera de cada trama del PN532
#define REINTENTOS_BUSQUEDA   0x05  // Veces que el PN532 busca una tarjeta antes de responder "no hay tarjeta"
#define MAX_DATOS             32    // Máximo de datos que se leen de una respuesta
#define LARGO_MAX_UID         10    // Los UID de las tarjetas MIFARE tienen 4, 7 o 10 bytes
#define LECTURAS_PARA_RETIRAR 2     // Lecturas seguidas sin tarjeta para dar por retirada la que estaba apoyada
#define TIEMPO_RESULTADO      4000  // ms que queda en pantalla el resultado de una operación (o hasta que se toca)

// Códigos de comando del PN532
#define CMD_GET_FIRMWARE_VERSION   0x02
#define CMD_SAM_CONFIGURATION      0x14
#define CMD_RF_CONFIGURATION       0x32
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A

// ---------- Pantalla (horizontal, 320x240) ----------
#define ALTO_TITULO 36
#define Y_PIE       218  // Línea de texto de abajo de todo

// Colores (formato RGB565 del display)
#define COLOR_TITULO  ILI9341_NAVY
#define COLOR_TECLA   0x3186  // Gris oscuro
#define COLOR_VERDE   ILI9341_DARKGREEN
#define COLOR_ROJO    0xB000  // Rojo oscuro: con letra blanca se lee mejor que el rojo puro
#define COLOR_AZUL    ILI9341_BLUE
#define COLOR_NARANJA 0xCB20  // Naranja oscuro
#define COLOR_VIOLETA ILI9341_PURPLE
#define COLOR_GRIS    ILI9341_DARKGREY

#define BANCO   -1  // En una operación, el banco en lugar de un jugador
#define NINGUNO -2  // Ningún jugador (tarjeta no registrada, operación cancelada)

#define VERSION_PARTIDA 1  // Cambiarla si cambia la estructura Partida, para no cargar datos guardados con otra forma

struct Jugador {
  uint8_t uid[LARGO_MAX_UID];  // UID de su tarjeta
  uint8_t largoUid;
  int32_t saldo;
};

// Una operación hecha, para poder deshacerla
struct Operacion {
  int8_t origen;   // Jugador que pagó, o BANCO
  int8_t destino;  // Jugador que cobró, o BANCO
  int32_t monto;
};

// Todo lo que se guarda en la memoria del ESP32
struct Partida {
  uint8_t version;
  uint8_t cantidadJugadores;
  uint8_t cantidadHistorial;
  Jugador jugadores[MAX_JUGADORES];
  Operacion historial[MAX_HISTORIAL];  // La última operación es la del final
};

// Nombre y color con los que se muestra cada jugador (conviene marcar cada tarjeta con su color)
struct Ficha {
  const char *nombre;
  uint16_t color;
  uint16_t colorLetra;
};

const Ficha FICHAS[MAX_JUGADORES] = {
  {"Rojo",     ILI9341_RED,    ILI9341_WHITE},
  {"Azul",     ILI9341_BLUE,   ILI9341_WHITE},
  {"Verde",    ILI9341_GREEN,  ILI9341_BLACK},
  {"Amarillo", ILI9341_YELLOW, ILI9341_BLACK},
  {"Violeta",  ILI9341_PURPLE, ILI9341_WHITE},
  {"Naranja",  ILI9341_ORANGE, ILI9341_BLACK},
  {"Celeste",  ILI9341_CYAN,   ILI9341_BLACK},
  {"Rosa",     ILI9341_PINK,   ILI9341_BLACK},
};
const Ficha FICHA_BANCO = {"Banco", ILI9341_LIGHTGREY, ILI9341_BLACK};

struct Boton {
  int16_t x, y, ancho, alto;
  const char *texto;
  const char *detalle;  // Segunda línea, en letra chica (nullptr si no tiene)
  uint16_t color;
  uint8_t tamanio;      // Tamaño de la letra del texto
};

// Menú principal: 2 columnas x 3 filas
enum { MENU_COBRAR, MENU_PAGAR, MENU_TRANSFERIR, MENU_SALIDA, MENU_JUGADORES, MENU_DESHACER, CANTIDAD_MENU };
char detalleSalida[16];  // "+$5.000": se arma en setup() con MONTO_SALIDA
const Boton MENU[CANTIDAD_MENU] = {
  {6,   42,  151, 52, "COBRAR",     "el banco paga",    COLOR_VERDE,   2},
  {163, 42,  151, 52, "PAGAR",      "al banco",         COLOR_ROJO,    2},
  {6,   100, 151, 52, "TRANSFERIR", "entre jugadores",  COLOR_AZUL,    2},
  {163, 100, 151, 52, "SALIDA",     detalleSalida,      COLOR_NARANJA, 2},
  {6,   158, 151, 52, "JUGADORES",  "saldos y partida", COLOR_VIOLETA, 2},
  {163, 158, 151, 52, "DESHACER",   "ultima operacion", COLOR_GRIS,    2},
};

// Teclado numérico: 4 columnas (x = 4, 83, 162, 241) x 4 filas (y = 88, 126, 164, 202)
// Las teclas 0 a 8 son los dígitos 1 a 9
enum { TECLA_00 = 9, TECLA_0, TECLA_000, TECLA_BORRAR, TECLA_SALIR, TECLA_OK, CANTIDAD_TECLAS };
const Boton TECLADO[CANTIDAD_TECLAS] = {
  {4,   88,  75, 34, "1",     nullptr, COLOR_TECLA,   3},
  {83,  88,  75, 34, "2",     nullptr, COLOR_TECLA,   3},
  {162, 88,  75, 34, "3",     nullptr, COLOR_TECLA,   3},
  {4,   126, 75, 34, "4",     nullptr, COLOR_TECLA,   3},
  {83,  126, 75, 34, "5",     nullptr, COLOR_TECLA,   3},
  {162, 126, 75, 34, "6",     nullptr, COLOR_TECLA,   3},
  {4,   164, 75, 34, "7",     nullptr, COLOR_TECLA,   3},
  {83,  164, 75, 34, "8",     nullptr, COLOR_TECLA,   3},
  {162, 164, 75, 34, "9",     nullptr, COLOR_TECLA,   3},
  {4,   202, 75, 34, "00",    nullptr, COLOR_TECLA,   3},
  {83,  202, 75, 34, "0",     nullptr, COLOR_TECLA,   3},
  {162, 202, 75, 34, "000",   nullptr, COLOR_TECLA,   3},
  {241, 88,  75, 34, "<",     nullptr, COLOR_NARANJA, 3},
  {241, 126, 75, 34, "SALIR", nullptr, COLOR_ROJO,    2},
  {241, 164, 75, 72, "OK",    nullptr, COLOR_VERDE,   3},
};

const Boton BOTON_CANCELAR = {60, 190, 200, 42, "CANCELAR", nullptr, COLOR_ROJO, 2};
const Boton BOTONES_CONFIRMAR[2] = {
  {6,   190, 150, 44, "NO", nullptr, COLOR_GRIS,  3},
  {164, 190, 150, 44, "SI", nullptr, COLOR_VERDE, 3},
};
const Boton BOTONES_JUGADORES[2] = {
  {6,   204, 110, 32, "VOLVER",        nullptr, COLOR_GRIS, 2},
  {122, 204, 192, 32, "NUEVA PARTIDA", nullptr, COLOR_ROJO, 2},
};
const Boton BOTONES_REGISTRO[2] = {
  {6,   200, 150, 36, "CANCELAR", nullptr, COLOR_ROJO,  2},
  {164, 200, 150, 36, "LISTO",    nullptr, COLOR_VERDE, 2},
};

const uint8_t TRAMA_ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);  // Instancia del display con SPI por hardware
XPT2046_Touchscreen touch(TOUCH_CS);             // Instancia del touch, en el mismo bus SPI
Preferences memoria;                             // Memoria no volátil (NVS) del ESP32

Partida partida;
uint8_t respuesta[MAX_DATOS];       // Datos de la última respuesta del PN532
bool detallar = true;               // Imprime cada paso de la comunicación (mientras se inicia el lector)
uint8_t uidApoyada[LARGO_MAX_UID];  // Tarjeta que está sobre el lector, para no leerla dos veces
uint8_t largoApoyada = 0;           // 0 = no hay ninguna apoyada
uint8_t lecturasSinTarjeta = 0;
bool redibujarMenu = true;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Terminal de tarjetas para el Estanciero");

  // El CS del touch tiene que estar en alto antes de hablar con el display, para que el touch no responda en el bus
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);

  tft.begin(VELOCIDAD_SPI_TFT);
  tft.setRotation(ROTACION);
  tft.setTextWrap(false);
  touch.begin();
  touch.setRotation(ROTACION_TOUCH);
  mostrarInicio("Iniciando lector...", ILI9341_WHITE);

  char monto[16];
  formatearMonto(MONTO_SALIDA, monto);
  sprintf(detalleSalida, "+%s", monto);

  cargarPartida();

  Wire.begin();  // SDA 21, SCL 22
  delay(500);    // Da tiempo al PN532 a terminar de arrancar

  // Si el PN532 quedó ejecutando un comando anterior (por ejemplo, si se reinició solo el ESP32),
  // una trama ACK lo cancela
  Wire.beginTransmission(DIRECCION_PN532);
  Wire.write(TRAMA_ACK, sizeof(TRAMA_ACK));
  Wire.endTransmission();
  delay(50);

  iniciarLector();
  Serial.println("Terminal lista");
}

// Menú principal. Si se acerca una tarjeta sin elegir una operación, se muestra su saldo
void loop() {
  if (redibujarMenu) {
    dibujarMenu();
    redibujarMenu = false;
  }

  int opcion = botonTocado(MENU, CANTIDAD_MENU);
  if (opcion >= 0) {
    redibujarMenu = true;
    if (opcion <= MENU_SALIDA && partida.cantidadJugadores == 0) {
      mostrarAviso("SIN JUGADORES", COLOR_ROJO, "Primero registra las", "tarjetas en JUGADORES");
      return;
    }
    switch (opcion) {
      case MENU_COBRAR:     cobrar(); break;
      case MENU_PAGAR:      pagar(); break;
      case MENU_TRANSFERIR: transferir(); break;
      case MENU_SALIDA:     cobrarSalida(); break;
      case MENU_JUGADORES:  verJugadores(); break;
      case MENU_DESHACER:   deshacer(); break;
    }
    return;
  }

  uint8_t uid[LARGO_MAX_UID];
  uint8_t largo;
  if (tarjetaNueva(uid, largo)) {
    consultarSaldo(uid, largo);
    redibujarMenu = true;
  }
}

// ---------- Operaciones ----------

// El banco le paga a un jugador
void cobrar() {
  uint16_t color = MENU[MENU_COBRAR].color;
  int32_t monto = pedirMonto("COBRAR DEL BANCO", color);
  if (monto == 0) return;
  int8_t jugador = esperarJugador("COBRAR DEL BANCO", color, monto, "de quien cobra", NINGUNO);
  if (jugador == NINGUNO) return;
  operar(BANCO, jugador, monto);
  mostrarOperacion("OPERACION APROBADA", BANCO, jugador, monto);
}

// Un jugador le paga al banco
void pagar() {
  uint16_t color = MENU[MENU_PAGAR].color;
  int32_t monto = pedirMonto("PAGAR AL BANCO", color);
  if (monto == 0) return;
  int8_t jugador = esperarJugador("PAGAR AL BANCO", color, monto, "de quien paga", NINGUNO);
  if (jugador == NINGUNO) return;
  if (!operar(jugador, BANCO, monto)) {
    mostrarSaldoInsuficiente(jugador, monto);
    return;
  }
  mostrarOperacion("OPERACION APROBADA", jugador, BANCO, monto);
}

// Un jugador le paga a otro
void transferir() {
  uint16_t color = MENU[MENU_TRANSFERIR].color;
  int32_t monto = pedirMonto("TRANSFERIR", color);
  if (monto == 0) return;
  int8_t origen = esperarJugador("TRANSFERIR", color, monto, "de quien paga", NINGUNO);
  if (origen == NINGUNO) return;

  // Se revisa el saldo antes de pedir la tarjeta de quien cobra
  if (partida.jugadores[origen].saldo < monto) {
    mostrarSaldoInsuficiente(origen, monto);
    return;
  }

  int8_t destino = esperarJugador("TRANSFERIR", color, monto, "de quien cobra", origen);
  if (destino == NINGUNO) return;
  operar(origen, destino, monto);
  mostrarOperacion("OPERACION APROBADA", origen, destino, monto);
}

// El banco le paga MONTO_SALIDA a un jugador que pasó por la salida
void cobrarSalida() {
  int8_t jugador = esperarJugador("SALIDA", MENU[MENU_SALIDA].color, MONTO_SALIDA, "de quien cobra", NINGUNO);
  if (jugador == NINGUNO) return;
  operar(BANCO, jugador, MONTO_SALIDA);
  mostrarOperacion("OPERACION APROBADA", BANCO, jugador, MONTO_SALIDA);
}

// Deshace la última operación (se puede repetir para ir deshaciendo las anteriores)
void deshacer() {
  uint16_t color = MENU[MENU_DESHACER].color;
  if (partida.cantidadHistorial == 0) {
    mostrarAviso("DESHACER", color, "No hay operaciones", "para deshacer");
    return;
  }

  Operacion operacion = partida.historial[partida.cantidadHistorial - 1];
  char participantes[32], monto[16];
  sprintf(participantes, "%s -> %s", ficha(operacion.origen).nombre, ficha(operacion.destino).nombre);
  formatearMonto(operacion.monto, monto);
  if (!confirmar("DESHACER", color, participantes, monto)) return;

  // Al deshacer en orden, quien cobró todavía tiene ese dinero, así que esto no debería fallar
  if (!mover(operacion.destino, operacion.origen, operacion.monto)) {
    mostrarAviso("DESHACER", COLOR_ROJO, "No se pudo deshacer", "la operacion");
    return;
  }
  partida.cantidadHistorial--;
  guardarPartida();
  imprimirOperacion("Operación deshecha", operacion.origen, operacion.destino, operacion.monto);
  mostrarOperacion("OPERACION DESHECHA", operacion.destino, operacion.origen, operacion.monto);
}

void consultarSaldo(const uint8_t *uid, uint8_t largo) {
  int8_t jugador = buscarJugador(partida.jugadores, partida.cantidadJugadores, uid, largo);
  if (jugador == NINGUNO) {
    Serial.println("Tarjeta no registrada");
    mostrarAviso("SALDO", COLOR_ROJO, "Tarjeta no registrada", "Se registra en JUGADORES");
    return;
  }

  char saldo[16];
  formatearMonto(partida.jugadores[jugador].saldo, saldo);
  Serial.printf("Saldo de %s: %s\n", FICHAS[jugador].nombre, saldo);

  dibujarTitulo("SALDO", COLOR_TITULO);
  dibujarEtiqueta(jugador, (tft.width() - anchoEtiqueta(jugador, 3)) / 2, 64, 3);
  escribirCentrado(saldo, 120, 4, ILI9341_WHITE);
  dibujarPie("Toca para seguir");
  esperarToque(TIEMPO_RESULTADO);
}

// Lista de jugadores con sus saldos, y el botón para empezar una partida nueva
void verJugadores() {
  dibujarTitulo("JUGADORES", MENU[MENU_JUGADORES].color);
  if (partida.cantidadJugadores == 0) {
    escribirCentrado("No hay jugadores", 90, 2, ILI9341_WHITE);
    escribirCentrado("Toca NUEVA PARTIDA", 120, 2, ILI9341_LIGHTGREY);
  }
  for (uint8_t i = 0; i < partida.cantidadJugadores; i++) dibujarFilaSaldo(i, 42 + i * 20);
  for (uint8_t i = 0; i < 2; i++) dibujarBoton(BOTONES_JUGADORES[i], false);

  if (esperarBoton(BOTONES_JUGADORES, 2) == 1) nuevaPartida();
}

// Borra la partida y registra las tarjetas de los jugadores, cada uno con DINERO_INICIAL
void nuevaPartida() {
  if (!confirmar("NUEVA PARTIDA", COLOR_ROJO, "Se borran los jugadores", "y todos los saldos")) return;

  // Los jugadores se anotan aparte: si se cancela, sigue la partida anterior
  Jugador jugadores[MAX_JUGADORES];
  uint8_t cantidad = 0;

  dibujarTitulo("NUEVA PARTIDA", COLOR_ROJO);
  escribirCentrado("Acerca cada tarjeta", 46, 2, ILI9341_WHITE);
  for (uint8_t i = 0; i < 2; i++) dibujarBoton(BOTONES_REGISTRO[i], false);
  olvidarTarjeta();  // Si ya hay una tarjeta apoyada, también se anota

  while (true) {
    int boton = botonTocado(BOTONES_REGISTRO, 2);
    if (boton == 0) return;  // CANCELAR
    if (boton == 1) {        // LISTO
      if (cantidad > 0) break;
      mostrarAvisoEnLinea("Falta acercar tarjetas", 176);
      continue;
    }

    uint8_t uid[LARGO_MAX_UID];
    uint8_t largo;
    if (!tarjetaNueva(uid, largo)) continue;
    if (buscarJugador(jugadores, cantidad, uid, largo) != NINGUNO) {
      mostrarAvisoEnLinea("Ya esta anotada", 176);
    } else if (cantidad == MAX_JUGADORES) {
      mostrarAvisoEnLinea("No entran mas jugadores", 176);
    } else {
      memcpy(jugadores[cantidad].uid, uid, largo);
      jugadores[cantidad].largoUid = largo;
      jugadores[cantidad].saldo = DINERO_INICIAL;
      dibujarEtiqueta(cantidad, cantidad % 2 == 0 ? 20 : 170, 76 + (cantidad / 2) * 24, 2);  // En 2 columnas
      mostrarAvisoEnLinea("", 176);
      cantidad++;
    }
  }

  memcpy(partida.jugadores, jugadores, cantidad * sizeof(Jugador));
  partida.cantidadJugadores = cantidad;
  partida.cantidadHistorial = 0;
  guardarPartida();
  Serial.print("Nueva partida. ");
  imprimirPartida();
}

// ---------- Saldos ----------

// Pasa dinero de un jugador (o del banco) a otro. Si el que paga no tiene saldo suficiente,
// no cambia nada y devuelve false
bool mover(int8_t origen, int8_t destino, int32_t monto) {
  if (origen != BANCO && partida.jugadores[origen].saldo < monto) return false;
  if (origen != BANCO) partida.jugadores[origen].saldo -= monto;
  if (destino != BANCO) partida.jugadores[destino].saldo += monto;
  return true;
}

// Hace una operación, la anota en el historial (para poder deshacerla) y guarda la partida
bool operar(int8_t origen, int8_t destino, int32_t monto) {
  if (!mover(origen, destino, monto)) return false;

  if (partida.cantidadHistorial == MAX_HISTORIAL) {
    // Historial lleno: se olvida la operación más vieja
    memmove(&partida.historial[0], &partida.historial[1], (MAX_HISTORIAL - 1) * sizeof(Operacion));
    partida.cantidadHistorial--;
  }
  partida.historial[partida.cantidadHistorial++] = {origen, destino, monto};
  guardarPartida();
  imprimirOperacion("Operación", origen, destino, monto);
  return true;
}

// Devuelve el número del jugador que tiene esa tarjeta, o NINGUNO
int8_t buscarJugador(const Jugador *jugadores, uint8_t cantidad, const uint8_t *uid, uint8_t largo) {
  for (uint8_t i = 0; i < cantidad; i++) {
    if (jugadores[i].largoUid == largo && memcmp(jugadores[i].uid, uid, largo) == 0) return i;
  }
  return NINGUNO;
}

const Ficha &ficha(int8_t jugador) {
  return jugador == BANCO ? FICHA_BANCO : FICHAS[jugador];
}

void cargarPartida() {
  memoria.begin("estanciero", false);
  if (memoria.isKey("partida") && memoria.getBytesLength("partida") == sizeof(Partida)) {
    memoria.getBytes("partida", &partida, sizeof(Partida));
    if (partida.version == VERSION_PARTIDA && partida.cantidadJugadores <= MAX_JUGADORES &&
        partida.cantidadHistorial <= MAX_HISTORIAL) {
      Serial.print("Partida recuperada. ");
      imprimirPartida();
      return;
    }
  }

  Serial.println("No hay una partida guardada");
  memset(&partida, 0, sizeof(Partida));
  partida.version = VERSION_PARTIDA;
}

void guardarPartida() {
  if (memoria.putBytes("partida", &partida, sizeof(Partida)) != sizeof(Partida)) {
    Serial.println("Error al guardar la partida");
  }
}

// Escribe el monto con puntos de miles, por ejemplo "$35.000"
void formatearMonto(int32_t monto, char *texto) {
  char digitos[12];
  int largo = sprintf(digitos, "%ld", (long)monto);
  *texto++ = '$';
  for (int i = 0; i < largo; i++) {
    if (i > 0 && (largo - i) % 3 == 0) *texto++ = '.';
    *texto++ = digitos[i];
  }
  *texto = '\0';
}

void imprimirPartida() {
  Serial.printf("Jugadores: %d\n", partida.cantidadJugadores);
  for (uint8_t i = 0; i < partida.cantidadJugadores; i++) {
    char saldo[16];
    formatearMonto(partida.jugadores[i].saldo, saldo);
    Serial.printf("  %s: %s. Tarjeta:", FICHAS[i].nombre, saldo);
    imprimirBytes(partida.jugadores[i].uid, partida.jugadores[i].largoUid);
  }
}

// Imprime una operación y cómo quedaron los saldos de los jugadores que participaron
void imprimirOperacion(const char *titulo, int8_t origen, int8_t destino, int32_t monto) {
  char texto[16];
  formatearMonto(monto, texto);
  Serial.printf("%s: %s -> %s %s.", titulo, ficha(origen).nombre, ficha(destino).nombre, texto);
  if (origen != BANCO) imprimirSaldo(origen);
  if (destino != BANCO) imprimirSaldo(destino);
  Serial.println();
}

void imprimirSaldo(int8_t jugador) {
  char saldo[16];
  formatearMonto(partida.jugadores[jugador].saldo, saldo);
  Serial.printf(" Saldo de %s: %s.", FICHAS[jugador].nombre, saldo);
}

// ---------- Pantallas ----------
// (la fuente del display no tiene tildes, por eso los textos van sin acentos)

void mostrarInicio(const char *mensaje, uint16_t color) {
  dibujarTitulo("BANCO ESTANCIERO", COLOR_TITULO);
  escribirCentrado(mensaje, 110, 2, color);
}

void dibujarMenu() {
  dibujarTitulo("BANCO ESTANCIERO", COLOR_TITULO);
  for (uint8_t i = 0; i < CANTIDAD_MENU; i++) dibujarBoton(MENU[i], false);
  dibujarPie("Ver saldo: acerca tarjeta");
}

// Pide un monto con el teclado. Devuelve el monto, o 0 si se toca SALIR
int32_t pedirMonto(const char *titulo, uint16_t color) {
  dibujarTitulo(titulo, color);
  for (uint8_t i = 0; i < CANTIDAD_TECLAS; i++) dibujarBoton(TECLADO[i], false);
  int32_t monto = 0;
  dibujarMontoTeclado(monto);

  while (true) {
    int tecla = esperarBoton(TECLADO, CANTIDAD_TECLAS);
    int64_t nuevo = monto;  // 64 bits: con "000" un monto grande no entra en 32
    if (tecla < 9) nuevo = monto * 10 + tecla + 1;
    else if (tecla == TECLA_00) nuevo = monto * 100LL;
    else if (tecla == TECLA_0) nuevo = monto * 10LL;
    else if (tecla == TECLA_000) nuevo = monto * 1000LL;
    else if (tecla == TECLA_BORRAR) nuevo = monto / 10;
    else if (tecla == TECLA_SALIR) return 0;
    else if (tecla == TECLA_OK && monto > 0) return monto;

    if (nuevo != monto && nuevo <= MONTO_MAXIMO) {
      monto = nuevo;
      dibujarMontoTeclado(monto);
    }
  }
}

void dibujarMontoTeclado(int32_t monto) {
  char texto[16];
  formatearMonto(monto, texto);
  tft.fillRect(0, ALTO_TITULO, tft.width(), 50, ILI9341_BLACK);
  escribirCentrado(texto, 46, 4, ILI9341_WHITE);
}

// Pide que se acerque la tarjeta de un jugador. Devuelve su número, o NINGUNO si se toca CANCELAR.
// No acepta la tarjeta de "excluido" (en una transferencia, quien paga no puede ser quien cobra)
int8_t esperarJugador(const char *titulo, uint16_t color, int32_t monto, const char *quien, int8_t excluido) {
  char texto[16];
  formatearMonto(monto, texto);
  dibujarTitulo(titulo, color);
  escribirCentrado(texto, 46, 4, ILI9341_WHITE);
  escribirCentrado("Acerca la tarjeta", 100, 2, ILI9341_WHITE);
  escribirCentrado(quien, 124, 2, ILI9341_WHITE);
  dibujarBoton(BOTON_CANCELAR, false);

  // Si ya hay una tarjeta apoyada, también cuenta. En el segundo paso de una transferencia no:
  // la tarjeta que sigue apoyada es la de quien paga
  if (excluido == NINGUNO) olvidarTarjeta();

  while (true) {
    if (botonTocado(&BOTON_CANCELAR, 1) == 0) return NINGUNO;

    uint8_t uid[LARGO_MAX_UID];
    uint8_t largo;
    if (!tarjetaNueva(uid, largo)) continue;
    int8_t jugador = buscarJugador(partida.jugadores, partida.cantidadJugadores, uid, largo);
    if (jugador == NINGUNO) mostrarAvisoEnLinea("Tarjeta no registrada", 158);
    else if (jugador == excluido) mostrarAvisoEnLinea("Es la tarjeta que paga", 158);
    else return jugador;
  }
}

// Muestra una operación hecha: el monto, quién pagó a quién y cómo quedaron los saldos
void mostrarOperacion(const char *titulo, int8_t origen, int8_t destino, int32_t monto) {
  char texto[16];
  formatearMonto(monto, texto);
  dibujarTitulo(titulo, COLOR_VERDE);
  escribirCentrado(texto, 46, 4, ILI9341_WHITE);

  // "Rojo -> Azul", centrado
  int16_t anchoOrigen = anchoEtiqueta(origen, 2);
  int16_t x = (tft.width() - anchoOrigen - 48 - anchoEtiqueta(destino, 2)) / 2;
  dibujarEtiqueta(origen, x, 92, 2);
  escribirEnCaja("->", x + anchoOrigen, 48, 93, 2, ILI9341_WHITE);
  dibujarEtiqueta(destino, x + anchoOrigen + 48, 92, 2);

  // El banco no tiene saldo
  escribir("Saldos:", 20, 126, 1, ILI9341_LIGHTGREY);
  int16_t y = 138;
  if (origen != BANCO) {
    dibujarFilaSaldo(origen, y);
    y += 24;
  }
  if (destino != BANCO) dibujarFilaSaldo(destino, y);

  dibujarPie("Toca para seguir");
  esperarToque(TIEMPO_RESULTADO);
}

void mostrarSaldoInsuficiente(int8_t jugador, int32_t monto) {
  char saldo[16], aPagar[16], texto[32];
  formatearMonto(partida.jugadores[jugador].saldo, saldo);
  formatearMonto(monto, aPagar);
  Serial.printf("Rechazada: %s no puede pagar %s (tiene %s)\n", FICHAS[jugador].nombre, aPagar, saldo);
  sprintf(texto, "%s tiene %s", FICHAS[jugador].nombre, saldo);
  mostrarAviso("RECHAZADA", COLOR_ROJO, "Saldo insuficiente", texto);
}

// Pantalla con un título y dos líneas de texto; queda hasta que se toca o pasa TIEMPO_RESULTADO
void mostrarAviso(const char *titulo, uint16_t color, const char *linea1, const char *linea2) {
  dibujarTitulo(titulo, color);
  escribirCentrado(linea1, 88, 2, ILI9341_WHITE);
  escribirCentrado(linea2, 116, 2, ILI9341_WHITE);
  dibujarPie("Toca para seguir");
  esperarToque(TIEMPO_RESULTADO);
}

// Pregunta si se quiere seguir. Devuelve true si se toca SI
bool confirmar(const char *titulo, uint16_t color, const char *linea1, const char *linea2) {
  dibujarTitulo(titulo, color);
  escribirCentrado(linea1, 80, 2, ILI9341_WHITE);
  escribirCentrado(linea2, 110, 2, ILI9341_WHITE);
  for (uint8_t i = 0; i < 2; i++) dibujarBoton(BOTONES_CONFIRMAR[i], false);
  return esperarBoton(BOTONES_CONFIRMAR, 2) == 1;
}

// ---------- Dibujo ----------

// Borra la pantalla y dibuja la barra de título (con letra grande si el título entra)
void dibujarTitulo(const char *titulo, uint16_t color) {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, tft.width(), ALTO_TITULO, color);
  uint8_t tamanio = anchoTexto(titulo, 3) <= tft.width() - 10 ? 3 : 2;
  escribirCentrado(titulo, (ALTO_TITULO - 8 * tamanio) / 2, tamanio, ILI9341_WHITE);
}

void dibujarPie(const char *texto) {
  escribirCentrado(texto, Y_PIE, 2, ILI9341_LIGHTGREY);
}

// Escribe un aviso en rojo en una línea, borrando el anterior
void mostrarAvisoEnLinea(const char *texto, int16_t y) {
  tft.fillRect(0, y - 2, tft.width(), 20, ILI9341_BLACK);
  escribirCentrado(texto, y, 2, ILI9341_RED);
}

// Dibuja un botón; "presionado" lo pinta con los colores invertidos
void dibujarBoton(const Boton &boton, bool presionado) {
  uint16_t fondo = presionado ? ILI9341_WHITE : boton.color;
  uint16_t letra = presionado ? boton.color : ILI9341_WHITE;
  tft.fillRoundRect(boton.x, boton.y, boton.ancho, boton.alto, 6, fondo);

  int16_t altoTexto = 8 * boton.tamanio;
  if (boton.detalle) altoTexto += 4 + 8;
  int16_t y = boton.y + (boton.alto - altoTexto) / 2;
  escribirEnCaja(boton.texto, boton.x, boton.ancho, y, boton.tamanio, letra);
  if (boton.detalle) escribirEnCaja(boton.detalle, boton.x, boton.ancho, y + 8 * boton.tamanio + 4, 1, letra);
}

int16_t anchoEtiqueta(int8_t jugador, uint8_t tamanio) {
  return anchoTexto(ficha(jugador).nombre, tamanio) + 6 * tamanio;
}

// Dibuja el nombre de un jugador (o del banco) sobre un rectángulo de su color
void dibujarEtiqueta(int8_t jugador, int16_t x, int16_t y, uint8_t tamanio) {
  const Ficha &f = ficha(jugador);
  tft.fillRoundRect(x, y, anchoEtiqueta(jugador, tamanio), 8 * tamanio + 2, 4, f.color);
  escribir(f.nombre, x + 3 * tamanio, y + 1, tamanio, f.colorLetra);
}

// Fila con el nombre de un jugador a la izquierda y su saldo a la derecha
void dibujarFilaSaldo(int8_t jugador, int16_t y) {
  char saldo[16];
  formatearMonto(partida.jugadores[jugador].saldo, saldo);
  dibujarEtiqueta(jugador, 20, y, 2);
  escribirDerecha(saldo, 300, y + 1, 2, ILI9341_WHITE);
}

// Ancho en pixeles de un texto: la fuente del display tiene 6x8 pixeles por letra, multiplicados por el tamaño
int16_t anchoTexto(const char *texto, uint8_t tamanio) {
  return strlen(texto) * 6 * tamanio;
}

void escribir(const char *texto, int16_t x, int16_t y, uint8_t tamanio, uint16_t color) {
  tft.setTextSize(tamanio);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(texto);
}

// Escribe un texto centrado entre x y x + ancho
void escribirEnCaja(const char *texto, int16_t x, int16_t ancho, int16_t y, uint8_t tamanio, uint16_t color) {
  escribir(texto, x + (ancho - anchoTexto(texto, tamanio)) / 2, y, tamanio, color);
}

void escribirCentrado(const char *texto, int16_t y, uint8_t tamanio, uint16_t color) {
  escribirEnCaja(texto, 0, tft.width(), y, tamanio, color);
}

// Escribe un texto que termina en x
void escribirDerecha(const char *texto, int16_t x, int16_t y, uint8_t tamanio, uint16_t color) {
  escribir(texto, x - anchoTexto(texto, tamanio), y, tamanio, color);
}

// ---------- Touch ----------

// Si se está tocando la pantalla, devuelve true y deja en x, y el punto tocado
bool leerToque(int16_t &x, int16_t &y) {
  if (!touch.touched()) return false;
  TS_Point punto = touch.getPoint();
  x = constrain(map(punto.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, tft.width() - 1), 0, tft.width() - 1);
  y = constrain(map(punto.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, tft.height() - 1), 0, tft.height() - 1);
  return true;
}

void esperarSoltar() {
  while (touch.touched()) delay(10);
  delay(50);  // Evita que un rebote del touch cuente como otro toque
}

// Si se está tocando uno de los botones, lo muestra presionado hasta que se suelta el dedo
// y devuelve su número; si no, devuelve -1
int botonTocado(const Boton *botones, int cantidad) {
  int16_t x, y;
  if (!leerToque(x, y)) return -1;
  for (int i = 0; i < cantidad; i++) {
    const Boton &boton = botones[i];
    if (x >= boton.x && x < boton.x + boton.ancho && y >= boton.y && y < boton.y + boton.alto) {
      dibujarBoton(boton, true);
      esperarSoltar();
      dibujarBoton(boton, false);
      return i;
    }
  }
  return -1;
}

// Espera hasta que se toque uno de los botones y devuelve su número
int esperarBoton(const Boton *botones, int cantidad) {
  int boton;
  while ((boton = botonTocado(botones, cantidad)) < 0) delay(10);
  return boton;
}

// Espera hasta que se toque la pantalla o pase el tiempo indicado
void esperarToque(unsigned long tiempo) {
  unsigned long inicio = millis();
  while (millis() - inicio < tiempo) {
    if (touch.touched()) {
      esperarSoltar();
      return;
    }
    delay(10);
  }
}

// ---------- PN532 ----------

// Busca una tarjeta en el lector. Devuelve true solo cuando se acerca una tarjeta, no mientras
// sigue apoyada; deja su UID en "uid" y su largo en "largo"
bool tarjetaNueva(uint8_t *uid, uint8_t &largo) {
  // InListPassiveTarget: busca 1 tarjeta ISO14443A (MIFARE) a 106 kbps
  uint8_t buscar[] = {CMD_IN_LIST_PASSIVE_TARGET, 0x01, 0x00};
  int largoRespuesta = enviarComando("InListPassiveTarget", buscar, sizeof(buscar));
  if (largoRespuesta < 0) {
    Serial.println("El PN532 dejó de responder. Reiniciándolo...");
    detallar = true;
    configurarLector();
    detallar = false;
    return false;
  }

  // Respuesta: cantidad de tarjetas, número de tarjeta, SENS_RES (2 bytes), SEL_RES, largo del UID, UID
  if (largoRespuesta < 6 || respuesta[0] != 1 || respuesta[5] > largoRespuesta - 6 || respuesta[5] > LARGO_MAX_UID) {
    if (lecturasSinTarjeta < LECTURAS_PARA_RETIRAR) lecturasSinTarjeta++;
    if (lecturasSinTarjeta >= LECTURAS_PARA_RETIRAR) largoApoyada = 0;
    return false;
  }
  lecturasSinTarjeta = 0;

  const uint8_t *leido = &respuesta[6];
  if (respuesta[5] == largoApoyada && memcmp(leido, uidApoyada, largoApoyada) == 0) return false;  // Sigue apoyada

  largoApoyada = respuesta[5];
  memcpy(uidApoyada, leido, largoApoyada);
  memcpy(uid, leido, largoApoyada);
  largo = largoApoyada;
  Serial.print("Tarjeta:");
  imprimirBytes(uid, largo);
  return true;
}

// Hace que la tarjeta que está apoyada cuente como recién acercada en la próxima lectura
void olvidarTarjeta() {
  largoApoyada = 0;
}

// Inicia el PN532 mostrando cada paso; si algo falla, lo reintenta cada 1 segundo
void iniciarLector() {
  detallar = true;
  while (!configurarLector()) {
    Serial.println("No se pudo iniciar el PN532. Reintentando...");
    mostrarInicio("Lector sin respuesta", ILI9341_RED);
    delay(1000);
  }
  detallar = false;
}

bool configurarLector() {
  uint8_t firmware[] = {CMD_GET_FIRMWARE_VERSION};
  if (enviarComando("GetFirmwareVersion", firmware, sizeof(firmware)) < 4) return false;
  Serial.print("Chip encontrado: PN5");
  Serial.println(respuesta[0], HEX);
  Serial.print("Firmware: ");
  Serial.print(respuesta[1]);
  Serial.print('.');
  Serial.println(respuesta[2]);

  // SAMConfiguration: modo normal, saca al PN532 del modo de bajo consumo
  uint8_t sam[] = {CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
  if (enviarComando("SAMConfiguration", sam, sizeof(sam)) < 0) return false;

  // RFConfiguration (MaxRetries): busca la tarjeta unas pocas veces y responde, en vez de esperar para siempre
  uint8_t reintentos[] = {CMD_RF_CONFIGURATION, 0x05, 0xFF, 0x01, REINTENTOS_BUSQUEDA};
  return enviarComando("RFConfiguration", reintentos, sizeof(reintentos)) >= 0;
}

// Envía un comando al PN532 y espera el ACK y la respuesta. Deja los datos de la respuesta
// en "respuesta" y devuelve cuántos son, o -1 si falla algún paso
int enviarComando(const char *nombre, const uint8_t *comando, uint8_t largoComando) {
  if (detallar) {
    Serial.print(nombre);
    Serial.println(":");
  }

  // Trama: preámbulo, código de inicio, largo, checksum del largo, TFI (host -> PN532), comando, checksum, postámbulo
  uint8_t trama[8 + largoComando];
  uint8_t largo = largoComando + 1;
  uint8_t suma = 0xD4;
  trama[0] = 0x00;
  trama[1] = 0x00;
  trama[2] = 0xFF;
  trama[3] = largo;
  trama[4] = ~largo + 1;
  trama[5] = 0xD4;
  for (uint8_t i = 0; i < largoComando; i++) {
    trama[6 + i] = comando[i];
    suma += comando[i];
  }
  trama[6 + largoComando] = ~suma + 1;
  trama[7 + largoComando] = 0x00;

  Wire.beginTransmission(DIRECCION_PN532);
  Wire.write(trama, 8 + largoComando);
  if (Wire.endTransmission() != 0) {
    Serial.print("  ");
    Serial.print(nombre);
    Serial.println(": el PN532 no aceptó el comando");
    return -1;
  }
  if (detallar) Serial.println("  Comando enviado");
  delay(PAUSA_BUS);

  // Primero llega el ACK, que confirma que el PN532 recibió el comando
  uint8_t buffer[MAX_DATOS + 9];
  if (!leerTrama(buffer, sizeof(TRAMA_ACK), "ACK")) return -1;
  if (memcmp(buffer, TRAMA_ACK, sizeof(TRAMA_ACK)) != 0) {
    Serial.print("  ACK inválido:");
    imprimirBytes(buffer, sizeof(TRAMA_ACK));
    return -1;
  }

  // Después llega la respuesta: 00 00 FF, largo, checksum del largo, D5, comando + 1, datos, checksum, 00
  if (!leerTrama(buffer, sizeof(buffer), "Respuesta")) return -1;
  uint8_t largoRespuesta = buffer[3];
  if (buffer[0] != 0x00 || buffer[1] != 0x00 || buffer[2] != 0xFF ||
      (uint8_t)(largoRespuesta + buffer[4]) != 0 || buffer[5] != 0xD5 || buffer[6] != comando[0] + 1 ||
      largoRespuesta < 2 || largoRespuesta - 2 > MAX_DATOS) {
    Serial.print("  Respuesta inválida:");
    imprimirBytes(buffer, 12);
    return -1;
  }
  memcpy(respuesta, &buffer[7], largoRespuesta - 2);
  return largoRespuesta - 2;
}

// Lee una trama del PN532. Cada lectura empieza con un byte de estado (bit 0 en 1 = listo);
// mientras no esté lista, espera y vuelve a leer
bool leerTrama(uint8_t *destino, uint8_t largo, const char *nombre) {
  unsigned long inicio = millis();
  int sinContestar = 0;  // Lecturas en las que el PN532 no contestó (NACK)
  int noListo = 0;       // Lecturas en las que contestó que todavía no estaba listo

  while (millis() - inicio < TIMEOUT_TRAMA) {
    if (Wire.requestFrom((uint8_t)DIRECCION_PN532, (uint8_t)(largo + 1)) == largo + 1) {
      if (Wire.read() & 0x01) {
        for (uint8_t i = 0; i < largo; i++) destino[i] = Wire.read();
        if (detallar) {
          Serial.printf("  %s llegó a los %lu ms (lecturas sin contestar: %d, no listo: %d)\n",
                        nombre, millis() - inicio, sinContestar, noListo);
        }
        delay(PAUSA_BUS);
        return true;
      }
      noListo++;
    } else {
      sinContestar++;
    }
    delay(PAUSA_BUS);
  }

  Serial.printf("  %s no llegó en %lu ms (lecturas sin contestar: %d, no listo: %d)\n",
                nombre, millis() - inicio, sinContestar, noListo);
  return false;
}

void imprimirBytes(const uint8_t *datos, uint8_t largo) {
  for (uint8_t i = 0; i < largo; i++) {
    Serial.print(datos[i] < 0x10 ? " 0" : " ");
    Serial.print(datos[i], HEX);
  }
  Serial.println();
}
