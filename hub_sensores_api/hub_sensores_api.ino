#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
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
constexpr uint8_t PINOS_GPIO_AUTOMACAO_SENSOR[] = {15, 16, 17, 18};
constexpr uint8_t PINOS_GATILHO_LOCAL[] = {4, 5, 6, 7};
constexpr uint8_t PINOS_SAIDA_GATILHO_LOCAL[] = {10, 11, 12, 13};
constexpr size_t TOTAL_PINOS_GPIO_AUTOMACAO_SENSOR = sizeof(PINOS_GPIO_AUTOMACAO_SENSOR) / sizeof(PINOS_GPIO_AUTOMACAO_SENSOR[0]);
constexpr size_t TOTAL_PINOS_GATILHO_LOCAL = sizeof(PINOS_GATILHO_LOCAL) / sizeof(PINOS_GATILHO_LOCAL[0]);
constexpr size_t TOTAL_PINOS_SAIDA_GATILHO_LOCAL = sizeof(PINOS_SAIDA_GATILHO_LOCAL) / sizeof(PINOS_SAIDA_GATILHO_LOCAL[0]);
constexpr char PREFERENCES_NAMESPACE[] = "hublocal";

WebServer deviceServer(DEVICE_COMMAND_PORT);
Preferences preferences;

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

struct RegraGatilhoLocal {
  bool enabled;
  uint8_t triggerPin;
  uint8_t outputPin;
  bool outputActiveHigh;
  unsigned long holdMs;
  bool lastInputActive;
};

struct EstadoSaidaLocal {
  uint8_t pin;
  bool active;
  bool activeHigh;
  unsigned long releaseAtMs;
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
bool backendDisponivel = false;
bool registroBackendTentado = false;
bool servidorLocalAtivo = false;
bool preferencesAtivas = false;
RegraGatilhoLocal regrasGatilhoLocal[TOTAL_PINOS_GATILHO_LOCAL];
EstadoSaidaLocal estadosSaidaLocal[TOTAL_PINOS_SAIDA_GATILHO_LOCAL];

enum EstadoIndicadorLed {
  LedBoot,
  LedWifiConectando,
  LedWifiOffline,
  LedBackendRegistrando,
  LedBackendOffline,
  LedOnline
};

struct FaseLed {
  unsigned long duracaoMs;
  bool ligado;
};

constexpr FaseLed PADRAO_LED_WIFI_CONECTANDO[] = {
  {180, true},
  {180, false},
  {180, true},
  {700, false}
};

constexpr FaseLed PADRAO_LED_WIFI_OFFLINE[] = {
  {120, true},
  {120, false},
  {120, true},
  {120, false},
  {120, true},
  {700, false}
};

constexpr FaseLed PADRAO_LED_BACKEND_REGISTRANDO[] = {
  {100, true},
  {500, false}
};

constexpr FaseLed PADRAO_LED_BACKEND_OFFLINE[] = {
  {350, true},
  {350, false}
};

EstadoIndicadorLed estadoIndicadorLed = LedBoot;
unsigned long estadoIndicadorLedDesdeMs = 0;

bool pinoGpioAutomacaoSensorPermitido(int pin) {
  for (uint8_t permitido : PINOS_GPIO_AUTOMACAO_SENSOR) {
    if (pin == permitido) {
      return true;
    }
  }
  return false;
}

bool pinoGatilhoLocalPermitido(int pin) {
  for (uint8_t permitido : PINOS_GATILHO_LOCAL) {
    if (pin == permitido) {
      return true;
    }
  }
  return false;
}

bool pinoSaidaGatilhoLocalPermitido(int pin) {
  for (uint8_t permitido : PINOS_SAIDA_GATILHO_LOCAL) {
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

int indiceRegraGatilhoLocal(uint8_t triggerPin) {
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    if (regrasGatilhoLocal[i].triggerPin == triggerPin) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

EstadoSaidaLocal* buscarEstadoSaidaLocal(uint8_t pin) {
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_GATILHO_LOCAL; i++) {
    if (estadosSaidaLocal[i].pin == pin) {
      return &estadosSaidaLocal[i];
    }
  }
  return nullptr;
}

void alimentarWatchdog() {
  if (watchdogAtivo) {
    esp_task_wdt_reset();
  }
}

void delayComWatchdog(unsigned long duracaoMs) {
  // Mantem o watchdog alimentado e o servidor local responsivo durante piscadas e esperas curtas.
  unsigned long inicio = millis();
  while (millis() - inicio < duracaoMs) {
    alimentarWatchdog();
    if (servidorLocalAtivo) {
      deviceServer.handleClient();
    }
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

bool faseLedLigada(const FaseLed* fases, size_t totalFases, unsigned long tempoDecorridoMs) {
  unsigned long cicloMs = 0;
  for (size_t i = 0; i < totalFases; i++) {
    cicloMs += fases[i].duracaoMs;
  }

  if (cicloMs == 0) {
    return false;
  }

  unsigned long posicaoMs = tempoDecorridoMs % cicloMs;
  for (size_t i = 0; i < totalFases; i++) {
    if (posicaoMs < fases[i].duracaoMs) {
      return fases[i].ligado;
    }
    posicaoMs -= fases[i].duracaoMs;
  }

  return false;
}

void definirEstadoIndicadorLed(EstadoIndicadorLed novoEstado) {
  if (estadoIndicadorLed == novoEstado) {
    return;
  }

  estadoIndicadorLed = novoEstado;
  estadoIndicadorLedDesdeMs = millis();
}

void sincronizarIndicadorLed() {
  if (WiFi.status() != WL_CONNECTED) {
    definirEstadoIndicadorLed(wifiDesconectadoDesdeMs == 0 ? LedWifiConectando : LedWifiOffline);
    return;
  }

  if (!sensoresRegistrados) {
    definirEstadoIndicadorLed(registroBackendTentado ? LedBackendOffline : LedBackendRegistrando);
    return;
  }

  definirEstadoIndicadorLed(falhasHealth > 0 ? LedBackendOffline : LedOnline);
}

void atualizarIndicadorLed() {
  bool ligado = false;
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  unsigned long tempoDecorridoMs = millis() - estadoIndicadorLedDesdeMs;

  switch (estadoIndicadorLed) {
    case LedBoot:
      ligado = true;
      red = 20;
      green = 20;
      blue = 20;
      break;
    case LedWifiConectando:
      ligado = faseLedLigada(PADRAO_LED_WIFI_CONECTANDO, sizeof(PADRAO_LED_WIFI_CONECTANDO) / sizeof(PADRAO_LED_WIFI_CONECTANDO[0]), tempoDecorridoMs);
      red = 32;
      green = 16;
      blue = 0;
      break;
    case LedWifiOffline:
      ligado = faseLedLigada(PADRAO_LED_WIFI_OFFLINE, sizeof(PADRAO_LED_WIFI_OFFLINE) / sizeof(PADRAO_LED_WIFI_OFFLINE[0]), tempoDecorridoMs);
      red = 32;
      green = 0;
      blue = 0;
      break;
    case LedBackendRegistrando:
      ligado = faseLedLigada(PADRAO_LED_BACKEND_REGISTRANDO, sizeof(PADRAO_LED_BACKEND_REGISTRANDO) / sizeof(PADRAO_LED_BACKEND_REGISTRANDO[0]), tempoDecorridoMs);
      red = 0;
      green = 0;
      blue = 32;
      break;
    case LedBackendOffline:
      ligado = faseLedLigada(PADRAO_LED_BACKEND_OFFLINE, sizeof(PADRAO_LED_BACKEND_OFFLINE) / sizeof(PADRAO_LED_BACKEND_OFFLINE[0]), tempoDecorridoMs);
      red = 32;
      green = 0;
      blue = 16;
      break;
    case LedOnline:
      ligado = true;
      red = 0;
      green = 32;
      blue = 0;
      break;
  }

  if (!ligado) {
    red = 0;
    green = 0;
    blue = 0;
  }

  setLedColor(red, green, blue);
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

bool extrairCampoJsonBool(const String& payload, const char* chave, bool valorPadrao) {
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

  if (payload.startsWith("true", inicioValor)) return true;
  if (payload.startsWith("false", inicioValor)) return false;
  if (payload[inicioValor] == '1') return true;
  if (payload[inicioValor] == '0') return false;
  return valorPadrao;
}

bool regraGatilhoLocalValida(const RegraGatilhoLocal& regra, int indiceIgnorado = -1) {
  if (!pinoGatilhoLocalPermitido(regra.triggerPin) || !pinoSaidaGatilhoLocalPermitido(regra.outputPin)) {
    return false;
  }
  if (regra.outputPin == regra.triggerPin || pinoEhSensorConfigurado(regra.outputPin)) {
    return false;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    if (static_cast<int>(i) == indiceIgnorado) continue;
    if (!regrasGatilhoLocal[i].enabled) continue;
    if (regrasGatilhoLocal[i].triggerPin == regra.outputPin || regrasGatilhoLocal[i].outputPin == regra.outputPin) {
      return false;
    }
  }

  return true;
}

void inicializarRegrasGatilhoLocal() {
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    regrasGatilhoLocal[i].enabled = false;
    regrasGatilhoLocal[i].triggerPin = PINOS_GATILHO_LOCAL[i];
    regrasGatilhoLocal[i].outputPin = PINOS_SAIDA_GATILHO_LOCAL[0];
    regrasGatilhoLocal[i].outputActiveHigh = true;
    regrasGatilhoLocal[i].holdMs = 1000;
    regrasGatilhoLocal[i].lastInputActive = false;
  }

  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_GATILHO_LOCAL; i++) {
    estadosSaidaLocal[i].pin = PINOS_SAIDA_GATILHO_LOCAL[i];
    estadosSaidaLocal[i].active = false;
    estadosSaidaLocal[i].activeHigh = true;
    estadosSaidaLocal[i].releaseAtMs = 0;
  }
}

void salvarRegrasGatilhoLocal() {
  if (!preferencesAtivas) return;

  preferences.putUChar("rule_count", static_cast<uint8_t>(TOTAL_PINOS_GATILHO_LOCAL));
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    char key[24];
    snprintf(key, sizeof(key), "r%u_en", static_cast<unsigned>(i));
    preferences.putBool(key, regrasGatilhoLocal[i].enabled);
    snprintf(key, sizeof(key), "r%u_tp", static_cast<unsigned>(i));
    preferences.putUChar(key, regrasGatilhoLocal[i].triggerPin);
    snprintf(key, sizeof(key), "r%u_op", static_cast<unsigned>(i));
    preferences.putUChar(key, regrasGatilhoLocal[i].outputPin);
    snprintf(key, sizeof(key), "r%u_ah", static_cast<unsigned>(i));
    preferences.putBool(key, regrasGatilhoLocal[i].outputActiveHigh);
    snprintf(key, sizeof(key), "r%u_ms", static_cast<unsigned>(i));
    preferences.putUInt(key, static_cast<uint32_t>(regrasGatilhoLocal[i].holdMs));
  }
}

void carregarRegrasGatilhoLocal() {
  inicializarRegrasGatilhoLocal();
  if (!preferencesAtivas) return;

  uint8_t quantidade = preferences.getUChar("rule_count", static_cast<uint8_t>(TOTAL_PINOS_GATILHO_LOCAL));
  if (quantidade != TOTAL_PINOS_GATILHO_LOCAL) {
    salvarRegrasGatilhoLocal();
    return;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    char key[24];
    RegraGatilhoLocal regra = regrasGatilhoLocal[i];
    snprintf(key, sizeof(key), "r%u_en", static_cast<unsigned>(i));
    regra.enabled = preferences.getBool(key, false);
    snprintf(key, sizeof(key), "r%u_tp", static_cast<unsigned>(i));
    regra.triggerPin = preferences.getUChar(key, PINOS_GATILHO_LOCAL[i]);
    snprintf(key, sizeof(key), "r%u_op", static_cast<unsigned>(i));
    regra.outputPin = preferences.getUChar(key, PINOS_SAIDA_GATILHO_LOCAL[0]);
    snprintf(key, sizeof(key), "r%u_ah", static_cast<unsigned>(i));
    regra.outputActiveHigh = preferences.getBool(key, true);
    snprintf(key, sizeof(key), "r%u_ms", static_cast<unsigned>(i));
    regra.holdMs = preferences.getUInt(key, 1000);
    regra.lastInputActive = false;

    if (regra.enabled && !regraGatilhoLocalValida(regra, static_cast<int>(i))) {
      regra.enabled = false;
    }
    regrasGatilhoLocal[i] = regra;
  }
}

void aplicarConfiguracaoRegrasGatilhoLocal() {
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_GATILHO_LOCAL; i++) {
    pinMode(estadosSaidaLocal[i].pin, INPUT);
    estadosSaidaLocal[i].active = false;
    estadosSaidaLocal[i].releaseAtMs = 0;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    RegraGatilhoLocal& regra = regrasGatilhoLocal[i];
    pinMode(regra.triggerPin, INPUT_PULLUP);
    regra.lastInputActive = digitalRead(regra.triggerPin) == LOW;

    if (!regra.enabled) {
      continue;
    }

    configurarGpioLocalInativo(regra.outputPin, regra.outputActiveHigh);
  }
}

void agendarSaidaLocal(uint8_t pin, bool ativoEmHigh, unsigned long duracaoMs) {
  EstadoSaidaLocal* estado = buscarEstadoSaidaLocal(pin);
  if (estado == nullptr) return;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, ativoEmHigh ? HIGH : LOW);
  estado->active = true;
  estado->activeHigh = ativoEmHigh;
  estado->releaseAtMs = millis() + duracaoMs;
}

void atualizarSaidasLocais() {
  unsigned long agora = millis();
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_GATILHO_LOCAL; i++) {
    EstadoSaidaLocal& estado = estadosSaidaLocal[i];
    if (!estado.active) continue;

    if (static_cast<long>(agora - estado.releaseAtMs) >= 0) {
      configurarGpioLocalInativo(estado.pin, estado.activeHigh);
      estado.active = false;
      estado.releaseAtMs = 0;
    }
  }
}

void avaliarGatilhosLocais() {
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    RegraGatilhoLocal& regra = regrasGatilhoLocal[i];
    if (!regra.enabled) continue;

    bool ativo = digitalRead(regra.triggerPin) == LOW;
    if (ativo && !regra.lastInputActive) {
      Serial.print("Gatilho local em GPIO ");
      Serial.print(regra.triggerPin);
      Serial.print(" -> saida GPIO ");
      Serial.print(regra.outputPin);
      Serial.print(" por ");
      Serial.print(regra.holdMs);
      Serial.println(" ms");
      agendarSaidaLocal(regra.outputPin, regra.outputActiveHigh, regra.holdMs);
    }
    regra.lastInputActive = ativo;
  }
}

String montarJsonRegrasGatilhoLocal() {
  String json = "{\"ok\":true,\"rules\":[";
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    if (i > 0) json += ",";
    const RegraGatilhoLocal& regra = regrasGatilhoLocal[i];
    json += "{";
    json += "\"trigger_pin\":" + String(regra.triggerPin) + ",";
    json += "\"enabled\":" + String(regra.enabled ? "true" : "false") + ",";
    json += "\"output_pin\":" + String(regra.outputPin) + ",";
    json += "\"output_level\":\"" + String(regra.outputActiveHigh ? "HIGH" : "LOW") + "\",";
    json += "\"hold_ms\":" + String(regra.holdMs);
    json += "}";
  }
  json += "]}";
  return json;
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

  if (!pinoGpioAutomacaoSensorPermitido(pin)) {
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

void processarRegraGatilhoLocal() {
  if (deviceServer.method() != HTTP_POST) {
    responderJsonLocal(405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    return;
  }

  String payload = deviceServer.arg("plain");
  int triggerPin = extrairCampoJsonInt(payload, "trigger_pin", -1);
  bool enabled = extrairCampoJsonBool(payload, "enabled", false);
  int outputPin = extrairCampoJsonInt(payload, "output_pin", -1);
  int holdMs = extrairCampoJsonInt(payload, "hold_ms", 1000);
  String outputLevel = extrairCampoJsonString(payload, "output_level");
  outputLevel.toUpperCase();

  if (!pinoGatilhoLocalPermitido(triggerPin)) {
    responderJsonLocal(400, "{\"ok\":false,\"error\":\"invalid_trigger_pin\"}");
    return;
  }
  if (!pinoSaidaGatilhoLocalPermitido(outputPin)) {
    responderJsonLocal(400, "{\"ok\":false,\"error\":\"invalid_output_pin\"}");
    return;
  }
  if (outputLevel != "HIGH" && outputLevel != "LOW") {
    responderJsonLocal(400, "{\"ok\":false,\"error\":\"invalid_output_level\"}");
    return;
  }
  if (holdMs < 50) holdMs = 50;
  if (holdMs > 600000) holdMs = 600000;

  int indice = indiceRegraGatilhoLocal(static_cast<uint8_t>(triggerPin));
  if (indice < 0) {
    responderJsonLocal(404, "{\"ok\":false,\"error\":\"trigger_rule_not_found\"}");
    return;
  }

  RegraGatilhoLocal candidata = regrasGatilhoLocal[indice];
  candidata.enabled = enabled;
  candidata.outputPin = static_cast<uint8_t>(outputPin);
  candidata.outputActiveHigh = outputLevel == "HIGH";
  candidata.holdMs = static_cast<unsigned long>(holdMs);
  candidata.lastInputActive = false;

  if (enabled && !regraGatilhoLocalValida(candidata, indice)) {
    responderJsonLocal(409, "{\"ok\":false,\"error\":\"rule_pin_conflict\"}");
    return;
  }

  regrasGatilhoLocal[indice] = candidata;
  salvarRegrasGatilhoLocal();
  aplicarConfiguracaoRegrasGatilhoLocal();
  responderJsonLocal(200, "{\"ok\":true}");
}

void processarResetRegrasGatilhoLocal() {
  if (deviceServer.method() != HTTP_POST) {
    responderJsonLocal(405, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
    return;
  }

  inicializarRegrasGatilhoLocal();
  salvarRegrasGatilhoLocal();
  aplicarConfiguracaoRegrasGatilhoLocal();
  responderJsonLocal(200, "{\"ok\":true}");
}

void setupServidorLocal() {
  if (servidorLocalAtivo) {
    return;
  }

  deviceServer.on("/gpio/pulse", HTTP_POST, processarPulsoGpioLocal);
  deviceServer.on("/local-trigger-rule", HTTP_POST, processarRegraGatilhoLocal);
  deviceServer.on("/local-trigger-rules/reset", HTTP_POST, processarResetRegrasGatilhoLocal);
  deviceServer.on("/local-trigger-rules", HTTP_GET, []() {
    responderJsonLocal(200, montarJsonRegrasGatilhoLocal());
  });
  deviceServer.on("/health", HTTP_GET, []() {
    responderJsonLocal(200, "{\"ok\":true,\"service\":\"esp32-local-gpio\"}");
  });
  deviceServer.begin();
  servidorLocalAtivo = true;
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
    backendDisponivel = false;
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

  backendDisponivel = httpCode >= 200 && httpCode < 300;
  return backendDisponivel;
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
  registroBackendTentado = true;
  if (!postJson(montarUrl(SENSOR_REGISTER_ROUTE).c_str(), payload, httpCode, resposta)) {
    backendDisponivel = false;
    sensoresRegistrados = false;
    return false;
  }

  Serial.print("POST register -> HTTP ");
  Serial.println(httpCode);
  if (resposta.length()) {
    Serial.println(resposta);
  }

  sensoresRegistrados = httpCode >= 200 && httpCode < 300;
  backendDisponivel = sensoresRegistrados;
  return sensoresRegistrados;
}

void conectarWifi() {
  Serial.print("Conectando no WiFi ");
  Serial.println(WIFI_SSID);

  definirEstadoIndicadorLed(LedWifiConectando);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < WIFI_RETRY_MS) {
    atualizarIndicadorLed();
    delayComWatchdog(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
    setupServidorLocal();
    sensoresRegistrados = false;
    backendDisponivel = false;
    registroBackendTentado = false;
    registrarSensoresNoBackend();
  } else {
    Serial.println("Falha ao conectar no WiFi.");
  }

  sincronizarIndicadorLed();
  atualizarIndicadorLed();
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
    if (!sensores[i].enabled) {
      sensores[i].lastState = false;
      continue;
    }

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
  preferencesAtivas = preferences.begin(PREFERENCES_NAMESPACE, false);
  inicializarRegrasGatilhoLocal();
  carregarRegrasGatilhoLocal();

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  setLedColor(0, 0, 0);

  setupSensores();
  aplicarConfiguracaoRegrasGatilhoLocal();
  conectarWifi();
  sincronizarIndicadorLed();
  atualizarIndicadorLed();

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
    if (!servidorLocalAtivo) {
      setupServidorLocal();
    }
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

  sincronizarIndicadorLed();
  atualizarIndicadorLed();
  atualizarSaidasLocais();
  avaliarGatilhosLocais();

  if (agora - ultimoPollMs < SENSOR_POLL_MS) {
    return;
  }
  ultimoPollMs = agora;

  for (size_t i = 0; i < TOTAL_SENSORES; i++) {
    if (!sensores[i].enabled) {
      continue;
    }

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
