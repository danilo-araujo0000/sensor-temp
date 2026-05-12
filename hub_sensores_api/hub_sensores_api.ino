#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "config.local.h"

#ifndef DEVICE_API_TOKEN
#define DEVICE_API_TOKEN ""
#endif

#ifndef BACKEND_TLS_INSECURE
#define BACKEND_TLS_INSECURE 1
#endif

#ifndef BACKEND_CA_CERT
#define BACKEND_CA_CERT ""
#endif

#ifndef DEVICE_POLL_INTERVAL_MS
#define DEVICE_POLL_INTERVAL_MS 1500
#endif

#ifndef DEVICE_POLL_WAIT_SECONDS
#define DEVICE_POLL_WAIT_SECONDS 0
#endif

#ifndef DEVICE_HTTP_TIMEOUT_MS
#define DEVICE_HTTP_TIMEOUT_MS 3000
#endif

constexpr char SENSOR_REGISTER_ROUTE[] = "/sensors/register";
constexpr char MOVEMENT_ROUTE[] = "/movements";
constexpr char HEARTBEAT_ROUTE[] = "/devices/heartbeat";
constexpr char HEALTH_ROUTE[] = "/health";
constexpr char DEVICE_POLL_ROUTE[] = "/api/device/poll";
constexpr char DEVICE_CONFIG_ROUTE[] = "/api/device/config";
constexpr char DEVICE_CONFIG_ACK_ROUTE[] = "/api/device/config/ack";
constexpr char DEVICE_COMMAND_ACK_ROUTE[] = "/api/device/command/ack";

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long SENSOR_POLL_MS = 25;
// Se a rede ou a API ficarem indisponiveis por muito tempo, o ESP reinicia para sair de estados ruins.
constexpr unsigned long WIFI_MAX_OFFLINE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long HEALTH_CHECK_MS = 60UL * 1000UL;
constexpr unsigned long DEVICE_HEARTBEAT_MS = 3UL * 1000UL;
constexpr unsigned long DEVICE_SYNC_FAIL_RETRY_MS = 5000UL;
constexpr unsigned long LOCAL_GPIO_GAP_MS = 250UL;
constexpr unsigned long LOCAL_TRIGGER_DEBOUNCE_MS = 60UL;
constexpr unsigned long LOCAL_TRIGGER_REARM_MS = 250UL;
constexpr uint8_t HEALTH_MAX_FALHAS = 5;
constexpr uint32_t WATCHDOG_TIMEOUT_SECONDS = 20;
constexpr uint8_t PINO_SEM_SECUNDARIO = 255;
constexpr uint16_t DEVICE_COMMAND_PORT = 8088;
constexpr uint8_t PINOS_GPIO_AUTOMACAO_SENSOR[] = {15, 16, 17, 18};
constexpr uint8_t PINOS_GATILHO_LOCAL[] = {4, 5, 6, 7};
constexpr uint8_t PINOS_SAIDA_GATILHO_LOCAL[] = {10, 11, 12, 13};
constexpr uint8_t PINOS_SAIDA_CONTROLADAS[] = {10, 11, 12, 13, 15, 16, 17, 18};
constexpr size_t TOTAL_PINOS_GPIO_AUTOMACAO_SENSOR = sizeof(PINOS_GPIO_AUTOMACAO_SENSOR) / sizeof(PINOS_GPIO_AUTOMACAO_SENSOR[0]);
constexpr size_t TOTAL_PINOS_GATILHO_LOCAL = sizeof(PINOS_GATILHO_LOCAL) / sizeof(PINOS_GATILHO_LOCAL[0]);
constexpr size_t TOTAL_PINOS_SAIDA_GATILHO_LOCAL = sizeof(PINOS_SAIDA_GATILHO_LOCAL) / sizeof(PINOS_SAIDA_GATILHO_LOCAL[0]);
constexpr size_t TOTAL_PINOS_SAIDA_CONTROLADAS = sizeof(PINOS_SAIDA_CONTROLADAS) / sizeof(PINOS_SAIDA_CONTROLADAS[0]);
constexpr char PREFERENCES_NAMESPACE[] = "hublocal";
constexpr char PREFERENCES_CONFIG_VERSION_KEY[] = "cfg_ver";
constexpr size_t DEVICE_LOG_QUEUE_SIZE = 24;
constexpr size_t DEVICE_LOG_BATCH_SIZE = 8;
constexpr size_t DEVICE_LOG_MESSAGE_MAX_LEN = 160;
constexpr unsigned long DEVICE_LOG_FLUSH_MS = 10000UL;
constexpr unsigned long LED_FLASH_HEARTBEAT_MS = 70UL;
constexpr unsigned long LED_FLASH_CONFIG_MS = 220UL;
constexpr unsigned long LED_FLASH_ERROR_MS = 260UL;

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
  uint8_t repeatCount;
  unsigned long repeatGapMs;
  bool rawInputActive;
  bool lastInputActive;
  bool armed;
  unsigned long rawChangedAtMs;
  unsigned long releaseSinceMs;
};

struct EstadoSaidaLocal {
  uint8_t pin;
  bool sequenceActive;
  bool activeHigh;
  bool pulseActive;
  uint8_t pendingPulses;
  unsigned long nextTransitionAtMs;
  unsigned long holdMs;
  unsigned long gapMs;
};

struct DeviceLogEntry {
  char level[8];
  char message[DEVICE_LOG_MESSAGE_MAX_LEN + 1];
};

#include "sensores_config.h"

constexpr size_t TOTAL_SENSORES = sizeof(sensores) / sizeof(sensores[0]);

unsigned long ultimoWifiRetryMs = 0;
unsigned long ultimoPollMs = 0;
unsigned long ultimoDeviceSyncMs = 0;
unsigned long ultimoLogFlushMs = 0;
unsigned long wifiDesconectadoDesdeMs = 0;
unsigned long ultimoHealthCheckMs = 0;
unsigned long ultimoHeartbeatMs = 0;
unsigned long intervaloPollingBackendMs = DEVICE_POLL_INTERVAL_MS;
unsigned long ledFlashAteMs = 0;
uint8_t falhasHealth = 0;
uint32_t versaoConfiguracaoAplicada = 0;
size_t totalLogsPendentes = 0;
uint8_t ledFlashRed = 0;
uint8_t ledFlashGreen = 0;
uint8_t ledFlashBlue = 0;
bool sensoresRegistrados = false;
bool watchdogAtivo = false;
bool backendDisponivel = false;
bool registroBackendTentado = false;
bool servidorLocalAtivo = false;
bool preferencesAtivas = false;
bool ultimoHeartbeatSucesso = true;
bool ultimoRegistroBackendSucesso = false;
RegraGatilhoLocal regrasGatilhoLocal[TOTAL_PINOS_GATILHO_LOCAL];
EstadoSaidaLocal estadosSaidaLocal[TOTAL_PINOS_SAIDA_CONTROLADAS];
DeviceLogEntry filaLogsDispositivo[DEVICE_LOG_QUEUE_SIZE];

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

int indicePinoGatilhoLocalPermitido(int triggerPin) {
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    if (PINOS_GATILHO_LOCAL[i] == triggerPin) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

EstadoSaidaLocal* buscarEstadoSaidaLocal(uint8_t pin) {
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_CONTROLADAS; i++) {
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
  anexarLogDispositivo("error", "restart " + String(motivo));
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

void dispararFlashLed(uint8_t red, uint8_t green, uint8_t blue, unsigned long duracaoMs) {
  ledFlashRed = red;
  ledFlashGreen = green;
  ledFlashBlue = blue;
  ledFlashAteMs = millis() + duracaoMs;
}

void anexarLogDispositivo(const char* level, const String& message) {
  String saneMessage = message;
  saneMessage.replace('\n', ' ');
  saneMessage.replace('\r', ' ');
  saneMessage.trim();
  if (saneMessage.length() == 0) {
    return;
  }

  if (totalLogsPendentes >= DEVICE_LOG_QUEUE_SIZE) {
    for (size_t i = 1; i < DEVICE_LOG_QUEUE_SIZE; i++) {
      filaLogsDispositivo[i - 1] = filaLogsDispositivo[i];
    }
    totalLogsPendentes = DEVICE_LOG_QUEUE_SIZE - 1;
  }

  DeviceLogEntry& entry = filaLogsDispositivo[totalLogsPendentes++];
  snprintf(entry.level, sizeof(entry.level), "%s", level && strlen(level) ? level : "info");
  snprintf(entry.message, sizeof(entry.message), "%s", saneMessage.substring(0, DEVICE_LOG_MESSAGE_MAX_LEN).c_str());
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
  if (ledFlashAteMs != 0 && static_cast<long>(millis() - ledFlashAteMs) < 0) {
    setLedColor(ledFlashRed, ledFlashGreen, ledFlashBlue);
    return;
  }
  ledFlashAteMs = 0;

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

String escaparJson(const String& valor) {
  String escaped;
  escaped.reserve(valor.length() + 8);
  for (size_t i = 0; i < valor.length(); i++) {
    char caractere = valor[i];
    switch (caractere) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += caractere; break;
    }
  }
  return escaped;
}

String extrairArrayJson(const String& payload, const char* chave) {
  String marcador = "\"" + String(chave) + "\"";
  int inicioChave = payload.indexOf(marcador);
  if (inicioChave < 0) {
    return "";
  }

  int inicioValor = payload.indexOf(':', inicioChave);
  if (inicioValor < 0) {
    return "";
  }

  int inicioArray = payload.indexOf('[', inicioValor);
  if (inicioArray < 0) {
    return "";
  }

  bool emString = false;
  bool escape = false;
  int profundidade = 0;
  for (int i = inicioArray; i < payload.length(); i++) {
    char caractere = payload[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (emString && caractere == '\\') {
      escape = true;
      continue;
    }
    if (caractere == '"') {
      emString = !emString;
      continue;
    }
    if (emString) {
      continue;
    }
    if (caractere == '[') {
      profundidade++;
      continue;
    }
    if (caractere == ']') {
      profundidade--;
      if (profundidade == 0) {
        return payload.substring(inicioArray + 1, i);
      }
    }
  }

  return "";
}

bool proximoObjetoJson(const String& payload, int& cursor, String& objeto) {
  objeto = "";
  bool emString = false;
  bool escape = false;
  int inicioObjeto = -1;
  int profundidade = 0;

  for (int i = cursor; i < payload.length(); i++) {
    char caractere = payload[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (emString && caractere == '\\') {
      escape = true;
      continue;
    }
    if (caractere == '"') {
      emString = !emString;
      continue;
    }
    if (emString) {
      continue;
    }
    if (caractere == '{') {
      if (inicioObjeto < 0) {
        inicioObjeto = i;
      }
      profundidade++;
      continue;
    }
    if (caractere == '}') {
      if (inicioObjeto < 0) {
        continue;
      }
      profundidade--;
      if (profundidade == 0) {
        objeto = payload.substring(inicioObjeto, i + 1);
        cursor = i + 1;
        return true;
      }
    }
  }

  cursor = payload.length();
  return false;
}

void preencherRegraGatilhoLocalPadrao(RegraGatilhoLocal& regra, size_t indice) {
  regra.enabled = false;
  regra.triggerPin = PINOS_GATILHO_LOCAL[indice];
  regra.outputPin = PINOS_SAIDA_GATILHO_LOCAL[0];
  regra.outputActiveHigh = true;
  regra.holdMs = 1000;
  regra.repeatCount = 1;
  regra.repeatGapMs = LOCAL_GPIO_GAP_MS;
  regra.rawInputActive = false;
  regra.lastInputActive = false;
  regra.armed = false;
  regra.rawChangedAtMs = 0;
  regra.releaseSinceMs = 0;
}

bool regraGatilhoLocalValidaNoConjunto(
  const RegraGatilhoLocal& regra,
  const RegraGatilhoLocal* conjunto,
  int indiceIgnorado = -1
) {
  if (!pinoGatilhoLocalPermitido(regra.triggerPin) || !pinoSaidaGatilhoLocalPermitido(regra.outputPin)) {
    return false;
  }
  if (regra.outputPin == regra.triggerPin || pinoEhSensorConfigurado(regra.outputPin)) {
    return false;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    if (static_cast<int>(i) == indiceIgnorado) continue;
    if (!conjunto[i].enabled) continue;
    if (conjunto[i].triggerPin == regra.outputPin || conjunto[i].outputPin == regra.outputPin) {
      return false;
    }
  }

  return true;
}

bool regraGatilhoLocalValida(const RegraGatilhoLocal& regra, int indiceIgnorado = -1) {
  return regraGatilhoLocalValidaNoConjunto(regra, regrasGatilhoLocal, indiceIgnorado);
}

void inicializarRegrasGatilhoLocal() {
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    preencherRegraGatilhoLocalPadrao(regrasGatilhoLocal[i], i);
  }

  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_CONTROLADAS; i++) {
    estadosSaidaLocal[i].pin = PINOS_SAIDA_CONTROLADAS[i];
    estadosSaidaLocal[i].sequenceActive = false;
    estadosSaidaLocal[i].activeHigh = true;
    estadosSaidaLocal[i].pulseActive = false;
    estadosSaidaLocal[i].pendingPulses = 0;
    estadosSaidaLocal[i].nextTransitionAtMs = 0;
    estadosSaidaLocal[i].holdMs = 0;
    estadosSaidaLocal[i].gapMs = 0;
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
    snprintf(key, sizeof(key), "r%u_rc", static_cast<unsigned>(i));
    preferences.putUChar(key, regrasGatilhoLocal[i].repeatCount);
    snprintf(key, sizeof(key), "r%u_gp", static_cast<unsigned>(i));
    preferences.putUInt(key, static_cast<uint32_t>(regrasGatilhoLocal[i].repeatGapMs));
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
    snprintf(key, sizeof(key), "r%u_rc", static_cast<unsigned>(i));
    regra.repeatCount = preferences.getUChar(key, 1);
    snprintf(key, sizeof(key), "r%u_gp", static_cast<unsigned>(i));
    regra.repeatGapMs = preferences.getUInt(key, LOCAL_GPIO_GAP_MS);
    if (regra.repeatCount < 1) regra.repeatCount = 1;
    if (regra.repeatCount > 20) regra.repeatCount = 20;
    if (regra.repeatGapMs > 600000UL) regra.repeatGapMs = 600000UL;
    regra.rawInputActive = false;
    regra.lastInputActive = false;
    regra.armed = false;
    regra.rawChangedAtMs = 0;
    regra.releaseSinceMs = 0;

    if (regra.enabled && !regraGatilhoLocalValida(regra, static_cast<int>(i))) {
      regra.enabled = false;
    }
    regrasGatilhoLocal[i] = regra;
  }
}

void aplicarConfiguracaoRegrasGatilhoLocal() {
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_CONTROLADAS; i++) {
    pinMode(estadosSaidaLocal[i].pin, INPUT);
    estadosSaidaLocal[i].sequenceActive = false;
    estadosSaidaLocal[i].pulseActive = false;
    estadosSaidaLocal[i].pendingPulses = 0;
    estadosSaidaLocal[i].nextTransitionAtMs = 0;
    estadosSaidaLocal[i].holdMs = 0;
    estadosSaidaLocal[i].gapMs = 0;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    RegraGatilhoLocal& regra = regrasGatilhoLocal[i];
    pinMode(regra.triggerPin, INPUT_PULLUP);
    bool entradaAtiva = digitalRead(regra.triggerPin) == LOW;
    regra.rawInputActive = entradaAtiva;
    regra.lastInputActive = entradaAtiva;
    regra.armed = !entradaAtiva;
    regra.rawChangedAtMs = millis();
    regra.releaseSinceMs = entradaAtiva ? 0 : millis();

    if (!regra.enabled) {
      continue;
    }

    configurarGpioLocalInativo(regra.outputPin, regra.outputActiveHigh);
  }
}

bool agendarSaidaLocal(uint8_t pin, bool ativoEmHigh, unsigned long duracaoMs, uint8_t repeticoes, unsigned long intervaloEntreRepeticoesMs) {
  EstadoSaidaLocal* estado = buscarEstadoSaidaLocal(pin);
  if (estado == nullptr) return false;
  if (pinoEhSensorConfigurado(pin)) return false;
  if (repeticoes < 1) repeticoes = 1;
  if (repeticoes > 20) repeticoes = 20;
  if (intervaloEntreRepeticoesMs > 600000UL) intervaloEntreRepeticoesMs = 600000UL;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, ativoEmHigh ? HIGH : LOW);
  estado->sequenceActive = true;
  estado->activeHigh = ativoEmHigh;
  estado->pulseActive = true;
  estado->pendingPulses = repeticoes - 1;
  estado->nextTransitionAtMs = millis() + duracaoMs;
  estado->holdMs = duracaoMs;
  estado->gapMs = intervaloEntreRepeticoesMs;
  return true;
}

void atualizarSaidasLocais() {
  unsigned long agora = millis();
  for (size_t i = 0; i < TOTAL_PINOS_SAIDA_CONTROLADAS; i++) {
    EstadoSaidaLocal& estado = estadosSaidaLocal[i];
    if (!estado.sequenceActive) continue;

    if (static_cast<long>(agora - estado.nextTransitionAtMs) < 0) continue;

    if (estado.pulseActive) {
      configurarGpioLocalInativo(estado.pin, estado.activeHigh);
      estado.pulseActive = false;
      if (estado.pendingPulses == 0) {
        estado.sequenceActive = false;
        estado.nextTransitionAtMs = 0;
        estado.holdMs = 0;
        estado.gapMs = 0;
      } else {
        estado.nextTransitionAtMs = agora + estado.gapMs;
      }
      continue;
    }

    pinMode(estado.pin, OUTPUT);
    digitalWrite(estado.pin, estado.activeHigh ? HIGH : LOW);
    estado.pulseActive = true;
    estado.pendingPulses--;
    estado.nextTransitionAtMs = agora + estado.holdMs;
  }
}

void avaliarGatilhosLocais() {
  unsigned long agora = millis();
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    RegraGatilhoLocal& regra = regrasGatilhoLocal[i];
    if (!regra.enabled) continue;
    bool ativacaoEstavel = false;

    bool leituraAtiva = digitalRead(regra.triggerPin) == LOW;
    if (leituraAtiva != regra.rawInputActive) {
      regra.rawInputActive = leituraAtiva;
      regra.rawChangedAtMs = agora;
    }

    if (
      regra.rawInputActive != regra.lastInputActive
      && static_cast<long>(agora - regra.rawChangedAtMs) >= static_cast<long>(LOCAL_TRIGGER_DEBOUNCE_MS)
    ) {
      regra.lastInputActive = regra.rawInputActive;
      if (regra.lastInputActive) {
        regra.releaseSinceMs = 0;
        ativacaoEstavel = true;
      } else {
        regra.releaseSinceMs = agora;
      }
    }

    if (!regra.lastInputActive && !regra.armed && regra.releaseSinceMs != 0 && static_cast<long>(agora - regra.releaseSinceMs) >= static_cast<long>(LOCAL_TRIGGER_REARM_MS)) {
      regra.armed = true;
    }

    EstadoSaidaLocal* estadoSaida = buscarEstadoSaidaLocal(regra.outputPin);
    bool saidaOcupada = estadoSaida != nullptr && estadoSaida->sequenceActive;
    if (ativacaoEstavel && regra.armed && !saidaOcupada) {
      Serial.print("Gatilho local em GPIO ");
      Serial.print(regra.triggerPin);
      Serial.print(" -> saida GPIO ");
      Serial.print(regra.outputPin);
      Serial.print(" por ");
      Serial.print(regra.holdMs);
      Serial.print(" ms x ");
      Serial.print(regra.repeatCount);
      Serial.print(" intervalo=");
      Serial.print(regra.repeatGapMs);
      Serial.println(" ms");
      agendarSaidaLocal(regra.outputPin, regra.outputActiveHigh, regra.holdMs, regra.repeatCount, regra.repeatGapMs);
      regra.armed = false;
    } else if (ativacaoEstavel && regra.armed && saidaOcupada) {
      regra.armed = false;
    }
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
    json += "\"hold_ms\":" + String(regra.holdMs) + ",";
    json += "\"repeat_count\":" + String(regra.repeatCount) + ",";
    json += "\"repeat_gap_ms\":" + String(regra.repeatGapMs);
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

bool agendarPulsoLocalValidado(
  int pin,
  int durationMs,
  int repeatCount,
  int repeatGapMs,
  String activeLevel,
  String& erro
) {
  erro = "";
  activeLevel.toUpperCase();

  if (!pinoGpioAutomacaoSensorPermitido(pin)) {
    erro = "invalid_pin";
    return false;
  }
  if (pinoEhSensorConfigurado(static_cast<uint8_t>(pin))) {
    erro = "pin_in_use_by_sensor";
    return false;
  }
  if (activeLevel != "HIGH" && activeLevel != "LOW") {
    erro = "invalid_active_level";
    return false;
  }

  if (durationMs < 50) durationMs = 50;
  if (durationMs > 600000) durationMs = 600000;
  if (repeatCount < 1) repeatCount = 1;
  if (repeatCount > 20) repeatCount = 20;
  if (repeatGapMs < 0) repeatGapMs = 0;
  if (repeatGapMs > 600000) repeatGapMs = 600000;

  bool ativoEmHigh = activeLevel == "HIGH";
  if (!agendarSaidaLocal(
    static_cast<uint8_t>(pin),
    ativoEmHigh,
    static_cast<unsigned long>(durationMs),
    static_cast<uint8_t>(repeatCount),
    static_cast<unsigned long>(repeatGapMs)
  )) {
    erro = "pin_schedule_failed";
    return false;
  }

  Serial.print("GPIO local acionado no pino ");
  Serial.print(pin);
  Serial.print(" nivel=");
  Serial.print(activeLevel);
  Serial.print(" duracaoMs=");
  Serial.print(durationMs);
  Serial.print(" repeticoes=");
  Serial.print(repeatCount);
  Serial.print(" intervalo=");
  Serial.print(repeatGapMs);
  Serial.println(" ms");
  return true;
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
  int repeatGapMs = extrairCampoJsonInt(payload, "repeat_gap_ms", LOCAL_GPIO_GAP_MS);
  String activeLevel = extrairCampoJsonString(payload, "active_level");
  String erro;
  if (!agendarPulsoLocalValidado(pin, durationMs, repeatCount, repeatGapMs, activeLevel, erro)) {
    int statusCode = erro == "invalid_pin" || erro == "invalid_active_level" ? 400 : 409;
    responderJsonLocal(statusCode, "{\"ok\":false,\"error\":\"" + erro + "\"}");
    return;
  }

  responderJsonLocal(200, "{\"ok\":true,\"scheduled\":true}");
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
  int repeatCount = extrairCampoJsonInt(payload, "repeat_count", 1);
  int repeatGapMs = extrairCampoJsonInt(payload, "repeat_gap_ms", LOCAL_GPIO_GAP_MS);
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
  if (repeatCount < 1) repeatCount = 1;
  if (repeatCount > 20) repeatCount = 20;
  if (repeatGapMs < 0) repeatGapMs = 0;
  if (repeatGapMs > 600000) repeatGapMs = 600000;

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
  candidata.repeatCount = static_cast<uint8_t>(repeatCount);
  candidata.repeatGapMs = static_cast<unsigned long>(repeatGapMs);
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

void adicionarCabecalhosBackend(HTTPClient& http, bool enviarJson) {
  http.addHeader("Accept", "application/json");
  if (enviarJson) {
    http.addHeader("Content-Type", "application/json");
  }
  if (strlen(DEVICE_API_TOKEN) > 0) {
    http.addHeader("Authorization", "Bearer " + String(DEVICE_API_TOKEN));
  }
}

bool backendRequest(const char* method, const String& url, const String& payload, int& httpCode, String& resposta) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  httpCode = 0;
  resposta = "";
  bool usarTls = url.startsWith("https://");

  if (usarTls) {
    WiFiClientSecure client;
    if (strlen(BACKEND_CA_CERT) > 0) {
      client.setCACert(BACKEND_CA_CERT);
    } else if (BACKEND_TLS_INSECURE) {
      client.setInsecure();
    } else {
      Serial.println("TLS sem BACKEND_CA_CERT e sem BACKEND_TLS_INSECURE.");
      return false;
    }

    HTTPClient http;
    if (!http.begin(client, url)) {
      Serial.print("Falha ao iniciar HTTPS em ");
      Serial.println(url);
      return false;
    }
    http.setConnectTimeout(DEVICE_HTTP_TIMEOUT_MS);
    http.setTimeout(DEVICE_HTTP_TIMEOUT_MS);
    adicionarCabecalhosBackend(http, String(method) != "GET");
    httpCode = String(method) == "GET" ? http.GET() : http.POST(payload);
    resposta = http.getString();
    http.end();
    return true;
  }

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.print("Falha ao iniciar HTTP em ");
    Serial.println(url);
    return false;
  }
  http.setConnectTimeout(DEVICE_HTTP_TIMEOUT_MS);
  http.setTimeout(DEVICE_HTTP_TIMEOUT_MS);
  adicionarCabecalhosBackend(http, String(method) != "GET");
  httpCode = String(method) == "GET" ? http.GET() : http.POST(payload);
  resposta = http.getString();
  http.end();
  return true;
}

bool postJson(const String& url, const String& payload, int& httpCode, String& resposta) {
  return backendRequest("POST", url, payload, httpCode, resposta);
}

bool getJson(const String& url, int& httpCode, String& resposta) {
  return backendRequest("GET", url, "", httpCode, resposta);
}

String montarUrl(const char* rota) {
  return String(url_default) + rota;
}

String montarUrlComQuery(const char* rota, const String& query) {
  String url = montarUrl(rota);
  if (query.length() > 0) {
    url += "?";
    url += query;
  }
  return url;
}

void atualizarIntervaloPollingBackend(unsigned long sugeridoMs) {
  if (sugeridoMs < 250UL) sugeridoMs = 250UL;
  if (sugeridoMs > 30000UL) sugeridoMs = 30000UL;
  intervaloPollingBackendMs = sugeridoMs;
}

void carregarVersaoConfiguracaoAplicada() {
  if (!preferencesAtivas) {
    versaoConfiguracaoAplicada = 0;
    return;
  }
  versaoConfiguracaoAplicada = preferences.getUInt(PREFERENCES_CONFIG_VERSION_KEY, 0);
}

void salvarVersaoConfiguracaoAplicada() {
  if (!preferencesAtivas) return;
  preferences.putUInt(PREFERENCES_CONFIG_VERSION_KEY, versaoConfiguracaoAplicada);
}

bool verificarHealthBackend() {
  // Confirma que a API local continua respondendo; falhas repetidas reiniciam o ESP.
  if (WiFi.status() != WL_CONNECTED) {
    backendDisponivel = false;
    return false;
  }

  int httpCode = 0;
  String resposta;
  if (!getJson(montarUrl(HEALTH_ROUTE), httpCode, resposta)) {
    backendDisponivel = false;
    return false;
  }

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
  if (!postJson(montarUrl(SENSOR_REGISTER_ROUTE), payload, httpCode, resposta)) {
    backendDisponivel = false;
    sensoresRegistrados = false;
    if (ultimoRegistroBackendSucesso) {
      anexarLogDispositivo("warn", "backend_register_failed");
    }
    ultimoRegistroBackendSucesso = false;
    return false;
  }

  Serial.print("POST register -> HTTP ");
  Serial.println(httpCode);
  if (resposta.length()) {
    Serial.println(resposta);
  }

  sensoresRegistrados = httpCode >= 200 && httpCode < 300;
  backendDisponivel = sensoresRegistrados;
  if (sensoresRegistrados) {
    ultimoDeviceSyncMs = 0;
    ultimoLogFlushMs = 0;
    if (!ultimoRegistroBackendSucesso) {
      anexarLogDispositivo("info", "backend_registered");
    }
    ultimoRegistroBackendSucesso = true;
  } else {
    if (ultimoRegistroBackendSucesso) {
      anexarLogDispositivo("warn", "backend_register_http_" + String(httpCode));
    }
    ultimoRegistroBackendSucesso = false;
  }
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
    anexarLogDispositivo("info", "wifi_connected ip=" + WiFi.localIP().toString());
    setupServidorLocal();
    sensoresRegistrados = false;
    backendDisponivel = false;
    registroBackendTentado = false;
    ultimoDeviceSyncMs = 0;
    ultimoLogFlushMs = 0;
    atualizarIntervaloPollingBackend(DEVICE_POLL_INTERVAL_MS);
    registrarSensoresNoBackend();
  } else {
    Serial.println("Falha ao conectar no WiFi.");
    anexarLogDispositivo("warn", "wifi_connect_failed");
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
  if (!postJson(montarUrl(MOVEMENT_ROUTE), payload, httpCode, resposta)) {
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

  String payload = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"command_port\":" + String(DEVICE_COMMAND_PORT) + ",\"config_version\":" + String(versaoConfiguracaoAplicada) + "}";
  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl(HEARTBEAT_ROUTE), payload, httpCode, resposta)) {
    if (ultimoHeartbeatSucesso) {
      anexarLogDispositivo("warn", "heartbeat_failed");
    }
    ultimoHeartbeatSucesso = false;
    return false;
  }

  backendDisponivel = httpCode >= 200 && httpCode < 300;
  if (backendDisponivel) {
    dispararFlashLed(0, 18, 26, LED_FLASH_HEARTBEAT_MS);
    if (!ultimoHeartbeatSucesso) {
      anexarLogDispositivo("info", "heartbeat_restored");
    }
    ultimoHeartbeatSucesso = true;
  } else {
    if (ultimoHeartbeatSucesso) {
      anexarLogDispositivo("warn", "heartbeat_http_" + String(httpCode));
    }
    ultimoHeartbeatSucesso = false;
  }
  return backendDisponivel;
}

bool enviarAckConfiguracao(uint32_t configVersion, const char* status, const String& message) {
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"config_version\":" + String(configVersion) + ",";
  payload += "\"status\":\"" + String(status) + "\"";
  if (message.length() > 0) {
    payload += ",\"message\":\"" + escaparJson(message) + "\"";
  }
  payload += "}";

  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl(DEVICE_CONFIG_ACK_ROUTE), payload, httpCode, resposta)) {
    return false;
  }
  return httpCode >= 200 && httpCode < 300;
}

bool enviarAckComando(const String& commandId, const char* status, const String& message) {
  if (commandId.length() == 0) {
    return false;
  }

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"command_id\":\"" + commandId + "\",";
  payload += "\"status\":\"" + String(status) + "\"";
  if (message.length() > 0) {
    payload += ",\"message\":\"" + escaparJson(message) + "\"";
  }
  payload += "}";

  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl(DEVICE_COMMAND_ACK_ROUTE), payload, httpCode, resposta)) {
    return false;
  }
  return httpCode >= 200 && httpCode < 300;
}

bool enviarLogsDispositivo() {
  if (totalLogsPendentes == 0 || WiFi.status() != WL_CONNECTED || !sensoresRegistrados) {
    return false;
  }

  size_t quantidade = totalLogsPendentes < DEVICE_LOG_BATCH_SIZE ? totalLogsPendentes : DEVICE_LOG_BATCH_SIZE;
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",\"logs\":[";
  for (size_t i = 0; i < quantidade; i++) {
    if (i > 0) payload += ",";
    payload += "{";
    payload += "\"level\":\"" + String(filaLogsDispositivo[i].level) + "\",";
    payload += "\"message\":\"" + escaparJson(String(filaLogsDispositivo[i].message)) + "\",";
    payload += "\"source\":\"firmware\"";
    payload += "}";
  }
  payload += "]}";

  int httpCode = 0;
  String resposta;
  if (!postJson(montarUrl("/api/device/logs"), payload, httpCode, resposta)) {
    return false;
  }
  if (httpCode < 200 || httpCode >= 300) {
    return false;
  }

  for (size_t i = quantidade; i < totalLogsPendentes; i++) {
    filaLogsDispositivo[i - quantidade] = filaLogsDispositivo[i];
  }
  totalLogsPendentes -= quantidade;
  return true;
}

bool aplicarConfiguracaoRemotaLocal(const String& resposta, uint32_t configVersion, String& erro) {
  erro = "";
  if (resposta.indexOf("\"local_trigger_rules\"") < 0) {
    erro = "local_trigger_rules_missing";
    return false;
  }

  String regrasArray = extrairArrayJson(resposta, "local_trigger_rules");
  RegraGatilhoLocal novasRegras[TOTAL_PINOS_GATILHO_LOCAL];
  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    preencherRegraGatilhoLocalPadrao(novasRegras[i], i);
  }

  int cursor = 0;
  String regraJson;
  while (proximoObjetoJson(regrasArray, cursor, regraJson)) {
    int triggerPin = extrairCampoJsonInt(regraJson, "trigger_pin", -1);
    int indice = indicePinoGatilhoLocalPermitido(triggerPin);
    if (indice < 0) {
      erro = "invalid_trigger_pin";
      return false;
    }

    RegraGatilhoLocal candidata = novasRegras[indice];
    candidata.enabled = extrairCampoJsonBool(regraJson, "enabled", false);
    candidata.outputPin = static_cast<uint8_t>(extrairCampoJsonInt(regraJson, "output_pin", candidata.outputPin));
    candidata.holdMs = static_cast<unsigned long>(extrairCampoJsonInt(regraJson, "hold_ms", static_cast<int>(candidata.holdMs)));
    candidata.repeatCount = static_cast<uint8_t>(extrairCampoJsonInt(regraJson, "repeat_count", static_cast<int>(candidata.repeatCount)));
    candidata.repeatGapMs = static_cast<unsigned long>(extrairCampoJsonInt(regraJson, "repeat_gap_ms", static_cast<int>(candidata.repeatGapMs)));
    String outputLevel = extrairCampoJsonString(regraJson, "output_level");
    outputLevel.toUpperCase();

    if (!pinoSaidaGatilhoLocalPermitido(candidata.outputPin)) {
      erro = "invalid_output_pin";
      return false;
    }
    if (outputLevel != "HIGH" && outputLevel != "LOW") {
      erro = "invalid_output_level";
      return false;
    }
    if (candidata.holdMs < 50UL) candidata.holdMs = 50UL;
    if (candidata.holdMs > 600000UL) candidata.holdMs = 600000UL;
    if (candidata.repeatCount < 1) candidata.repeatCount = 1;
    if (candidata.repeatCount > 20) candidata.repeatCount = 20;
    if (candidata.repeatGapMs > 600000UL) candidata.repeatGapMs = 600000UL;
    candidata.outputActiveHigh = outputLevel == "HIGH";
    candidata.rawInputActive = false;
    candidata.lastInputActive = false;
    candidata.armed = false;
    candidata.rawChangedAtMs = 0;
    candidata.releaseSinceMs = 0;

    if (candidata.enabled && !regraGatilhoLocalValidaNoConjunto(candidata, novasRegras, indice)) {
      erro = "rule_pin_conflict";
      return false;
    }

    novasRegras[indice] = candidata;
  }

  for (size_t i = 0; i < TOTAL_PINOS_GATILHO_LOCAL; i++) {
    regrasGatilhoLocal[i] = novasRegras[i];
  }
  salvarRegrasGatilhoLocal();
  aplicarConfiguracaoRegrasGatilhoLocal();
  if (configVersion > versaoConfiguracaoAplicada) {
    dispararFlashLed(28, 18, 0, LED_FLASH_CONFIG_MS);
  }
  versaoConfiguracaoAplicada = configVersion;
  salvarVersaoConfiguracaoAplicada();
  anexarLogDispositivo("info", "config_applied v=" + String(configVersion));
  return true;
}

bool baixarEAplicarConfiguracaoRemota(uint32_t versaoEsperada) {
  int httpCode = 0;
  String resposta;
  String url = montarUrlComQuery(DEVICE_CONFIG_ROUTE, "device_id=" + String(DEVICE_ID));
  if (!getJson(url, httpCode, resposta)) {
    return false;
  }

  Serial.print("GET config -> HTTP ");
  Serial.println(httpCode);
  if (resposta.length()) {
    Serial.println(resposta);
  }
  if (httpCode < 200 || httpCode >= 300) {
    return false;
  }

  uint32_t configVersion = static_cast<uint32_t>(extrairCampoJsonInt(resposta, "config_version", static_cast<int>(versaoEsperada)));
  if (configVersion < versaoEsperada) {
    configVersion = versaoEsperada;
  }
  atualizarIntervaloPollingBackend(static_cast<unsigned long>(extrairCampoJsonInt(resposta, "poll_after_ms", static_cast<int>(intervaloPollingBackendMs))));

  String erro;
  if (!aplicarConfiguracaoRemotaLocal(resposta, configVersion, erro)) {
    dispararFlashLed(28, 0, 0, LED_FLASH_ERROR_MS);
    anexarLogDispositivo("error", "config_apply_error " + erro);
    enviarAckConfiguracao(configVersion, "error", erro);
    return false;
  }

  enviarAckConfiguracao(configVersion, "applied", "");
  return true;
}

bool executarComandoRemoto(const String& comandoJson) {
  String commandId = extrairCampoJsonString(comandoJson, "id");
  String tipo = extrairCampoJsonString(comandoJson, "type");
  tipo.toLowerCase();
  if (commandId.length() == 0) {
    return false;
  }

  if (tipo == "gpio_pulse") {
    int pin = extrairCampoJsonInt(comandoJson, "pin", -1);
    int durationMs = extrairCampoJsonInt(comandoJson, "duration_ms", 1000);
    int repeatCount = extrairCampoJsonInt(comandoJson, "repeat_count", 1);
    int repeatGapMs = extrairCampoJsonInt(comandoJson, "repeat_gap_ms", LOCAL_GPIO_GAP_MS);
    String activeLevel = extrairCampoJsonString(comandoJson, "active_level");
    String erro;
    if (!agendarPulsoLocalValidado(pin, durationMs, repeatCount, repeatGapMs, activeLevel, erro)) {
      anexarLogDispositivo("error", "command_gpio_pulse_error " + erro);
      enviarAckComando(commandId, "error", erro);
      return false;
    }
    anexarLogDispositivo("info", "command_gpio_pulse pin=" + String(pin));
    enviarAckComando(commandId, "accepted", "");
    return true;
  }

  if (tipo == "refresh_config") {
    bool ok = baixarEAplicarConfiguracaoRemota(versaoConfiguracaoAplicada);
    anexarLogDispositivo(ok ? "info" : "error", ok ? "command_refresh_config" : "command_refresh_config_failed");
    enviarAckComando(commandId, ok ? "done" : "error", ok ? "" : "config_refresh_failed");
    return ok;
  }

  anexarLogDispositivo("warn", "command_ignored type=" + tipo);
  enviarAckComando(commandId, "ignored", "unsupported_command");
  return true;
}

bool processarRespostaPolling(const String& resposta) {
  atualizarIntervaloPollingBackend(static_cast<unsigned long>(extrairCampoJsonInt(resposta, "poll_after_ms", static_cast<int>(intervaloPollingBackendMs))));
  uint32_t configVersionDesejada = static_cast<uint32_t>(extrairCampoJsonInt(resposta, "config_version", static_cast<int>(versaoConfiguracaoAplicada)));
  bool configChanged = extrairCampoJsonBool(resposta, "config_changed", false) || configVersionDesejada > versaoConfiguracaoAplicada;
  bool tudoOk = true;

  if (configChanged && !baixarEAplicarConfiguracaoRemota(configVersionDesejada)) {
    tudoOk = false;
  }

  String comandosArray = extrairArrayJson(resposta, "commands");
  int cursor = 0;
  String comandoJson;
  while (proximoObjetoJson(comandosArray, cursor, comandoJson)) {
    if (!executarComandoRemoto(comandoJson)) {
      tudoOk = false;
    }
  }

  return tudoOk;
}

bool sincronizarBackendDispositivo() {
  if (WiFi.status() != WL_CONNECTED || !sensoresRegistrados) {
    return false;
  }

  String query = "device_id=" + String(DEVICE_ID);
  query += "&config_version=" + String(versaoConfiguracaoAplicada);
  query += "&wait_seconds=" + String(DEVICE_POLL_WAIT_SECONDS);
  query += "&limit=5";

  int httpCode = 0;
  String resposta;
  if (!getJson(montarUrlComQuery(DEVICE_POLL_ROUTE, query), httpCode, resposta)) {
    backendDisponivel = false;
    return false;
  }

  bool configChanged = extrairCampoJsonBool(resposta, "config_changed", false);
  bool temComandos = extrairArrayJson(resposta, "commands").length() > 0;
  if (httpCode < 200 || httpCode >= 300 || configChanged || temComandos) {
    Serial.print("GET poll -> HTTP ");
    Serial.println(httpCode);
    Serial.println(resposta);
  }

  backendDisponivel = httpCode >= 200 && httpCode < 300;
  if (!backendDisponivel) {
    return false;
  }

  return processarRespostaPolling(resposta);
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
  anexarLogDispositivo("info", "boot");
  setupWatchdog();
  preferencesAtivas = preferences.begin(PREFERENCES_NAMESPACE, false);
  carregarVersaoConfiguracaoAplicada();
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

  Serial.print("Config version aplicada: ");
  Serial.println(versaoConfiguracaoAplicada);
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

  if (WiFi.status() == WL_CONNECTED && sensoresRegistrados && agora - ultimoDeviceSyncMs >= intervaloPollingBackendMs) {
    ultimoDeviceSyncMs = agora;
    if (!sincronizarBackendDispositivo()) {
      atualizarIntervaloPollingBackend(DEVICE_SYNC_FAIL_RETRY_MS);
    }
  }

  if (WiFi.status() == WL_CONNECTED && sensoresRegistrados && totalLogsPendentes > 0 && agora - ultimoLogFlushMs >= DEVICE_LOG_FLUSH_MS) {
    ultimoLogFlushMs = agora;
    enviarLogsDispositivo();
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
