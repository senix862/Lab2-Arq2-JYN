#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>

#define DHTPIN D5
#define DHTTYPE DHT22
#define SDA_PIN D3
#define SCL_PIN D2

DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println(F("Iniciando sensores..."));

  Wire.begin(SDA_PIN, SCL_PIN);
  dht.begin();

  if (!bmp.begin(0x76)) {
    Serial.println(F("No se pudo encontrar el BMP280, revisalo!!"));
  } else {
    Serial.println(F("BMP280 inicializado correctamente."));
  }
}

void loop() {
  Serial.println("-----------------------------");
  Serial.println("Leyendo sensores...");

  float h = dht.readHumidity();
  float t_dht = dht.readTemperature();

  if (isnan(h) || isnan(t_dht)) {
    Serial.println("Error al leer del sensor DHT22, revisalo!!");
  } else {
    Serial.print("Humedad (DHT22): ");
    Serial.print(h);
    Serial.println(" %");

    Serial.print("Temperatura (DHT22): ");
    Serial.print(t_dht);
    Serial.println(" °C");
  }

  if (bmp.begin(0x76)) {
    float t_bmp = bmp.readTemperature();
    float p = bmp.readPressure() / 100.0F;

    Serial.print("Temperatura (BMP280): ");
    Serial.print(t_bmp);
    Serial.println(" °C");

    Serial.print("Presión atmosférica: ");
    Serial.print(p);
    Serial.println(" hPa");
  }

 //Sensar cada 3000 ms, wachin.
  Serial.println("-----------------------------");
  delay(3000);
}

