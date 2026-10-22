#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>

#define SS_PIN 21   // Pin SDA del RC522
#define RST_PIN 22  // Pin RST del RC522
#define RELAY_PIN 17 // Pin de control del relé

MFRC522 rfid(SS_PIN, RST_PIN);  // Instancia del objeto RFID

MFRC522::MIFARE_Key key;

// UID de las tarjetas maestras (para alta y baja)
byte altaMaster[4] = {0x63, 0x9A, 0xD7, 0x1B}; // Tarjeta de alta
byte bajaMaster[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // Tarjeta de baja

bool modoAlta = false;  // Controla si se está en modo alta
bool modoBaja = false;  // Controla si se está en modo baja

void setup() {
  Serial.begin(115200);   // Inicializa la comunicación serie
  SPI.begin(18, 19, 23, 21); // Inicia el bus SPI en ESP32 con MOSI, MISO, SCK y SS
  rfid.PCD_Init();      // Inicia el RC522
  pinMode(RELAY_PIN, OUTPUT); // Configura el pin del relé como salida
  digitalWrite(RELAY_PIN, LOW); // Asegura que la cerradura esté cerrada por defecto

  Serial.println("Sistema de control de acceso con RFID en ESP32");
}

void loop() {
  // Verifica si hay una nueva tarjeta
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  Serial.print("UID leído: ");
  printUID(rfid.uid.uidByte, rfid.uid.size);

  if (modoAlta) {
    registrarTarjeta();
    modoAlta = false;
  } else if (modoBaja) {
    eliminarTarjeta();
    modoBaja = false;
  } else if (compararUID(rfid.uid.uidByte, altaMaster)) {
    Serial.println("Tarjeta de alta detectada. Pasa la siguiente tarjeta para dar de alta.");
    modoAlta = true;
  } else if (compararUID(rfid.uid.uidByte, bajaMaster)) {
    Serial.println("Tarjeta de baja detectada. Pasa la siguiente tarjeta para eliminar.");
    modoBaja = true;
  } else if (tarjetaRegistrada()) {
    abrirCerradura();
  } else {
    Serial.println("Acceso denegado. Tarjeta no registrada.");
  }

  rfid.PICC_HaltA(); // Detiene la comunicación con la tarjeta
}

void registrarTarjeta() {
  // Graba la UID de la tarjeta en EEPROM
  int direccion = encontrarEspacioLibre();
  if (direccion == -1) {
    Serial.println("No hay espacio para registrar más tarjetas.");
    return;
  }
  
  for (byte i = 0; i < rfid.uid.size; i++) {
    EEPROM.write(direccion + i, rfid.uid.uidByte[i]);
  }
  
  Serial.println("Tarjeta registrada con éxito.");
}

void eliminarTarjeta() {
  // Elimina la tarjeta de EEPROM
  int direccion = encontrarTarjeta();
  if (direccion == -1) {
    Serial.println("Tarjeta no registrada.");
    return;
  }
  
  for (byte i = 0; i < rfid.uid.size; i++) {
    EEPROM.write(direccion + i, 0xFF);  // Borra el UID
  }
  
  Serial.println("Tarjeta eliminada con éxito.");
}

bool tarjetaRegistrada() {
  // Verifica si la tarjeta leída está registrada en EEPROM
  return (encontrarTarjeta() != -1);
}

int encontrarEspacioLibre() {
  // Encuentra un espacio libre en la EEPROM
  for (int i = 0; i < 512; i += 4) {
    if (EEPROM.read(i) == 0xFF) {
      return i;  // Espacio libre encontrado
    }
  }
  return -1; // No hay espacio libre
}

int encontrarTarjeta() {
  // Busca si la tarjeta leída ya está registrada en EEPROM
  for (int i = 0; i < 512; i += 4) {
    bool coincide = true;
    for (byte j = 0; j < rfid.uid.size; j++) {
      if (EEPROM.read(i + j) != rfid.uid.uidByte[j]) {
        coincide = false;
        break;
      }
    }
    if (coincide) {
      return i;  // Tarjeta encontrada
    }
  }
  return -1; // Tarjeta no encontrada
}

void abrirCerradura() {
  // Abre la cerradura activando el relé
  Serial.println("Acceso concedido. Abriendo cerradura...");
  digitalWrite(RELAY_PIN, HIGH); // Activa el relé (abre la cerradura)
  delay(5000); // Mantiene la cerradura abierta por 5 segundos
  digitalWrite(RELAY_PIN, LOW);  // Cierra la cerradura
  Serial.println("Cerradura cerrada.");
}

bool compararUID(byte *uid, byte *master) {
  for (byte i = 0; i < 4; i++) {
    if (uid[i] != master[i]) return false;
  }
  return true;
}

void printUID(byte *uid, byte size) {
  for (byte i = 0; i < size; i++) {
    Serial.print(uid[i] < 0x10 ? " 0" : " ");
    Serial.print(uid[i], HEX);
  }
  Serial.println();
}
