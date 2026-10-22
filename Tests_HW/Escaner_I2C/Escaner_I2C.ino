#include <Wire.h>
#include "esp_log.h"

#define PIN_SDA 21
#define PIN_SCL 22
#define DIRECCION_PN532 0x24  // Dirección I2C del PN532

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie

  // El driver I2C del ESP32 imprime un error por cada dirección que no contesta; se ocultan
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  Serial.println("Escáner I2C");

  // Antes de iniciar el I2C: con el módulo alimentado, sus resistencias de pull-up dejan las dos líneas en alto
  pinMode(PIN_SDA, INPUT);
  pinMode(PIN_SCL, INPUT);
  Serial.print("Nivel de SDA (GPIO 21): ");
  Serial.println(digitalRead(PIN_SDA) ? "ALTO" : "BAJO");
  Serial.print("Nivel de SCL (GPIO 22): ");
  Serial.println(digitalRead(PIN_SCL) ? "ALTO" : "BAJO");

  Wire.begin(PIN_SDA, PIN_SCL);
}

void loop() {
  Serial.println("Buscando dispositivos...");
  int encontrados = 0;

  for (uint8_t direccion = 1; direccion < 127; direccion++) {
    Wire.beginTransmission(direccion);
    if (Wire.endTransmission() == 0) {
      Serial.print("  Dispositivo en 0x");
      if (direccion < 0x10) Serial.print('0');
      Serial.print(direccion, HEX);
      if (direccion == DIRECCION_PN532) Serial.print(" (PN532)");
      Serial.println();
      encontrados++;
    }
  }

  if (encontrados == 0) Serial.println("  No se encontró ningún dispositivo.");
  delay(2000);  // Repite la búsqueda cada 2 segundos
}
