#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "esp_log.h"

// PN532 por I2C: SDA -> GPIO 21, SCL -> GPIO 22 (pines I2C por defecto del ESP32)
// Se habla con el PN532 directamente con Wire, sin librería, dejando una pausa entre transacciones I2C
#define DIRECCION_PN532 0x24

// Display TFT ILI9341 240x320 por SPI: SCK -> GPIO 18, SDI (MOSI) -> GPIO 23, SDO (MISO) -> GPIO 19
#define TFT_CS  5
#define TFT_DC  4
#define TFT_RST 15
#define VELOCIDAD_SPI_TFT 10000000  // 10 MHz, seguro con cables de protoboard (la librería usa 40 MHz por defecto)

#define PAUSA_BUS           5     // ms de pausa entre transacciones I2C
#define TIMEOUT_TRAMA       1000  // ms máximos de espera de cada trama del PN532
#define REINTENTOS_BUSQUEDA 0x05  // Veces que el PN532 busca una tarjeta antes de responder "no hay tarjeta"
#define INTERVALO_AVISO     2000  // ms entre avisos de "sin tarjeta"
#define MAX_DATOS           32    // Máximo de datos que se leen de una respuesta
#define TIEMPO_UID          2000  // ms que se muestra el UID en pantalla

// Códigos de comando del PN532
#define CMD_GET_FIRMWARE_VERSION   0x02
#define CMD_SAM_CONFIGURATION      0x14
#define CMD_RF_CONFIGURATION       0x32
#define CMD_IN_LIST_PASSIVE_TARGET 0x4A

// Zonas de la pantalla (horizontal, 320x240)
#define ALTO_TITULO 40

const uint8_t TRAMA_ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);  // Instancia del display con SPI por hardware

uint8_t respuesta[MAX_DATOS];  // Datos de la última respuesta del PN532
bool detallar = true;          // Imprime cada paso de la comunicación (mientras se inicia el lector)
unsigned long ultimoAviso = 0;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // Los errores del driver I2C se reemplazan por los mensajes de este sketch
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Prueba de lector PN532 con display en ESP32");

  iniciarDisplay();
  mostrarMensaje("Iniciando lector...", ILI9341_WHITE);

  Wire.begin();  // SDA 21, SCL 22
  delay(500);    // Da tiempo al PN532 a terminar de arrancar

  // Si el PN532 quedó ejecutando un comando anterior (por ejemplo, si se reinició solo el ESP32),
  // una trama ACK lo cancela
  Wire.beginTransmission(DIRECCION_PN532);
  Wire.write(TRAMA_ACK, sizeof(TRAMA_ACK));
  Wire.endTransmission();
  delay(50);

  iniciarLector();
  Serial.println("Acercá una tarjeta...");
  mostrarMensaje("Esperando tarjeta...", ILI9341_WHITE);
}

void loop() {
  // InListPassiveTarget: busca 1 tarjeta ISO14443A (MIFARE) a 106 kbps
  uint8_t buscar[] = {CMD_IN_LIST_PASSIVE_TARGET, 0x01, 0x00};
  int largo = enviarComando("InListPassiveTarget", buscar, sizeof(buscar));

  if (largo < 0) {
    Serial.println("El PN532 dejó de responder. Reiniciándolo...");
    mostrarMensaje("Reiniciando lector...", ILI9341_YELLOW);
    iniciarLector();
    mostrarMensaje("Esperando tarjeta...", ILI9341_WHITE);
    return;
  }

  // Respuesta: cantidad de tarjetas, número de tarjeta, SENS_RES (2 bytes), SEL_RES, largo del UID, UID
  if (largo >= 6 && respuesta[0] == 1 && respuesta[5] <= largo - 6) {
    Serial.print("Tarjeta detectada. UID (");
    Serial.print(respuesta[5]);
    Serial.print(" bytes):");
    imprimirBytes(&respuesta[6], respuesta[5]);
    mostrarUID(&respuesta[6], respuesta[5]);
    delay(TIEMPO_UID);  // Deja el UID en pantalla y evita leer la misma tarjeta muchas veces seguidas
    mostrarMensaje("Esperando tarjeta...", ILI9341_WHITE);
  } else if (millis() - ultimoAviso >= INTERVALO_AVISO) {
    ultimoAviso = millis();
    Serial.println("PN532 responde. Sin tarjeta.");
  }
}

// ---------- Display ----------

void iniciarDisplay() {
  tft.begin(VELOCIDAD_SPI_TFT);

  // Lee el modo de energía del display: 0x94 indica que está encendido y funcionando.
  // Si da 0x00 o 0xFF, el display no contesta (o SDO/MISO no está conectado)
  uint8_t modo = tft.readcommand8(ILI9341_RDMODE);
  Serial.print("Display - modo de energía: 0x");
  Serial.println(modo, HEX);

  tft.setRotation(1);  // Horizontal: 320x240

  // Prueba de colores: si se ven rojo, verde y azul, el display funciona
  tft.fillScreen(ILI9341_RED);
  delay(300);
  tft.fillScreen(ILI9341_GREEN);
  delay(300);
  tft.fillScreen(ILI9341_BLUE);
  delay(300);

  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, tft.width(), ALTO_TITULO, ILI9341_NAVY);
  escribirCentrado("Lector NFC", 10, 3, ILI9341_WHITE);
}

// Borra la zona de abajo del título y escribe un mensaje centrado
// (la fuente del display no tiene tildes, por eso los textos van sin acentos)
void mostrarMensaje(const char *mensaje, uint16_t color) {
  tft.fillRect(0, ALTO_TITULO, tft.width(), tft.height() - ALTO_TITULO, ILI9341_BLACK);
  escribirCentrado(mensaje, 130, 2, color);
}

void mostrarUID(const uint8_t *uid, uint8_t largo) {
  char texto[3 * 10];  // "XX " por byte, hasta 10 bytes
  int pos = 0;
  for (uint8_t i = 0; i < largo && i < 10; i++) {
    pos += sprintf(&texto[pos], i == 0 ? "%02X" : " %02X", uid[i]);
  }

  char bytes[16];
  sprintf(bytes, "(%d bytes)", largo);

  tft.fillRect(0, ALTO_TITULO, tft.width(), tft.height() - ALTO_TITULO, ILI9341_BLACK);
  escribirCentrado("Tarjeta detectada", 80, 2, ILI9341_GREEN);
  escribirCentrado(texto, 125, largo <= 4 ? 4 : 2, ILI9341_WHITE);  // Los UID de 7 bytes no entran en tamaño 4
  escribirCentrado(bytes, 180, 2, ILI9341_LIGHTGREY);
}

void escribirCentrado(const char *texto, int16_t y, uint8_t tamanio, uint16_t color) {
  int16_t x1, y1;
  uint16_t ancho, alto;
  tft.setTextSize(tamanio);
  tft.setTextColor(color);
  tft.getTextBounds(texto, 0, y, &x1, &y1, &ancho, &alto);
  tft.setCursor((tft.width() - ancho) / 2, y);
  tft.print(texto);
}

// ---------- PN532 ----------

// Inicia el PN532 mostrando cada paso; si algo falla, lo reintenta cada 1 segundo
void iniciarLector() {
  detallar = true;
  while (!configurarLector()) {
    Serial.println("No se pudo iniciar el PN532. Reintentando...");
    mostrarMensaje("Lector sin respuesta", ILI9341_RED);
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
