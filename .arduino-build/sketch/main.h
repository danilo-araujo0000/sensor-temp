#line 1 "C:\\Users\\danilo\\Documents\\build\\sensor pre\\sensor-temp\\main.h"
#include "Adafruit_GC9A01A.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHTesp.h>
#include <Preferences.h>
#include <math.h>

// PINOS
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_DC 10
#define TFT_CS 9
#define TFT_RST 14
#define ONE_WIRE_BUS 4
#define DHT11_PIN 17
#define ENC_CLK 5
#define ENC_DT 6
#define ENC_SW 7

const char* ssid = "Tablet";
const char* password = "TABLET@admin2024";

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DHTesp dhtSensor;
WebServer server(80);
Preferences prefs;

float tempAtual = 0.0f;
float umidadeAtual = NAN;
float temperaturaDhtAtual = NAN;
bool sensorOnline = false;
bool umidadeOnline = false;
bool temperaturaDhtOnline = false;
String discordWebhookUrl = "";
bool alertaDiscordHabilitado = false;
bool alerta28Disparado = false;
float temperaturaAlertaDiscord = 28.0f;
float temperaturaResolvidoDiscord = 27.5f;
String mensagemAlertaDiscord = "Alerta de temperatura: {{temp}} C no sensor ESP32 | IP: {{ip}}";
String mensagemResolvidoDiscord = "Temperatura normalizada: {{temp}} C no sensor ESP32 | IP: {{ip}}";

unsigned long ultimoTempMs = 0;
const unsigned long intervaloTempMs = 1000;
unsigned long ultimoDhtMs = 0;
const unsigned long intervaloDhtMs = 1000;
unsigned long ultimoBlinkLedMs = 0;
bool ledBlinkLigado = false;

constexpr uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const int centroTela = 120;
const int raioArcoInterno = 96;
const int raioArcoExterno = 114;
const uint16_t corFundo = RGB565(229, 236, 240);
const uint16_t corPainel = RGB565(248, 250, 252);
const uint16_t corCartao = RGB565(255, 255, 255);
const uint16_t corTrilha = RGB565(198, 208, 215);
const uint16_t corInfo = RGB565(76, 90, 102);
const uint16_t corInfoSuave = RGB565(126, 140, 151);
const uint16_t corVerde = RGB565(46, 191, 113);
const uint16_t corAmarelo = RGB565(242, 191, 73);
const uint16_t corVermelho = RGB565(226, 87, 76);
const float deltaMinimoRedraw = 0.3f;

enum FaixaTemperatura {
  FaixaNormal,
  FaixaAlerta,
  FaixaCritica
};

enum TelaAtual {
  TelaTemperatura,
  TelaSensores,
  TelaIp
};

float temperaturaExibida = NAN;
float umidadeExibida = NAN;
float temperaturaDhtExibida = NAN;
FaixaTemperatura faixaAtual = FaixaNormal;
TelaAtual telaAtual = TelaTemperatura;
String ipExibido = "";
int ultimoClkEstado = HIGH;
unsigned long ultimoGiroMs = 0;
const unsigned long debounceGiroMs = 80;

FaixaTemperatura obterFaixaTemperatura(float temperatura) {
  if (temperatura <= 26.0f) return FaixaNormal;
  if (temperatura <= 30.0f) return FaixaAlerta;
  return FaixaCritica;
}

bool leituraValida(float leitura) {
  return leitura != DEVICE_DISCONNECTED_C && leitura > -55.0f && leitura < 125.0f && leitura != 85.0f;
}

bool leituraUmidadeValida(float leitura) {
  return !isnan(leitura) && leitura >= 0.0f && leitura <= 100.0f;
}

bool leituraTemperaturaDhtValida(float leitura) {
  return !isnan(leitura) && leitura > -40.0f && leitura < 80.0f;
}

String escapeJson(const String& input) {
  String output;
  output.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\\' || c == '"') {
      output += '\\';
      output += c;
    } else if (c == '\n') {
      output += "\\n";
    } else if (c != '\r') {
      output += c;
    }
  }

  return output;
}

void carregarConfigDiscord() {
  prefs.begin("alertas", true);
  discordWebhookUrl = prefs.getString("discord_url", "");
  alertaDiscordHabilitado = prefs.getBool("discord_on", false);
  temperaturaAlertaDiscord = prefs.getFloat("temp_alerta", 28.0f);
  temperaturaResolvidoDiscord = prefs.getFloat("temp_ok", 27.5f);
  mensagemAlertaDiscord = prefs.getString("msg_alerta", "Alerta de temperatura: {{temp}} C no sensor ESP32 | IP: {{ip}}");
  mensagemResolvidoDiscord = prefs.getString("msg_ok", "Temperatura normalizada: {{temp}} C no sensor ESP32 | IP: {{ip}}");
  prefs.end();
}

void salvarConfigDiscord() {
  prefs.begin("alertas", false);
  prefs.putString("discord_url", discordWebhookUrl);
  prefs.putBool("discord_on", alertaDiscordHabilitado);
  prefs.putFloat("temp_alerta", temperaturaAlertaDiscord);
  prefs.putFloat("temp_ok", temperaturaResolvidoDiscord);
  prefs.putString("msg_alerta", mensagemAlertaDiscord);
  prefs.putString("msg_ok", mensagemResolvidoDiscord);
  prefs.end();
}

String renderMensagemDiscord(const String& modelo, float leitura) {
  String ipAtual = WiFi.isConnected() ? WiFi.localIP().toString() : "Sem WiFi";
  String mensagem = modelo;
  mensagem.replace("{{temp}}", String(leitura, 1));
  mensagem.replace("{{ip}}", ipAtual);
  return mensagem;
}

bool enviarWebhookDiscord(const String& mensagem) {
  if (!alertaDiscordHabilitado || discordWebhookUrl.isEmpty() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, discordWebhookUrl)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  String payload = "{\"content\":\"" + escapeJson(mensagem) + "\"}";
  int httpCode = http.POST(payload);
  http.end();
  return httpCode > 0 && httpCode < 300;
}

void processarAlertaDiscord(float leitura) {
  if (!sensorOnline) return;

  if (leitura >= temperaturaAlertaDiscord && !alerta28Disparado) {
    String mensagem = renderMensagemDiscord(mensagemAlertaDiscord, leitura);
    if (enviarWebhookDiscord(mensagem)) {
      alerta28Disparado = true;
    }
  } else if (leitura <= temperaturaResolvidoDiscord && alerta28Disparado) {
    String mensagem = renderMensagemDiscord(mensagemResolvidoDiscord, leitura);
    if (enviarWebhookDiscord(mensagem)) {
      alerta28Disparado = false;
    }
  }
}

uint16_t corFaixaTemperatura() {
  if (faixaAtual == FaixaNormal) return corVerde;
  if (faixaAtual == FaixaAlerta) return corAmarelo;
  return corVermelho;
}

uint16_t corValorTemperatura() {
  if (faixaAtual == FaixaNormal) return corVerde;
  if (faixaAtual == FaixaAlerta) return corAmarelo;
  return corVermelho;
}

void drawTextoCentralizado(const String& texto, int y, uint8_t tamanho, uint16_t cor) {
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;

  tft.setTextSize(tamanho);
  tft.setTextColor(cor);
  tft.getTextBounds(texto, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((240 - w) / 2, y);
  tft.print(texto);
}

void drawArco(int inicioGraus, int fimGraus, uint16_t cor) {
  for (int angulo = inicioGraus; angulo <= fimGraus; angulo += 2) {
    float rad = angulo * DEG_TO_RAD;
    int xCentro = centroTela + static_cast<int>(cosf(rad) * ((raioArcoInterno + raioArcoExterno) / 2.0f));
    int yCentro = centroTela - static_cast<int>(sinf(rad) * ((raioArcoInterno + raioArcoExterno) / 2.0f));
    int raioSegmento = (raioArcoExterno - raioArcoInterno) / 2;
    tft.fillCircle(xCentro, yCentro, raioSegmento, cor);
  }
}

void drawContornoAnimado(int inicioGraus, int fimGraus, uint16_t cor) {
  const int raio = 118;
  const int raioSegmento = 2;

  for (int angulo = inicioGraus; angulo <= fimGraus; angulo += 3) {
    float rad = angulo * DEG_TO_RAD;
    int x = centroTela + static_cast<int>(cosf(rad) * raio);
    int y = centroTela - static_cast<int>(sinf(rad) * raio);
    tft.fillCircle(x, y, raioSegmento, cor);
  }
}

uint16_t corAnimacaoTela(TelaAtual tela) {
  if (tela == TelaTemperatura) return corFaixaTemperatura();
  if (tela == TelaSensores) return corInfo;
  return corInfoSuave;
}

void drawIndicadoresTelaAnimados(uint8_t preenchimentoTemp, uint8_t preenchimentoSensores, uint8_t preenchimentoIp) {
  const int y = 18;
  const int xTemp = 100;
  const int xSensores = 120;
  const int xIp = 140;
  const int raio = 4;

  tft.fillRect(84, 10, 72, 16, corFundo);
  tft.drawCircle(xTemp, y, raio, corInfoSuave);
  tft.drawCircle(xSensores, y, raio, corInfoSuave);
  tft.drawCircle(xIp, y, raio, corInfoSuave);

  if (preenchimentoTemp > 0) {
    tft.fillCircle(xTemp, y, preenchimentoTemp, corInfo);
  }
  if (preenchimentoSensores > 0) {
    tft.fillCircle(xSensores, y, preenchimentoSensores, corInfo);
  }
  if (preenchimentoIp > 0) {
    tft.fillCircle(xIp, y, preenchimentoIp, corInfo);
  }
}

void drawIndicadoresTela() {
  if (telaAtual == TelaTemperatura) {
    drawIndicadoresTelaAnimados(3, 0, 0);
  } else if (telaAtual == TelaSensores) {
    drawIndicadoresTelaAnimados(0, 3, 0);
  } else {
    drawIndicadoresTelaAnimados(0, 0, 3);
  }
}

void drawPainelBase() {
  tft.fillScreen(corFundo);
  drawIndicadoresTela();
  tft.fillCircle(centroTela, centroTela, 74, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 76, corTrilha);

  drawArco(22, 68, corTrilha);
  drawArco(112, 158, corTrilha);
  drawArco(202, 248, corTrilha);
  drawArco(292, 338, corTrilha);
}

void drawArcosFaixa() {
  uint16_t corAtual = corFaixaTemperatura();
  drawArco(22, 68, corAtual);
  drawArco(112, 158, corAtual);
  drawArco(202, 248, corAtual);
  drawArco(292, 338, corAtual);
}

void drawTemperaturaCentral() {
  String texto = sensorOnline ? String(tempAtual, 1) : "--.-";
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  uint16_t corTexto = sensorOnline ? corValorTemperatura() : corInfoSuave;
  const int yTexto = 96;
  const int raioGrau = 5;
  const int espacamento = 10;

  tft.fillRect(28, 90, 184, 48, corPainel);
  tft.setTextSize(5);
  tft.setTextColor(corTexto);
  tft.getTextBounds(texto, 0, yTexto, &x1, &y1, &w, &h);

  int xTexto = (240 - static_cast<int>(w) - espacamento - raioGrau * 2) / 2;
  tft.setCursor(xTexto, yTexto);
  tft.print(texto);

  if (sensorOnline) {
    int xGrau = xTexto + static_cast<int>(w) + espacamento;
    int yGrau = yTexto + 8;
    tft.drawCircle(xGrau, yGrau, raioGrau, corTexto);
  }
}

void drawUmidade() {
  String textoUmidade = umidadeOnline ? String(static_cast<int>(roundf(umidadeAtual))) + "%" : "--%";
  uint16_t corTexto = umidadeOnline ? corInfo : corInfoSuave;

  tft.fillRect(56, 152, 128, 22, corPainel);
  drawTextoCentralizado(textoUmidade, 156, 2, corTexto);
}

void drawTextoDireita(const String& texto, int xDireita, int y, uint8_t tamanho, uint16_t cor) {
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;

  tft.setTextSize(tamanho);
  tft.setTextColor(cor);
  tft.getTextBounds(texto, 0, y, &x1, &y1, &w, &h);
  tft.setCursor(xDireita - static_cast<int>(w), y);
  tft.print(texto);
}

void drawIconeTermometro(int x, int y, uint16_t cor, char identificador) {
  tft.drawRoundRect(x + 6, y - 10, 6, 18, 3, cor);
  tft.fillRect(x + 8, y - 2, 2, 8, cor);
  tft.drawCircle(x + 9, y + 8, 5, cor);
  tft.fillCircle(x + 9, y + 8, 3, cor);
  tft.setTextSize(1);
  tft.setTextColor(cor);
  tft.setCursor(x + 18, y + 2);
  tft.print(identificador);
}

void drawIconeUmidade(int x, int y, uint16_t cor) {
  tft.fillTriangle(x + 9, y - 4, x + 3, y + 7, x + 15, y + 7, cor);
  tft.fillCircle(x + 9, y + 8, 6, cor);
}

void drawCartaoSensorBase(int y, const String& rotulo, uint8_t tipoIcone) {
  const int x = 24;
  const int largura = 192;
  const int altura = 38;

  tft.fillRoundRect(x, y, largura, altura, 12, corCartao);
  tft.drawRoundRect(x, y, largura, altura, 12, corTrilha);

  if (tipoIcone == 0) {
    drawIconeTermometro(x + 12, y + 11, corInfo, '1');
  } else if (tipoIcone == 1) {
    drawIconeUmidade(x + 12, y + 11, corInfo);
  } else {
    drawIconeTermometro(x + 12, y + 11, corInfo, '2');
  }

  tft.setTextSize(1);
  tft.setTextColor(corInfoSuave);
  tft.setCursor(x + 44, y + 12);
  tft.print(rotulo);
}

void drawValorSensor(int y, const String& valor, bool online) {
  const int x = 130;
  const int largura = 74;
  const int altura = 24;
  uint16_t corTexto = online ? corInfo : corInfoSuave;

  tft.fillRect(x, y + 7, largura, altura, corCartao);
  drawTextoDireita(valor, x + largura - 4, y + 15, 2, corTexto);
}

void drawLinhaSensorPrincipal() {
  String valor = sensorOnline ? String(tempAtual, 1) + " C" : "--";
  drawValorSensor(58, valor, sensorOnline);
}

void drawLinhaSensorUmidade() {
  String valor = umidadeOnline ? String(static_cast<int>(roundf(umidadeAtual))) + "%" : "--";
  drawValorSensor(104, valor, umidadeOnline);
}

void drawLinhaSensorDht() {
  String valor = temperaturaDhtOnline ? String(temperaturaDhtAtual, 1) + " C" : "--";
  drawValorSensor(150, valor, temperaturaDhtOnline);
}

void atualizarDht11() {
  TempAndHumidity leituraDht = dhtSensor.getTempAndHumidity();
  bool redrawUmidadeMain = false;

  if (leituraUmidadeValida(leituraDht.humidity)) {
    umidadeAtual = leituraDht.humidity;
    if (!umidadeOnline || isnan(umidadeExibida) || fabsf(umidadeAtual - umidadeExibida) >= 1.0f) {
      redrawUmidadeMain = true;
      umidadeExibida = umidadeAtual;
    }
    umidadeOnline = true;
  } else if (umidadeOnline) {
    umidadeOnline = false;
    umidadeExibida = NAN;
    redrawUmidadeMain = true;
  }

  if (leituraTemperaturaDhtValida(leituraDht.temperature)) {
    temperaturaDhtAtual = leituraDht.temperature;
    temperaturaDhtOnline = true;
    temperaturaDhtExibida = temperaturaDhtAtual;
  } else {
    temperaturaDhtOnline = false;
    temperaturaDhtExibida = NAN;
  }

  if (redrawUmidadeMain && telaAtual == TelaTemperatura) {
    drawUmidade();
  }

  if (telaAtual == TelaSensores) {
    drawLinhaSensorUmidade();
    drawLinhaSensorDht();
  }
}

void drawTelaInicial() {
  drawPainelBase();
  drawArcosFaixa();
  drawTemperaturaCentral();
  drawUmidade();
}

void drawTelaSensores() {
  tft.fillScreen(corFundo);
  drawIndicadoresTela();
  tft.fillCircle(centroTela, centroTela, 92, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 94, corTrilha);
  drawTextoCentralizado("Sensores", 34, 2, corInfo);

  drawCartaoSensorBase(58, "Principal", 0);
  drawCartaoSensorBase(104, "Umidade", 1);
  drawCartaoSensorBase(150, "DHT11", 2);

  drawLinhaSensorPrincipal();
  drawLinhaSensorUmidade();
  drawLinhaSensorDht();
}

void drawTelaIp() {
  String ipAtual = WiFi.isConnected() ? WiFi.localIP().toString() : "Sem WiFi";

  tft.fillScreen(corFundo);
  drawIndicadoresTela();
  tft.fillCircle(centroTela, centroTela, 74, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 76, corTrilha);
  drawTextoCentralizado("IP", 72, 2, corInfo);

  tft.fillRect(32, 102, 176, 36, corPainel);
  drawTextoCentralizado(ipAtual, 110, 2, corInfo);
  drawTextoCentralizado("DHCP", 156, 2, corInfoSuave);
  ipExibido = ipAtual;
}

void drawTelaAtualCompleta() {
  if (telaAtual == TelaTemperatura) {
    drawTelaInicial();
  } else if (telaAtual == TelaSensores) {
    drawTelaSensores();
  } else {
    drawTelaIp();
  }
}

void animarTrocaTela(TelaAtual novaTela) {
  const int segmentos[][2] = {
    {198, 234},
    {243, 279},
    {288, 324},
    {333, 360},
    {0, 27},
    {36, 72},
    {81, 117},
    {126, 162}
  };

  telaAtual = novaTela;
  drawTelaAtualCompleta();

  uint16_t corDestaque = corAnimacaoTela(novaTela);
  for (uint8_t i = 0; i < 8; i++) {
    drawContornoAnimado(segmentos[i][0], segmentos[i][1], corDestaque);
    delay(6);
    drawContornoAnimado(segmentos[i][0], segmentos[i][1], corTrilha);
  }
}

void atualizarLedIntegrado() {
#if defined(RGB_BUILTIN)
  if (!sensorOnline) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
    return;
  }
  if (faixaAtual == FaixaNormal) {
    rgbLedWrite(RGB_BUILTIN, 0, 64, 0);
    return;
  }
  if (faixaAtual == FaixaAlerta && ledBlinkLigado) {
    rgbLedWrite(RGB_BUILTIN, 64, 64, 0);
    return;
  }
  if (faixaAtual == FaixaCritica && ledBlinkLigado) {
    rgbLedWrite(RGB_BUILTIN, 64, 0, 0);
    return;
  }
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
#elif defined(LED_BUILTIN)
  if (!sensorOnline) {
    digitalWrite(LED_BUILTIN, LOW);
    return;
  }
  if (faixaAtual == FaixaNormal) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }
  digitalWrite(LED_BUILTIN, ledBlinkLigado ? HIGH : LOW);
#endif
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>";
  html += "<style>body{background:#121212; color:#00ff88; font-family:sans-serif; text-align:center; padding-top:50px;}";
  html += ".box{border:2px solid #00f2ff; display:inline-block; padding:40px; border-radius:20px; background:#1e1e1e;}";
  html += "h1{font-size:80px; margin:20px 0;} a{color:#00f2ff;}</style>";
  html += "<script>setInterval(function(){fetch('/readTemp').then(r=>r.text()).then(v=>{document.getElementById('v').innerHTML=v;});}, 1000);</script>";
  html += "</head><body><div class='box'><h2>TERMOMETRO S3</h2>";
  html += "<h1 id='v'>" + String(tempAtual, 2) + "</h1><h2>Celsius</h2>";
  if (umidadeOnline) {
    html += "<p>Umidade: " + String(static_cast<int>(roundf(umidadeAtual))) + "%</p>";
  }
  html += "<p>IP atual: " + WiFi.localIP().toString() + "</p>";
  html += "<p>Rede via DHCP</p>";
  html += "<p><a href='/alertas'>Configurar alertas</a></p>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleTemp() {
  server.send(200, "text/plain", String(tempAtual, 2));
}

void handleApiTemperature() {
  String json = "{";
  json += "\"ok\":true,";
  json += "\"temperature_c\":" + String(tempAtual, 2) + ",";
  json += "\"humidity_percent\":";
  json += umidadeOnline ? String(umidadeAtual, 0) : "null";
  json += ",";
  json += "\"dht11_temperature_c\":";
  json += temperaturaDhtOnline ? String(temperaturaDhtAtual, 1) : "null";
  json += ",";
  json += "\"unit\":\"C\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleAlertas() {
  String html = "<!DOCTYPE html><html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>";
  html += "<style>body{background:#121212;color:#ddd;font-family:sans-serif;padding:20px;}";
  html += ".box{max-width:560px;margin:0 auto;border:1px solid #00f2ff;padding:20px;border-radius:12px;background:#1e1e1e;}";
  html += "label{display:block;margin-top:12px;}input[type=text],input[type=number],textarea{width:100%;padding:10px;border-radius:8px;border:1px solid #444;background:#111;color:#fff;}";
  html += "textarea{min-height:84px;resize:vertical;}";
  html += "button{margin-top:16px;padding:10px 14px;background:#00a878;color:white;border:0;border-radius:8px;}";
  html += "a{color:#00f2ff;}small{color:#9aa4ad;display:block;margin-top:8px;}</style></head><body><div class='box'>";
  html += "<h2>Alertas Discord</h2>";
  html += "<form method='POST' action='/saveAlertas'>";
  html += "<label><input type='checkbox' name='discord_on' ";
  if (alertaDiscordHabilitado) html += "checked";
  html += "> Habilitar webhook Discord</label>";
  html += "<label>Webhook URL</label><input type='text' name='discord_url' value='" + discordWebhookUrl + "'>";
  html += "<label>Temperatura de alerta</label><input type='number' step='0.1' name='temp_alerta' value='" + String(temperaturaAlertaDiscord, 1) + "'>";
  html += "<label>Temperatura de resolvido</label><input type='number' step='0.1' name='temp_ok' value='" + String(temperaturaResolvidoDiscord, 1) + "'>";
  html += "<label>Mensagem de alerta</label><textarea name='msg_alerta'>" + mensagemAlertaDiscord + "</textarea>";
  html += "<label>Mensagem de resolvido</label><textarea name='msg_ok'>" + mensagemResolvidoDiscord + "</textarea>";
  html += "<small>Use</small>";
  html += "<button type='submit'>Salvar</button></form>";
  html += "<form method='POST' action='/testAlertas'><button type='submit'>Enviar teste</button></form>";
  html += "<p><a href='/'>Voltar</a></p></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveAlertas() {
  float novoTempAlerta = server.arg("temp_alerta").toFloat();
  float novoTempOk = server.arg("temp_ok").toFloat();
  String novaMensagemAlerta = server.arg("msg_alerta");
  String novaMensagemOk = server.arg("msg_ok");
  String novaWebhook = server.arg("discord_url");
  novaWebhook.trim();

  if (novoTempAlerta <= 0.0f || novoTempOk <= 0.0f || novoTempOk > novoTempAlerta) {
    server.send(400, "text/html",
                "<html><body style='font-family:sans-serif;background:#121212;color:#fff;padding:20px;'>"
                "<h3>Temperaturas invalidas.</h3><p>A temperatura de resolvido nao pode ser maior que a de alerta.</p>"
                "<p><a href='/alertas' style='color:#00f2ff'>Voltar</a></p></body></html>");
    return;
  }

  alertaDiscordHabilitado = server.hasArg("discord_on");
  discordWebhookUrl = novaWebhook;
  temperaturaAlertaDiscord = novoTempAlerta;
  temperaturaResolvidoDiscord = novoTempOk;
  mensagemAlertaDiscord = novaMensagemAlerta.length() ? novaMensagemAlerta : "Alerta de temperatura: {{temp}} C no sensor ESP32 | IP: {{ip}}";
  mensagemResolvidoDiscord = novaMensagemOk.length() ? novaMensagemOk : "Temperatura normalizada: {{temp}} C no sensor ESP32 | IP: {{ip}}";
  salvarConfigDiscord();

  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif;background:#121212;color:#fff;padding:20px;'>"
              "<h3>Configuração salva.</h3><p><a href='/alertas' style='color:#00f2ff'>Voltar</a></p></body></html>");
}

void handleTestAlertas() {
  bool ok = enviarWebhookDiscord(renderMensagemDiscord(mensagemAlertaDiscord, tempAtual));
  String html = "<html><body style='font-family:sans-serif;background:#121212;color:#fff;padding:20px;'>";
  html += ok ? "<h3>Teste enviado com sucesso.</h3>" : "<h3>Falha ao enviar teste.</h3>";
  html += "<p><a href='/alertas' style='color:#00f2ff'>Voltar</a></p></body></html>";
  server.send(ok ? 200 : 500, "text/html", html);
}

void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(corFundo);
  tft.drawCircle(120, 120, 118, corTrilha);

  sensors.begin();
  sensors.setWaitForConversion(true);
  sensors.requestTemperatures();
  float leituraInicial = sensors.getTempCByIndex(0);
  if (leituraValida(leituraInicial)) {
    tempAtual = leituraInicial;
    temperaturaExibida = leituraInicial;
    faixaAtual = obterFaixaTemperatura(leituraInicial);
    sensorOnline = true;
  }
  carregarConfigDiscord();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  dhtSensor.setup(DHT11_PIN, DHTesp::DHT11);
  atualizarDht11();
  ultimoTempMs = millis();
  ultimoDhtMs = millis();
  ultimoBlinkLedMs = millis();

#if defined(RGB_BUILTIN)
  pinMode(RGB_BUILTIN, OUTPUT);
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  atualizarLedIntegrado();

  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  ultimoClkEstado = digitalRead(ENC_CLK);

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
  }

  server.on("/", handleRoot);
  server.on("/readTemp", handleTemp);
  server.on("/api/temperature", HTTP_GET, handleApiTemperature);
  server.on("/alertas", HTTP_GET, handleAlertas);
  server.on("/saveAlertas", HTTP_POST, handleSaveAlertas);
  server.on("/testAlertas", HTTP_POST, handleTestAlertas);
  server.begin();

  drawTelaAtualCompleta();
}

void loop() {
  server.handleClient();

  int clkEstado = digitalRead(ENC_CLK);
  if (clkEstado != ultimoClkEstado && clkEstado == LOW) {
    if (millis() - ultimoGiroMs > debounceGiroMs) {
      int direcao = (digitalRead(ENC_DT) != clkEstado) ? 1 : -1;
      if (direcao != 0) {
        int indiceTela = static_cast<int>(telaAtual) + direcao;
        if (indiceTela < 0) indiceTela = 2;
        if (indiceTela > 2) indiceTela = 0;
        TelaAtual novaTela = static_cast<TelaAtual>(indiceTela);
        animarTrocaTela(novaTela);
      }
      ultimoGiroMs = millis();
    }
  }
  ultimoClkEstado = clkEstado;

  unsigned long intervaloBlinkLedMs = 0;
  if (faixaAtual == FaixaAlerta) intervaloBlinkLedMs = 800;
  if (faixaAtual == FaixaCritica) intervaloBlinkLedMs = 220;

  if (intervaloBlinkLedMs > 0 && millis() - ultimoBlinkLedMs >= intervaloBlinkLedMs) {
    ledBlinkLigado = !ledBlinkLigado;
    atualizarLedIntegrado();
    ultimoBlinkLedMs = millis();
  }

  if (millis() - ultimoTempMs >= intervaloTempMs) {
    float leitura = sensors.getTempCByIndex(0);
    if (leituraValida(leitura)) {
      bool sensorMudou = !sensorOnline;
      FaixaTemperatura novaFaixa = obterFaixaTemperatura(leitura);
      bool mudouFaixa = novaFaixa != faixaAtual;
      bool mudouValor = isnan(temperaturaExibida) || fabsf(leitura - temperaturaExibida) >= deltaMinimoRedraw;

      sensorOnline = true;
      tempAtual = leitura;
      processarAlertaDiscord(leitura);
      if (sensorMudou || mudouFaixa) {
        faixaAtual = novaFaixa;
        ledBlinkLigado = false;
        if (telaAtual == TelaTemperatura) {
          drawArcosFaixa();
          drawTemperaturaCentral();
          drawUmidade();
        }
        atualizarLedIntegrado();
        ultimoBlinkLedMs = millis();
        temperaturaExibida = leitura;
      } else if (mudouValor && telaAtual == TelaTemperatura) {
        drawTemperaturaCentral();
        temperaturaExibida = leitura;
      } else if (mudouValor) {
        temperaturaExibida = leitura;
      }

      if (telaAtual == TelaSensores) {
        drawLinhaSensorPrincipal();
      }
    } else if (sensorOnline) {
      sensorOnline = false;
      temperaturaExibida = NAN;
      ledBlinkLigado = false;
      alerta28Disparado = false;
      if (telaAtual == TelaTemperatura) {
        drawTemperaturaCentral();
        drawUmidade();
      }
      if (telaAtual == TelaSensores) {
        drawLinhaSensorPrincipal();
      }
      atualizarLedIntegrado();
    }
    sensors.requestTemperatures();
    ultimoTempMs = millis();
  }

  if (millis() - ultimoDhtMs >= intervaloDhtMs) {
    atualizarDht11();
    ultimoDhtMs = millis();
  }

  if (telaAtual == TelaIp) {
    String ipAtual = WiFi.isConnected() ? WiFi.localIP().toString() : "Sem WiFi";
    if (ipAtual != ipExibido) {
      drawTelaIp();
    }
  }
}
