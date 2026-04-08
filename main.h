#include "Adafruit_GC9A01A.h"
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// PINOS
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_DC 10
#define TFT_CS 9
#define TFT_RST 14
#define ONE_WIRE_BUS 4
#define ENC_CLK 5
#define ENC_DT 6
#define ENC_SW 7
#define IR_RX 15
#define IR_TX 16

const char* ssid = "Danilo";
const char* password = "996639078Dd*";
bool netDhcp = true;
String netIp = "192.168.1.50";
String netGateway = "192.168.1.1";
String netSubnet = "255.255.255.0";
String netDns = "8.8.8.8";

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WebServer server(80);
GFXcanvas16 canvas(200, 60);
Preferences prefs;
IRrecv irrecv(IR_RX);
IRsend irsend(IR_TX);
decode_results irResults;

float tempAtual = 0.0f;
int telaAtual = 0; // 0=Temp, 1=Cadastro IR, 2=Emitir IR
bool precisaRedesenhar = true;
String statusTela = "Pronto";

bool btnLeituraAnterior = HIGH;
bool btnEstadoEstavel = HIGH;
unsigned long ultimoDebounce = 0;
const unsigned long debounceMs = 40;

int ultimoClkEstado = HIGH;
unsigned long ultimoGiroMs = 0;
const unsigned long debounceGiroMs = 80;

unsigned long ultimoTempMs = 0;
const unsigned long intervaloTempMs = 1000;

bool aguardandoCadastro = false;
unsigned long inicioCadastroMs = 0;
const unsigned long timeoutCadastroMs = 15000;

bool irSalvo = false;
uint8_t irTipo = 0;
uint64_t irValor = 0;
uint16_t irBits = 0;
const uint16_t kIrFreqKHz = 38;
const uint16_t kMaxRawSlots = 750;
uint16_t irRawLen = 0;
uint16_t irRawData[kMaxRawSlots];

void emitirRobusto() {
  const uint16_t freqs[] = {36, 38, 40};

  if (irRawLen > 0) {
    for (uint8_t f = 0; f < 3; f++) {
      for (uint8_t r = 0; r < 3; r++) {
        irsend.sendRaw(irRawData, irRawLen, freqs[f]);
        delay(45);
      }
      delay(70);
    }
  } else {
    for (uint8_t r = 0; r < 6; r++) {
      irsend.send(static_cast<decode_type_t>(irTipo), irValor, irBits);
      delay(45);
    }
  }
}

void salvarIR() {
  prefs.begin("sentinel", false);
  prefs.putBool("valid", true);
  prefs.putUChar("proto", irTipo);
  prefs.putULong64("value", irValor);
  prefs.putUShort("bits", irBits);
  prefs.putUShort("rawlen", irRawLen);
  prefs.putBytes("rawbuf", irRawData, irRawLen * sizeof(uint16_t));
  prefs.end();
}

bool parseIpOrFail(const String& text, IPAddress& out) {
  return out.fromString(text);
}

void salvarRede() {
  prefs.begin("netcfg", false);
  prefs.putBool("dhcp", netDhcp);
  prefs.putString("ip", netIp);
  prefs.putString("gw", netGateway);
  prefs.putString("mask", netSubnet);
  prefs.putString("dns", netDns);
  prefs.end();
}

void carregarRede() {
  prefs.begin("netcfg", true);
  netDhcp = prefs.getBool("dhcp", true);
  netIp = prefs.getString("ip", "192.168.1.50");
  netGateway = prefs.getString("gw", "192.168.1.1");
  netSubnet = prefs.getString("mask", "255.255.255.0");
  netDns = prefs.getString("dns", "8.8.8.8");
  prefs.end();
}

void aplicarConfigRede() {
  if (netDhcp) return;

  IPAddress ip;
  IPAddress gw;
  IPAddress mask;
  IPAddress dns;
  if (!parseIpOrFail(netIp, ip)) return;
  if (!parseIpOrFail(netGateway, gw)) return;
  if (!parseIpOrFail(netSubnet, mask)) return;
  if (!parseIpOrFail(netDns, dns)) return;
  WiFi.config(ip, gw, mask, dns);
}

void carregarIR() {
  prefs.begin("sentinel", true);
  irSalvo = prefs.getBool("valid", false);
  if (irSalvo) {
    irTipo = prefs.getUChar("proto", 0);
    irValor = prefs.getULong64("value", 0);
    irBits = prefs.getUShort("bits", 0);
    irRawLen = prefs.getUShort("rawlen", 0);
    if (irRawLen > kMaxRawSlots) {
      irRawLen = 0;
    }
    if (irRawLen > 0) {
      size_t expected = irRawLen * sizeof(uint16_t);
      size_t got = prefs.getBytes("rawbuf", irRawData, expected);
      if (got != expected) {
        irRawLen = 0;
      }
    }
    if (irBits == 0) irSalvo = false;
    if (irBits == 0 && irRawLen == 0) irSalvo = false;
  }
  prefs.end();
}

void drawTelaTemperatura() {
  canvas.fillScreen(0);
  canvas.setTextSize(4);
  canvas.setTextColor(GC9A01A_GREEN);
  canvas.setCursor(10, 8);
  canvas.print(tempAtual, 2);
  tft.drawRGBBitmap(35, 85, canvas.getBuffer(), 200, 60);

  tft.fillRect(20, 150, 200, 70, GC9A01A_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(82, 155);
  tft.print("Celsius");
  tft.setTextSize(1);
  tft.setCursor(62, 182);
  tft.print("Gire: Menu de telas");
}

void drawTelaCadastro() {
  tft.fillRect(20, 70, 200, 150, GC9A01A_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_CYAN);
  tft.setCursor(48, 80);
  tft.print("Cadastro IR");

  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(30, 120);
  tft.print("[ Cadastrar ]");

  tft.setTextSize(1);
  tft.setCursor(38, 150);
  tft.print("Clique para capturar IR");

  tft.setCursor(52, 170);
  tft.setTextColor(GC9A01A_YELLOW);
  tft.print(statusTela);

  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(62, 195);
  tft.print("Gire para outra tela");
}

void drawTelaEmitir() {
  tft.fillRect(20, 70, 200, 150, GC9A01A_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_MAGENTA);
  tft.setCursor(62, 80);
  tft.print("Emitir IR");

  tft.setTextSize(2);
  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(48, 120);
  tft.print("[ Emitir ]");

  tft.setTextSize(1);
  tft.setCursor(34, 150);
  tft.print(irSalvo ? "Clique para enviar clone IR" : "Nenhum IR cadastrado");

  tft.setCursor(52, 170);
  tft.setTextColor(GC9A01A_YELLOW);
  tft.print(statusTela);

  tft.setTextColor(GC9A01A_WHITE);
  tft.setCursor(62, 195);
  tft.print("Gire para outra tela");
}

void drawTelaAtual() {
  if (telaAtual == 0) {
    drawTelaTemperatura();
  } else if (telaAtual == 1) {
    drawTelaCadastro();
  } else {
    drawTelaEmitir();
  }
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
  html += "<p>IP atual: " + WiFi.localIP().toString() + "</p>";
  html += "<p><a href='/network'>Configurar rede</a></p>";
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
  json += "\"unit\":\"C\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleNetwork() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>";
  html += "<style>body{background:#121212;color:#ddd;font-family:sans-serif;padding:20px;}";
  html += ".box{max-width:460px;margin:0 auto;border:1px solid #00f2ff;padding:20px;border-radius:12px;background:#1e1e1e;}";
  html += "label{display:block;margin-top:12px;}input{width:100%;padding:8px;border-radius:8px;border:1px solid #444;background:#111;color:#fff;}";
  html += "button{margin-top:14px;padding:10px 14px;background:#00a878;color:white;border:0;border-radius:8px;}";
  html += "a{color:#00f2ff;}</style></head><body><div class='box'>";
  html += "<h2>Configurar IP</h2>";
  html += "<p>IP atual: " + WiFi.localIP().toString() + "</p>";
  html += "<form method='POST' action='/saveNetwork'>";
  html += "<label><input type='checkbox' name='dhcp' ";
  if (netDhcp) html += "checked";
  html += "> Usar DHCP automatico</label>";
  html += "<label>IP fixo</label><input name='ip' value='" + netIp + "'>";
  html += "<label>Gateway</label><input name='gw' value='" + netGateway + "'>";
  html += "<label>Subnet mask</label><input name='mask' value='" + netSubnet + "'>";
  html += "<label>DNS</label><input name='dns' value='" + netDns + "'>";
  html += "<button type='submit'>Salvar e reiniciar</button></form>";
  html += "<p><a href='/'>Voltar</a></p></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSaveNetwork() {
  bool novoDhcp = server.hasArg("dhcp");
  String novoIp = server.arg("ip");
  String novoGw = server.arg("gw");
  String novoMask = server.arg("mask");
  String novoDns = server.arg("dns");

  if (!novoDhcp) {
    IPAddress ip;
    IPAddress gw;
    IPAddress mask;
    IPAddress dns;
    if (!parseIpOrFail(novoIp, ip) || !parseIpOrFail(novoGw, gw) ||
        !parseIpOrFail(novoMask, mask) || !parseIpOrFail(novoDns, dns)) {
      server.send(400, "text/plain", "IP/Gateway/Subnet/DNS invalido.");
      return;
    }
  }

  netDhcp = novoDhcp;
  if (novoIp.length()) netIp = novoIp;
  if (novoGw.length()) netGateway = novoGw;
  if (novoMask.length()) netSubnet = novoMask;
  if (novoDns.length()) netDns = novoDns;
  salvarRede();

  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif;background:#121212;color:#fff;padding:20px;'>"
              "<h3>Rede salva. Reiniciando...</h3></body></html>");
  delay(700);
  ESP.restart();
}

void executarAcaoTela() {
  if (telaAtual == 1) {
    aguardandoCadastro = true;
    inicioCadastroMs = millis();
    statusTela = "Aguardando sinal IR";
    precisaRedesenhar = true;
    return;
  }

  if (telaAtual == 2) {
    if (!irSalvo) {
      statusTela = "Sem sinal salvo";
      precisaRedesenhar = true;
      return;
    }

    // Evita conflito entre RX e TX no periférico de IR durante envio.
    irrecv.disableIRIn();
    delay(20);
    emitirRobusto();
    irrecv.enableIRIn();
    statusTela = irRawLen > 0 ? "RAW emitido (sweep)" : "Proto emitido x6";
    precisaRedesenhar = true;
  }
}

void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);
  tft.drawCircle(120, 120, 118, GC9A01A_CYAN);

  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();
  ultimoTempMs = millis();

  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  ultimoClkEstado = digitalRead(ENC_CLK);

  irsend.begin();
  irrecv.enableIRIn();
  carregarIR();
  carregarRede();

  aplicarConfigRede();
  WiFi.begin(ssid, password);
  tft.setTextColor(GC9A01A_WHITE);
  tft.setTextSize(1);
  tft.setCursor(40, 30);
  tft.print("Conectando WiFi...");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
  }

  server.on("/", handleRoot);
  server.on("/readTemp", handleTemp);
  server.on("/api/temperature", HTTP_GET, handleApiTemperature);
  server.on("/network", HTTP_GET, handleNetwork);
  server.on("/saveNetwork", HTTP_POST, handleSaveNetwork);
  server.begin();

  tft.setCursor(55, 55);
  tft.print("IP: ");
  tft.print(WiFi.localIP());

  if (irSalvo) {
    statusTela = "IR ja cadastrado";
  }

  drawTelaAtual();
}

void loop() {
  server.handleClient();

  bool btnAtual = digitalRead(ENC_SW);
  if (btnAtual != btnLeituraAnterior) {
    ultimoDebounce = millis();
  }
  if ((millis() - ultimoDebounce) > debounceMs) {
    if (btnAtual != btnEstadoEstavel) {
      btnEstadoEstavel = btnAtual;
      if (btnEstadoEstavel == LOW) {
        executarAcaoTela();
      }
    }
  }
  btnLeituraAnterior = btnAtual;

  int clkEstado = digitalRead(ENC_CLK);
  if (clkEstado != ultimoClkEstado && clkEstado == LOW) {
    if (millis() - ultimoGiroMs > debounceGiroMs) {
      int direcao = (digitalRead(ENC_DT) != clkEstado) ? 1 : -1;
      telaAtual = (telaAtual + direcao + 3) % 3;
      precisaRedesenhar = true;
      ultimoGiroMs = millis();
    }
  }
  ultimoClkEstado = clkEstado;

  if (aguardandoCadastro) {
    if (irrecv.decode(&irResults)) {
      uint16_t correctedLen = getCorrectedRawLength(&irResults);
      if (correctedLen > kMaxRawSlots) correctedLen = kMaxRawSlots;
      if (correctedLen > 0) {
        uint16_t* capturedRaw = resultToRawArray(&irResults);
        if (capturedRaw != nullptr) {
          for (uint16_t i = 0; i < correctedLen; i++) {
            irRawData[i] = capturedRaw[i];
          }
          free(capturedRaw);
          irRawLen = correctedLen;
        } else {
          irRawLen = 0;
        }
      } else {
        irRawLen = 0;
      }

      if ((irResults.decode_type != decode_type_t::UNKNOWN && irResults.bits > 0) || irRawLen > 0) {
        irTipo = static_cast<uint8_t>(irResults.decode_type);
        irValor = irResults.value;
        irBits = irResults.bits;
        irSalvo = true;
        salvarIR();
        statusTela = irRawLen > 0 ? "RAW cadastrado" : "Sinal cadastrado";
      } else {
        statusTela = "Sinal nao suportado";
      }
      aguardandoCadastro = false;
      irrecv.resume();
      precisaRedesenhar = true;
    } else if (millis() - inicioCadastroMs > timeoutCadastroMs) {
      aguardandoCadastro = false;
      statusTela = "Timeout cadastro";
      precisaRedesenhar = true;
    }
  }

  if (millis() - ultimoTempMs >= intervaloTempMs) {
    float leitura = sensors.getTempCByIndex(0);
    if (leitura != DEVICE_DISCONNECTED_C) {
      tempAtual = leitura;
      if (telaAtual == 0) {
        precisaRedesenhar = true;
      }
    }
    sensors.requestTemperatures();
    ultimoTempMs = millis();
  }

  if (precisaRedesenhar) {
    drawTelaAtual();
    precisaRedesenhar = false;
  }
}
