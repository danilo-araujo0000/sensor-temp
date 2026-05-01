#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include "esp_task_wdt.h"
#include "config.local.h"

constexpr char SENSOR_REGISTER_ROUTE[] = "/sensors/register";
constexpr char MOVEMENT_ROUTE[] = "/movements";
constexpr char HEARTBEAT_ROUTE[] = "/devices/heartbeat";
constexpr char HEALTH_ROUTE[] = "/health";

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long SENSOR_POLL_MS = 25;
// Se a rede ou a API ficarem indisponiveis por muito tempo, o ESP reinicia para sair de estados ruins.
constexpr unsigned long WIFI_MAX_OFFLINE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long HEALTH_CHECK_MS = 60UL * 1000UL;
constexpr unsigned long DEVICE_HEARTBEAT_MS = 3UL * 1000UL;
constexpr unsigned long LOCAL_GPIO_GAP_MS = 250UL;
constexpr uint8_t HEALTH_MAX_FALHAS = 5;
constexpr uint32_t WATCHDOG_TIMEOUT_SECONDS = 20;
constexpr uint8_t PINO_SEM_SECUNDARIO = 255;
constexpr uint16_t DEVICE_COMMAND_PORT = 8088;
constexpr uint8_t PINOS_GPIO_LOCAL[] = {4, 5, 6, 7};

WebServer deviceServer(DEVICE_COMMAND_PORT);

enum TipoLigacao {
  // Sensor fecha contato entre GPIO e 3V3; usa INPUT_PULLDOWN e ativo = HIGH.
  LigacaoGpio3V3,
  // Sensor fecha contato entre GPIO e GND; usa INPUT_PULLUP e ativo = LOW.
  LigacaoGpioGnd
};

struct SensorConfig {
  const char* id;
  uint8_t pinPrincipal;
  uint8_t pinSecundario;
  TipoLigacao ligacao;
  bool enabled;
  bool showOnDashboard;
  bool lastState;
};

#include "sensores_config.h"

constexpr size_t TOTAL_SENSORES = sizeof(sensores) / sizeof(sensores[0]);

unsigned long ultimoWifiRetryMs = 0;
unsigned long ultimoPollMs = 0;
unsigned long wifiDesconectadoDesdeMs = 0;
unsigned long ultimoHealthCheckMs = 0;
unsigned long ultimoHeartbeatMs = 0;
uint8_t falhasHealth = 0;
bool sensoresRegistrados = false;
bool watchdogAtivo = false;

bool pinoGpioLocalPermitido(int pin) {
  for (uint8_t permitido : PINOS_GPIO_LOCAL) {
    if (pin == permitido) {
      return true;
    }
  }
  return false;
}

bool pinoEhSensorConfigurado(uint8_t pin) {
  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    if (sensores[i].pinPrincipal == pin) {
      return true;
    }
    if (sensores[i].pinSecundario != PINO_SEM_SECUNDARIO && sensores[i].pinSecundario == pin) {
      return true;
    }
  }
  return false;
}

void alimentarWatchdog() {
  if (watchdogAtivo) {
    esp_task_wdt_reset();
  }
}

void delayComWatchdog(unsigned long duracaoMs) {
  // Mantem o watchdog alimentado durante piscadas e esperas curtas.
  unsigned long inicio = millis();
  while (millis() - inicio < duracaoMs) {
    alimentarWatchdog();
    delay(20);
  }
}

void reiniciarDispositivo(const char* motivo) {
  Serial.print("Reiniciando: ");
  Serial.println(motivo);
  Serial.flush();
  delay(100);
  ESP.restart();
}

void setupWatchdog() {
  // Reinicia automaticamente se a task principal travar e parar de alimentar o watchdog.
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WATCHDOG_TIMEOUT_SECONDS * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };

  esp_err_t err = esp_task_wdt_init(&wdtConfig);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_add(NULL);
  }

  watchdogAtivo = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
  Serial.print("Watchdog: ");
  Serial.println(watchdogAtivo ? "ativo" : "falha ao ativar");
}

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
    delayComWatchdog(intervaloMs);
    setLedColor(0, 0, 0);
    delayComWatchdog(intervaloMs);
  }
}

void acenderStatus(uint8_t red, uint8_t green, uint8_t blue, unsigned long duracaoMs) {
  setLedColor(red, green, blue);
  delayComWatchdog(duracaoMs);
  setLedColor(0, 0, 0);
}

void piscarStatusPorDuracao(uint8_t red, uint8_t green, uint8_t blue, unsigned long duracaoMs, unsigned long intervaloMs) {
  unsigned long inicio = millis();
  while (millis() - inicio < duracaoMs) {
    setLedColor(red, green, blue);
    delayComWatchdog(intervaloMs);
    setLedColor(0, 0, 0);
    delayComWatchdog(intervaloMs);
  }
}

String montarSensorId(const SensorConfig& sensor) {
  return String(DEVICE_ID) + ":" + String(sensor.id);
}

String extrairCampoJsonString(const String& payload, const char* chave) {
  String marcador = "\"" + String(chave) + "\"";
  int inicioChave = payload.indexOf(marcador);
  if (inicioChave < 0) {
    return "";
  }

  int inicioValor = payload.indexOf(':', inicioChave);
  if (inicioValor < 0) {
    return "";
  }
  inicioValor = payload.indexOf('"', inicioValor);
  if (inicioValor < 0) {
    return "";
  }
  int fimValor = payload.indexOf('"', inicioValor + 1);
  if (fimValor < 0) {
    return "";
  }
  return payload.substring(inicioValor + 1, fimValor);
}

int extrairCampoJsonInt(const String& payload, const char* chave, int valorPadrao) {
  String marcador = "\"" + String(chave) + "\"";
  int inicioChave = payload.indexOf(marcador);
  if (inicioChave < 0) {
    return valorPadrao;
  }

  int inicioValor = payload.indexOf(':', inicioChave);
  if (inicioValor < 0) {
    return valorPadrao;
  }

  inicioValor++;
  while (inicioValor < payload.length() && (payload[inicioValor] == ' ' || payload[inicioValor] == '\t')) {
    inicioValor++;
  }

  int fimValor = inicioValor;
  while (fimValor < payload.length() && isDigit(payload[fimValor])) {
    fimValor++;
  }

  if (fimValor == inicioValor) {
    return valorPadrao;
  }
  return payload.substring(inicioValor, fimValor).toInt();
}

void responderJsonLocal(int httpCode, const String& corpo) {
  deviceServer.send(httpCode, "application/json", corpo);
}

void configurarGpioLocalInativo(uint8_t pin, bool ativoEmHigh) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, ativoEmHigh ? LOW : HIGH);
}

void acionarGpioLocal(uint8_t pin, bool ativoEmHigh, unsigned long duracaoMs, uint8_t repeticoes) {
  if (pinoEhSensorConfigurado(pin)) {
    Serial.print("GPIO local recusado no pino ");
    Serial.print(pin);
    Serial.println(" porque ele esta configurado como sensor.");
    return;
  }

  uint8_t nivelAtivo = ativoEmHigh ? HIGH : LOW;
  uint8_t nivelInativo = ativoEmHigh ? LOW : HIGH;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, nivelInativo);

  for (uint8_t i = 0; i < repeticoes; i++) {
    digitalWrite(pin, nivelAtivo);
    delayComWatchdog(duracaoMs);
    digitalWrite(pin, nivelInativo);

    if (i + 1 < repeticoes) {
      delayComWatchdog(LOCAL_GPIO_GAP_MS);
    }
  }
}

void processarPulsoGpioLocal() {
  if (deviceServer.method() != HTTP_POST) {
    responderJsonLocal(405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    return;
  }

  String payload = deviceServer.arg("plain");
  int pin = extrairCampoJsonInt(payload, "pin", -1);
  int durationMs = extrairCampoJsonInt(payload, "duration_ms", 1000);
  int repeatCount = extrairCampoJsonInt(payload, "repeat_count", 1);
  String activeLevel = extrairCampoJsonString(payload, "active_level");
  activeLevel.toUpperCase();

  if (!pinoGpioLocalPermitido(pin)) {
    responderJsonLocal(400, "{\"ok\":false,\"error\":\"invalid_pin\"}");
    return;
  }

  if (pinoEhSensorConfigurado(static_cast<uint8_t>(pin))) {
    responderJsonLocal(409, "{\"ok\":false,\"error\":\"pin_in_use_by_sensor\"}");
    return;
  }

  if (activeLevel != "HIGH" && activeLevel != "LOW") {
    responderJsonLocal(400, "{\"ok\":false,\"error\":\"invalid_active_level\"}");
    return;
  }

  if (durationMs < 50) durationMs = 50;
  if (durationMs > 600000) durationMs = 600000;
  if (repeatCount < 1) repeatCount = 1;
  if (repeatCount > 20) repeatCount = 20;

  bool ativoEmHigh = activeLevel == "HIGH";
  Serial.print("GPIO local acionado no pino ");
  Serial.print(pin);
  Serial.print(" nivel=");
  Serial.print(activeLevel);
  Serial.print(" duracaoMs=");
  Serial.print(durationMs);
  Serial.print(" repeticoes=");
  Serial.println(repeatCount);

  acionarGpioLocal(static_cast<uint8_t>(pin), ativoEmHigh, static_cast<unsigned long>(durationMs), static_cast<uint8_t>(repeatCount));
  responderJsonLocal(200, "{\"ok\":true}");
}

void setupServidorLocal() {
  deviceServer.on("/gpio/pulse", HTTP_POST, processarPulsoGpioLocal);
  deviceServer.on("/health", HTTP_GET, []() {
    responderJsonLocal(200, "{\"ok\":true,\"service\":\"esp32-local-gpio\"}");
  });
  deviceServer.begin();
  Serial.print("Servidor local GPIO em porta ");
  Serial.println(DEVICE_COMMAND_PORT);
}

bool pinoEstaAtivo(uint8_t pin, TipoLigacao ligacao) {
  bool leitura = digitalRead(pin);
  return ligacao == LigacaoGpio3V3 ? leitura == HIGH : leitura == LOW;
}

bool sensorEstaAtivo(const SensorConfig& sensor) {
  bool principalAtivo = pinoEstaAtivo(sensor.pinPrincipal, sensor.ligacao);
  bool secundarioAtivo = false;

  if (sensor.pinSecundario != PINO_SEM_SECUNDARIO) {
    secundarioAtivo = pinoEstaAtivo(sensor.pinSecundario, sensor.ligacao);
  }

  // Redundancia: qualquer pino ativo indica movimento, sem duplicar evento.
  return principalAtivo || secundarioAtivo;
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

  http.setTimeout(3000);
  http.addHeader("Content-Type", "application/json");
  httpCode = http.POST(payload);
  resposta = http.getString();
  http.end();
  return true;
}

String montarUrl(const char* rota) {
  return String(url_default) + rota;
}

bool verificarHealthBackend() {
  // Confirma que a API local continua respondendo; falhas repetidas reiniciam o ESP.
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  if (!http.begin(montarUrl(HEALTH_ROUTE))) {
    return false;
  }

  http.setTimeout(3000);
  int httpCode = http.GET();
  String resposta = http.getString();
  http.end();

  Serial.print("GET health -> HTTP ");
  Serial.println(httpCode);
  if (resposta.length()) {
    Serial.println(resposta);
  }

  return httpCode >= 200 && httpCode < 300;
}

bool registrarSensoresNoBackend() {
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"sensors\":[";

  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    if (i > 0) payload += ",";
    payload += "{";
    payload += "\"sensor_id\":\"" + montarSensorId(sensores[i]) + "\",";
    payload += "\"pin\":" + String(sensores[i].pinPrincipal) + ",";
    payload += "\"enabled\":" + String(sensores[i].enabled ? "true" : "false") + ",";
    payload += "\"show_on_dashboard\":" + String(sensores[i].showOnDashboard ? "true" : "false");
    payload += "}";
  }

  payload += "]}";

  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl(SENSOR_REGISTER_ROUTE).c_str(), payload, httpCode, resposta)) {
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
    delayComWatchdog(400);
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
  if (!postJson(montarUrl(MOVEMENT_ROUTE).c_str(), payload, httpCode, resposta)) {
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

bool enviarHeartbeatDispositivo() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String payload = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"command_port\":" + String(DEVICE_COMMAND_PORT) + "}";
  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl(HEARTBEAT_ROUTE).c_str(), payload, httpCode, resposta)) {
    return false;
  }

  return httpCode >= 200 && httpCode < 300;
}

void configurarPinoSensor(uint8_t pin, TipoLigacao ligacao) {
  if (ligacao == LigacaoGpio3V3) {
    pinMode(pin, INPUT_PULLDOWN);
  } else {
    pinMode(pin, INPUT_PULLUP);
  }
}

void setupSensores() {
  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    configurarPinoSensor(sensores[i].pinPrincipal, sensores[i].ligacao);

    if (sensores[i].pinSecundario != PINO_SEM_SECUNDARIO) {
      configurarPinoSensor(sensores[i].pinSecundario, sensores[i].ligacao);
    }

    // Evita evento falso no boot: o primeiro estado vira a referencia inicial.
    sensores[i].lastState = sensorEstaAtivo(sensores[i]);
  }
}

void setup() {
  Serial.begin(115200);
  setupWatchdog();

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  setLedColor(0, 0, 0);

  setupSensores();
  setupServidorLocal();
  conectarWifi();

  Serial.println("Hub de sensores iniciado.");
}

void loop() {
  alimentarWatchdog();
  deviceServer.handleClient();
  unsigned long agora = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDesconectadoDesdeMs == 0) {
      wifiDesconectadoDesdeMs = agora;
    } else if (agora - wifiDesconectadoDesdeMs >= WIFI_MAX_OFFLINE_MS) {
      reiniciarDispositivo("WiFi offline por tempo limite");
    }
  } else {
    wifiDesconectadoDesdeMs = 0;
  }

  if (WiFi.status() != WL_CONNECTED && agora - ultimoWifiRetryMs >= WIFI_RETRY_MS) {
    ultimoWifiRetryMs = agora;
    conectarWifi();
  }

  if (WiFi.status() == WL_CONNECTED && !sensoresRegistrados) {
    registrarSensoresNoBackend();
  }

  if (WiFi.status() == WL_CONNECTED && agora - ultimoHeartbeatMs >= DEVICE_HEARTBEAT_MS) {
    ultimoHeartbeatMs = agora;
    enviarHeartbeatDispositivo();
  }

  if (WiFi.status() == WL_CONNECTED && agora - ultimoHealthCheckMs >= HEALTH_CHECK_MS) {
    ultimoHealthCheckMs = agora;
    if (verificarHealthBackend()) {
      falhasHealth = 0;
    } else if (++falhasHealth >= HEALTH_MAX_FALHAS) {
      reiniciarDispositivo("falhas consecutivas no health do backend");
    }
  }

  if (agora - ultimoPollMs < SENSOR_POLL_MS) {
    return;
  }
  ultimoPollMs = agora;

  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    bool movimentoAtivo = sensorEstaAtivo(sensores[i]);
    // Evento so nasce na transicao sem movimento -> com movimento.
    bool bordaSubida = movimentoAtivo && !sensores[i].lastState;

    if (bordaSubida) {
      Serial.print("Movimento detectado em ");
      Serial.println(sensores[i].id);
      bool eventoEnviado = enviarEvento(sensores[i]);
      piscarStatusPorDuracao(0, 64, 0, 400, 100);

      if (eventoEnviado) {
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
