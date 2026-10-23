#include <Wire.h>
#include <Preferences.h>
#include "esp_log.h"

// Control de acceso con tarjetas NFC: un lector PN532 y un display LCD 16x2 con adaptador I2C, los dos en el
// mismo bus I2C, y un relé para la cerradura. No tiene teclado: se maneja acercando tarjetas. Las tarjetas
// habilitadas se guardan en la memoria del ESP32, así que sobreviven a un corte de luz.
// La configuración más completa se va a hacer desde el celular; mientras tanto se hace con comandos por el
// monitor serie (ver ejecutarComando()).
//
// Uso:
// - Con la memoria vacía, la primera tarjeta que se acerca queda como tarjeta maestra.
// - Una tarjeta habilitada abre la cerradura durante TIEMPO_APERTURA.
// - La maestra no abre: pasa al modo ALTA, en el que cada tarjeta nueva que se acerca queda habilitada.
//   Acercándola otra vez se pasa al modo BAJA (cada tarjeta que se acerca se deshabilita), y una vez más se
//   vuelve al modo normal. Si no se acerca ninguna tarjeta durante TIEMPO_ADMIN, también se vuelve.
// - Mantener apretado el botón BOOT durante TIEMPO_BORRADO borra todas las tarjetas, maestra incluida.

// ---------- Configuración ----------
#define MAX_TARJETAS    100
#define LARGO_NOMBRE    16     // Lo que entra en una línea del LCD
#define TIEMPO_APERTURA 3000   // ms que el relé queda activado
#define TIEMPO_MENSAJE  2500   // ms que queda en el LCD el resultado de una lectura
#define TIEMPO_ADMIN    15000  // ms sin tarjetas para salir solo de los modos ALTA y BAJA
#define TIEMPO_BORRADO  5000   // ms que hay que mantener apretado BOOT para borrar todas las tarjetas

// ---------- Hardware ----------
// Bus I2C compartido por el PN532 y el LCD: SDA -> GPIO 21, SCL -> GPIO 22 (pines I2C por defecto del ESP32)
// Se habla con los dos directamente con Wire, sin librerías. El bus va a 100 kHz porque el PCF8574 no admite más

// Relé de la cerradura
#define PIN_RELE    17
#define RELE_ACTIVO HIGH  // Muchos módulos de relé se activan con LOW: en ese caso, cambiarlo

#define PIN_BOOT 0  // Botón BOOT de la placa (apretado = LOW)

#define INTERVALO_RECONEXION 2000  // ms entre intentos de volver a iniciar el lector o el LCD si no responden

// PN532: se deja una pausa entre transacciones I2C
#define DIRECCION_PN532 0x24

#define PAUSA_BUS             5     // ms de pausa entre transacciones I2C
#define TIMEOUT_TRAMA         1000  // ms máximos de espera de cada trama del PN532
#define REINTENTOS_BUSQUEDA   0x05  // Veces que el PN532 busca una tarjeta antes de responder "no hay tarjeta"
#define MAX_DATOS             32    // Máximo de datos que se leen de una respuesta
#define LARGO_MAX_UID         10    // Los UID de las tarjetas MIFARE tienen 4, 7 o 10 bytes
#define LECTURAS_PARA_RETIRAR 2     // Lecturas seguidas sin tarjeta para dar por retirada la que estaba apoyada

// Códigos de comando del PN532
#define CMD_GET_FIRMWARE_VERSION   0x02
#define CMD_SAM_CONFIGURATION      0x14
#define CMD_RF_CONFIGURATION       0x32
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A

// LCD: pines del PCF8574 en el adaptador: P0 = RS, P1 = RW, P2 = E, P3 = luz de fondo, P4 a P7 = D4 a D7
#define LCD_RS  0x01
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

#define VERSION_DATOS 1  // Cambiarla si cambia la estructura Datos, para no cargar datos guardados con otra forma

enum { ROL_USUARIO, ROL_MAESTRA };
enum Modo { MODO_NORMAL, MODO_ALTA, MODO_BAJA };
const char *NOMBRES_MODO[] = {
  "normal",
  "ALTA (cada tarjeta nueva que se acerque queda habilitada)",
  "BAJA (cada tarjeta que se acerque se deshabilita)",
};

struct Tarjeta {
  uint8_t uid[LARGO_MAX_UID];
  uint8_t largoUid;
  uint8_t rol;
  char nombre[LARGO_NOMBRE + 1];
};

// Todo lo que se guarda en la memoria del ESP32
struct Datos {
  uint8_t version;
  uint8_t cantidad;
  uint16_t numeroSiguiente;  // Para el nombre de la próxima tarjeta ("Tarjeta 7")
  Tarjeta tarjetas[MAX_TARJETAS];
};

const uint8_t TRAMA_ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

Preferences memoria;  // Memoria no volátil (NVS) del ESP32
Datos datos;

Modo modo = MODO_NORMAL;
unsigned long ultimaActividad = 0;  // Última tarjeta acercada, para salir solo de los modos ALTA y BAJA
bool hayMensaje = false;            // Si el LCD muestra el resultado de una lectura en vez de la pantalla del modo
unsigned long inicioMensaje = 0;
bool puertaAbierta = false;
unsigned long inicioApertura = 0;
char lineaSerie[64];  // Comando que se está recibiendo por serie
uint8_t largoSerie = 0;

// PN532
uint8_t respuesta[MAX_DATOS];       // Datos de la última respuesta del PN532
bool detallar = true;               // Imprime cada paso de la comunicación (mientras se inicia el lector)
bool lectorListo = false;
unsigned long ultimoIntentoLector = 0;
uint8_t uidApoyada[LARGO_MAX_UID];  // Tarjeta que está sobre el lector, para no leerla dos veces
uint8_t largoApoyada = 0;           // 0 = no hay ninguna apoyada
uint8_t lecturasSinTarjeta = 0;

// LCD
uint8_t direccionLcd = 0;  // 0 = no se encontró
bool lcdListo = false;
unsigned long ultimoIntentoLcd = 0;
char lineasLcd[2][LCD_COLUMNAS + 1];  // Lo que muestra el LCD, para no reescribir lo que no cambió

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Control de acceso");

  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, !RELE_ACTIVO);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  Wire.begin();  // SDA 21, SCL 22, a 100 kHz
  delay(50);     // El LCD necesita 40 ms desde que se alimenta antes de recibir comandos
  conectarLcd();
  mostrar("Control acceso", "Iniciando...");

  cargarDatos();

  delay(500);  // Da tiempo al PN532 a terminar de arrancar

  // Si el PN532 quedó ejecutando un comando anterior (por ejemplo, si se reinició solo el ESP32),
  // una trama ACK lo cancela
  Wire.beginTransmission(DIRECCION_PN532);
  Wire.write(TRAMA_ACK, sizeof(TRAMA_ACK));
  Wire.endTransmission();
  delay(50);

  conectarLector();
  Serial.println("Comandos por serie: lista, nombre N texto, borrar N, abrir");
}

void loop() {
  revisarBotonBorrado();
  revisarSerie();
  revisarPuerta();

  if (modo != MODO_NORMAL && millis() - ultimaActividad >= TIEMPO_ADMIN) {
    modo = MODO_NORMAL;
    Serial.println("Modo normal (pasó el tiempo sin tarjetas)");
  }

  uint8_t uid[LARGO_MAX_UID];
  uint8_t largo;
  if (tarjetaNueva(uid, largo)) procesarTarjeta(uid, largo);

  actualizarPantalla();
}

// ---------- Acceso ----------

// Lo que pasa al acercar una tarjeta, según el modo
void procesarTarjeta(const uint8_t *uid, uint8_t largo) {
  ultimaActividad = millis();

  // Los celulares y las tarjetas bancarias mandan un UID distinto cada vez: no sirven para identificar
  if (uidAleatorio(uid, largo)) {
    Serial.println("UID aleatorio: no sirve para identificar");
    mostrarMensaje(modo == MODO_NORMAL ? SIMBOLO_NO " Denegado" : SIMBOLO_NO " No se puede", "UID aleatorio");
    return;
  }

  if (datos.cantidad == 0) {
    agregarTarjeta(uid, largo, ROL_MAESTRA);
    mostrarMensaje(SIMBOLO_SI " Maestra", "registrada");
    return;
  }

  int indice = buscarTarjeta(uid, largo);
  if (indice >= 0 && datos.tarjetas[indice].rol == ROL_MAESTRA) {
    modo = modo == MODO_NORMAL ? MODO_ALTA : modo == MODO_ALTA ? MODO_BAJA : MODO_NORMAL;
    Serial.printf("Modo %s\n", NOMBRES_MODO[modo]);
    hayMensaje = false;  // El cambio se ve en la pantalla del modo
    return;
  }

  switch (modo) {
    case MODO_NORMAL:
      if (indice >= 0) {
        Serial.printf("Acceso permitido: %s\n", datos.tarjetas[indice].nombre);
        abrirPuerta();
        mostrarMensaje(SIMBOLO_SI " Puede pasar", datos.tarjetas[indice].nombre);
      } else {
        Serial.println("Acceso denegado: tarjeta no habilitada");
        mostrarMensaje(SIMBOLO_NO " Denegado", "No habilitada");
      }
      break;

    case MODO_ALTA:
      if (indice >= 0) {
        mostrarMensaje("Ya habilitada", datos.tarjetas[indice].nombre);
      } else if (datos.cantidad >= MAX_TARJETAS) {
        Serial.println("No se puede habilitar: la memoria está llena");
        mostrarMensaje(SIMBOLO_NO " Memoria llena", "");
      } else {
        indice = agregarTarjeta(uid, largo, ROL_USUARIO);
        mostrarMensaje(SIMBOLO_SI " Habilitada", datos.tarjetas[indice].nombre);
      }
      break;

    case MODO_BAJA:
      if (indice < 0) {
        mostrarMensaje("No habilitada", "");
      } else {
        char nombre[LARGO_NOMBRE + 1];
        strcpy(nombre, datos.tarjetas[indice].nombre);
        quitarTarjeta(indice);
        mostrarMensaje(SIMBOLO_NO " Deshabilitada", nombre);
      }
      break;
  }
}

void abrirPuerta() {
  digitalWrite(PIN_RELE, RELE_ACTIVO);
  puertaAbierta = true;
  inicioApertura = millis();
}

// Cierra la cerradura cuando pasa TIEMPO_APERTURA
void revisarPuerta() {
  if (puertaAbierta && millis() - inicioApertura >= TIEMPO_APERTURA) {
    digitalWrite(PIN_RELE, !RELE_ACTIVO);
    puertaAbierta = false;
  }
}

// Mantener apretado BOOT borra todas las tarjetas. Mientras está apretado, el LCD muestra cuánto falta
void revisarBotonBorrado() {
  if (digitalRead(PIN_BOOT) != LOW) return;

  Serial.println("Botón BOOT apretado: mantenelo para borrar todas las tarjetas");
  unsigned long inicio = millis();
  while (digitalRead(PIN_BOOT) == LOW) {
    unsigned long pasado = millis() - inicio;
    if (pasado >= TIEMPO_BORRADO) {
      borrarTodo();
      mostrarMensaje(SIMBOLO_SI " Todo borrado", "Suelte el boton");
      while (digitalRead(PIN_BOOT) == LOW) delay(10);
      return;
    }
    char linea1[LCD_COLUMNAS + 1];
    snprintf(linea1, sizeof(linea1), "Borrar todo: %lus", (TIEMPO_BORRADO - pasado + 999) / 1000);
    mostrar(linea1, "Suelte: cancela");
    revisarPuerta();
    delay(20);
  }
  Serial.println("Borrado cancelado");
}

// ---------- Tarjetas habilitadas ----------

int buscarTarjeta(const uint8_t *uid, uint8_t largo) {
  for (uint8_t i = 0; i < datos.cantidad; i++) {
    if (datos.tarjetas[i].largoUid == largo && memcmp(datos.tarjetas[i].uid, uid, largo) == 0) return i;
  }
  return -1;
}

// Un UID de 4 bytes que empieza con 0x08 es aleatorio (ISO 14443-3): cambia cada vez que se acerca
bool uidAleatorio(const uint8_t *uid, uint8_t largo) {
  return largo == 4 && uid[0] == 0x08;
}

// Agrega la tarjeta al final de la lista y devuelve su posición
uint8_t agregarTarjeta(const uint8_t *uid, uint8_t largo, uint8_t rol) {
  Tarjeta &tarjeta = datos.tarjetas[datos.cantidad];
  memset(&tarjeta, 0, sizeof(Tarjeta));
  memcpy(tarjeta.uid, uid, largo);
  tarjeta.largoUid = largo;
  tarjeta.rol = rol;
  if (rol == ROL_MAESTRA) {
    strcpy(tarjeta.nombre, "Maestra");
  } else {
    snprintf(tarjeta.nombre, sizeof(tarjeta.nombre), "Tarjeta %u", datos.numeroSiguiente++);
  }
  datos.cantidad++;
  guardarDatos();

  Serial.printf("Habilitada: %s, UID:", tarjeta.nombre);
  imprimirBytes(uid, largo);
  return datos.cantidad - 1;
}

void quitarTarjeta(uint8_t indice) {
  Serial.printf("Deshabilitada: %s\n", datos.tarjetas[indice].nombre);
  memmove(&datos.tarjetas[indice], &datos.tarjetas[indice + 1], (datos.cantidad - indice - 1) * sizeof(Tarjeta));
  datos.cantidad--;
  guardarDatos();
}

void borrarTodo() {
  vaciarDatos();
  guardarDatos();
  modo = MODO_NORMAL;
  Serial.println("Se borraron todas las tarjetas. La próxima que se acerque queda como maestra");
}

void vaciarDatos() {
  memset(&datos, 0, sizeof(Datos));
  datos.version = VERSION_DATOS;
  datos.numeroSiguiente = 1;
}

void cargarDatos() {
  memoria.begin("acceso", false);
  if (memoria.isKey("datos") && memoria.getBytesLength("datos") == sizeof(Datos)) {
    memoria.getBytes("datos", &datos, sizeof(Datos));
    if (datos.version == VERSION_DATOS && datos.cantidad <= MAX_TARJETAS) {
      imprimirTarjetas();
      return;
    }
  }

  Serial.println("No hay tarjetas guardadas: la primera que se acerque queda como maestra");
  vaciarDatos();
}

void guardarDatos() {
  if (memoria.putBytes("datos", &datos, sizeof(Datos)) != sizeof(Datos)) {
    Serial.println("Error al guardar las tarjetas");
  }
}

void imprimirTarjetas() {
  Serial.printf("Tarjetas habilitadas: %u de %u\n", datos.cantidad, MAX_TARJETAS);
  for (uint8_t i = 0; i < datos.cantidad; i++) {
    const Tarjeta &tarjeta = datos.tarjetas[i];
    Serial.printf("  %3u. %-16s %s UID:", i + 1, tarjeta.nombre, tarjeta.rol == ROL_MAESTRA ? "(maestra)" : "         ");
    imprimirBytes(tarjeta.uid, tarjeta.largoUid);
  }
}

// Copia un nombre dejando solo lo que el LCD puede mostrar: las vocales con acento y la ñ quedan sin acento,
// y se descarta cualquier otra letra que no sea ASCII
void copiarNombre(char *destino, const char *origen) {
  // Segundo byte en UTF-8 (el primero es 0xC3) de á é í ó ú ü ñ Á É Í Ó Ú Ü Ñ
  const char CON_ACENTO[] = "\xA1\xA9\xAD\xB3\xBA\xBC\xB1\x81\x89\x8D\x93\x9A\x9C\x91";
  const char SIN_ACENTO[] = "aeiouunAEIOUUN";
  uint8_t largo = 0;
  for (const char *c = origen; *c != '\0' && largo < LARGO_NOMBRE; c++) {
    if (*c >= ' ' && *c <= '~') {
      destino[largo++] = *c;
    } else if ((uint8_t)*c == 0xC3 && c[1] != '\0') {
      const char *letra = strchr(CON_ACENTO, c[1]);
      if (letra != nullptr) destino[largo++] = SIN_ACENTO[letra - CON_ACENTO];
      c++;
    }
  }
  while (largo > 0 && destino[largo - 1] == ' ') largo--;  // Sin espacios al final
  destino[largo] = '\0';
}

// ---------- Comandos por serie ----------

// Junta lo que llega por serie hasta el fin de línea (en el monitor serie, elegir "Nueva línea")
void revisarSerie() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineaSerie[largoSerie] = '\0';
      largoSerie = 0;
      ejecutarComando(lineaSerie);
    } else if (largoSerie < sizeof(lineaSerie) - 1) {
      lineaSerie[largoSerie++] = c;
    }
  }
}

// lista            muestra las tarjetas habilitadas
// nombre N texto   le cambia el nombre a la tarjeta N de la lista
// borrar N         deshabilita la tarjeta N (la maestra no)
// abrir            abre la cerradura
void ejecutarComando(char *linea) {
  char *orden = strtok(linea, " ");
  if (orden == nullptr) return;
  char *resto = strtok(nullptr, "");

  if (strcmp(orden, "lista") == 0) {
    imprimirTarjetas();
  } else if (strcmp(orden, "abrir") == 0) {
    Serial.println("Apertura por serie");
    abrirPuerta();
    mostrarMensaje(SIMBOLO_SI " Puede pasar", "");
  } else if (strcmp(orden, "nombre") == 0) {
    char *texto;
    int indice = leerNumeroTarjeta(resto, &texto);
    if (indice < 0) return;
    char nombre[LARGO_NOMBRE + 1];
    while (*texto == ' ') texto++;
    copiarNombre(nombre, texto);
    if (nombre[0] == '\0') {
      Serial.println("Falta el nombre: nombre N texto");
      return;
    }
    strcpy(datos.tarjetas[indice].nombre, nombre);
    guardarDatos();
    Serial.printf("Tarjeta %d: %s\n", indice + 1, nombre);
  } else if (strcmp(orden, "borrar") == 0) {
    char *texto;
    int indice = leerNumeroTarjeta(resto, &texto);
    if (indice < 0) return;
    if (datos.tarjetas[indice].rol == ROL_MAESTRA) {
      Serial.println("La maestra no se puede borrar. Para cambiarla, borrá todo con el botón BOOT");
      return;
    }
    quitarTarjeta(indice);
  } else {
    Serial.println("Comandos: lista, nombre N texto, borrar N, abrir");
  }
}

// Lee el número de tarjeta (como en "lista") al principio del texto. Devuelve su posición, o -1 si no es válido;
// deja en "fin" lo que sigue después del número
int leerNumeroTarjeta(char *texto, char **fin) {
  long numero = 0;
  if (texto != nullptr) numero = strtol(texto, fin, 10);
  if (numero < 1 || numero > datos.cantidad) {
    Serial.println("Número de tarjeta inválido (ver \"lista\")");
    return -1;
  }
  return numero - 1;
}

// ---------- Pantalla ----------

void mostrarMensaje(const char *linea1, const char *linea2) {
  mostrar(linea1, linea2);
  hayMensaje = true;
  inicioMensaje = millis();
}

// Pantalla del modo actual, cuando no hay un mensaje a la vista
void actualizarPantalla() {
  if (hayMensaje && millis() - inicioMensaje < TIEMPO_MENSAJE) return;
  hayMensaje = false;

  if (!lectorListo) {
    mostrar("Lector NFC", "sin respuesta");
  } else if (datos.cantidad == 0) {
    mostrar("Sin tarjetas", "Acerque maestra");
  } else if (modo == MODO_NORMAL) {
    mostrar("Acerque tarjeta", puertaAbierta ? SIMBOLO_ABIERTO " Abierto" : SIMBOLO_CERRADO " Cerrado");
  } else {
    unsigned long pasado = millis() - ultimaActividad;
    unsigned long quedan = pasado >= TIEMPO_ADMIN ? 0 : (TIEMPO_ADMIN - pasado + 999) / 1000;
    char linea1[LCD_COLUMNAS + 1];
    snprintf(linea1, sizeof(linea1), "%-13s%2lus", modo == MODO_ALTA ? "MODO ALTA" : "MODO BAJA", quedan);
    // La segunda línea alterna entre la instrucción y lo que pasa si se acerca la maestra
    bool instruccion = (millis() / 2000) % 2 == 0;
    mostrar(linea1, instruccion ? "Acerque tarjeta" : modo == MODO_ALTA ? "Maestra = baja" : "Maestra = salir");
  }
}

// ---------- LCD ----------

// Busca el adaptador del LCD y lo inicia
void conectarLcd() {
  static bool avisado = false;  // Para no repetir el aviso en cada intento
  ultimoIntentoLcd = millis();
  direccionLcd = 0;
  for (uint8_t direccion : DIRECCIONES_LCD) {
    Wire.beginTransmission(direccion);
    if (Wire.endTransmission() == 0) {
      direccionLcd = direccion;
      break;
    }
  }
  lcdListo = direccionLcd != 0 && iniciarLcd();
  lineasLcd[0][0] = lineasLcd[1][0] = '\0';  // Iniciarlo lo deja en blanco

  if (lcdListo) {
    Serial.printf("LCD en 0x%02X\n", direccionLcd);
    avisado = false;
  } else if (!avisado) {
    Serial.println("No se encontró el LCD. Se reintenta cada 2 segundos");
    avisado = true;
  }
}

// Muestra dos líneas en el LCD. Solo reescribe las que cambiaron; si el LCD no responde, lo vuelve a iniciar
void mostrar(const char *linea1, const char *linea2) {
  if (!lcdListo) {
    if (millis() - ultimoIntentoLcd < INTERVALO_RECONEXION) return;
    conectarLcd();
    if (!lcdListo) return;
  }
  const char *lineas[2] = {linea1, linea2};
  for (uint8_t fila = 0; fila < 2; fila++) {
    if (strncmp(lineas[fila], lineasLcd[fila], LCD_COLUMNAS) == 0) continue;
    if (!escribirLcd(fila, lineas[fila])) {
      Serial.println("El LCD dejó de responder");
      lcdListo = false;
      return;
    }
    strlcpy(lineasLcd[fila], lineas[fila], sizeof(lineasLcd[fila]));
  }
}

// Secuencia de arranque del HD44780 en modo de 4 bits
bool iniciarLcd() {
  bool ok = escribirAdaptador(LCD_LUZ);
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

// Manda bytes al LCD en una sola transacción I2C: comandos (rs = 0) o letras (rs = LCD_RS), hasta 31 bytes.
// Cada byte va en dos mitades de 4 bits, y el LCD toma cada mitad cuando E baja
bool enviarLcd(const uint8_t *bytes, uint8_t cantidad, uint8_t rs) {
  if (direccionLcd == 0) return false;
  uint8_t base = rs | LCD_LUZ;
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
  uint8_t valor = (mitad << 4) | LCD_LUZ;
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

// ---------- PN532 ----------

// Busca una tarjeta en el lector. Devuelve true solo cuando se acerca una tarjeta, no mientras
// sigue apoyada; deja su UID en "uid" y su largo en "largo"
bool tarjetaNueva(uint8_t *uid, uint8_t &largo) {
  if (!lectorListo) {
    if (millis() - ultimoIntentoLector >= INTERVALO_RECONEXION) conectarLector();
    return false;
  }

  // InListPassiveTarget: busca 1 tarjeta ISO14443A (MIFARE) a 106 kbps
  uint8_t buscar[] = {CMD_IN_LIST_PASSIVE_TARGET, 0x01, 0x00};
  int largoRespuesta = enviarComando("InListPassiveTarget", buscar, sizeof(buscar));
  if (largoRespuesta < 0) {
    Serial.println("El PN532 dejó de responder");
    lectorListo = false;
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

// Inicia el PN532 mostrando cada paso
void conectarLector() {
  ultimoIntentoLector = millis();
  detallar = true;
  lectorListo = configurarLector();
  detallar = false;
  Serial.println(lectorListo ? "Lector listo" : "No se pudo iniciar el PN532. Se reintenta cada 2 segundos");
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
