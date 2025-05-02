#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <vector>
#include <time.h>

// --- Definiciones de Pines ---
#define DHTPIN D5
#define DHTTYPE DHT22

#define SDA_PIN D3
#define SCL_PIN D2

// --- Configuración WiFi ---
const char* ssid = "OreoInvertido";
const char* password = "NoRacist";

// --- Configuración NTP ---
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* time_zone = "<-03>3";

// --- Objetos y Servidor ---
ESP8266WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;

// --- Estructura para el Historial ---
struct SensorReading {
  time_t timestamp;
  float hum;
  float tempDHT;
  float tempBMP;
  float pres;
  bool synced = false; // offline resilience
};

#define MAX_HISTORY_SIZE 50
std::vector<SensorReading> history;
std::vector<SensorReading> unsyncedReadings; // Almacena lecturas sin conexión
bool wasDisconnected = false; // Para detectar reconexiones

// --- Variables Globales ---
unsigned long lastRead = 0;
const unsigned long readInterval = 5000;
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 10000;
unsigned long lastInternetCheck = 0;
const unsigned long internetCheckInterval = 60000;
unsigned long lastLogUpdate = 0; // Para controlar la frecuencia del log
const unsigned long logUpdateInterval = 420000; // Actualizar log cada 7 minutos
unsigned long lastMemoryCleanup = 0; // Para limpieza periódica de memoria
const unsigned long memoryCleanupInterval = 600000; // Limpiar memoria cada 10 minutos (600000 ms)

float currentHumidity = NAN;
float currentTempDHT = NAN;
float currentTempBMP = NAN;
float currentPressure = NAN;

bool bmpSensorOk = false;
bool dhtSensorOk = false;
bool wifiConnected = false;
bool internetAvailable = false;
bool timeSyncd = false;

String systemLog = "";

// --- Funciones Auxiliares ---
String getFormattedTime() {
  if (!timeSyncd) {
    return "--:--";
  }
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char timeStr[6];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  return String(timeStr);
}

void addToLog(String message) {
  String timestamp = "[" + getFormattedTime() + "] ";
  String logEntry = timestamp + message;
  systemLog += logEntry + "<br>";
  if (systemLog.length() > 2000) {
     int firstBr = systemLog.indexOf("<br>", systemLog.length() - 1900);
     if (firstBr != -1) {
         systemLog = systemLog.substring(firstBr + 4);
     } else {
         systemLog = systemLog.substring(systemLog.length() - 2000);
     }
  }
  Serial.println(logEntry);
}

void addReadingToHistory(float h, float tDHT, float tBMP, float p) {
    if (isnan(h) && isnan(tDHT) && isnan(tBMP) && isnan(p)) {
        return;
    }

    SensorReading reading;
    reading.timestamp = time(nullptr);
    reading.hum = h;
    reading.tempDHT = tDHT;
    reading.tempBMP = tBMP;
    reading.pres = p;
    reading.synced = wifiConnected;

    if (!wifiConnected) {
        if (unsyncedReadings.size() >= MAX_HISTORY_SIZE) {
            unsyncedReadings.erase(unsyncedReadings.begin());
        }
        unsyncedReadings.push_back(reading);
    } else {
        if (history.size() >= MAX_HISTORY_SIZE) {
            history.erase(history.begin());
        }
        history.push_back(reading);
    }
}

void syncPendingReadings() {
    if (!wifiConnected || unsyncedReadings.empty()) return;

    addToLog("Sincronizando lecturas pendientes...");
    for (auto& reading : unsyncedReadings) {
        if (!reading.synced) {
            if (history.size() >= MAX_HISTORY_SIZE) {
                history.erase(history.begin());
            }
            history.push_back(reading);
            reading.synced = true;
        }
    }
    unsyncedReadings.clear();
    addToLog("Se cargaron datos anteriores por pérdida de señal");
}

bool checkInternet() {
  WiFiClient client;
  bool result = client.connect("8.8.8.8", 53);
  if (result) {
    client.stop();
  }
  return result;
}

void syncTimeNTP() {
    addToLog("Configurando reloj...");
    configTime(time_zone, ntpServer1, ntpServer2);

    addToLog("Esperando sincronización de reloj...");
    time_t now = time(nullptr);
    int retries = 0;
    while (now < 1000000000L && retries < 20) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        retries++;
    }
    Serial.println();

    if (now < 1000000000L) {
        addToLog("Error: Fallo al sincronizar reloj.");
        timeSyncd = false;
    } else {
        timeSyncd = true;
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char timeStr[30];
        strftime(timeStr, sizeof(timeStr),"%d/%m/%Y %H:%M:%S", &timeinfo);
        addToLog("Hora sincronizada: " + String(timeStr));
    }
}

void checkWifiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      addToLog("WiFi desconectado, guardando datos localmente...");
      wifiConnected = false;
      internetAvailable = false;
      timeSyncd = false;
      wasDisconnected = true;
    }
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    delay(100);
    return;
  }

  if (!wifiConnected) {
    wifiConnected = true;
    Serial.println("WiFi Conectado.");
    Serial.println("IP de la web : " + WiFi.localIP().toString());
    
    if (wasDisconnected) {
      syncPendingReadings();
      wasDisconnected = false;
    }
    
    syncTimeNTP();
    internetAvailable = checkInternet();
    addToLog(internetAvailable ? "Conexión a Internet verificada." : "Sin conexión a Internet.");
    lastInternetCheck = millis();
  }
}

void performMemoryCleanup() {
    Serial.println("\n[LIMPEZA] Iniciando limpieza de memoria...");
    size_t beforeHistory = history.size();
    size_t beforeUnsynced = unsyncedReadings.size();
    size_t beforeLog = systemLog.length();

    // Conservar solo las últimas lecturas (mitad del máximo)
    if (history.size() > MAX_HISTORY_SIZE/2) {
        history.erase(history.begin(), history.begin() + (history.size() - MAX_HISTORY_SIZE/2));
    }
    
    if (unsyncedReadings.size() > MAX_HISTORY_SIZE/2) {
        unsyncedReadings.erase(unsyncedReadings.begin(), unsyncedReadings.begin() + (unsyncedReadings.size() - MAX_HISTORY_SIZE/2));
    }

    // Limpiar log conservando las últimas líneas
    if (systemLog.length() > 1000) {
        int lastBr = systemLog.lastIndexOf("<br>", 1000);
        if (lastBr != -1) {
            systemLog = systemLog.substring(lastBr + 4);
        }
    }

    Serial.printf("[LIMPEZA] Resultados: Historico %d->%d | Pendientes %d->%d | Log %d->%d bytes\n",
                 beforeHistory, history.size(),
                 beforeUnsynced, unsyncedReadings.size(),
                 beforeLog, systemLog.length());
}

void readSensors() {
  bool readingSuccess = false;

  // Leer DHT22
  float h = dht.readHumidity();
  float t_dht = dht.readTemperature();
  if (isnan(h) || isnan(t_dht)) {
    dhtSensorOk = false;
  } else {
    currentHumidity = h;
    currentTempDHT = t_dht;
    dhtSensorOk = true;
    readingSuccess = true;
  }

  // Leer BMP280
  if (bmpSensorOk) {
    float t_bmp = bmp.readTemperature();
    float p = bmp.readPressure() / 100.0F;
    if (isnan(t_bmp) || isnan(p) || p < 800 || p > 1200 ) {
        currentTempBMP = NAN;
        currentPressure = NAN;
    } else {
        currentTempBMP = t_bmp;
        currentPressure = p;
        readingSuccess = true;
    }
  } else {
    static unsigned long lastBmpRetry = 0;
    if (millis() - lastBmpRetry > 60000) {
        bmpSensorOk = bmp.begin(0x76);
        if(bmpSensorOk) {
             addToLog("Sensor BMP280 funcionando (tras reintento).");
        }
        lastBmpRetry = millis();
    }
    currentTempBMP = NAN;
    currentPressure = NAN;
  }

  if (readingSuccess) {
      addReadingToHistory(currentHumidity, currentTempDHT, currentTempBMP, currentPressure);
  }
}

// --- HTML Embebido ---
const char html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Estación Meteorológica - Oreo Invertido</title>
  <meta http-equiv="refresh" content="1200"> <style>
    body { font-family: sans-serif; padding: 1rem; text-align: center; background: #f4f4f4; }
    canvas { max-width: 100%; margin: 1rem auto; }
    .charts { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; }
    .chart-container { width: 45%; min-width: 300px; background: white; padding: 1rem; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
    @media (max-width: 768px) { .chart-container { width: 90%; } }
    #log { background: #222; color: lime; padding: 1rem; text-align: left; font-family: monospace; max-height: 200px; overflow-y: auto; margin-top: 1rem; border-radius: 5px;}
    button { padding: 0.5rem 1rem; margin: 0.5rem; border: none; border-radius: 5px; cursor: pointer; }
    #syncBtn { background: #007bff; color: white; }
    .status { display: flex; flex-wrap: wrap; justify-content: center; gap: 10px; margin: 20px 0; }
    .status-item { padding: 8px 15px; border-radius: 5px; color: white; font-weight: bold; font-size: 0.9em;}
    .online { background: #28a745; }
    .offline { background: #dc3545; }
    .unknown { background: #ffc107; color: #333;}
    .current-values { display: flex; flex-wrap: wrap; justify-content: center; gap: 15px; margin: 20px 0; }
    .value-card { background: white; padding: 15px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); width: 180px; }
    .value-title { font-weight: bold; margin-bottom: 8px; font-size: 0.95em; }
    .value { font-size: 22px; }
    .unit { font-size: 13px; color: #666; }
    .nan-value { color: #999; font-style: italic; }
  </style>
</head>
<body>
  <h1>Estación Meteorológica - Oreo Invertido</h1>
  <p>Alumnos: Yanzón Brisa, Mario Navarro, Emiliano Jalowicki</p>
  <p>Profesor: Matias Loiseau</p>

  <div class="status">
    <div class="status-item unknown" id="wifi-status">WiFi: Verificando...</div>
    <div class="status-item unknown" id="internet-status">Internet: Verificando...</div>
    <div class="status-item unknown" id="sensors-status">Sensores: Verificando...</div>
  </div>

  <div class="current-values">
    <div class="value-card">
      <div class="value-title">Humedad (DHT22)</div>
      <div class="value" id="current-hum">--</div>
      <div class="unit">%</div>
    </div>
    <div class="value-card">
      <div class="value-title">Temperatura (DHT22)</div>
      <div class="value" id="current-tempDHT">--</div>
      <div class="unit">°C</div>
    </div>
    <div class="value-card">
      <div class="value-title">Temperatura (BMP280)</div>
      <div class="value" id="current-tempBMP">--</div>
      <div class="unit">°C</div>
    </div>
    <div class="value-card">
      <div class="value-title">Presión Atmosférica</div>
      <div class="value" id="current-pres">--</div>
      <div class="unit">hPa</div>
    </div>
  </div>

  <button id="syncBtn">Forzar Lectura Sensores</button>
  <div id="log">Cargando log...</div>

  <div class="charts">
    <div class="chart-container"><canvas id="tempBMPChart"></canvas></div>
    <div class="chart-container"><canvas id="presChart"></canvas></div>
    <div class="chart-container"><canvas id="tempDHTChart"></canvas></div>
    <div class="chart-container"><canvas id="humChart"></canvas></div>
  </div>

  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <script>
    function formatValue(value, decimals = 1) {
        if (value === null || isNaN(value) || typeof value === 'undefined') {
            return '<span class="nan-value">--</span>';
        }
        if (typeof value === 'number') {
             return value.toFixed(decimals);
        }
        return value;
    }

    function updateConnectionStatus() {
      fetch('/status')
        .then(res => res.ok ? res.json() : Promise.reject('Status fetch failed'))
        .then(data => {
          const wifiStatus = document.getElementById('wifi-status');
          const internetStatus = document.getElementById('internet-status');
          const sensorsStatus = document.getElementById('sensors-status');

          wifiStatus.textContent = `WiFi: ${data.wifi ? 'Conectado' : 'Desconectado'}`;
          wifiStatus.className = `status-item ${data.wifi ? 'online' : 'offline'}`;

          internetStatus.textContent = `Internet: ${data.internet === null ? 'Verificando' : data.internet ? 'Disponible' : 'No Disponible'}`;
          internetStatus.className = `status-item ${data.internet === null ? 'unknown' : data.internet ? 'online' : 'offline'}`;

          sensorsStatus.textContent = `Sensores: ${data.sensors ? 'OK' : 'Error'}`;
          sensorsStatus.className = `status-item ${data.sensors ? 'online' : 'offline'}`;
        })
        .catch(error => console.error('Error fetching status:', error));
    }

    function updateCurrentValues() {
      fetch('/current')
        .then(res => res.ok ? res.json() : Promise.reject('Current fetch failed'))
        .then(data => {
          document.getElementById('current-hum').innerHTML = formatValue(data.hum);
          document.getElementById('current-tempDHT').innerHTML = formatValue(data.tempDHT);
          document.getElementById('current-tempBMP').innerHTML = formatValue(data.tempBMP);
          document.getElementById('current-pres').innerHTML = formatValue(data.pres);
        })
        .catch(error => console.error('Error fetching current values:', error));
    }

    function updateLog() {
      fetch('/getlog')
        .then(res => res.ok ? res.text() : Promise.reject('Log fetch failed'))
        .then(text => {
          const logDiv = document.getElementById('log');
          const shouldScroll = logDiv.scrollHeight - logDiv.clientHeight <= logDiv.scrollTop + 5;
          logDiv.innerHTML = text;
          if(shouldScroll) {
            logDiv.scrollTop = logDiv.scrollHeight;
          }
        })
        .catch(error => console.error('Error fetching log:', error));
    }

    function setupChart(id, label, color) {
      const ctx = document.getElementById(id).getContext('2d');
      return new Chart(ctx, {
        type: 'line',
        data: {
          labels: [],
          datasets: [{
            label: label,
            data: [],
            borderColor: color || 'rgb(75, 192, 192)',
            backgroundColor: (color || 'rgb(75, 192, 192)').replace(')',', 0.1)').replace('rgb','rgba'),
            borderWidth: 1.5,
            fill: true,
            tension: 0.2,
            pointRadius: 1,
            spanGaps: true
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: {
              ticks: {
                 maxTicksLimit: 10
              }
            },
            y: {
               beginAtZero: false,
               ticks: {
                   callback: function(value) {
                       return typeof value === 'number' ? value.toFixed(1) : value;
                   }
               }
            }
          },
          plugins: {
            legend: { display: false },
            title: {
                display: true,
                text: label,
                padding: { top: 5, bottom: 10 }
            }
          }
        }
      });
    }

    const tempBMPChart = setupChart('tempBMPChart', 'Temperatura BMP280 (°C)', 'rgb(255, 99, 132)');
    const presChart = setupChart('presChart', 'Presión (hPa)', 'rgb(54, 162, 235)');
    const tempDHTChart = setupChart('tempDHTChart', 'Temperatura DHT22 (°C)', 'rgb(255, 159, 64)');
    const humChart = setupChart('humChart', 'Humedad DHT22 (%)', 'rgb(75, 192, 192)');

    function updateSingleChart(chart, labels, data) {
        chart.data.labels = labels;
        chart.data.datasets[0].data = data;
        chart.update('none');
    }

    function updateCharts() {
      fetch('/data')
        .then(res => res.ok ? res.json() : Promise.reject('Data fetch failed'))
        .then(data => {
          if (!data || data.length === 0) { return; }

          const maxChartPoints = 50;
          const chartData = data.length > maxChartPoints ? data.slice(-maxChartPoints) : data;

          const labels = chartData.map(item => {
              const date = new Date(item.time * 1000);
              return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          });

          const tempBMPData = chartData.map(item => parseFloat(item.tempBMP));
          const presData = chartData.map(item => parseFloat(item.pres));
          const tempDHTData = chartData.map(item => parseFloat(item.tempDHT));
          const humData = chartData.map(item => parseFloat(item.hum));

          updateSingleChart(tempBMPChart, labels, tempBMPData);
          updateSingleChart(presChart, labels, presData);
          updateSingleChart(tempDHTChart, labels, tempDHTData);
          updateSingleChart(humChart, labels, humData);
        })
        .catch(error => console.error('Error fetching historical data:', error));
    }

    document.getElementById('syncBtn').onclick = () => {
        console.log("Sync button clicked.");
        fetch('/sync')
          .then(response => {
              if (response.ok) {
                  console.log("Sync successful. Updating UI.");
                  setTimeout(() => {
                     updateCurrentValues();
                     updateCharts();
                     updateLog();
                     updateConnectionStatus();
                  }, 500);
              } else {
                  console.error("Sync request failed:", response.statusText);
              }
          })
          .catch(error => {
              console.error('Error during sync fetch:', error);
          });
    };

    updateConnectionStatus();
    updateCurrentValues();
    updateLog();
    updateCharts();

    setInterval(updateConnectionStatus, 10000);
    setInterval(updateCurrentValues, 5000);
    setInterval(updateLog, 20000); // Actualizado a 20 segundos
    setInterval(updateCharts, 15000);

    console.log("Weather station page loaded and auto-updates started.");
  </script>
</body>
</html>
)rawliteral";

// --- Setup ---
void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n--- Iniciando Estacion Meteorologica del equipo Oreo Invertido ---");

  history.reserve(MAX_HISTORY_SIZE);
  unsyncedReadings.reserve(MAX_HISTORY_SIZE);

  Wire.begin(SDA_PIN, SCL_PIN);

  // Inicializa Sensor DHT22
  dht.begin();
  pinMode(DHTPIN, INPUT);
  delay(100);
  if (isnan(dht.readTemperature()) || isnan(dht.readHumidity())) {
       Serial.println("Sensor DHT22: Error inicial (Verificar la conexion/pin D " + String(DHTPIN)+"!)");
       dhtSensorOk = false;
  } else {
       Serial.println("Sensor DHT22: Funcionando");
       dhtSensorOk = true;
  }

  // Inicializa Sensor BMP280
  if (!bmp.begin(0x76)) {
    Serial.println("Sensor BMP280: Error al inicializar (Verificar la conexion!)");
    bmpSensorOk = false;
  } else {
    Serial.println("Sensor BMP280: Funcionando");
    bmpSensorOk = true;
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X4);
  }

  // Conectar a WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi: " + String(ssid));
  unsigned long startWifi = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifi < 20000) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  checkWifiConnection();

  // Configuración de Rutas del Servidor Web
  server.on("/", HTTP_GET, []() {
       server.send_P(200, "text/html", html);
  });

  server.on("/data", HTTP_GET, []() {
    String json = "[";
    // Combinar history y unsyncedReadings para mostrar todos los datos
    std::vector<SensorReading> allReadings;
    allReadings.insert(allReadings.end(), history.begin(), history.end());
    allReadings.insert(allReadings.end(), unsyncedReadings.begin(), unsyncedReadings.end());
    
    for (size_t i = 0; i < allReadings.size(); ++i) {
        const auto& reading = allReadings[i];
        json += "{";
        json += "\"time\":" + String(reading.timestamp) + ",";
        json += "\"hum\":" + (isnan(reading.hum) ? "null" : String(reading.hum, 2)) + ",";
        json += "\"tempDHT\":" + (isnan(reading.tempDHT) ? "null" : String(reading.tempDHT, 2)) + ",";
        json += "\"tempBMP\":" + (isnan(reading.tempBMP) ? "null" : String(reading.tempBMP, 2)) + ",";
        json += "\"pres\":" + (isnan(reading.pres) ? "null" : String(reading.pres, 2));
        json += "}";
        if (i < allReadings.size() - 1) { json += ","; }
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/current", HTTP_GET, []() {
    String json = "{";
    json += "\"hum\":" + (isnan(currentHumidity) ? "null" : String(currentHumidity, 1)) + ",";
    json += "\"tempDHT\":" + (isnan(currentTempDHT) ? "null" : String(currentTempDHT, 1)) + ",";
    json += "\"tempBMP\":" + (isnan(currentTempBMP) ? "null" : String(currentTempBMP, 1)) + ",";
    json += "\"pres\":" + (isnan(currentPressure) ? "null" : String(currentPressure, 1));
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, []() {
    String json = "{";
    json += "\"wifi\":"; json += (wifiConnected ? "true" : "false"); json += ",";
    json += "\"internet\":";
    if (wifiConnected) { json += (internetAvailable ? "true" : "false"); }
    else { json += "false"; }
    json += ",";
    json += "\"sensors\":"; json += ((dhtSensorOk || bmpSensorOk) ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/getlog", HTTP_GET, []() {
    server.send(200, "text/html", systemLog);
  });

  server.on("/sync", HTTP_GET, []() {
    readSensors();
    if (dhtSensorOk || bmpSensorOk) { server.send(200, "text/plain", "OK"); }
    else { server.send(500, "text/plain", "Error"); }
  });

  server.begin();
  addToLog("Servidor web iniciado.");

  readSensors();
  Serial.println("--- Impresion de datos ---");
}

// --- Loop ---
void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  // Limpieza de memoria cada 10 minutos
  if (currentMillis - lastMemoryCleanup >= memoryCleanupInterval) {
    performMemoryCleanup();
    lastMemoryCleanup = currentMillis;
  }

  // Lectura de sensores
  if (currentMillis - lastRead >= readInterval) {
    readSensors();
    lastRead = currentMillis;
    
    Serial.print("[" + getFormattedTime() + "] ");
    Serial.print("Temperatura DHT:"); Serial.print(isnan(currentTempDHT) ? "--" : String(currentTempDHT, 1)); Serial.print("C ");
    Serial.print("Humedad DHT:"); Serial.print(isnan(currentHumidity) ? "--" : String(currentHumidity, 1)); Serial.print("% ");
    Serial.print("Temperatura BMP:"); Serial.print(isnan(currentTempBMP) ? "--" : String(currentTempBMP, 1)); Serial.print("C ");
    Serial.print("Presión Atm. BMP:"); Serial.print(isnan(currentPressure) ? "--" : String(currentPressure, 1)); Serial.println("hPa");
  }

  // Verificación de WiFi
  if (currentMillis - lastWifiCheck >= wifiCheckInterval) {
    checkWifiConnection();
    lastWifiCheck = currentMillis;
  }

  // Verificación de Internet
  if (wifiConnected && (currentMillis - lastInternetCheck >= internetCheckInterval)) {
    bool currentInternet = checkInternet();
    if (currentInternet != internetAvailable) {
      internetAvailable = currentInternet;
      addToLog(internetAvailable ? "Internet OK" : "Internet NO");
    }
    lastInternetCheck = currentMillis;
  }

  // Actualización del log cada 20 segundos
  if (currentMillis - lastLogUpdate >= logUpdateInterval) {
    lastLogUpdate = currentMillis;
    // El log se actualiza automáticamente mediante las funciones addToLog
  }

  delay(5000);
}