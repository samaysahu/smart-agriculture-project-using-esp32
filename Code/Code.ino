#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "Admin"; // Replace with your WiFi SSID
const char* password = "12345678"; // Replace with your WiFi password

// ====== Web Server and WebSocket ======
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// ====== LCD Setup ======
LiquidCrystal_I2C lcd(0x27, 16, 2);  // LCD address 0x27, 16 chars, 2 rows

// ====== Flow Sensor ======
const int flowSensorPin = 27;
volatile uint32_t pulseCount = 0;
float calibrationFactor = 7.5;
float flowRate = 0.0;
float totalLiters = 0.0;
unsigned long oldTime = 0;

void IRAM_ATTR pulseCounter() {
  pulseCount++;
}

// ====== Soil Moisture Sensor ======
#define MoistureDigitalPin 32
int soilStatus = HIGH; // initialized as dry

// ====== Relay Pin ======
#define RelayPin 26

// ====== TDS Sensor ======
#define TdsSensorPin 33
#define VREF 3.3
#define SCOUNT 30
int analogBuffer[SCOUNT];
int analogBufferIndex = 0;
float tdsValue = 0.0;

float getMedianNum(int* array, int size) {
  int sorted[size];
  memcpy(sorted, array, sizeof(int) * size);
  for (int i = 0; i < size - 1; i++) {
    for (int j = i + 1; j < size; j++) {
      if (sorted[j] < sorted[i]) {
        int temp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = temp;
      }
    }
  }
  if (size % 2 == 0)
    return (sorted[size / 2] + sorted[size / 2 - 1]) / 2.0;
  else
    return sorted[size / 2];
}

int displayIndex = 0;
unsigned long lastDisplayTime = 0;
const unsigned long displayInterval = 2000; // 2 seconds per reading

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_DISCONNECTED) {
    Serial.printf("Client [%u] Disconnected\n", num);
  } else if (type == WStype_CONNECTED) {
    Serial.printf("Client [%u] Connected\n", num);
  }
}

void setup() {
  Serial.begin(115200);

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // Flow sensor pin
  pinMode(flowSensorPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

  // Soil moisture pin
  pinMode(MoistureDigitalPin, INPUT);

  // Relay pin
  pinMode(RelayPin, OUTPUT);

  // TDS sensor pin
  pinMode(TdsSensorPin, INPUT);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Start WebSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // Serve a simple webpage
  server.on("/", []() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Sensor Dashboard</title>
  <script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-gray-100 font-sans">
  <div class="container mx-auto p-4">
    <h1 class="text-3xl font-bold text-center mb-6">Sensor Dashboard</h1>
    <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
      <div class="bg-white p-4 rounded shadow">
        <h2 class="text-xl font-semibold">Flow Rate</h2>
        <p id="flowRate" class="text-2xl">0.00 L/min</p>
      </div>
      <div class="bg-white p-4 rounded shadow">
        <h2 class="text-xl font-semibold">Total Liters</h2>
        <p id="totalLiters" class="text-2xl">0.000 L</p>
      </div>
      <div class="bg-white p-4 rounded shadow">
        <h2 class="text-xl font-semibold">Soil Moisture</h2>
        <p id="soilMoisture" class="text-2xl">Dry</p>
      </div>
      <div class="bg-white p-4 rounded shadow">
        <h2 class="text-xl font-semibold">TDS Value</h2>
        <p id="tdsValue" class="text-2xl">0 ppm</p>
      </div>
    </div>
  </div>
  <script>
    const ws = new WebSocket('ws://' + window.location.hostname + ':81/');
    ws.onmessage = function(event) {
      const data = JSON.parse(event.data);
      document.getElementById('flowRate').textContent = data.flowRate.toFixed(2) + ' L/min';
      document.getElementById('totalLiters').textContent = data.totalLiters.toFixed(3) + ' L';
      document.getElementById('soilMoisture').textContent = data.soilMoisture;
      document.getElementById('tdsValue').textContent = data.tdsValue.toFixed(0) + ' ppm';
    };
    ws.onclose = function() {
      console.log('WebSocket connection closed');
    };
    ws.onerror = function(error) {
      console.error('WebSocket error:', error);
    };
  </script>
</body>
</html>
    )rawliteral";
    server.send(200, "text/html", html);
  });

  server.begin();
}

void loop() {
  webSocket.loop();
  server.handleClient();

  unsigned long currentTime = millis();

  // Read soil moisture
  soilStatus = digitalRead(MoistureDigitalPin);

  // Control relay based on soil moisture
  if (soilStatus == HIGH) { // Dry soil
    digitalWrite(RelayPin, HIGH); // Turn relay ON
  } else { // Wet soil
    digitalWrite(RelayPin, LOW); // Turn relay OFF
  }

  // TDS sampling every 40ms
  static unsigned long analogSampleTimepoint = 0;
  if (currentTime - analogSampleTimepoint > 40) {
    analogSampleTimepoint = currentTime;
    analogBuffer[analogBufferIndex] = analogRead(TdsSensorPin);
    analogBufferIndex++;
    if (analogBufferIndex == SCOUNT) {
      analogBufferIndex = 0;
      float averageVoltage = getMedianNum(analogBuffer, SCOUNT) * VREF / 4096.0;
      float compensationCoefficient = 1.0 + 0.02 * (25.0 - 25.0);
      float compensationVoltage = averageVoltage / compensationCoefficient;
      tdsValue = (133.42 * pow(compensationVoltage, 3)
                  - 255.86 * pow(compensationVoltage, 2)
                  + 857.39 * compensationVoltage) * 0.5;
    }
  }

  // Update flow sensor every second and send data via WebSocket
  if (currentTime - oldTime > 1000) {
    detachInterrupt(digitalPinToInterrupt(flowSensorPin));
    flowRate = (pulseCount / calibrationFactor); // L/min
    totalLiters += (flowRate / 60.0);
    pulseCount = 0;
    oldTime = currentTime;
    attachInterrupt(digitalPinToInterrupt(flowSensorPin), pulseCounter, FALLING);

    // Prepare JSON data
    StaticJsonDocument<200> doc;
    doc["flowRate"] = flowRate;
    doc["totalLiters"] = totalLiters;
    doc["soilMoisture"] = (soilStatus == HIGH) ? "Dry" : "Wet";
    doc["tdsValue"] = tdsValue;
    String json;
    serializeJson(doc, json);

    // Send to all WebSocket clients
    webSocket.broadcastTXT(json);

    // Print to Serial for debugging
    Serial.print("Flow: "); Serial.print(flowRate, 2);
    Serial.print(" L/min | Total: "); Serial.print(totalLiters, 3);
    Serial.print(" L | Soil: "); Serial.print((soilStatus == HIGH) ? "Dry" : "Wet");
    Serial.print(" | TDS: "); Serial.print(tdsValue, 0);
    Serial.println(" ppm");
  }

  // Update LCD display every 2 seconds
  if (currentTime - lastDisplayTime >= displayInterval) {
    lastDisplayTime = currentTime;
    lcd.clear();
    switch(displayIndex) {
      case 0:
        lcd.setCursor(0, 0); lcd.print("Flow Rate:");
        lcd.setCursor(0, 1); lcd.print(flowRate, 2); lcd.print(" L/min");
        break;
      case 1:
        lcd.setCursor(0, 0); lcd.print("Total Liters:");
        lcd.setCursor(0, 1); lcd.print(totalLiters, 3); lcd.print(" L");
        break;
      case 2:
        lcd.setCursor(0, 0); lcd.print("Soil Moisture:");
        lcd.setCursor(0, 1); lcd.print((soilStatus == HIGH) ? "Dry" : "Wet");
        break;
      case 3:
        lcd.setCursor(0, 0); lcd.print("TDS Value:");
        lcd.setCursor(0, 1); lcd.print(tdsValue, 0); lcd.print(" ppm");
        break;
    }
    displayIndex++;
    if (displayIndex > 3) displayIndex = 0;
  }
}
