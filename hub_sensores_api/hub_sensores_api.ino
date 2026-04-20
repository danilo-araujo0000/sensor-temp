#include <WiFi.h>
#include <HTTPClient.h>

constexpr char WIFI_SSID[] = "Tablet";
constexpr char WIFI_PASSWORD[] = "TABLET@admin2024";
constexpr char DEVICE_ID[] = "esp32s3-hub-01";
constexpr char SENSOR_REGISTER_URL[] = "http://172.19.200.1:5080/sensors/register";
constexpr char MOVEMENT_URL[] = "http://172.19.200.1:5080/movements";

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long SENSOR_POLL_MS = 25;


struct SensorConfig {
  const char* id;
  const char* name;
  const char* icon;
  uint8_t pin;
  bool activeHigh;
  bool usePulldown;
  bool lastState;
};

SensorConfig sensores[] = {
  { "mov_entrada", "Entrada Principal", "entry", 8, true, true, false },
  { "mov_sala", "Sala de Estar", "sofa", 17, true, true, false },
  { "mov_corredor", "Corredor", "hall", 18, true, true, false }
};

constexpr size_t TOTAL_SENSORES = sizeof(sensores) / sizeof(sensores[0]);

unsigned long ultimoWifiRetryMs = 0;
unsigned long ultimoPollMs = 0;
bool sensoresRegistrados = false;

void setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_BUILTIN, red, green, blue);
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, (red || green || blue) ? HIGH : LOW);
#endif
}

void piscarStatus(uint8_t red, uint8_t green, uint8_t blue, uint8_t repeticoes, unsigned long intervaloMs) {
  for (uint8_t i = 0; i < repeticoes; i++) {
    setLedColor(red, green, blue);
    delay(intervaloMs);
    setLedColor(0, 0, 0);
    delay(intervaloMs);
  }
}

void acenderStatus(uint8_t red, uint8_t green, uint8_t blue, unsigned long duracaoMs) {
  setLedColor(red, green, blue);
  delay(duracaoMs);
  setLedColor(0, 0, 0);
}

String montarSensorId(const SensorConfig& sensor) {
  return String(DEVICE_ID) + ":" + String(sensor.id);
}

bool postJson(const char* url, const String& payload, int& httpCode, String& resposta) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.print("Falha ao iniciar HTTPClient em ");
    Serial.println(url);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  httpCode = http.POST(payload);
  resposta = http.getString();
  http.end();
  return true;
}

bool registrarSensoresNoBackend() {
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"sensors\":[";

  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    if (i > 0) payload += ",";
    payload += "{";
    payload += "\"sensor_id\":\"" + montarSensorId(sensores[i]) + "\",";
    payload += "\"pin\":" + String(sensores[i].pin) + ",";
    payload += "\"name\":\"" + String(sensores[i].name) + "\",";
    payload += "\"icon\":\"" + String(sensores[i].icon) + "\"";
    payload += "}";
  }

  payload += "]}";

  int httpCode = 0;
  String resposta;
  if (!postJson(SENSOR_REGISTER_URL, payload, httpCode, resposta)) {
    return false;
  }

  Serial.print("POST register -> HTTP ");
  Serial.println(httpCode);
  if (resposta.length()) {
    Serial.println(resposta);
  }

  sensoresRegistrados = httpCode >= 200 && httpCode < 300;
  return sensoresRegistrados;
}

void conectarWifi() {
  Serial.print("Conectando no WiFi ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_RETRY_MS) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
    piscarStatus(0, 32, 0, 2, 120);
    sensoresRegistrados = false;
    registrarSensoresNoBackend();
  } else {
    Serial.println("Falha ao conectar no WiFi.");
    piscarStatus(32, 16, 0, 2, 120);
  }
}

bool enviarEvento(const SensorConfig& sensor) {
  if (WiFi.status() != WL_CONNECTED || !sensoresRegistrados) {
    return false;
  }

  String payload = "{\"sensor_id\":\"" + montarSensorId(sensor) + "\"}";
  int httpCode = 0;
  String resposta;
  if (!postJson(MOVEMENT_URL, payload, httpCode, resposta)) {
    return false;
  }

  Serial.print("POST ");
  Serial.print(montarSensorId(sensor));
  Serial.print(" -> HTTP ");
  Serial.println(httpCode);

  if (resposta.length()) {
    Serial.println(resposta);
  }

  return httpCode >= 200 && httpCode < 300;
}

void setupSensores() {
  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    if (sensores[i].usePulldown) {
      pinMode(sensores[i].pin, INPUT_PULLDOWN);
    } else {
      pinMode(sensores[i].pin, INPUT);
    }

    bool estadoBruto = digitalRead(sensores[i].pin);
    sensores[i].lastState = sensores[i].activeHigh ? estadoBruto : !estadoBruto;
  }
}

void setup() {
  Serial.begin(115200);

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  setLedColor(0, 0, 0);

  setupSensores();
  conectarWifi();

  Serial.println("Hub de sensores iniciado.");
}

void loop() {
  unsigned long agora = millis();

  if (WiFi.status() != WL_CONNECTED && agora - ultimoWifiRetryMs >= WIFI_RETRY_MS) {
    ultimoWifiRetryMs = agora;
    conectarWifi();
  }

  if (WiFi.status() == WL_CONNECTED && !sensoresRegistrados) {
    registrarSensoresNoBackend();
  }

  if (agora - ultimoPollMs < SENSOR_POLL_MS) {
    return;
  }
  ultimoPollMs = agora;

  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    bool leituraBruta = digitalRead(sensores[i].pin);
    bool movimentoAtivo = sensores[i].activeHigh ? leituraBruta : !leituraBruta;
    bool bordaSubida = movimentoAtivo && !sensores[i].lastState;

    if (bordaSubida) {
      Serial.print("Movimento detectado em ");
      Serial.println(sensores[i].id);
      acenderStatus(32, 0, 0, 3000);

      if (enviarEvento(sensores[i])) {
        piscarStatus(0, 0, 32, 1, 80);
      } else {
        piscarStatus(32, 0, 0, 1, 100);
      }
    }

    if (!movimentoAtivo && sensores[i].lastState) {
      Serial.print("Movimento encerrado em ");
      Serial.println(sensores[i].id);
    }

    sensores[i].lastState = movimentoAtivo;
  }
}
