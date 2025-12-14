#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <Adafruit_GPS.h>
#include <vl53l4cx_class.h>
#include <SD.h> 
#include "FS.h"
#include <WebSocketsServer.h>

const char* WIFI_SSID = "绵绵的索尼1000XM4";
const char* WIFI_PASS = "qwertyuiop";
const uint16_t TCP_PORT = 5000;

// ===== I2C  =====
#define I2C_SDA_PIN  3
#define I2C_SCL_PIN  4

// ===== SPI  =====
#define SPI_MOSI_PIN 11
#define SPI_MISO_PIN 12
#define SPI_SCK_PIN  13
#define SD_CS_PIN    10

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3D
   
#define SD_SPEED    8000000  
#define TOF_XSHUT_PIN -1

VL53L4CX tof(&Wire, TOF_XSHUT_PIN);

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME680 bme(&Wire);   // I2C
Adafruit_GPS GPS(&Wire);

#define GPSECHO false


WebServer http(80);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;

WebSocketsServer wsServer(81); 

uint32_t t_lastPrint = 0;
uint32_t t_lastGPSRead = 0;
uint32_t t_lastOLED = 0;
uint32_t t_lastTCPPush = 0;

struct SensorSnapshot {
  float tempC = NAN;
  float pressure_kPa = NAN;
  float humidity = NAN;
  float gas_kOhm = NAN;
  bool  bme_ok = false;
  float iaq = NAN;
  float gasR0 = -1;

  // GPS
  bool fix = false;
  uint8_t fixQuality = 0;
  float lat = NAN, lon = NAN;     
  float speed_knots = NAN;
  float angle_deg = NAN;
  float altitude_m = NAN;
  uint8_t sats = 0;
  uint8_t hour=0, minute=0, seconds=0, day=0, month=0;
  uint16_t year=0;
  int16_t tof_mm = -1;     // -1  ineffective
  bool tof_ok = false;
} SNAP;


String jsonEscape(const String& s) {
  String out; out.reserve(s.length()+8);
  for (size_t i=0;i<s.length();++i) {
    char c = s[i];
    if (c=='"' || c=='\\') { out += '\\'; out += c; }
    else if (c=='\n') out += "\\n";
    else if (c=='\r') out += "\\r";
    else out += c;
  }
  return out;
}

String buildSensorJSON() {
  String js = "{";
  js += "\"bme_ok\":" + String(SNAP.bme_ok ? "true":"false") + ",";
  js += "\"temperature_C\":" + String(SNAP.tempC, 2) + ",";
  js += "\"pressure_kPa\":" + String(SNAP.pressure_kPa, 2) + ",";
  js += "\"humidity_pct\":" + String(SNAP.humidity, 2) + ",";
  js += "\"gas_kOhm\":" + String(SNAP.gas_kOhm, 3) + ",";
  js += "\"iaq\":" + String(SNAP.iaq, 1) + ",";

  js += "\"gps\":{";
    js += "\"fix\":" + String(SNAP.fix ? "true":"false") + ",";
    js += "\"quality\":" + String(SNAP.fixQuality) + ",";
    js += "\"lat\":" + String(SNAP.lat, 6) + ",";
    js += "\"lon\":" + String(SNAP.lon, 6) + ",";
    js += "\"speed_knots\":" + String(SNAP.speed_knots, 2) + ",";
    js += "\"angle_deg\":" + String(SNAP.angle_deg, 2) + ",";
    js += "\"altitude_m\":" + String(SNAP.altitude_m, 2) + ",";
    js += "\"sats\":" + String(SNAP.sats) + ",";
    js += "\"time\":\"";
      if (SNAP.hour<10) js += '0'; js += String(SNAP.hour); js += ":";
      if (SNAP.minute<10) js += '0'; js += String(SNAP.minute); js += ":";
      if (SNAP.seconds<10) js += '0'; js += String(SNAP.seconds);
      js += "\",";
    js += "\"date\":\"";
      js += String(SNAP.day) + "/";
      js += String(SNAP.month) + "/";
      js += String(2000 + SNAP.year);
    js += "\"";
  js += "}";
  js += ",\"tof_ok\":" + String(SNAP.tof_ok ? "true":"false");
  js += ",\"tof_mm\":" + String(SNAP.tof_mm);
  js += "}";
  return js;
}

File g_logFile;
char g_logPath[64] = {0};
bool g_sd_ok = false;

void handleRoot() {
  String html = R"rawliteral(
    <!doctype html>
    <html>
    <head>
      <meta charset="utf-8">
      <title>ESP32-S3 Env Realtime</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; padding: 12px; background:#f5f5f5; }
        h2 { margin-top:0; }
        .card {
          background:#fff;
          border-radius:12px;
          padding:12px 14px;
          margin-bottom:10px;
          box-shadow:0 2px 6px rgba(0,0,0,0.08);
        }
        .row {
          display:flex;
          justify-content:space-between;
          margin:4px 0;
          font-size:14px;
        }
        .label { color:#666; }
        .value { font-weight:600; }
        #status { font-size:12px; color:#888; margin-top:4px; }
        #tempBig { font-size:32px; font-weight:700; }
        canvas { width:100%; max-width:400px; height:180px; }
      </style>
    </head>
    <body>
      <h2>ESP32-S3 ENV Dashboard</h2>

      <div class="card">
        <div class="row"><span class="label">Device IP</span><span class="value" id="ip"></span></div>
        <div id="status">WebSocket: <span id="wsStatus">Connecting...</span></div>
      </div>

      <div class="card">
        <div class="row"><span class="label">Temperature</span><span class="value" id="tempBig">-- °C</span></div>
        <div class="row"><span class="label">Humidity</span><span class="value" id="hum">-- %</span></div>
        <div class="row"><span class="label">Pressure</span><span class="value" id="press">-- kPa</span></div>
        <div class="row"><span class="label">IAQ</span><span class="value" id="iaq">-- </span></div>
      </div>

      <div class="card">
        <div class="row"><span class="label">GPS Fix</span><span class="value" id="fix">NO</span></div>
        <div class="row"><span class="label">SA Num</span><span class="value" id="sat">0</span></div>
        <div class="row"><span class="label">Time</span><span class="value" id="time">--</span></div>
        <div class="row"><span class="label">Location</span><span class="value" id="pos">N/A</span></div>
      </div>

      <div class="card">
        <div class="row"><span class="label">Temperature</span></div>
        <canvas id="tempChart" width="400" height="180"></canvas>
      </div>

      <div class="card">
        <div class="row"><span class="label">Humidity</span></div>
        <canvas id="humChart" width="400" height="180"></canvas>
      </div>

      <script>
        const ipSpan    = document.getElementById('ip');
        const wsStatus  = document.getElementById('wsStatus');
        const tempBig   = document.getElementById('tempBig');
        const humSpan   = document.getElementById('hum');
        const pressSpan = document.getElementById('press');
        const gasSpan   = document.getElementById('iaq');
        const fixSpan   = document.getElementById('fix');
        const satSpan   = document.getElementById('sat');
        const timeSpan  = document.getElementById('time');
        const posSpan   = document.getElementById('pos');

        ipSpan.textContent = location.hostname;

        // -------- WebSocket 连接 --------
        let ws = null;
        function connectWS() {
          const url = 'ws://' + location.hostname + ':81/';
          ws = new WebSocket(url);

          ws.onopen = () => {
            wsStatus.textContent = 'Connected';
          };
          ws.onclose = () => {
            wsStatus.textContent = 'Reconnecting...';
            setTimeout(connectWS, 2000);
          };
          ws.onerror = () => {
            wsStatus.textContent = 'Error';
          };
          ws.onmessage = (evt) => {
            try {
              const data = JSON.parse(evt.data);
              updateValues(data);
              updateChart(data.temperature_C);
              updateHumChart(data.humidity_pct);
            } catch(e) {
              console.log('JSON parse error', e);
            }
          };
        }
        connectWS();

        function fmt(x, d) {
          if (x === null || x === undefined || !isFinite(x)) return '--';
          return Number(x).toFixed(d);
        }

        function updateValues(data) {
          tempBig.textContent = fmt(data.temperature_C, 2) + ' °C';
          humSpan.textContent = fmt(data.humidity_pct, 2) + ' %';
          pressSpan.textContent = fmt(data.pressure_kPa, 2) + ' kPa';
          gasSpan.textContent = fmt(data.iaq, 3) + ' ';

          if (data.gps) {
            fixSpan.textContent = data.gps.fix ? 'YES' : 'NO';
            satSpan.textContent = data.gps.sats;
            timeSpan.textContent = data.gps.time || '--';
            if (isFinite(data.gps.lat) && isFinite(data.gps.lon)) {
              posSpan.textContent = data.gps.lat.toFixed(5) + ', ' + data.gps.lon.toFixed(5);
            } else {
              posSpan.textContent = 'N/A';
            }
          }
        }

                // -------- 温度 / 湿度折线图 --------
        const canvas = document.getElementById('tempChart');
        const ctx = canvas.getContext('2d');

        const humCanvas = document.getElementById('humChart');
        const humCtx = humCanvas.getContext('2d');

        const MAX_POINTS = 60;
        const tempData = []; // {t, y}
        const humData  = []; // {t, y}

        // 温度曲线
        function updateChart(temp) {
          if (!isFinite(temp)) return;
          const t = Date.now() / 1000;
          tempData.push({ t, y: temp });
          if (tempData.length > MAX_POINTS) tempData.shift();
          drawChart();
        }

        function drawChart() {
          const w = canvas.width;
          const h = canvas.height;
          ctx.clearRect(0, 0, w, h);

          if (tempData.length < 2) return;

          const tMin = tempData[0].t;
          const tMax = tempData[tempData.length - 1].t;
          let yMin = tempData[0].y;
          let yMax = tempData[0].y;
          for (const p of tempData) {
            if (p.y < yMin) yMin = p.y;
            if (p.y > yMax) yMax = p.y;
          }
          if (yMax === yMin) { yMax += 0.5; yMin -= 0.5; }

          // 背景网格
          ctx.strokeStyle = '#ddd';
          ctx.lineWidth = 1;
          ctx.beginPath();
          ctx.moveTo(40, 10);
          ctx.lineTo(40, h-20);
          ctx.lineTo(w-10, h-20);
          ctx.stroke();

          // 画折线
          ctx.strokeStyle = '#007aff';
          ctx.lineWidth = 2;
          ctx.beginPath();
          tempData.forEach((p, idx) => {
            const x = 40 + (w-50) * (p.t - tMin) / (tMax - tMin || 1);
            const y = (h-20) - (h-40) * (p.y - yMin) / (yMax - yMin || 1);
            if (idx === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
          });
          ctx.stroke();

          // 左侧标尺
          ctx.fillStyle = '#666';
          ctx.font = '10px -apple-system';
          ctx.fillText(yMax.toFixed(1) + '°C', 4, 14);
          ctx.fillText(yMin.toFixed(1) + '°C', 4, h-22);
        }

        // 湿度曲线
        function updateHumChart(hum) {
          if (!isFinite(hum)) return;
          const t = Date.now() / 1000;
          humData.push({ t, y: hum });
          if (humData.length > MAX_POINTS) humData.shift();
          drawHumChart();
        }

        function drawHumChart() {
          const w = humCanvas.width;
          const h = humCanvas.height;
          humCtx.clearRect(0, 0, w, h);

          if (humData.length < 2) return;

          const tMin = humData[0].t;
          const tMax = humData[humData.length - 1].t;
          let yMin = humData[0].y;
          let yMax = humData[0].y;
          for (const p of humData) {
            if (p.y < yMin) yMin = p.y;
            if (p.y > yMax) yMax = p.y;
          }
          if (yMax === yMin) { yMax += 1.0; yMin -= 1.0; }

          // 背景网格
          humCtx.strokeStyle = '#ddd';
          humCtx.lineWidth = 1;
          humCtx.beginPath();
          humCtx.moveTo(40, 10);
          humCtx.lineTo(40, h-20);
          humCtx.lineTo(w-10, h-20);
          humCtx.stroke();

          // 画折线
          humCtx.strokeStyle = '#00aa55';
          humCtx.lineWidth = 2;
          humCtx.beginPath();
          humData.forEach((p, idx) => {
            const x = 40 + (w-50) * (p.t - tMin) / (tMax - tMin || 1);
            const y = (h-20) - (h-40) * (p.y - yMin) / (yMax - yMin || 1);
            if (idx === 0) humCtx.moveTo(x, y);
            else humCtx.lineTo(x, y);
          });
          humCtx.stroke();

          // 左侧标尺
          humCtx.fillStyle = '#666';
          humCtx.font = '10px -apple-system';
          humCtx.fillText(yMax.toFixed(1) + '%', 4, 14);
          humCtx.fillText(yMin.toFixed(1) + '%', 4, h-22);
        }
      </script>
    </body>
    </html>
    )rawliteral";

  http.send(200, "text/html; charset=utf-8", html);
}


void handleHealth() {
  http.send(200, "text/plain", "OK");
}

void handleSensors() {
  String js = buildSensorJSON();
  http.send(200, "application/json", js);
}

void handleOLED() {
  String text = http.hasArg("text") ? http.arg("text") : "Hello";
  text = jsonEscape(text);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println(text);
  display.display();
  http.send(200, "application/json", String("{\"ok\":true,\"shown\":\"") + text + "\"}");
}

void setupHTTP() {
  http.on("/", handleRoot);
  http.on("/health", HTTP_GET, handleHealth);
  http.on("/api/sensors", HTTP_GET, handleSensors);
  http.on("/api/oled", HTTP_GET, handleOLED);
  http.begin();
}

void onWsEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.printf("[WS] Client %u connected\n", num);
    String js = buildSensorJSON();
    wsServer.sendTXT(num, js);
  } 
  else if (type == WStype_DISCONNECTED) {
    Serial.printf("[WS] Client %u disconnected\n", num);
  }
  else if (type == WStype_TEXT) {
    Serial.printf("[WS] Got text from %u: %s\n", num, (const char*)payload);
  }
}


void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - t0 > 15000) break;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi failed (will keep retrying in loop).");
  }
}

// 计算给定温度(°C)下的饱和水汽压 (hPa)
// Magnus 公式近似
float saturationVaporPressure_hPa(float tC) {
  return 6.112f * expf((17.62f * tC) / (243.12f + tC));
}

void readBME680() {
  SNAP.bme_ok = bme.performReading();
  if (SNAP.bme_ok) {
    // 原始读数
    float T_raw   = bme.temperature;   // °C
    float RH_raw  = bme.humidity;      // %

    // 1) 温度校准：减 5 ℃
    float T_cal = T_raw - 5.0f;

    // 2) 用饱和水汽压比例校正相对湿度
    float es_raw = saturationVaporPressure_hPa(T_raw);
    float es_cal = saturationVaporPressure_hPa(T_cal);

    float RH_cal = RH_raw * (es_raw / es_cal);

    // 3) 合理范围裁剪到 [0, 100] %
    if (RH_cal > 100.0f) RH_cal = 100.0f;
    if (RH_cal <   0.0f) RH_cal = 0.0f;

    // 4) 写回 SNAP（之后所有 JSON / WebSocket / OLED / SD 都用校准后的值）
    SNAP.tempC        = T_cal;
    SNAP.humidity     = RH_cal;
    SNAP.pressure_kPa = bme.pressure / 1000.0f;
    SNAP.gas_kOhm     = bme.gas_resistance;
    float R = bme.gas_resistance;

    SNAP.gasR0 = 39420.00f;

    float ratio = SNAP.gasR0 / R;              // R 越小 VOC 越多
    float iaq = 100.0f * powf(ratio, 1.4f);

    // 限制范围（模仿 BSEC）
    if (iaq > 500.0f) iaq = 500.0f;
    if (iaq <   0.0f) iaq =   0.0f;

    SNAP.iaq = iaq;
  }
}

void pollGPS() {
  char c = GPS.read();
  if (GPSECHO && c) Serial.print(c);

  if (GPS.newNMEAreceived()) {
    if (GPS.parse(GPS.lastNMEA())) {
      SNAP.fix         = GPS.fix;
      SNAP.fixQuality  = GPS.fixquality;

      float latAbs = GPS.latitude / 100.0; 
      float lonAbs = GPS.longitude / 100.0;
      SNAP.lat = (GPS.lat == 'S') ? -GPS.latitude : GPS.latitude;
      SNAP.lon = (GPS.lon == 'W') ? -GPS.longitude : GPS.longitude;

      SNAP.speed_knots = GPS.speed;
      SNAP.angle_deg    = GPS.angle;
      SNAP.altitude_m   = GPS.altitude;
      SNAP.sats         = GPS.satellites;

      SNAP.hour   = GPS.hour;
      SNAP.minute = GPS.minute;
      SNAP.seconds= GPS.seconds;
      SNAP.day    = GPS.day;
      SNAP.month  = GPS.month;
      SNAP.year   = GPS.year;
    }
  }
}

void handleTCP() {
  if (!tcpClient || !tcpClient.connected()) {
    tcpClient.stop();
    WiFiClient newClient = tcpServer.available();
    if (newClient) {
      tcpClient = newClient;
      tcpClient.println("ESP32-S3 connected. Type 'help' or 'json'.");
    }
  } else {
    while (tcpClient.available()) {
      String line = tcpClient.readStringUntil('\n');
      line.trim();
      if (line.equalsIgnoreCase("help")) {
        tcpClient.println("Commands: json, id, help, oled <text>");
      } else if (line.equalsIgnoreCase("id")) {
        tcpClient.println("ESP32-S3 Env Station");
      } else if (line.equalsIgnoreCase("json")) {
        tcpClient.println(buildSensorJSON());
      } else if (line.startsWith("oled ")) {
        String msg = line.substring(5);
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0,0);
        display.println(msg);
        display.display();
        tcpClient.println(String("OLED shown: ") + msg);
      } else {
        tcpClient.println(String("echo: ") + line);
      }
    }
    if (millis() - t_lastTCPPush > 1000) {
      t_lastTCPPush = millis();
      tcpClient.println(buildSensorJSON());
    }
  }
}

bool setupToF() {
  // 可选：如果接了 XSHUT
  if (TOF_XSHUT_PIN != -1) {
    pinMode(TOF_XSHUT_PIN, OUTPUT);
    digitalWrite(TOF_XSHUT_PIN, HIGH);
    delay(5);
  }

  if (tof.begin() != 0) {                     // 0 = OK
    Serial.println("[ToF] VL53L4CX begin FAIL");
    SNAP.tof_ok = false;
    return false;
  }

  tof.VL53L4CX_Off();
  if (tof.InitSensor(0x52) != 0) {            // 0x52 = 0x29 的写地址形式
    Serial.println("[ToF] InitSensor FAIL");
    SNAP.tof_ok = false;
    return false;
  }

  // 连续测量（也可按需设置测量预算/距离模式）
  tof.VL53L4CX_StartMeasurement();
  SNAP.tof_ok = true;
  Serial.println("[ToF] VL53L4CX ready");
  return true;
}

void readToF() {
  if (!SNAP.tof_ok) return;

  uint8_t ready = 0;
  tof.VL53L4CX_GetMeasurementDataReady(&ready);
  if (!ready) return;

  VL53L4CX_MultiRangingData_t data;
  if (tof.VL53L4CX_GetMultiRangingData(&data) == 0) {
    if (data.NumberOfObjectsFound > 0) {
      uint8_t st = data.RangeData[0].RangeStatus; // 0 = OK
      uint16_t mm = data.RangeData[0].RangeMilliMeter;

      //Serial.print("[ToF] objects="); Serial.print(data.NumberOfObjectsFound);
      //Serial.print(" status="); Serial.print(st);
      //Serial.print(" dist="); Serial.print(mm);
      //Serial.println(" mm");

      if (st == 0) {
        SNAP.tof_mm = (int16_t)mm;
      } else {
        SNAP.tof_mm = -1;
      }
    } else {
      Serial.println("[ToF] no object found");
      SNAP.tof_mm = -1;
    }
    tof.VL53L4CX_ClearInterruptAndStartMeasurement();
  } else {
    Serial.println("[ToF] GetMultiRangingData FAIL");
    SNAP.tof_mm = -1;
  }
}


void makeLogPath() {
  int y = 2000 + SNAP.year;   // 若 GPS 未 fix，年可能为 0；处理见下面
  int m = SNAP.month;
  int d = SNAP.day;
  if (y < 2010 || m < 1 || m > 12 || d < 1 || d > 31) {
    // GPS 未定位时，用开机日期占位
    snprintf(g_logPath, sizeof(g_logPath), "/logs/env_%lu.csv", (unsigned long)(millis()/1000));
  } else {
    snprintf(g_logPath, sizeof(g_logPath), "/logs/env_%04d%02d%02d.csv", y, m, d);
  }
}

bool setupSD() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[SD] init FAIL (Card Mount Failed)");
    g_sd_ok = false;
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("[SD] No SD card attached");
    g_sd_ok = false;
    return false;
  }

  Serial.print("[SD] Card Type: ");
  if (cardType == CARD_MMC)      Serial.println("MMC");
  else if (cardType == CARD_SD)  Serial.println("SDSC");
  else if (cardType == CARD_SDHC)Serial.println("SDHC");
  else                           Serial.println("UNKNOWN");

  uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] Size: %llu MB\n", (unsigned long long)cardSizeMB);

  // 目录准备 + 列根目录
  if (!SD.exists("/logs")) SD.mkdir("/logs");

  // 打开日志文件
  makeLogPath();
  bool isNew = !SD.exists(g_logPath);
  g_logFile = SD.open(g_logPath, FILE_APPEND, true);
  if (!g_logFile) {
    Serial.println("[SD] open log FAIL");
    g_sd_ok = false;
    return false;
  }
  if (isNew) {
    g_logFile.println("timestamp,time,date,temp_C,humidity_pct,pressure_kPa,gas_kOhm,tof_mm,fix,lat,lon,sats");
    g_logFile.flush();
  }
  Serial.print("[SD] logging to "); Serial.println(g_logPath);

  g_sd_ok = true;
  return true;
}


void logToSD() {
  if (!g_sd_ok) return;

  char ts[24], tstr[16], dstr[16];
  if (SNAP.year > 0) {
    snprintf(tstr, sizeof(tstr), "%02u:%02u:%02u", SNAP.hour, SNAP.minute, SNAP.seconds);
    snprintf(dstr, sizeof(dstr), "%02u/%02u/%04u", SNAP.day, SNAP.month, 2000 + SNAP.year);
    snprintf(ts, sizeof(ts), "%04u%02u%02u-%02u%02u%02u",
             2000 + SNAP.year, SNAP.month, SNAP.day, SNAP.hour, SNAP.minute, SNAP.seconds);
  } else {
    unsigned long s = millis()/1000;
    snprintf(tstr, sizeof(tstr), "%02lu:%02lu:%02lu", s/3600, (s/60)%60, s%60);
    snprintf(dstr, sizeof(dstr), "N/A");
    snprintf(ts, sizeof(ts), "boot+%lus", s);
  }

  String line;
  line.reserve(160);
  line += ts; line += ",";
  line += tstr; line += ",";
  line += dstr; line += ",";
  line += String(SNAP.tempC,2); line += ",";
  line += String(SNAP.humidity,2); line += ",";
  line += String(SNAP.pressure_kPa,2); line += ",";
  line += String(SNAP.iaq,3); line += ",";
  line += String(SNAP.tof_mm); line += ",";
  line += (SNAP.fix ? "1":"0"); line += ",";
  line += String(SNAP.lat,6); line += ",";
  line += String(SNAP.lon,6); line += ",";
  line += String(SNAP.sats);

  g_logFile.println(line);
  g_logFile.flush(); 
}

void scanI2C() {
  Serial.println("I2C scan...");
  for (uint8_t addr=1; addr<127; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission()==0) {
      Serial.printf("  - 0x%02X found\n", addr);
      delay(2);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // I2C for all sensors + OLED
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);

  // SPI for SD
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SD_CS_PIN);

  // OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Booting...");
  display.display();

  // BME680
  if (!bme.begin()) {
    Serial.println("BME680 not found!");
    display.println("BME680 not found!");
    display.display();
  }

  // ToF
  setupToF();

  // SD
  setupSD();

  // GPS I2C
  GPS.begin(0x10);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  GPS.sendCommand(PGCMD_ANTENNA);
  delay(1000);
  GPS.println(PMTK_Q_RELEASE);
  delay(200);

  // Wi-Fi + HTTP + TCP + WebSocket
  connectWiFi();
  setupHTTP();
  tcpServer.begin();
  tcpServer.setNoDelay(true);
  wsServer.begin();
  wsServer.onEvent(onWsEvent);

  scanI2C();
}



void loop() {
  // Wi-Fi Reconnection
  static uint32_t t_wifi = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - t_wifi > 5000) {
    t_wifi = millis();
    connectWiFi();
  }

  // GPS
  pollGPS();

  // 500 ms  BME680
  if (millis() - t_lastGPSRead > 500) {
    t_lastGPSRead = millis();
    readBME680();
  }

  // HTTP 
  http.handleClient();

  // TCP 
  handleTCP();

  //WebSocket
  wsServer.loop();

  static uint32_t t_ws = 0;
  if (millis() - t_ws > 5000) {
    t_ws = millis();
    String js = buildSensorJSON();
    wsServer.broadcastTXT(js);
  }

  // —— ToF 读取：每 100 ms 一次 ——
  static uint32_t t_tof = 0;
  if (millis() - t_tof > 100) {
    t_tof = millis();
    readToF();
  }

  // —— SD 写日志：每 1 秒一行 ——
  static uint32_t t_log = 0;
  if (millis() - t_log > 3000) {
    t_log = millis();
    logToSD();
  }

  //OLED Refresh / s
  if (millis() - t_lastOLED > 1000) {
    t_lastOLED = millis();
    display.clearDisplay();
    display.setCursor(0,0);
    display.print("WiFi: ");
    if (WiFi.status()==WL_CONNECTED) display.println(WiFi.localIP()); else display.println("…");
    if (SNAP.bme_ok) {
      display.print("T:"); display.print(SNAP.tempC,2); display.print("C ");
      display.print("H:"); display.print(SNAP.humidity,2); display.println("%");
      display.print("P:"); display.print(SNAP.pressure_kPa,2); display.println("kPa");
      display.print("G:"); display.print(SNAP.iaq,2); display.println("");
    } else {
      display.println("BME680 NA");
    }
    display.print("GPS fix:");
    display.print(SNAP.fix ? "Y ":"N ");
    display.print("sat:"); display.println(SNAP.sats);

    display.print("Location: ");
    display.print(GPS.latitude, 4); display.print(GPS.lat);
    display.print(", ");
    display.print(GPS.longitude, 4); display.println(GPS.lon);
    //Serial.print("Speed (knots): "); Serial.println(GPS.speed);
    //Serial.print("Angle: "); Serial.println(GPS.angle);
    display.print("Altitude: "); display.println(GPS.altitude);
    //Serial.print("Satellites: "); Serial.println((int)GPS.satellites);
    display.display();
  }

  // Serial print
  if (millis() - t_lastPrint > 2000) {
    t_lastPrint = millis();
    Serial.println(buildSensorJSON());
  }
}
