#include <Arduino.h>

constexpr uint8_t MOTION_PIN = 8;
constexpr uint8_t MOTION_ACTIVE_LEVEL = HIGH;
constexpr uint8_t TOTAL_FLASHES = 5;
constexpr unsigned long HALF_BLINK_MS = 500;

bool movimentoAnteriorAtivo = false;
bool sequenciaAtiva = false;
bool ledLigado = false;
uint8_t flashesConcluidos = 0;
unsigned long ultimoBlinkMs = 0;

void setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
#if defined(RGB_BUILTIN)
  rgbLedWrite(RGB_BUILTIN, red, green, blue);
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, (red || green || blue) ? HIGH : LOW);
#endif
}

void desligarLed() {
  ledLigado = false;
  setLedColor(0, 0, 0);
}

void iniciarSequencia() {
  sequenciaAtiva = true;
  ledLigado = true;
  flashesConcluidos = 0;
  ultimoBlinkMs = millis();
  setLedColor(64, 0, 0);
  Serial.println("Movimento detectado: iniciando 5 piscadas em vermelho.");
}

void atualizarSequenciaLed() {
  if (!sequenciaAtiva) return;

  if (millis() - ultimoBlinkMs < HALF_BLINK_MS) return;

  ultimoBlinkMs = millis();
  ledLigado = !ledLigado;

  if (ledLigado) {
    setLedColor(64, 0, 0);
    return;
  }

  desligarLed();
  flashesConcluidos++;

  if (flashesConcluidos >= TOTAL_FLASHES) {
    sequenciaAtiva = false;
    Serial.println("Sequencia finalizada.");
  }
}

void setup() {
  Serial.begin(115200);

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif

  desligarLed();

  pinMode(MOTION_PIN, INPUT_PULLUP);

  Serial.println("Sketch de movimento no GPIO pronto.");
  Serial.println("Sensor NC ligado entre GPIO e GND.");
  Serial.println("Sem movimento = LOW | Movimento = HIGH");
}

void loop() {
  bool movimentoAtivo = digitalRead(MOTION_PIN) == MOTION_ACTIVE_LEVEL;

  if (movimentoAtivo && !movimentoAnteriorAtivo) {
    iniciarSequencia();
  }

  movimentoAnteriorAtivo = movimentoAtivo;
  atualizarSequenciaLed();
}