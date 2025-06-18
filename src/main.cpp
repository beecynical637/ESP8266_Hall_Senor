#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <TM1637Display.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include "wifi_credentials.h"

// Пины для первого дисплея (Датчик 1)
#define CLK_PIN1 D1
#define DIO_PIN1 D2

// Пины для второго дисплея (Датчик 2)  
#define CLK_PIN2 D3
#define DIO_PIN2 D4

TM1637Display display1(CLK_PIN1, DIO_PIN1); // Дисплей для датчика 1
TM1637Display display2(CLK_PIN2, DIO_PIN2); // Дисплей для датчика 2

#define HALL1_DIGITAL_PIN D5  // Датчик Холла 1
#define HALL2_DIGITAL_PIN D6  // Датчик Холла 2
#define DEBOUNCE_DELAY 5      // Уменьшен для высоких оборотов
#define MAX_DATA_POINTS 60

AsyncWebServer server(80);
bool darkMode = false;
bool recording = false;

// Переменные для более точного измерения времени
volatile unsigned long lastTriggerTime1 = 0;
volatile unsigned long currentTriggerTime1 = 0;
volatile bool triggered1 = false;
volatile unsigned long lastTriggerTime2 = 0;
volatile unsigned long currentTriggerTime2 = 0;
volatile bool triggered2 = false;

float rpm1 = 0.0; // RPM для датчика 1
float rpm2 = 0.0; // RPM для датчика 2

// Для сглаживания показаний
#define SMOOTHING_SAMPLES 3
float rpm1Samples[SMOOTHING_SAMPLES];
float rpm2Samples[SMOOTHING_SAMPLES];
int sampleIndex = 0;

float rpm1History[MAX_DATA_POINTS];
float rpm2History[MAX_DATA_POINTS];
unsigned long timeHistory[MAX_DATA_POINTS];
int dataIndex = 0;
bool dataFull = false;

void IRAM_ATTR hallTrigger1() {
  static unsigned long lastDebounceTime = 0;
  unsigned long currentMillis = millis();
  
  // Более быстрый дебаунс для высоких оборотов
  if ((currentMillis - lastDebounceTime) > DEBOUNCE_DELAY) {
    currentTriggerTime1 = currentMillis;
    triggered1 = true;
    lastDebounceTime = currentMillis;
  }
}

void IRAM_ATTR hallTrigger2() {
  static unsigned long lastDebounceTime = 0;
  unsigned long currentMillis = millis();
  
  // Более быстрый дебаунс для высоких оборотов
  if ((currentMillis - lastDebounceTime) > DEBOUNCE_DELAY) {
    currentTriggerTime2 = currentMillis;
    triggered2 = true;
    lastDebounceTime = currentMillis;
  }
}

float getAverageRPM(float samples[], float newSample) {
  // Добавляем новый образец
  samples[sampleIndex] = newSample;
  sampleIndex = (sampleIndex + 1) % SMOOTHING_SAMPLES;
  
  // Считаем среднее значение
  float sum = 0;
  for (int i = 0; i < SMOOTHING_SAMPLES; i++) {
    sum += samples[i];
  }
  return sum / SMOOTHING_SAMPLES;
}

void calculateRPM() {
  unsigned long currentMillis = millis();
  
  if (triggered1 && lastTriggerTime1 > 0) {
    unsigned long pulseInterval = currentTriggerTime1 - lastTriggerTime1;
    
    // Проверка на разумные значения: от 20ms (3000 RPM) до 3000ms (20 RPM)
    if (pulseInterval >= 20 && pulseInterval <= 3000) {
      float instantRpm = 60000.0 / (float)pulseInterval;
      
      // Ограничение для предотвращения сбоев
      if (instantRpm <= 5000) {
        rpm1 = getAverageRPM(rpm1Samples, instantRpm);
      }
    }
    
    lastTriggerTime1 = currentTriggerTime1;
    triggered1 = false;
  }
  
  if (triggered2 && lastTriggerTime2 > 0) {
    unsigned long pulseInterval = currentTriggerTime2 - lastTriggerTime2;
    
    // Проверка на разумные значения: от 20ms (3000 RPM) до 3000ms (20 RPM)
    if (pulseInterval >= 20 && pulseInterval <= 3000) {
      float instantRpm = 60000.0 / (float)pulseInterval;
      
      // Ограничение для предотвращения сбоев
      if (instantRpm <= 5000) {
        rpm2 = getAverageRPM(rpm2Samples, instantRpm);
      }
    }
    
    lastTriggerTime2 = currentTriggerTime2;
    triggered2 = false;
  }
  
  // Сброс RPM при отсутствии сигнала (увеличено время для высоких оборотов)
  if (currentMillis - lastTriggerTime1 > 1500) rpm1 = 0.0;
  if (currentMillis - lastTriggerTime2 > 1500) rpm2 = 0.0;
}

void updateDisplays() {
  // Корректное приведение типов с округлением
  int displayRpm1 = (int)round(rpm1);
  int displayRpm2 = (int)round(rpm2);
  
  // Ограничение для 4-разрядного дисплея
  if (displayRpm1 > 9999) displayRpm1 = 9999;
  if (displayRpm2 > 9999) displayRpm2 = 9999;
  
  display1.showNumberDecEx(displayRpm1, 0, true);
  display2.showNumberDecEx(displayRpm2, 0, true);
}

void updateHistory() {
  if (recording) {
    unsigned long currentTime = millis();
    rpm1History[dataIndex] = rpm1;
    rpm2History[dataIndex] = rpm2;
    timeHistory[dataIndex] = currentTime / 1000;
    dataIndex++;
    if (dataIndex >= MAX_DATA_POINTS) {
      dataIndex = 0;
      dataFull = true;
    }
  }
}

String getHTML() {
  return R"rawliteral(
  <!DOCTYPE html>
  <html lang="ru">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Двойной счетчик оборотов</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js@3.9.1/dist/chart.min.js"></script>
    <style>
      * { box-sizing: border-box; margin: 0; padding: 0; }
      body {
        font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
        background: #f0f2f5;
        color: #1a1a1a;
        transition: all 0.3s ease;
        line-height: 1.6;
      }
      .container {
        max-width: 900px;
        margin: 40px auto;
        padding: 0 20px;
      }
      h1 {
        text-align: center;
        font-size: 2.5rem;
        margin-bottom: 20px;
        color: #1a1a1a;
      }
      .buttons {
        display: flex;
        justify-content: center;
        gap: 20px;
        margin-bottom: 30px;
        flex-wrap: wrap;
      }
      .theme-toggle, .download-btn, .record-toggle {
        padding: 12px 24px;
        font-size: 1rem;
        border: none;
        border-radius: 25px;
        cursor: pointer;
        transition: transform 0.2s, background 0.3s;
      }
      .theme-toggle {
        background: #6200ea;
        color: #fff;
      }
      .download-btn {
        background: #4CAF50;
        color: white;
      }
      .record-toggle {
        background: #ff5722;
        color: white;
      }
      .theme-toggle:hover { background: #7c4dff; }
      .download-btn:hover { background: #45a049; }
      .record-toggle:hover { background: #e64a19; }
      .sensor-data {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
        gap: 20px;
        margin-top: 30px;
      }
      .sensor-card {
        background: #fff;
        border-radius: 12px;
        padding: 20px;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
        text-align: center;
        transition: transform 0.2s;
      }
      .sensor-card:hover {
        transform: translateY(-5px);
      }
      .sensor-card h3 {
        font-size: 1.2rem;
        color: #555;
        margin-bottom: 10px;
      }
      .sensor-card .rpm-value {
        font-size: 2rem;
        font-weight: bold;
        color: #6200ea;
      }
      .sensor-card .rpm-value span {
        font-size: 1rem;
        font-weight: normal;
        color: #777;
      }
      .chart-container {
        margin-top: 40px;
        background: #fff;
        border-radius: 12px;
        padding: 20px;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
      }
      body.dark {
        background: #121212;
        color: #e0e0e0;
      }
      body.dark h1 { color: #ffffff; }
      body.dark .sensor-card, body.dark .chart-container {
        background: #1e1e1e;
        box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
      }
      body.dark .sensor-card h3 { color: #b0b0b0; }
      body.dark .sensor-card .rpm-value { color: #bb86fc; }
      body.dark .sensor-card .rpm-value span { color: #aaa; }
    </style>
  </head>
  <body>
    <div class="container">
      <h1>Двойной счетчик оборотов</h1>
      <div class="buttons">
        <button class="theme-toggle" onclick="toggleTheme()">
          <span id="theme-icon">🌙</span> Переключить тему
        </button>
        <button class="download-btn" onclick="downloadData()">💾 Скачать данные</button>
        <button class="record-toggle" onclick="toggleRecording()">⏺️ Начать запись</button>
      </div>
      
      <div class="sensor-data">
        <div class="sensor-card">
          <h3>Датчик 1</h3>
          <div class="rpm-value" id="rpm1-value"><strong>0</strong> <span>RPM</span></div>
        </div>
        <div class="sensor-card">
          <h3>Датчик 2</h3>
          <div class="rpm-value" id="rpm2-value"><strong>0</strong> <span>RPM</span></div>
        </div>
      </div>
      <div class="chart-container">
        <canvas id="rpmChart"></canvas>
      </div>
    </div>

    <script>
      let rpmChart;
      document.addEventListener('DOMContentLoaded', function() {
        applyTheme(localStorage.getItem('theme') || 'light');
        initChart();
        updateSensorData();
        setInterval(updateSensorData, 500); // Увеличена частота обновления
      });

      function toggleTheme() {
        const isDark = document.body.classList.toggle('dark');
        localStorage.setItem('theme', isDark ? 'dark' : 'light');
        document.getElementById('theme-icon').textContent = isDark ? '☀️' : '🌙';
        fetch('/theme?mode=' + (isDark ? '1' : '0'));
        updateChartTheme(isDark);
      }

      function applyTheme(theme) {
        if (theme === 'dark') {
          document.body.classList.add('dark');
          document.getElementById('theme-icon').textContent = '☀️';
        } else {
          document.body.classList.remove('dark');
          document.getElementById('theme-icon').textContent = '🌙';
        }
      }

      function toggleRecording() {
        fetch('/toggle-recording')
          .then(response => response.text())
          .then(data => {
            const button = document.querySelector('.record-toggle');
            button.textContent = data === '1' ? '⏹️ Остановить запись' : '⏺️ Начать запись';
          });
      }

      function initChart() {
        const ctx = document.getElementById('rpmChart').getContext('2d');
        rpmChart = new Chart(ctx, {
          type: 'line',
          data: {
            labels: [],
            datasets: [{
              label: 'Датчик 1 (RPM)',
              borderColor: '#6200ea',
              backgroundColor: 'rgba(98, 0, 234, 0.1)',
              data: [],
              fill: false
            }, {
              label: 'Датчик 2 (RPM)',
              borderColor: '#03dac6',
              backgroundColor: 'rgba(3, 218, 198, 0.1)',
              data: [],
              fill: false
            }]
          },
          options: {
            responsive: true,
            scales: {
              x: {
                title: { display: true, text: 'Время (секунды)', color: '#1a1a1a' },
                ticks: { color: '#1a1a1a' }
              },
              y: {
                title: { display: true, text: 'Обороты в минуту (RPM)', color: '#1a1a1a' },
                ticks: { color: '#1a1a1a' },
                beginAtZero: true,
                max: 3500 // Увеличен максимум для двигателя 3К
              }
            },
            plugins: {
              legend: { labels: { color: '#1a1a1a' } }
            }
          }
        });
      }

      function updateChartTheme(isDark) {
        const color = isDark ? '#e0e0e0' : '#1a1a1a';
        rpmChart.options.scales.x.title.color = color;
        rpmChart.options.scales.x.ticks.color = color;
        rpmChart.options.scales.y.title.color = color;
        rpmChart.options.scales.y.ticks.color = color;
        rpmChart.options.plugins.legend.labels.color = color;
        rpmChart.update();
      }

      function updateSensorData() {
        fetch('/sensor-data')
          .then(response => response.json())
          .then(data => {
            document.querySelector('#rpm1-value strong').textContent = Math.round(data.rpm1);
            document.querySelector('#rpm2-value strong').textContent = Math.round(data.rpm2);
            updateChart(data);
            const button = document.querySelector('.record-toggle');
            button.textContent = data.recording ? '⏹️ Остановить запись' : '⏺️ Начать запись';
          });
      }

      function updateChart(data) {
        const labels = data.time.map(t => t - data.time[0]);
        rpmChart.data.labels = labels;
        rpmChart.data.datasets[0].data = data.rpm1History;
        rpmChart.data.datasets[1].data = data.rpm2History;
        rpmChart.update();
      }

      function downloadData() {
        fetch('/download-data')
          .then(response => response.text())
          .then(data => {
            const blob = new Blob([data], { type: 'text/csv;charset=utf-8;' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = 'rpm_data.csv';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
            URL.revokeObjectURL(url);
          });
      }
    </script>
  </body>
  </html>
  )rawliteral";
}

void handleSensorData(AsyncWebServerRequest *request) {
  DynamicJsonDocument doc(2048);
  doc["rpm1"] = rpm1;
  doc["rpm2"] = rpm2;
  doc["recording"] = recording;
  
  JsonArray rpm1Array = doc.createNestedArray("rpm1History");
  JsonArray rpm2Array = doc.createNestedArray("rpm2History");
  JsonArray timeArray = doc.createNestedArray("time");
  
  int start = dataFull ? dataIndex : 0;
  int count = dataFull ? MAX_DATA_POINTS : dataIndex;
  
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_DATA_POINTS;
    rpm1Array.add(rpm1History[idx]);
    rpm2Array.add(rpm2History[idx]);
    timeArray.add(timeHistory[idx]);
  }

  String json;
  serializeJson(doc, json);

  request->send(200, "application/json", json);
}

void handleDownloadData(AsyncWebServerRequest *request) {
  String csv = "Время (с),RPM1,RPM2\n";
  
  if (dataFull) {
    for (int i = dataIndex; i < MAX_DATA_POINTS; i++) {
      csv += String(timeHistory[i]) + "," + String(rpm1History[i]) + "," + String(rpm2History[i]) + "\n";
    }
    for (int i = 0; i < dataIndex; i++) {
      csv += String(timeHistory[i]) + "," + String(rpm1History[i]) + "," + String(rpm2History[i]) + "\n";
    }
  } else {
    for (int i = 0; i < dataIndex; i++) {
      csv += String(timeHistory[i]) + "," + String(rpm1History[i]) + "," + String(rpm2History[i]) + "\n";
    }
  }

  AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", csv);
  response->addHeader("Content-Disposition", "attachment; filename=rpm_data.csv");
  request->send(response);
}

void handleTheme(AsyncWebServerRequest *request) {
  if (request->hasParam("mode")) {
    darkMode = request->getParam("mode")->value() == "1";
    EEPROM.write(0, darkMode);
    EEPROM.commit();
  }
  request->send(200, "text/plain", "OK");
}

void handleToggleRecording(AsyncWebServerRequest *request) {
  recording = !recording;
  request->send(200, "text/plain", recording ? "1" : "0");
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(1);
  darkMode = EEPROM.read(0);

  // Инициализация массивов сглаживания
  for (int i = 0; i < SMOOTHING_SAMPLES; i++) {
    rpm1Samples[i] = 0.0;
    rpm2Samples[i] = 0.0;
  }

  pinMode(HALL1_DIGITAL_PIN, INPUT_PULLUP);
  pinMode(HALL2_DIGITAL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL1_DIGITAL_PIN), hallTrigger1, FALLING);
  attachInterrupt(digitalPinToInterrupt(HALL2_DIGITAL_PIN), hallTrigger2, FALLING);
  
  // Инициализация дисплеев
  display1.setBrightness(7);
  display2.setBrightness(7);
  display1.clear();
  display2.clear();
  updateDisplays();

  // Инициализация истории
  memset(rpm1History, 0, sizeof(rpm1History));
  memset(rpm2History, 0, sizeof(rpm2History));
  memset(timeHistory, 0, sizeof(timeHistory));

  // Подключение к WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/html", getHTML());
    });
    server.on("/sensor-data", HTTP_GET, handleSensorData);
    server.on("/theme", HTTP_GET, handleTheme);
    server.on("/download-data", HTTP_GET, handleDownloadData);
    server.on("/toggle-recording", HTTP_GET, handleToggleRecording);
    
    server.begin();
  } else {
    Serial.println("Не удалось подключиться к WiFi");
  }
}

void loop() {
  calculateRPM();
  
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 500) {
    updateHistory();
    updateDisplays();
  }
}