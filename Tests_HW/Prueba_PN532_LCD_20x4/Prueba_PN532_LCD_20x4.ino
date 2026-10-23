#include <Wire.h>
#include "esp_log.h"

// Prueba del lector PN532 y el display LCD 20x4 juntos, en el mismo bus I2C: SDA -> GPIO 21, SCL -> GPIO 22
// (el LCD a través del conversor de nivel). Muestra en el LCD el tipo de cada tarjeta que se acerca, su UID
// completo, el ATQA y el SAK, y cuántas lecturas van. Por serie da más detalle

// PN532: se habla directamente con Wire, sin librería, dejando una pausa entre transacciones I2C
#define DIRECCION_PN532 0x24

#define PAUSA_BUS           5     // ms de pausa entre transacciones I2C
#define TIMEOUT_TRAMA       1000  // ms máximos de espera de cada trama del PN532
#define REINTENTOS_BUSQUEDA 0x05  // Veces que el PN532 busca una tarjeta antes de responder "no hay tarjeta"
#define INTERVALO_AVISO     2000  // ms entre avisos de "sin tarjeta"
#define MAX_DATOS           32    // Máximo de datos que se leen de una respuesta
#define TIEMPO_UID          2000  // ms que se muestra el UID en el LCD

// Códigos de comando del PN532
#define CMD_GET_FIRMWARE_VERSION   0x02
#define CMD_SAM_CONFIGURATION      0x14
#define CMD_RF_CONFIGURATION       0x32
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A

// LCD: adaptador PCF8574, también sin librería (el bus va a 100 kHz porque el PCF8574 no admite más)
// Pines del PCF8574 en el adaptador: P0 = RS, P1 = RW, P2 = E, P3 = luz de fondo, P4 a P7 = D4 a D7 del LCD
#define LCD_RS  0x01
#define LCD_E   0x04
#define LCD_LUZ 0x08

#define LCD_COLUMNAS 20
#define LCD_FILAS    4

// Comandos del controlador HD44780 del LCD
#define LCD_BORRAR       0x01
#define LCD_MODO_ENTRADA 0x06  // El cursor avanza a la derecha después de cada letra
#define LCD_APAGAR       0x08
#define LCD_ENCENDER     0x0C  // Display encendido, cursor oculto
#define LCD_FUNCION      0x28  // Bus de 4 bits, 2 líneas (el de 4 filas son 2 líneas partidas), letras de 5x8 puntos
#define LCD_DIR_DDRAM    0x80  // + dirección: memoria de lo que se ve

// Dirección en la DDRAM del comienzo de cada fila: la fila 3 es la continuación de la 1, y la 4 la de la 2
const uint8_t FILAS_DDRAM[LCD_FILAS] = {0x00, 0x40, 0x14, 0x54};

// Direcciones posibles del adaptador: PCF8574 (0x20 a 0x27) o PCF8574A (0x38 a 0x3F), las más comunes primero.
// No está la 0x24, que es la del PN532
const uint8_t DIRECCIONES_LCD[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26,
                                   0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};

const uint8_t TRAMA_ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

uint8_t respuesta[MAX_DATOS];  // Datos de la última respuesta del PN532
bool detallar = true;          // Imprime cada paso de la comunicación (mientras se inicia el lector)
unsigned long ultimoAviso = 0;
char versionLector[LCD_COLUMNAS + 1];  // Para la pantalla de espera
unsigned long lecturas = 0;            // Tarjetas leídas desde el arranque
uint8_t direccionLcd = 0;              // 0 = no se encontró
bool lcdResponde = true;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Prueba de lector PN532 con LCD 20x4 en ESP32");

  Wire.begin();  // SDA 21, SCL 22, a 100 kHz
  delay(50);     // El LCD necesita 40 ms desde que se alimenta antes de recibir comandos

  for (uint8_t direccion : DIRECCIONES_LCD) {
    Wire.beginTransmission(direccion);
    if (Wire.endTransmission() == 0) {
      direccionLcd = direccion;
      break;
    }
  }
  if (direccionLcd == 0) {
    Serial.println("No se encontró el LCD: la prueba sigue solo por serie");
  } else {
    Serial.printf("LCD en 0x%02X\n", direccionLcd);
    if (!iniciarLcd()) Serial.println("El LCD no aceptó algún comando de arranque");
  }
  mostrar("PN532 + LCD 20x4", "", "", "Iniciando lector...");

  delay(500);  // Da tiempo al PN532 a terminar de arrancar

  // Si el PN532 quedó ejecutando un comando anterior (por ejemplo, si se reinició solo el ESP32),
  // una trama ACK lo cancela
  Wire.beginTransmission(DIRECCION_PN532);
  Wire.write(TRAMA_ACK, sizeof(TRAMA_ACK));
  Wire.endTransmission();
  delay(50);

  iniciarLector();
  Serial.println("Acercá una tarjeta...");
  mostrarEspera();
}

void loop() {
  // InListPassiveTarget: busca 1 tarjeta ISO14443A (MIFARE) a 106 kbps
  uint8_t buscar[] = {CMD_IN_LIST_PASSIVE_TARGET, 0x01, 0x00};
  int largo = enviarComando("InListPassiveTarget", buscar, sizeof(buscar));

  if (largo < 0) {
    Serial.println("El PN532 dejó de responder. Reiniciándolo...");
    iniciarLector();
    mostrarEspera();
    return;
  }

  // Respuesta: cantidad de tarjetas, número de tarjeta, SENS_RES (2 bytes), SEL_RES, largo del UID, UID
  if (largo >= 6 && respuesta[0] == 1 && respuesta[5] <= largo - 6) {
    mostrarTarjeta(&respuesta[2], respuesta[4], &respuesta[6], respuesta[5]);
    delay(TIEMPO_UID);  // Deja el UID en el LCD y evita leer la misma tarjeta muchas veces seguidas
    mostrarEspera();
  } else if (millis() - ultimoAviso >= INTERVALO_AVISO) {
    ultimoAviso = millis();
    Serial.println("PN532 responde. Sin tarjeta.");
  }
}

// ---------- Tarjetas ----------

void mostrarEspera() {
  mostrar("PN532 + LCD 20x4", versionLector, "", "Acerque tarjeta");
}

// Fila 1: tipo de tarjeta. Fila 2: UID completo. Fila 3: ATQA y SAK. Fila 4: lecturas, o aviso de UID aleatorio
void mostrarTarjeta(const uint8_t *atqa, uint8_t sak, const uint8_t *uid, uint8_t largo) {
  lecturas++;
  const char *tipo = tipoTarjeta(sak, uid, largo);
  Serial.printf("Tarjeta detectada: %s (ATQA %02X%02X, SAK %02X). UID (%u bytes):", tipo, atqa[0], atqa[1], sak, largo);
  imprimirBytes(uid, largo);
  if (uidAleatorio(uid, largo)) {
    Serial.println("  El UID es aleatorio: cambia en cada lectura, así que no sirve para identificar");
    Serial.println("  (pasa con los celulares y las tarjetas bancarias)");
  }

  char linea2[LCD_COLUMNAS + 1];
  char linea3[LCD_COLUMNAS + 1];
  char linea4[LCD_COLUMNAS + 1];
  uidEnTexto(uid, largo, linea2);
  snprintf(linea3, sizeof(linea3), "ATQA %02X%02X  SAK %02X", atqa[0], atqa[1], sak);
  if (uidAleatorio(uid, largo)) {
    strcpy(linea4, "Cambia cada lectura");
  } else {
    snprintf(linea4, sizeof(linea4), "%uB  Lecturas: %lu", largo, lecturas);
  }
  mostrar(tipo, linea2, linea3, linea4);
}

// Tipo de tarjeta según el SAK (SEL_RES), con hasta 20 letras para que entre en una fila del LCD
const char *tipoTarjeta(uint8_t sak, const uint8_t *uid, uint8_t largo) {
  if (uidAleatorio(uid, largo)) return "UID aleatorio";
  if (sak & 0x20) return "ISO 14443-4";  // DESFire, tarjetas bancarias, celulares
  switch (sak) {
    case 0x00: return "NTAG / Ultralight";
    case 0x08:
    case 0x88: return "MIFARE Classic 1K";
    case 0x09: return "MIFARE Mini";
    case 0x18: return "MIFARE Classic 4K";
    default:   return "Otro tipo";
  }
}

// Un UID de 4 bytes que empieza con 0x08 es aleatorio (ISO 14443-3): cambia cada vez que se acerca
bool uidAleatorio(const uint8_t *uid, uint8_t largo) {
  return largo == 4 && uid[0] == 0x08;
}

// Escribe el UID en hexadecimal: con espacios si entra en una fila (hasta 7 bytes), sin espacios si no
// (los de 10 bytes ocupan justo las 20 columnas)
void uidEnTexto(const uint8_t *uid, uint8_t largo, char *texto) {
  bool espacios = largo <= 7;
  char *p = texto;
  for (uint8_t i = 0; i < largo && p - texto + 2 <= LCD_COLUMNAS; i++) {
    if (espacios && i > 0) *p++ = ' ';
    p += sprintf(p, "%02X", uid[i]);
  }
  *p = '\0';
}

// ---------- PN532 ----------

// Inicia el PN532 mostrando cada paso; si algo falla, lo reintenta cada 1 segundo
void iniciarLector() {
  detallar = true;
  while (!configurarLector()) {
    Serial.println("No se pudo iniciar el PN532. Reintentando...");
    mostrar("Lector NFC", "sin respuesta", "", "Reintentando...");
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
  snprintf(versionLector, sizeof(versionLector), "PN5%02X firmware %u.%u", respuesta[0], respuesta[1], respuesta[2]);

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

// ---------- LCD ----------

// Muestra las 4 filas en el LCD y avisa por serie si deja de responder o vuelve
void mostrar(const char *linea1, const char *linea2, const char *linea3, const char *linea4) {
  if (direccionLcd == 0) return;
  const char *lineas[LCD_FILAS] = {linea1, linea2, linea3, linea4};
  bool ok = true;
  for (uint8_t fila = 0; fila < LCD_FILAS && ok; fila++) ok = escribirLcd(fila, lineas[fila]);
  if (ok != lcdResponde) {
    lcdResponde = ok;
    Serial.println(ok ? "El LCD volvió a responder" : "El LCD no respondió");
    if (ok && iniciarLcd()) {  // Si se cortó la alimentación, el LCD necesita arrancar de nuevo
      for (uint8_t fila = 0; fila < LCD_FILAS; fila++) escribirLcd(fila, lineas[fila]);
    }
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
  ok &= comandoLcd(LCD_ENCENDER);
  return ok;
}

// Escribe una fila entera (0 a 3), completando con espacios lo que falte
bool escribirLcd(uint8_t fila, const char *texto) {
  uint8_t linea[LCD_COLUMNAS];
  uint8_t i = 0;
  for (; i < LCD_COLUMNAS && texto[i] != '\0'; i++) linea[i] = texto[i];
  for (; i < LCD_COLUMNAS; i++) linea[i] = ' ';
  return comandoLcd(LCD_DIR_DDRAM | FILAS_DDRAM[fila]) && enviarLcd(linea, LCD_COLUMNAS, LCD_RS);
}

bool comandoLcd(uint8_t comando) {
  return enviarLcd(&comando, 1, 0);
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
