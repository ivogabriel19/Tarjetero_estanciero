#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

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

// Valores crudos del touch (0 a 4095) en los bordes de la pantalla. Cambian de un display a otro:
// si el punto no cae donde se toca, ajustarlos con los valores que se imprimen por serie.
// Si un eje sale invertido, intercambiar su MIN y su MAX.
// Son para ROTACION_TOUCH 1 (con otra rotación, la librería del touch invierte o intercambia los ejes)
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define PAUSA_PRUEBA 2000  // ms que se muestra cada prueba
#define ALTO_BARRA   32    // Alto de la barra de arriba en la prueba del touch
#define INTERVALO_SERIE 200  // ms entre impresiones por serie de los valores del touch

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);  // Instancia del display con SPI por hardware
XPT2046_Touchscreen touch(TOUCH_CS);             // Instancia del touch, en el mismo bus SPI

unsigned long ultimaImpresion = 0;

void setup() {
  Serial.begin(115200);  // Inicializa la comunicación serie
  Serial.println("Prueba de display TFT ILI9341 con touch XPT2046 en ESP32");

  // El CS del touch tiene que estar en alto antes de hablar con el display, para que el touch no responda en el bus
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);

  tft.begin(VELOCIDAD_SPI_TFT);

  // Registros de diagnóstico: si todos dan 0x00 o 0xFF, el display no contesta (o SDO/MISO no está conectado)
  imprimirRegistro("Modo de energía", ILI9341_RDMODE);      // 0x94 = encendido y funcionando
  imprimirRegistro("MADCTL", ILI9341_RDMADCTL);             // 0x48 = orientación inicial
  imprimirRegistro("Formato de pixel", ILI9341_RDPIXFMT);   // 0x05 = 16 bits por pixel
  imprimirRegistro("Formato de imagen", ILI9341_RDIMGFMT);
  imprimirRegistro("Autodiagnóstico", ILI9341_RDSELFDIAG);  // 0xC0 = sin errores

  touch.begin();
  touch.setRotation(ROTACION_TOUCH);

  // Las pruebas de imagen se hacen una vez; después queda la prueba del touch (reiniciar para repetirlas)
  tft.setRotation(ROTACION);
  pruebaColores();
  pruebaBarras();
  pruebaTexto();
  pruebaFiguras();
  for (uint8_t rotacion = 0; rotacion < 4; rotacion++) pruebaRotacion(rotacion);
  tft.setRotation(ROTACION);
  pruebaVelocidad();

  Serial.println("Prueba: touch. Tocá la pantalla para dibujar");
  dibujarPantallaTouch();
}

// Prueba del touch: dibuja donde se toca y muestra las coordenadas
void loop() {
  if (!touch.touched()) return;

  TS_Point punto = touch.getPoint();
  int16_t x = constrain(map(punto.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, tft.width() - 1), 0, tft.width() - 1);
  int16_t y = constrain(map(punto.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, tft.height() - 1), 0, tft.height() - 1);

  if (millis() - ultimaImpresion >= INTERVALO_SERIE) {
    ultimaImpresion = millis();
    Serial.printf("Touch crudo: x=%d y=%d presión=%d -> pantalla: x=%d y=%d\n", punto.x, punto.y, punto.z, x, y);
  }

  char texto[24];
  sprintf(texto, "x=%3d y=%3d      ", x, y);  // Los espacios tapan el resto de "Toca para dibujar"
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE, ILI9341_NAVY);  // Con fondo, para pisar el texto anterior
  tft.setCursor(8, 9);
  tft.print(texto);

  if (y < ALTO_BARRA) {
    if (x >= tft.width() - 90) {  // Botón "BORRAR"
      dibujarPantallaTouch();
      while (touch.touched()) delay(10);  // Espera a que se suelte el dedo
    }
    return;
  }
  tft.fillCircle(x, y, 3, ILI9341_YELLOW);
}

// Barra de arriba con las coordenadas y el botón "BORRAR", y el resto de la pantalla en negro para dibujar
void dibujarPantallaTouch() {
  tft.fillScreen(ILI9341_BLACK);
  tft.fillRect(0, 0, tft.width(), ALTO_BARRA, ILI9341_NAVY);
  tft.fillRoundRect(tft.width() - 88, 3, 85, ALTO_BARRA - 6, 6, ILI9341_RED);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(tft.width() - 81, 9);
  tft.print("BORRAR");
  tft.setCursor(8, 9);
  tft.print("Toca para dibujar");
}

// Pinta la pantalla entera de varios colores
void pruebaColores() {
  Serial.println("Prueba: colores");
  const uint16_t colores[] = {ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE, ILI9341_WHITE, ILI9341_BLACK};
  for (uint8_t i = 0; i < sizeof(colores) / sizeof(colores[0]); i++) {
    tft.fillScreen(colores[i]);
    delay(400);
  }
}

// Barras verticales de colores. Si el rojo y el azul aparecen cambiados, el display usa otro orden de color (RGB/BGR)
void pruebaBarras() {
  Serial.println("Prueba: barras de color");
  const uint16_t colores[] = {ILI9341_WHITE, ILI9341_YELLOW, ILI9341_CYAN, ILI9341_GREEN,
                              ILI9341_MAGENTA, ILI9341_RED, ILI9341_BLUE, ILI9341_BLACK};
  const uint8_t cantidad = sizeof(colores) / sizeof(colores[0]);
  int16_t ancho = tft.width() / cantidad;
  for (uint8_t i = 0; i < cantidad; i++) {
    tft.fillRect(i * ancho, 0, ancho, tft.height(), colores[i]);
  }
  delay(PAUSA_PRUEBA);
}

// Texto en los distintos tamaños de la fuente (la fuente no tiene tildes)
void pruebaTexto() {
  Serial.println("Prueba: texto");
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(0, 0);
  tft.setTextColor(ILI9341_WHITE);
  for (uint8_t tamanio = 1; tamanio <= 4; tamanio++) {
    tft.setTextSize(tamanio);
    tft.print("Tamanio ");
    tft.println(tamanio);
  }
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_YELLOW);
  tft.println();
  tft.println("0123456789 ABCDEF");
  tft.setTextColor(ILI9341_GREEN);
  tft.println("Lector NFC");
  delay(PAUSA_PRUEBA);
}

// Líneas, rectángulos, círculos y triángulos
void pruebaFiguras() {
  Serial.println("Prueba: figuras");
  tft.fillScreen(ILI9341_BLACK);
  for (int16_t x = 0; x < tft.width(); x += 20) {
    tft.drawLine(0, 0, x, tft.height() - 1, ILI9341_DARKGREY);
  }
  tft.drawRect(20, 20, 80, 60, ILI9341_RED);
  tft.fillRect(120, 20, 80, 60, ILI9341_GREEN);
  tft.fillRoundRect(220, 20, 80, 60, 12, ILI9341_BLUE);
  tft.drawCircle(60, 170, 40, ILI9341_YELLOW);
  tft.fillCircle(160, 170, 40, ILI9341_CYAN);
  tft.fillTriangle(220, 210, 300, 210, 260, 130, ILI9341_MAGENTA);
  delay(PAUSA_PRUEBA);
}

// Muestra las 4 orientaciones con un borde, para ver que la imagen llega a todos los bordes
void pruebaRotacion(uint8_t rotacion) {
  Serial.print("Prueba: rotación ");
  Serial.println(rotacion);
  tft.setRotation(rotacion);
  tft.fillScreen(ILI9341_BLACK);
  tft.drawRect(0, 0, tft.width(), tft.height(), ILI9341_RED);
  tft.fillTriangle(0, 0, 30, 0, 0, 30, ILI9341_RED);  // Marca la esquina de origen (arriba a la izquierda)

  char texto[24];
  sprintf(texto, "Rotacion %d", rotacion);
  escribirCentrado(texto, tft.height() / 2 - 20, 3, ILI9341_WHITE);
  sprintf(texto, "%d x %d", tft.width(), tft.height());
  escribirCentrado(texto, tft.height() / 2 + 20, 2, ILI9341_LIGHTGREY);
  delay(PAUSA_PRUEBA);
}

// Mide cuánto tarda en pintar la pantalla entera
void pruebaVelocidad() {
  unsigned long inicio = millis();
  tft.fillScreen(ILI9341_NAVY);
  unsigned long tiempo = millis() - inicio;

  Serial.print("Prueba: velocidad. Pintar la pantalla tarda ");
  Serial.print(tiempo);
  Serial.println(" ms");

  char texto[24];
  sprintf(texto, "%lu ms", tiempo);
  escribirCentrado("Pintar la pantalla:", 90, 2, ILI9341_WHITE);
  escribirCentrado(texto, 125, 4, ILI9341_YELLOW);
  delay(PAUSA_PRUEBA);
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

void imprimirRegistro(const char *nombre, uint8_t registro) {
  uint8_t valor = tft.readcommand8(registro);
  Serial.print("Display - ");
  Serial.print(nombre);
  Serial.print(": 0x");
  if (valor < 0x10) Serial.print('0');
  Serial.println(valor, HEX);
}
