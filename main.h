#include "Adafruit_GC9A01A.h"
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
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

SPIClass tftSpi(FSPI);
Adafruit_GC9A01A tft(&tftSpi, TFT_DC, TFT_CS, TFT_RST);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DHTesp dhtSensor;
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

float tempAtual = 0.0f;
float umidadeAtual = NAN;
float temperaturaDhtAtual = NAN;
bool sensorOnline = false;
bool umidadeOnline = false;
bool temperaturaDhtOnline = false;
String discordWebhookUrl = "";
bool alertaDiscordHabilitado = false;
String telegramBotToken = "";
String telegramChatId = "";
bool alertaTelegramHabilitado = false;
String whatsappApiUrl = "";
String whatsappApiKey = "";
String whatsappDestinos = "";
bool alertaWhatsappHabilitado = false;
bool alertaDiscordDisparado = false;
bool alertaTelegramDisparado = false;
bool alertaWhatsappDisparado = false;
float temperaturaAlertaDiscord = 28.0f;
float temperaturaResolvidoDiscord = 27.5f;
String mensagemAlertaDiscord = "Alerta de temperatura: {{temp}} C°nome";
String mensagemResolvidoDiscord = "Temperatura normalizada: {{temp}} C° ";
String wifiSsidSalvo = "";
String wifiSenhaSalva = "";
String wifiApSsid = "";
bool wifiTemConfig = false;
bool portalConfiguracaoAtivo = false;

unsigned long ultimoTempMs = 0;
const unsigned long intervaloTempMs = 1000;
unsigned long ultimoDhtMs = 0;
const unsigned long intervaloDhtMs = 1000;
unsigned long ultimoBlinkLedMs = 0;
bool ledBlinkLigado = false;
unsigned long wifiTentativaInicioMs = 0;
unsigned long ultimoWifiRetryMs = 0;
const unsigned long wifiTempoConexaoMs = 15000;
const unsigned long wifiRetryMs = 10000;
const unsigned long alertaRetryMs = 15000;
const byte dnsPort = 53;
unsigned long inicioHoldResetMs = 0;
unsigned long ultimoRedrawResetMs = 0;
bool resetFactoryEmAndamento = false;
float progressoResetExibido = -1.0f;
const unsigned long holdResetFactoryMs = 7000;

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
const uint16_t corPreto = RGB565(0, 0, 0);
const float deltaMinimoRedraw = 0.3f;

const char CSS_WEB[] PROGMEM = R"rawliteral(
<style>
body{background:#121212;color:#d9f1e5;font-family:sans-serif;margin:0;padding:24px;}
.box{max-width:760px;margin:0 auto;border:1px solid #00f2ff;padding:24px;border-radius:8px;background:#1e1e1e;box-shadow:0 12px 30px rgba(0,0,0,.35);}
h1{font-size:72px;line-height:1;margin:12px 0;color:#00ff88;}
h2{margin:0 0 8px 0;color:#fff;}
p{margin:8px 0;color:#b7c3cd;}
a{color:#00f2ff;}
label{display:block;margin-top:14px;color:#b7c3cd;}
select,input,textarea{box-sizing:border-box;width:100%;padding:12px;margin-top:6px;border-radius:10px;border:1px solid #46535c;background:#111;color:#fff;}
textarea{min-height:96px;resize:vertical;}
form{margin:0;}
code{padding:2px 6px;border-radius:6px;background:#111822;color:#d7faff;}
.section-copy{margin-bottom:18px;}
.checkbox-row{display:flex;align-items:center;gap:10px;margin-top:16px;padding:12px 14px;border:1px solid #31424d;border-radius:10px;background:#161d22;color:#fff;}
.checkbox-row input{width:18px;height:18px;margin:0;accent-color:#00a878;flex:0 0 auto;}
.checkbox-row span{display:block;}
.btn-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:18px;}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:10px;width:100%;margin-top:18px;padding:12px 14px;border-radius:10px;border:1px solid transparent;background:linear-gradient(135deg,#00c58e,#008f72);color:#fff;font-weight:700;text-decoration:none;box-sizing:border-box;box-shadow:0 10px 24px rgba(0,168,120,.24);cursor:pointer;}
button.btn{appearance:none;-webkit-appearance:none;}
.btn:hover{filter:brightness(1.04);}
.btn.secondary{background:linear-gradient(135deg,#1e2a31,#152027);border-color:#36505d;color:#e6fbff;box-shadow:0 10px 24px rgba(0,0,0,.24);}
.btn.danger{background:linear-gradient(135deg,#d84c4c,#a62d2d);box-shadow:0 10px 24px rgba(184,50,50,.24);}
.btn.ghost{background:transparent;border-color:#36505d;color:#bdefff;box-shadow:none;}
.btn-icon{display:inline-flex;align-items:center;justify-content:center;width:18px;height:18px;flex:0 0 18px;}
.fa-icon i{font-size:16px;line-height:1;display:inline-block;}
.fa-icon .icon-fallback{display:none;font-size:16px;line-height:1;font-weight:700;}
.fa-missing .fa-icon i{display:none;}
.fa-missing .fa-icon .icon-fallback{display:inline;}
.config-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-top:18px;}
.config-card{padding:16px;border-radius:12px;background:#151515;border:1px solid #2e3a42;}
.config-card h3{margin:0 0 6px 0;color:#fff;font-size:1rem;}
.config-card p{margin:0 0 10px 0;}
.media-switcher{display:flex;flex-wrap:wrap;gap:10px;margin:18px 0 12px 0;}
.media-tab{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:10px 14px;border-radius:999px;border:1px solid #36505d;background:#151d23;color:#d8edf7;font-weight:700;cursor:pointer;}
.media-tab.active{background:linear-gradient(135deg,#00c58e,#008f72);border-color:transparent;color:#fff;box-shadow:0 10px 24px rgba(0,168,120,.24);}
.media-panel{display:none;}
.media-panel.active{display:block;}
.input-action{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:10px;align-items:end;}
.input-action-buttons{display:flex;gap:8px;align-items:center;}
.action-button{margin-top:6px;min-width:112px;padding:12px 14px;border-radius:10px;border:1px solid #36505d;background:#152027;color:#e6fbff;font-weight:700;cursor:pointer;}
.action-button.icon-only{display:inline-flex;align-items:center;justify-content:center;min-width:48px;width:48px;padding:12px;}
.action-button:disabled{opacity:.55;cursor:not-allowed;}
.status-inline{margin-top:10px;min-height:20px;font-size:.9rem;color:#9aa4ad;}
.hero{margin-bottom:20px;}
.hero-title{font-size:1rem;font-weight:700;letter-spacing:.04em;color:#fff;margin:0 0 10px 0;}
.hero-reading{font-size:3.75rem;line-height:1;margin:0;color:#00ff88;}
.hero-reading.offline{color:#9aa4ad;}
.hero-subtitle{font-size:1rem;font-weight:700;color:#fff;margin:10px 0 6px 0;}
.hero-status{font-size:1.35rem;font-weight:700;margin:0;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;margin-top:20px;}
.card{padding:16px;border-radius:8px;background:#151515;border:1px solid #2e3a42;text-align:left;}
.label{font-size:.85rem;text-transform:uppercase;letter-spacing:.08em;color:#9aa4ad;}
.value{font-size:1.8rem;margin-top:8px;color:#fff;}
.online{color:#00ff88;}
.offline{color:#ff6b6b;}
.muted{color:#9aa4ad;}
.hint{font-size:.9rem;color:#9aa4ad;}
</style>
)rawliteral";

const char FONT_AWESOME_HEAD[] PROGMEM = R"rawliteral(
<link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.1/css/all.min.css' crossorigin='anonymous' referrerpolicy='no-referrer'>
<script>
window.addEventListener('load',function(){
  setTimeout(function(){
    try{
      var probe=document.createElement('i');
      probe.className='fa-solid fa-bell';
      probe.style.position='absolute';
      probe.style.visibility='hidden';
      document.body.appendChild(probe);
      var family=(window.getComputedStyle(probe).fontFamily||'').toLowerCase();
      probe.remove();
      if(family.indexOf('font awesome')===-1){
        document.documentElement.classList.add('fa-missing');
      }
    }catch(e){
      document.documentElement.classList.add('fa-missing');
    }
  },120);
});
</script>
)rawliteral";

const char ICON_WIFI[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-wifi' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_SAVE[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-floppy-disk' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_RESET[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-rotate-left' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_BACK[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-arrow-left' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_BELL[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-bell' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_TELEGRAM[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-brands fa-telegram' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_WHATSAPP[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-brands fa-whatsapp' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_EYE[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-eye' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";
const char ICON_TEST[] PROGMEM = R"rawliteral(<span class='btn-icon fa-icon'><i class='fa-solid fa-paper-plane' aria-hidden='true'></i><span class='icon-fallback'>.</span></span>)rawliteral";

const char ROOT_STATUS_SCRIPT[] PROGMEM = R"rawliteral(
<script>
async function refreshStatus(){
  try{
    const r=await fetch('/api/temperature');
    const data=await r.json();
    const mainOnline=!!data.sensor_online;
    document.getElementById('topStatus').textContent=mainOnline?'Online':'Offline';
    document.getElementById('topStatus').className=mainOnline?'value online':'value offline';
    document.getElementById('topTemp').textContent=mainOnline?(Number(data.temperature_c).toFixed(1)+' C'):'--.- C';
    document.getElementById('topTemp').className=mainOnline?'hero-reading':'hero-reading offline';
    document.getElementById('mainCardStatus').textContent=mainOnline?'Online':'Offline';
    document.getElementById('mainCardStatus').className=mainOnline?'value online':'value offline';
    document.getElementById('mainCardTemp').textContent=mainOnline?(Number(data.temperature_c).toFixed(1)+' C'):'--.- C';
    document.getElementById('mainCardTemp').className=mainOnline?'value':'value muted';
    const dhtOnline=!!data.dht11_online;
    document.getElementById('dhtCardStatus').textContent=dhtOnline?'Online':'Offline';
    document.getElementById('dhtCardStatus').className=dhtOnline?'value online':'value offline';
    document.getElementById('dhtCardTemp').textContent=dhtOnline && data.dht11_temperature_c !== null ? (Number(data.dht11_temperature_c).toFixed(1)+' C') : '--.- C';
    document.getElementById('dhtCardTemp').className=dhtOnline?'value':'value muted';
    const humOnline=!!data.humidity_online;
    document.getElementById('humCardStatus').textContent=humOnline?'Online':'Offline';
    document.getElementById('humCardStatus').className=humOnline?'value online':'value offline';
    document.getElementById('humCardValue').textContent=humOnline && data.humidity_percent !== null ? (Math.round(Number(data.humidity_percent))+' %') : '-- %';
    document.getElementById('humCardValue').className=humOnline?'value':'value muted';
    document.getElementById('ipValue').textContent=data.ip || 'Sem WiFi';
    document.getElementById('wifiState').textContent=data.wifi_state || '--';
  }catch(e){
    ['topStatus','mainCardStatus','dhtCardStatus','humCardStatus'].forEach(function(id){var el=document.getElementById(id);if(el){el.textContent='Offline';el.className='value offline';}});
    var topTemp=document.getElementById('topTemp');if(topTemp){topTemp.textContent='--.- C';topTemp.className='hero-reading offline';}
    ['mainCardTemp','dhtCardTemp'].forEach(function(id){var el=document.getElementById(id);if(el){el.textContent='--.- C';el.className='value muted';}});
    var hum=document.getElementById('humCardValue');if(hum){hum.textContent='-- %';hum.className='value muted';}
    var ip=document.getElementById('ipValue');if(ip){ip.textContent='Sem WiFi';}
  }
}
setInterval(refreshStatus,1000);
window.addEventListener('load',refreshStatus);
</script>
)rawliteral";

const char WIFI_FORM_SCRIPT[] PROGMEM = R"rawliteral(
<script>
const ssidSelect=document.getElementById('ssid_select');
const ssidManualGroup=document.getElementById('ssid_manual_group');
const ssidManualInput=document.getElementById('ssid_manual');
function updateWifiForm(){
  const manualMode=!ssidSelect.value;
  ssidManualGroup.style.display=manualMode?'block':'none';
  ssidManualInput.disabled=!manualMode;
}
ssidSelect.addEventListener('change',updateWifiForm);
updateWifiForm();
</script>
)rawliteral";

const char TELEGRAM_CHAT_SCRIPT[] PROGMEM = R"rawliteral(
<script>
const alertasForm=document.getElementById('alertas_form');
const alertasSaveButton=document.getElementById('alertas_save');
const alertasTestButton=document.getElementById('alertas_test');
const telegramTokenInput=document.getElementById('telegram_token');
const telegramChatIdInput=document.getElementById('telegram_chat_id');
const telegramFetchButton=document.getElementById('telegram_fetch_chat_id');
const telegramOpenApiButton=document.getElementById('telegram_open_api');
const telegramFetchStatus=document.getElementById('telegram_fetch_status');

function updateTelegramFetchButton(){
  const hasToken=telegramTokenInput && telegramTokenInput.value.trim().length>0;
  telegramFetchButton.disabled=!hasToken;
  if(telegramOpenApiButton){
    telegramOpenApiButton.disabled=!hasToken;
  }
}

function buildAlertasPayload(){
  const payload=new URLSearchParams();
  const byName=function(name){return alertasForm ? alertasForm.querySelector('[name="'+name+'"]') : null;};
  const discordEnabled=byName('discord_on');
  const telegramEnabled=byName('telegram_on');
  const whatsappEnabled=byName('whatsapp_on');
  const tempAlerta=byName('temp_alerta');
  const tempOk=byName('temp_ok');
  const msgAlerta=byName('msg_alerta');
  const msgOk=byName('msg_ok');

  if(discordEnabled && discordEnabled.checked) payload.append('discord_on','on');
  if(telegramEnabled && telegramEnabled.checked) payload.append('telegram_on','on');
  if(whatsappEnabled && whatsappEnabled.checked) payload.append('whatsapp_on','on');

  payload.append('discord_url',document.getElementById('discord_url') ? document.getElementById('discord_url').value : '');
  payload.append('telegram_token',telegramTokenInput ? telegramTokenInput.value : '');
  payload.append('telegram_chat_id',telegramChatIdInput ? telegramChatIdInput.value : '');
  payload.append('whatsapp_url',document.getElementById('whatsapp_url') ? document.getElementById('whatsapp_url').value : '');
  payload.append('whatsapp_key',document.getElementById('whatsapp_key') ? document.getElementById('whatsapp_key').value : '');
  payload.append('whatsapp_destinos',document.getElementById('whatsapp_destinos') ? document.getElementById('whatsapp_destinos').value : '');
  payload.append('temp_alerta',tempAlerta ? tempAlerta.value : '');
  payload.append('temp_ok',tempOk ? tempOk.value : '');
  payload.append('msg_alerta',msgAlerta ? msgAlerta.value : '');
  payload.append('msg_ok',msgOk ? msgOk.value : '');
  return payload;
}

async function submitAlertas(action){
  const payload=buildAlertasPayload();
  if(alertasSaveButton) alertasSaveButton.disabled=true;
  if(alertasTestButton) alertasTestButton.disabled=true;
  try{
    const response=await fetch(action,{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},
      body:payload.toString()
    });
    const text=await response.text();
    if(response.redirected){
      window.location.href=response.url;
      return;
    }
    if(response.ok){
      window.location.href='/alertas?ts='+Date.now();
      return;
    }
    document.open();
    document.write(text);
    document.close();
  }catch(e){
    telegramFetchStatus.textContent='Falha ao enviar configuracao.';
  }finally{
    if(alertasSaveButton) alertasSaveButton.disabled=false;
    if(alertasTestButton) alertasTestButton.disabled=false;
  }
}

function openTelegramApi(){
  const token=telegramTokenInput.value.trim();
  if(!token){
    updateTelegramFetchButton();
    return;
  }
  window.open('https://api.telegram.org/bot'+encodeURIComponent(token)+'/getUpdates','_blank','noopener');
}

async function fetchTelegramChatId(){
  const token=telegramTokenInput.value.trim();
  if(!token){
    updateTelegramFetchButton();
    return;
  }

  telegramFetchButton.disabled=true;
  telegramFetchStatus.textContent='Aguardando nova mensagem para o bot...';

  try{
    const body='telegram_token='+encodeURIComponent(token);
    const response=await fetch('/telegram/chatid',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});
    const data=await response.json();

    if(response.ok && data.ok){
      telegramChatIdInput.value=data.chat_id || '';
      telegramFetchStatus.textContent='Chat ID obtido com sucesso.';
    }else{
      telegramFetchStatus.textContent=data.error || 'Nao foi possivel obter o chat ID.';
    }
  }catch(e){
    telegramFetchStatus.textContent='Falha na consulta do Telegram.';
  }finally{
    updateTelegramFetchButton();
  }
}

if(telegramTokenInput && telegramFetchButton){
  telegramTokenInput.addEventListener('input',updateTelegramFetchButton);
  telegramFetchButton.addEventListener('click',fetchTelegramChatId);
  if(telegramOpenApiButton){
    telegramOpenApiButton.addEventListener('click',openTelegramApi);
  }
  updateTelegramFetchButton();
}

if(alertasSaveButton){
  alertasSaveButton.addEventListener('click',function(){
    submitAlertas('/saveAlertas');
  });
}

if(alertasTestButton){
  alertasTestButton.addEventListener('click',function(){
    submitAlertas('/testAlertas');
  });
}

const mediaTabs=document.querySelectorAll('.media-tab');
const mediaPanels=document.querySelectorAll('.media-panel');

function activateMediaPanel(target){
  mediaTabs.forEach(function(tab){
    tab.classList.toggle('active',tab.dataset.target===target);
  });
  mediaPanels.forEach(function(panel){
    panel.classList.toggle('active',panel.id===target);
  });
}

mediaTabs.forEach(function(tab){
  tab.addEventListener('click',function(){
    activateMediaPanel(tab.dataset.target);
  });
});

activateMediaPanel('panel-discord');
</script>
)rawliteral";

enum FaixaTemperatura {
  FaixaNormal,
  FaixaAlerta,
  FaixaCritica
};

enum TelaAtual {
  TelaTemperatura,
  TelaSensores,
  TelaIp,
  TelaResetFactory
};

enum EstadoWifi {
  WifiNaoConfigurado,
  WifiConectando,
  WifiConectado,
  WifiDesconectado,
  WifiModoConfiguracao
};

enum PendenciaAlerta {
  PendenciaNenhuma,
  PendenciaTipoAlerta,
  PendenciaResolvido
};

enum EtapaTesteAlertas {
  TesteEtapaNenhuma,
  TesteEtapaDiscord,
  TesteEtapaTelegram,
  TesteEtapaWhatsapp,
  TesteEtapaFinalizar
};

enum CanalEnvioAlerta {
  CanalEnvioNenhum,
  CanalEnvioDiscord,
  CanalEnvioTelegram,
  CanalEnvioWhatsapp
};

float temperaturaExibida = NAN;
float umidadeExibida = NAN;
float temperaturaDhtExibida = NAN;
FaixaTemperatura faixaAtual = FaixaNormal;
TelaAtual telaAtual = TelaTemperatura;
EstadoWifi estadoWifi = WifiNaoConfigurado;
String ipExibido = "";
int ultimoClkEstado = HIGH;
unsigned long ultimoGiroMs = 0;
const unsigned long debounceGiroMs = 80;
unsigned long ultimoEnvioDiscordMs = 0;
unsigned long ultimoEnvioTelegramMs = 0;
unsigned long ultimoEnvioWhatsappMs = 0;
PendenciaAlerta pendenciaDiscord = PendenciaNenhuma;
PendenciaAlerta pendenciaTelegram = PendenciaNenhuma;
PendenciaAlerta pendenciaWhatsapp = PendenciaNenhuma;
bool testeAlertasEmAndamento = false;
String resultadoTesteAlertas = "Nenhum teste executado.";
String detalheTesteDiscord = "desativado";
String detalheTesteTelegram = "desativado";
String detalheTesteWhatsapp = "desativado";
bool testeDiscordOk = false;
bool testeTelegramOk = false;
bool testeWhatsappOk = false;
TaskHandle_t tarefaTesteAlertasHandle = nullptr;
SemaphoreHandle_t mutexTesteAlertas = nullptr;
SemaphoreHandle_t mutexEnvioAlertas = nullptr;
EtapaTesteAlertas etapaTesteAlertas = TesteEtapaNenhuma;
String mensagemTesteAlertas = "";
TaskHandle_t tarefaEnvioCanalHandle = nullptr;
CanalEnvioAlerta canalEnvioAtual = CanalEnvioNenhum;
PendenciaAlerta pendenciaEnvioAtual = PendenciaNenhuma;
bool envioAtualEhTeste = false;
bool envioCanalEmAndamento = false;
bool envioCanalConcluido = false;
bool envioCanalOk = false;
String detalheEnvioCanal = "";

void drawTextoCentralizado(const String& texto, int y, uint8_t tamanho, uint16_t cor);
void drawTelaIp();
void drawTelaResetFactory();
void drawStatusResetFactory(float progresso, bool emAndamento);
void atualizarProgressoResetFactory();
void resetarConfiguracaoDeFabrica();
void drawCartaoArredondado(int x, int y, int largura, int altura, int raio, uint16_t corFundoCartao);
void sendWebMessage(int httpCode, const String& mensagem, const String& linkVoltar = "");
void atualizarEstadoTesteAlertas(const String& mensagem);
bool iniciarEnvioAlerta(String* detalhe);
void finalizarEnvioAlerta();
bool iniciarEnvioCanal(CanalEnvioAlerta canal, const String& mensagem, bool ehTeste, PendenciaAlerta pendencia);
void processarConclusaoEnvioCanal();
void tarefaEnvioCanal(void* parameter);
void processarTesteAlertasPendente();
void handleWifiConfig();
void handleTelegramChatId();
void handleCaptivePortal();
void handleNotFound();
void limparPendenciasAlerta();
void processarFilaAlertas();
void executarTesteAlertasTask(void* parameter);

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

void atualizarEstadoTesteAlertas(const String& mensagem) {
  if (mutexTesteAlertas != nullptr && xSemaphoreTake(mutexTesteAlertas, pdMS_TO_TICKS(100)) == pdTRUE) {
    resultadoTesteAlertas = mensagem;
    xSemaphoreGive(mutexTesteAlertas);
  } else {
    resultadoTesteAlertas = mensagem;
  }
}

bool iniciarEnvioAlerta(String* detalhe) {
  (void)detalhe;
  return true;
}

void finalizarEnvioAlerta() {
}

bool iniciarEnvioCanal(CanalEnvioAlerta canal, const String& mensagem, bool ehTeste, PendenciaAlerta pendencia) {
  if (envioCanalEmAndamento) {
    return false;
  }

  canalEnvioAtual = canal;
  pendenciaEnvioAtual = pendencia;
  envioAtualEhTeste = ehTeste;
  mensagemTesteAlertas = mensagem;
  detalheEnvioCanal = "";
  envioCanalOk = false;
  envioCanalConcluido = false;
  envioCanalEmAndamento = true;

  BaseType_t criada = xTaskCreatePinnedToCore(tarefaEnvioCanal, "EnvioCanal", 24576, nullptr, 1, &tarefaEnvioCanalHandle, 1);
  if (criada != pdPASS) {
    envioCanalEmAndamento = false;
    canalEnvioAtual = CanalEnvioNenhum;
    pendenciaEnvioAtual = PendenciaNenhuma;
    envioAtualEhTeste = false;
    detalheEnvioCanal = "falha ao iniciar task de envio";
    return false;
  }

  return true;
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

String escapeHtml(const String& input) {
  String output;
  output.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '&') {
      output += "&amp;";
    } else if (c == '<') {
      output += "&lt;";
    } else if (c == '>') {
      output += "&gt;";
    } else if (c == '"') {
      output += "&quot;";
    } else if (c == '\'') {
      output += "&#39;";
    } else {
      output += c;
    }
  }

  return output;
}

String ipAtualWifi() {
  if (WiFi.isConnected()) {
    return WiFi.localIP().toString();
  }
  if (portalConfiguracaoAtivo) {
    return WiFi.softAPIP().toString();
  }
  return "Sem WiFi";
}

const char* textoEstadoWifi() {
  if (estadoWifi == WifiNaoConfigurado) return "Nao configurado";
  if (estadoWifi == WifiConectando) return "Conectando";
  if (estadoWifi == WifiConectado) return "Conectado";
  if (estadoWifi == WifiDesconectado) return "Desconectado";
  return "Configuracao";
}

void carregarConfigWifi() {
  prefs.begin("wifi", true);
  wifiSsidSalvo = prefs.getString("ssid", "");
  wifiSenhaSalva = prefs.getString("pass", "");
  prefs.end();

  wifiSsidSalvo.trim();
  wifiTemConfig = wifiSsidSalvo.length() > 0;
}

void salvarConfigWifi(const String& ssidWifi, const String& senhaWifi) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssidWifi);
  prefs.putString("pass", senhaWifi);
  prefs.end();

  wifiSsidSalvo = ssidWifi;
  wifiSenhaSalva = senhaWifi;
  wifiTemConfig = wifiSsidSalvo.length() > 0;
}

void limparConfigWifi() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  wifiSsidSalvo = "";
  wifiSenhaSalva = "";
  wifiTemConfig = false;
}

void restaurarDefaultsAlertasEmMemoria() {
  discordWebhookUrl = "";
  alertaDiscordHabilitado = false;
  telegramBotToken = "";
  telegramChatId = "";
  alertaTelegramHabilitado = false;
  whatsappApiUrl = "";
  whatsappApiKey = "";
  whatsappDestinos = "";
  alertaWhatsappHabilitado = false;
  alertaDiscordDisparado = false;
  alertaTelegramDisparado = false;
  alertaWhatsappDisparado = false;
  temperaturaAlertaDiscord = 28.0f;
  temperaturaResolvidoDiscord = 27.5f;
  mensagemAlertaDiscord = "Alerta de temperatura: {{temp}} C°";
  mensagemResolvidoDiscord = "Temperatura normalizada: {{temp}} C° ";
}

void limparConfigAlertas() {
  prefs.begin("alertas", false);
  prefs.clear();
  prefs.end();
  restaurarDefaultsAlertasEmMemoria();
}

void resetarConfiguracaoDeFabrica() {
  limparConfigWifi();
  limparConfigAlertas();
  WiFi.disconnect(true, true);
  dnsServer.stop();
  tft.fillScreen(corFundo);
  drawTextoCentralizado("Reset concluido", 96, 2, corVermelho);
  drawTextoCentralizado("Reiniciando...", 126, 2, corInfo);
  delay(1000);
  ESP.restart();
}

String gerarNomeApWifi() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String sufixo = mac.length() >= 4 ? mac.substring(mac.length() - 4) : "0000";
  return "SensorTemp-" + sufixo;
}

void conectarWifiSalvo() {
  if (!wifiTemConfig) {
    estadoWifi = WifiNaoConfigurado;
    return;
  }

  portalConfiguracaoAtivo = false;
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.begin(wifiSsidSalvo.c_str(), wifiSenhaSalva.c_str());
  estadoWifi = WifiConectando;
  wifiTentativaInicioMs = millis();
  ultimoWifiRetryMs = millis();
}

void iniciarPortalConfiguracaoWifi() {
  portalConfiguracaoAtivo = true;
  estadoWifi = WifiModoConfiguracao;
  wifiApSsid = gerarNomeApWifi();

  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  WiFi.softAP(wifiApSsid.c_str());
  dnsServer.start(dnsPort, "*", WiFi.softAPIP());
}

void iniciarWifi() {
  carregarConfigWifi();
  if (!wifiTemConfig) {
    iniciarPortalConfiguracaoWifi();
    return;
  }

  conectarWifiSalvo();
}

bool resetWifiSolicitadoNoBoot() {
  if (digitalRead(ENC_SW) == HIGH) {
    return false;
  }

  drawTextoCentralizado("Reset WiFi", 94, 2, corInfo);
  drawTextoCentralizado("segure...", 124, 2, corInfoSuave);

  unsigned long inicio = millis();
  while (millis() - inicio < 4000) {
    if (digitalRead(ENC_SW) == HIGH) {
      return false;
    }
    delay(20);
  }

  return true;
}

void atualizarWifi() {
  if (portalConfiguracaoAtivo) {
    dnsServer.processNextRequest();
    return;
  }

  if (!wifiTemConfig) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (estadoWifi != WifiConectado) {
      estadoWifi = WifiConectado;
      if (telaAtual == TelaIp) {
        drawTelaIp();
      }
    }
    return;
  }

  if (estadoWifi == WifiConectando && millis() - wifiTentativaInicioMs >= wifiTempoConexaoMs) {
    estadoWifi = WifiDesconectado;
    ultimoWifiRetryMs = millis();
    if (telaAtual == TelaIp) {
      drawTelaIp();
    }
  }

  if (estadoWifi == WifiConectado) {
    estadoWifi = WifiDesconectado;
    ultimoWifiRetryMs = millis();
    if (telaAtual == TelaIp) {
      drawTelaIp();
    }
  }

  if (estadoWifi == WifiDesconectado && millis() - ultimoWifiRetryMs >= wifiRetryMs) {
    WiFi.disconnect(false, false);
    WiFi.begin(wifiSsidSalvo.c_str(), wifiSenhaSalva.c_str());
    estadoWifi = WifiConectando;
    wifiTentativaInicioMs = millis();
    ultimoWifiRetryMs = millis();
  }
}

void carregarConfigAlertas() {
  prefs.begin("alertas", true);
  discordWebhookUrl = prefs.getString("discord_url", "");
  alertaDiscordHabilitado = prefs.getBool("discord_on", false);
  telegramBotToken = prefs.getString("telegram_token", "");
  telegramChatId = prefs.getString("tg_chat_id", "");
  alertaTelegramHabilitado = prefs.getBool("telegram_on", false);
  whatsappApiUrl = prefs.getString("whatsapp_url", "");
  whatsappApiKey = prefs.getString("whatsapp_key", "");
  whatsappDestinos = prefs.getString("wa_destinos", "");
  alertaWhatsappHabilitado = prefs.getBool("whatsapp_on", false);
  temperaturaAlertaDiscord = prefs.getFloat("temp_alerta", 28.0f);
  temperaturaResolvidoDiscord = prefs.getFloat("temp_ok", 27.5f);
  mensagemAlertaDiscord = prefs.getString("msg_alerta", "Alerta de temperatura: {{temp}} C° ");
  mensagemResolvidoDiscord = prefs.getString("msg_ok", "Temperatura normalizada: {{temp}}C° ");
  prefs.end();
}

void salvarConfigAlertas() {
  prefs.begin("alertas", false);
  prefs.putString("discord_url", discordWebhookUrl);
  prefs.putBool("discord_on", alertaDiscordHabilitado);
  prefs.putString("telegram_token", telegramBotToken);
  prefs.putString("tg_chat_id", telegramChatId);
  prefs.putBool("telegram_on", alertaTelegramHabilitado);
  prefs.putString("whatsapp_url", whatsappApiUrl);
  prefs.putString("whatsapp_key", whatsappApiKey);
  prefs.putString("wa_destinos", whatsappDestinos);
  prefs.putBool("whatsapp_on", alertaWhatsappHabilitado);
  prefs.putFloat("temp_alerta", temperaturaAlertaDiscord);
  prefs.putFloat("temp_ok", temperaturaResolvidoDiscord);
  prefs.putString("msg_alerta", mensagemAlertaDiscord);
  prefs.putString("msg_ok", mensagemResolvidoDiscord);
  prefs.end();
}

String renderMensagemAlerta(const String& modelo, float leitura) {
  String ipAtual = ipAtualWifi();
  String mensagem = modelo;
  mensagem.replace("{{temp}}", String(leitura, 1));
  mensagem.replace("{{ip}}", ipAtual);
  return mensagem;
}

bool extrairUltimoTelegramUpdate(const String& resposta, long long& updateId, String& chatId) {
  int posUpdate = resposta.lastIndexOf("\"update_id\":");
  if (posUpdate < 0) {
    return false;
  }

  int inicioNumero = posUpdate + 12;
  while (inicioNumero < static_cast<int>(resposta.length()) && (resposta[inicioNumero] == ' ' || resposta[inicioNumero] == '\t')) {
    inicioNumero++;
  }

  int fimNumero = inicioNumero;
  while (fimNumero < static_cast<int>(resposta.length()) && isdigit(static_cast<unsigned char>(resposta[fimNumero]))) {
    fimNumero++;
  }
  if (fimNumero == inicioNumero) {
    return false;
  }

  String updateIdTexto = resposta.substring(inicioNumero, fimNumero);
  updateId = atoll(updateIdTexto.c_str());

  int posChat = resposta.indexOf("\"chat\":", fimNumero);
  if (posChat < 0) {
    return false;
  }

  int posChatId = resposta.indexOf("\"id\":", posChat);
  if (posChatId < 0) {
    return false;
  }

  int inicioChatId = posChatId + 5;
  while (inicioChatId < static_cast<int>(resposta.length()) && (resposta[inicioChatId] == ' ' || resposta[inicioChatId] == '\t')) {
    inicioChatId++;
  }

  int fimChatId = inicioChatId;
  if (fimChatId < static_cast<int>(resposta.length()) && resposta[fimChatId] == '-') {
    fimChatId++;
  }
  while (fimChatId < static_cast<int>(resposta.length()) && isdigit(static_cast<unsigned char>(resposta[fimChatId]))) {
    fimChatId++;
  }
  if (fimChatId == inicioChatId) {
    return false;
  }

  chatId = resposta.substring(inicioChatId, fimChatId);
  return chatId.length() > 0;
}

bool obterTelegramChatIdDoBot(const String& token, String& chatId, String& erro) {
  if (token.length() == 0) {
    erro = "Informe o token do bot.";
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    erro = "WiFi desconectado.";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String baseUrl = "https://api.telegram.org/bot" + token + "/getUpdates";

  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, baseUrl + "?limit=1")) {
    erro = "Falha ao abrir a API do Telegram.";
    return false;
  }

  int httpCode = http.GET();
  String respostaInicial = httpCode > 0 ? http.getString() : "";
  http.end();

  if (httpCode <= 0) {
    erro = "Falha ao consultar updates do Telegram.";
    return false;
  }
  if (httpCode >= 300) {
    erro = "Telegram recusou o token informado.";
    return false;
  }

  long long ultimoUpdateId = -1;
  String chatIdIgnorado;
  extrairUltimoTelegramUpdate(respostaInicial, ultimoUpdateId, chatIdIgnorado);

  String urlAguardando = baseUrl + "?timeout=20&allowed_updates=%5B%22message%22,%22channel_post%22%5D";
  if (ultimoUpdateId >= 0) {
    urlAguardando += "&offset=" + String(ultimoUpdateId + 1);
  }

  http.setConnectTimeout(5000);
  http.setTimeout(25000);
  if (!http.begin(client, urlAguardando)) {
    erro = "Falha ao iniciar a espera por mensagem.";
    return false;
  }

  httpCode = http.GET();
  String resposta = httpCode > 0 ? http.getString() : "";
  http.end();

  if (httpCode <= 0) {
    erro = "Falha ao aguardar nova mensagem.";
    return false;
  }
  if (httpCode >= 300) {
    erro = "Telegram nao aceitou a consulta do bot.";
    return false;
  }

  long long updateId = -1;
  if (!extrairUltimoTelegramUpdate(resposta, updateId, chatId)) {
    erro = "Nenhuma nova mensagem recebida. Clique em Obter e envie uma mensagem ao bot.";
    return false;
  }

  return true;
}

String extrairDigitos(const String& valor) {
  String digitos = "";
  digitos.reserve(valor.length());
  for (size_t i = 0; i < valor.length(); i++) {
    char c = valor[i];
    if (isdigit(static_cast<unsigned char>(c))) {
      digitos += c;
    }
  }
  return digitos;
}

bool normalizarDestinoWhatsapp(const String& origem, String& destino) {
  String valor = origem;
  valor.trim();
  if (valor.length() == 0) {
    return false;
  }

  if (valor.indexOf('@') >= 0) {
    destino = valor;
    return true;
  }

  String digitos = extrairDigitos(valor);
  if (digitos.length() == 0) {
    return false;
  }

  if (digitos.length() >= 16) {
    destino = digitos + "@g.us";
    return true;
  }

  if (digitos.length() == 10 || digitos.length() == 11) {
    destino = "55" + digitos + "@c.us";
    return true;
  }

  if (digitos.length() >= 12 && digitos.length() <= 15) {
    destino = digitos + "@c.us";
    return true;
  }

  return false;
}

bool parseDestinosWhatsapp(const String& texto, String destinos[], size_t capacidade, size_t& quantidade) {
  quantidade = 0;
  String atual = "";

  for (size_t i = 0; i < texto.length(); i++) {
    char c = texto[i];
    if (c == ',' || c == ';' || c == '\n' || c == '\r') {
      atual.trim();
      if (atual.length() > 0) {
        if (quantidade >= capacidade) {
          return false;
        }
        String normalizado;
        if (!normalizarDestinoWhatsapp(atual, normalizado)) {
          return false;
        }
        destinos[quantidade++] = normalizado;
      }
      atual = "";
    } else {
      atual += c;
    }
  }

  atual.trim();
  if (atual.length() > 0) {
    if (quantidade >= capacidade) {
      return false;
    }
    String normalizado;
    if (!normalizarDestinoWhatsapp(atual, normalizado)) {
      return false;
    }
    destinos[quantidade++] = normalizado;
  }

  return quantidade > 0;
}

String resumirHttpResposta(int httpCode, const String& corpo) {
  String detalhe = "HTTP " + String(httpCode);
  String resumo = corpo;
  resumo.trim();
  if (resumo.length() > 80) {
    resumo = resumo.substring(0, 80) + "...";
  }
  if (resumo.length() > 0) {
    detalhe += " - " + resumo;
  }
  return detalhe;
}

bool enviarWebhookDiscord(const String& mensagem, String* detalhe) {
  if (!iniciarEnvioAlerta(detalhe)) {
    return false;
  }
  if (!alertaDiscordHabilitado) {
    if (detalhe != nullptr) *detalhe = "desativado";
    finalizarEnvioAlerta();
    return false;
  }
  if (discordWebhookUrl.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "webhook vazio";
    finalizarEnvioAlerta();
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (detalhe != nullptr) *detalhe = "WiFi desconectado";
    finalizarEnvioAlerta();
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, discordWebhookUrl)) {
    if (detalhe != nullptr) *detalhe = "falha ao abrir webhook";
    finalizarEnvioAlerta();
    return false;
  }

  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"content\":\"" + escapeJson(mensagem) + "\"}";
  int httpCode = http.POST(payload);
  String corpo = (httpCode >= 300) ? http.getString() : "";
  http.end();

  bool ok = httpCode > 0 && httpCode < 300;
  if (detalhe != nullptr) {
    *detalhe = ok ? "ok" : (httpCode <= 0 ? "erro de conexao" : resumirHttpResposta(httpCode, corpo));
  }
  finalizarEnvioAlerta();
  return ok;
}

bool enviarWebhookDiscord(const String& mensagem) {
  return enviarWebhookDiscord(mensagem, nullptr);
}

bool enviarMensagemTelegram(const String& mensagem, String* detalhe) {
  if (!iniciarEnvioAlerta(detalhe)) {
    return false;
  }
  if (!alertaTelegramHabilitado) {
    if (detalhe != nullptr) *detalhe = "desativado";
    finalizarEnvioAlerta();
    return false;
  }
  if (telegramBotToken.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "token vazio";
    finalizarEnvioAlerta();
    return false;
  }
  if (telegramChatId.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "chat ID vazio";
    finalizarEnvioAlerta();
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (detalhe != nullptr) *detalhe = "WiFi desconectado";
    finalizarEnvioAlerta();
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + telegramBotToken + "/sendMessage";
  if (!http.begin(client, url)) {
    if (detalhe != nullptr) *detalhe = "falha ao abrir API";
    finalizarEnvioAlerta();
    return false;
  }

  http.setConnectTimeout(4000);
  http.setTimeout(6000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"chat_id\":\"" + escapeJson(telegramChatId) + "\",\"text\":\"" + escapeJson(mensagem) + "\",\"disable_web_page_preview\":true}";
  int httpCode = http.POST(payload);
  String corpo = (httpCode >= 300) ? http.getString() : "";
  http.end();

  bool ok = httpCode > 0 && httpCode < 300;
  if (detalhe != nullptr) {
    *detalhe = ok ? "ok" : (httpCode <= 0 ? "erro de conexao" : resumirHttpResposta(httpCode, corpo));
  }
  finalizarEnvioAlerta();
  return ok;
}

bool enviarMensagemTelegram(const String& mensagem) {
  return enviarMensagemTelegram(mensagem, nullptr);
}

bool enviarMensagemWhatsapp(const String& mensagem, String* detalhe) {
  if (!iniciarEnvioAlerta(detalhe)) {
    return false;
  }
  if (!alertaWhatsappHabilitado) {
    if (detalhe != nullptr) *detalhe = "desativado";
    finalizarEnvioAlerta();
    return false;
  }
  if (whatsappApiUrl.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "URL da API vazia";
    finalizarEnvioAlerta();
    return false;
  }
  if (whatsappApiKey.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "chave API vazia";
    finalizarEnvioAlerta();
    return false;
  }
  if (whatsappDestinos.isEmpty()) {
    if (detalhe != nullptr) *detalhe = "sem destinos";
    finalizarEnvioAlerta();
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (detalhe != nullptr) *detalhe = "WiFi desconectado";
    finalizarEnvioAlerta();
    return false;
  }

  String destinos[12];
  size_t quantidade = 0;
  if (!parseDestinosWhatsapp(whatsappDestinos, destinos, 12, quantidade)) {
    if (detalhe != nullptr) *detalhe = "lista de destinos invalida ou acima do limite de 12";
    finalizarEnvioAlerta();
    return false;
  }

  bool algumOk = false;
  String resumo = "";
  for (size_t i = 0; i < quantidade; i++) {
    WiFiClient client;
    WiFiClientSecure secureClient;
    HTTPClient http;
    bool usaHttps = whatsappApiUrl.startsWith("https://");
    if (usaHttps) {
      secureClient.setInsecure();
    }

    bool abriuApi = usaHttps ? http.begin(secureClient, whatsappApiUrl) : http.begin(client, whatsappApiUrl);
    if (!abriuApi) {
      if (resumo.length() > 0) resumo += " | ";
      resumo += destinos[i] + ": falha ao abrir API";
      continue;
    }

    http.setConnectTimeout(3000);
    http.setTimeout(6000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", whatsappApiKey);
    String payload = "{\"chatId\":\"" + escapeJson(destinos[i]) + "\",\"text\":\"" + escapeJson(mensagem) + "\"}";
    int httpCode = http.POST(payload);
    String corpo = (httpCode >= 300) ? http.getString() : "";
    http.end();

    if (resumo.length() > 0) resumo += " | ";
    if (httpCode > 0 && httpCode < 300) {
      algumOk = true;
      resumo += destinos[i] + ": ok";
    } else if (httpCode <= 0) {
      resumo += destinos[i] + ": erro de conexao";
    } else {
      resumo += destinos[i] + ": " + resumirHttpResposta(httpCode, corpo);
    }
  }

  if (detalhe != nullptr) {
    *detalhe = resumo.length() > 0 ? resumo : "sem resposta";
  }
  finalizarEnvioAlerta();
  return algumOk;
}

bool enviarMensagemWhatsapp(const String& mensagem) {
  return enviarMensagemWhatsapp(mensagem, nullptr);
}

void limparPendenciasAlerta() {
  pendenciaDiscord = PendenciaNenhuma;
  pendenciaTelegram = PendenciaNenhuma;
  pendenciaWhatsapp = PendenciaNenhuma;
}

String montarResumoTesteAlertas() {
  String mensagem = (testeDiscordOk || testeTelegramOk || testeWhatsappOk) ? "Resultado do teste de alertas:\n" : "Falha ao enviar teste de alertas:\n";
  mensagem += "Discord: ";
  mensagem += alertaDiscordHabilitado ? (testeDiscordOk ? "ok" : "falhou") : "desativado";
  mensagem += " - ";
  mensagem += detalheTesteDiscord;
  mensagem += "\nTelegram: ";
  mensagem += alertaTelegramHabilitado ? (testeTelegramOk ? "ok" : "falhou") : "desativado";
  mensagem += " - ";
  mensagem += detalheTesteTelegram;
  mensagem += "\nWhatsApp: ";
  mensagem += alertaWhatsappHabilitado ? (testeWhatsappOk ? "ok" : "falhou") : "desativado";
  mensagem += " - ";
  mensagem += detalheTesteWhatsapp;
  return mensagem;
}

void executarTesteAlertasTask(void* parameter) {
  bool discordOk = false;
  bool telegramOk = false;
  bool whatsappOk = false;
  String discordDetalhe = "desativado";
  String telegramDetalhe = "desativado";
  String whatsappDetalhe = "desativado";

  carregarConfigAlertas();
  String mensagemTeste = renderMensagemAlerta(mensagemAlertaDiscord, tempAtual);

  if (alertaDiscordHabilitado) {
    atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando Discord...");
    discordOk = enviarWebhookDiscord(mensagemTeste, &discordDetalhe);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  if (alertaTelegramHabilitado) {
    atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando Telegram...");
    telegramOk = enviarMensagemTelegram(mensagemTeste, &telegramDetalhe);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  if (alertaWhatsappHabilitado) {
    atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando WhatsApp...");
    whatsappOk = enviarMensagemWhatsapp(mensagemTeste, &whatsappDetalhe);
  }

  if (mutexTesteAlertas != nullptr && xSemaphoreTake(mutexTesteAlertas, pdMS_TO_TICKS(100)) == pdTRUE) {
    testeDiscordOk = discordOk;
    testeTelegramOk = telegramOk;
    testeWhatsappOk = whatsappOk;
    detalheTesteDiscord = discordDetalhe;
    detalheTesteTelegram = telegramDetalhe;
    detalheTesteWhatsapp = whatsappDetalhe;
    resultadoTesteAlertas = montarResumoTesteAlertas();
    testeAlertasEmAndamento = false;
    tarefaTesteAlertasHandle = nullptr;
    xSemaphoreGive(mutexTesteAlertas);
  } else {
    testeDiscordOk = discordOk;
    testeTelegramOk = telegramOk;
    testeWhatsappOk = whatsappOk;
    detalheTesteDiscord = discordDetalhe;
    detalheTesteTelegram = telegramDetalhe;
    detalheTesteWhatsapp = whatsappDetalhe;
    resultadoTesteAlertas = montarResumoTesteAlertas();
    testeAlertasEmAndamento = false;
    tarefaTesteAlertasHandle = nullptr;
  }

  vTaskDelete(nullptr);
}

void tarefaEnvioCanal(void* parameter) {
  String detalhe = "";
  bool ok = false;
  String mensagem = mensagemTesteAlertas;
  CanalEnvioAlerta canal = canalEnvioAtual;

  if (canal == CanalEnvioDiscord) {
    ok = enviarWebhookDiscord(mensagem, &detalhe);
  } else if (canal == CanalEnvioTelegram) {
    ok = enviarMensagemTelegram(mensagem, &detalhe);
  } else if (canal == CanalEnvioWhatsapp) {
    ok = enviarMensagemWhatsapp(mensagem, &detalhe);
  } else {
    detalhe = "canal invalido";
  }

  envioCanalOk = ok;
  detalheEnvioCanal = detalhe;
  envioCanalConcluido = true;
  envioCanalEmAndamento = false;
  tarefaEnvioCanalHandle = nullptr;
  vTaskDelete(nullptr);
}

void processarConclusaoEnvioCanal() {
  if (!envioCanalConcluido) {
    return;
  }

  CanalEnvioAlerta canal = canalEnvioAtual;
  bool ok = envioCanalOk;
  String detalhe = detalheEnvioCanal;
  bool ehTeste = envioAtualEhTeste;
  PendenciaAlerta pendencia = pendenciaEnvioAtual;

  envioCanalConcluido = false;
  canalEnvioAtual = CanalEnvioNenhum;
  pendenciaEnvioAtual = PendenciaNenhuma;
  envioAtualEhTeste = false;

  if (ehTeste) {
    if (canal == CanalEnvioDiscord) {
      testeDiscordOk = ok;
      detalheTesteDiscord = detalhe;
      etapaTesteAlertas = TesteEtapaTelegram;
    } else if (canal == CanalEnvioTelegram) {
      testeTelegramOk = ok;
      detalheTesteTelegram = detalhe;
      etapaTesteAlertas = TesteEtapaWhatsapp;
    } else if (canal == CanalEnvioWhatsapp) {
      testeWhatsappOk = ok;
      detalheTesteWhatsapp = detalhe;
      etapaTesteAlertas = TesteEtapaFinalizar;
    }
    return;
  }

  if (canal == CanalEnvioDiscord) {
    if (ok) {
      alertaDiscordDisparado = pendencia == PendenciaTipoAlerta;
      pendenciaDiscord = PendenciaNenhuma;
    }
    return;
  }

  if (canal == CanalEnvioTelegram) {
    if (ok) {
      alertaTelegramDisparado = pendencia == PendenciaTipoAlerta;
      pendenciaTelegram = PendenciaNenhuma;
    }
    return;
  }

  if (canal == CanalEnvioWhatsapp && ok) {
    alertaWhatsappDisparado = pendencia == PendenciaTipoAlerta;
    pendenciaWhatsapp = PendenciaNenhuma;
  }
}

void processarTesteAlertasPendente() {
  if (!testeAlertasEmAndamento) {
    return;
  }

  if (envioCanalEmAndamento || envioCanalConcluido) {
    return;
  }

  switch (etapaTesteAlertas) {
    case TesteEtapaDiscord:
      if (alertaDiscordHabilitado) {
        atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando Discord...");
        if (!iniciarEnvioCanal(CanalEnvioDiscord, mensagemTesteAlertas, true, PendenciaNenhuma)) {
          detalheTesteDiscord = "falha ao iniciar envio Discord";
          etapaTesteAlertas = TesteEtapaTelegram;
        }
        return;
      }
      etapaTesteAlertas = TesteEtapaTelegram;
      return;

    case TesteEtapaTelegram:
      if (alertaTelegramHabilitado) {
        atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando Telegram...");
        if (!iniciarEnvioCanal(CanalEnvioTelegram, mensagemTesteAlertas, true, PendenciaNenhuma)) {
          detalheTesteTelegram = "falha ao iniciar envio Telegram";
          etapaTesteAlertas = TesteEtapaWhatsapp;
        }
        return;
      }
      etapaTesteAlertas = TesteEtapaWhatsapp;
      return;

    case TesteEtapaWhatsapp:
      if (alertaWhatsappHabilitado) {
        atualizarEstadoTesteAlertas("Teste em andamento...\nEnviando WhatsApp...");
        if (!iniciarEnvioCanal(CanalEnvioWhatsapp, mensagemTesteAlertas, true, PendenciaNenhuma)) {
          detalheTesteWhatsapp = "falha ao iniciar envio WhatsApp";
          etapaTesteAlertas = TesteEtapaFinalizar;
        }
        return;
      }
      etapaTesteAlertas = TesteEtapaFinalizar;
      return;

    case TesteEtapaFinalizar:
      resultadoTesteAlertas = montarResumoTesteAlertas();
      testeAlertasEmAndamento = false;
      tarefaTesteAlertasHandle = nullptr;
      etapaTesteAlertas = TesteEtapaNenhuma;
      return;

    case TesteEtapaNenhuma:
    default:
      resultadoTesteAlertas = montarResumoTesteAlertas();
      testeAlertasEmAndamento = false;
      tarefaTesteAlertasHandle = nullptr;
      etapaTesteAlertas = TesteEtapaNenhuma;
      return;
  }
}

void processarAlertaTemperatura(float leitura) {
  if (!sensorOnline) return;

  if (leitura >= temperaturaAlertaDiscord) {
    if (alertaDiscordHabilitado && !alertaDiscordDisparado) {
      pendenciaDiscord = PendenciaTipoAlerta;
    }
    if (alertaTelegramHabilitado && !alertaTelegramDisparado) {
      pendenciaTelegram = PendenciaTipoAlerta;
    }
    if (alertaWhatsappHabilitado && !alertaWhatsappDisparado) {
      pendenciaWhatsapp = PendenciaTipoAlerta;
    }
  } else if (leitura <= temperaturaResolvidoDiscord) {
    if (alertaDiscordHabilitado && alertaDiscordDisparado) {
      pendenciaDiscord = PendenciaResolvido;
    } else if (!alertaDiscordDisparado) {
      pendenciaDiscord = PendenciaNenhuma;
    }
    if (alertaTelegramHabilitado && alertaTelegramDisparado) {
      pendenciaTelegram = PendenciaResolvido;
    } else if (!alertaTelegramDisparado) {
      pendenciaTelegram = PendenciaNenhuma;
    }
    if (alertaWhatsappHabilitado && alertaWhatsappDisparado) {
      pendenciaWhatsapp = PendenciaResolvido;
    } else if (!alertaWhatsappDisparado) {
      pendenciaWhatsapp = PendenciaNenhuma;
    }
  }

  if (!alertaDiscordHabilitado) {
    alertaDiscordDisparado = false;
    pendenciaDiscord = PendenciaNenhuma;
  }
  if (!alertaTelegramHabilitado) {
    alertaTelegramDisparado = false;
    pendenciaTelegram = PendenciaNenhuma;
  }
  if (!alertaWhatsappHabilitado) {
    alertaWhatsappDisparado = false;
    pendenciaWhatsapp = PendenciaNenhuma;
  }
}

void processarFilaAlertas() {
  if (testeAlertasEmAndamento) {
    return;
  }
  if (envioCanalEmAndamento || envioCanalConcluido) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long agora = millis();
  String mensagem;

  if (pendenciaDiscord != PendenciaNenhuma && (ultimoEnvioDiscordMs == 0 || agora - ultimoEnvioDiscordMs >= alertaRetryMs)) {
    ultimoEnvioDiscordMs = agora;
    mensagem = renderMensagemAlerta(pendenciaDiscord == PendenciaTipoAlerta ? mensagemAlertaDiscord : mensagemResolvidoDiscord, tempAtual);
    iniciarEnvioCanal(CanalEnvioDiscord, mensagem, false, pendenciaDiscord);
    return;
  }

  if (pendenciaTelegram != PendenciaNenhuma && (ultimoEnvioTelegramMs == 0 || agora - ultimoEnvioTelegramMs >= alertaRetryMs)) {
    ultimoEnvioTelegramMs = agora;
    mensagem = renderMensagemAlerta(pendenciaTelegram == PendenciaTipoAlerta ? mensagemAlertaDiscord : mensagemResolvidoDiscord, tempAtual);
    iniciarEnvioCanal(CanalEnvioTelegram, mensagem, false, pendenciaTelegram);
    return;
  }

  if (pendenciaWhatsapp != PendenciaNenhuma && (ultimoEnvioWhatsappMs == 0 || agora - ultimoEnvioWhatsappMs >= alertaRetryMs)) {
    ultimoEnvioWhatsappMs = agora;
    mensagem = renderMensagemAlerta(pendenciaWhatsapp == PendenciaTipoAlerta ? mensagemAlertaDiscord : mensagemResolvidoDiscord, tempAtual);
    iniciarEnvioCanal(CanalEnvioWhatsapp, mensagem, false, pendenciaWhatsapp);
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

void drawCartaoArredondado(int x, int y, int largura, int altura, int raio, uint16_t corFundoCartao) {
  tft.fillRoundRect(x, y, largura, altura, raio, corFundoCartao);
  tft.drawRoundRect(x, y, largura, altura, raio, corPreto);
}

void sendWebMessage(int httpCode, const String& mensagem, const String& linkVoltar) {
  String html;
  html.reserve(768);
  html += F("<html><body style='font-family:sans-serif;background:#121212;color:#fff;padding:20px;'><div style='max-width:760px;margin:0 auto;'><div style='white-space:pre-line;line-height:1.5;'>");
  html += mensagem;
  html += F("</div>");
  if (linkVoltar.length() > 0) {
    html += F("<p><a href='");
    html += linkVoltar;
    html += F("' style='color:#00f2ff'>Voltar</a></p>");
  }
  html += F("</div></body></html>");
  server.send(httpCode, "text/html", html);
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
  if (tela == TelaIp) return corInfoSuave;
  return corVermelho;
}

void drawIndicadoresTelaAnimados(uint8_t preenchimentoTemp, uint8_t preenchimentoSensores, uint8_t preenchimentoIp, uint8_t preenchimentoReset) {
  const int y = 18;
  const int xTemp = 90;
  const int xSensores = 110;
  const int xIp = 130;
  const int xReset = 150;
  const int raio = 4;

  tft.fillRect(74, 10, 92, 16, corFundo);
  tft.drawCircle(xTemp, y, raio, corInfoSuave);
  tft.drawCircle(xSensores, y, raio, corInfoSuave);
  tft.drawCircle(xIp, y, raio, corInfoSuave);
  tft.drawCircle(xReset, y, raio, corInfoSuave);

  if (preenchimentoTemp > 0) {
    tft.fillCircle(xTemp, y, preenchimentoTemp, corInfo);
  }
  if (preenchimentoSensores > 0) {
    tft.fillCircle(xSensores, y, preenchimentoSensores, corInfo);
  }
  if (preenchimentoIp > 0) {
    tft.fillCircle(xIp, y, preenchimentoIp, corInfo);
  }
  if (preenchimentoReset > 0) {
    tft.fillCircle(xReset, y, preenchimentoReset, corInfo);
  }
}

void drawIndicadoresTela() {
  if (telaAtual == TelaTemperatura) {
    drawIndicadoresTelaAnimados(3, 0, 0, 0);
  } else if (telaAtual == TelaSensores) {
    drawIndicadoresTelaAnimados(0, 3, 0, 0);
  } else if (telaAtual == TelaIp) {
    drawIndicadoresTelaAnimados(0, 0, 3, 0);
  } else {
    drawIndicadoresTelaAnimados(0, 0, 0, 3);
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

  drawCartaoArredondado(28, 90, 184, 48, 12, corCartao);
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

  drawCartaoArredondado(56, 152, 128, 24, 10, corCartao);
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

  drawCartaoArredondado(x, y, largura, altura, 12, corCartao);

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

  tft.fillRoundRect(x, y + 7, largura, altura, 8, corCartao);
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

void drawAnelResetFactory(float progresso) {
  const int raio = 112;
  const int raioSegmento = 4;
  int progressoGraus = static_cast<int>(360.0f * progresso);

  for (int angulo = -90; angulo < 270; angulo += 3) {
    float rad = angulo * DEG_TO_RAD;
    int x = centroTela + static_cast<int>(cosf(rad) * raio);
    int y = centroTela + static_cast<int>(sinf(rad) * raio);
    uint16_t cor = (angulo + 90) <= progressoGraus ? corVermelho : corTrilha;
    tft.fillCircle(x, y, raioSegmento, cor);
  }
}

void drawStatusResetFactory(float progresso, bool emAndamento) {
  drawCartaoArredondado(40, 148, 160, 44, 10, corCartao);

  String textoStatus = emAndamento ? "Apagando..." : "Segure para iniciar";
  uint16_t corStatus = emAndamento ? corVermelho : corInfoSuave;
  drawTextoCentralizado(textoStatus, 156, 1, corStatus);

  String porcentagem = String(static_cast<int>(progresso * 100.0f)) + "%";
  drawTextoCentralizado(porcentagem, 176, 2, emAndamento ? corVermelho : corInfo);
}

void drawTelaResetFactory() {
  float progresso = 0.0f;
  if (resetFactoryEmAndamento) {
    progresso = static_cast<float>(millis() - inicioHoldResetMs) / static_cast<float>(holdResetFactoryMs);
    if (progresso < 0.0f) progresso = 0.0f;
    if (progresso > 1.0f) progresso = 1.0f;
  }

  tft.fillScreen(corFundo);
  drawIndicadoresTelaAnimados(0, 0, 0, 3);
  tft.fillCircle(centroTela, centroTela, 84, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 86, corTrilha);
  drawAnelResetFactory(progresso);

  drawTextoCentralizado("Reset Fabrica", 48, 2, corInfo);
  drawTextoCentralizado("Pressione e segure", 84, 1, corInfo);
  drawTextoCentralizado("o botao por 7s", 102, 1, corInfo);
  drawTextoCentralizado("para apagar tudo", 120, 1, corInfo);
  drawStatusResetFactory(progresso, resetFactoryEmAndamento);
  progressoResetExibido = progresso;
}

void atualizarProgressoResetFactory() {
  if (portalConfiguracaoAtivo || telaAtual != TelaResetFactory) {
    if (resetFactoryEmAndamento) {
      resetFactoryEmAndamento = false;
      inicioHoldResetMs = 0;
      ultimoRedrawResetMs = 0;
      progressoResetExibido = -1.0f;
    }
    return;
  }

  bool pressionado = digitalRead(ENC_SW) == LOW;
  if (!pressionado) {
    if (resetFactoryEmAndamento) {
      resetFactoryEmAndamento = false;
      inicioHoldResetMs = 0;
      ultimoRedrawResetMs = 0;
      drawTelaResetFactory();
    }
    return;
  }

  if (!resetFactoryEmAndamento) {
    resetFactoryEmAndamento = true;
    inicioHoldResetMs = millis();
    ultimoRedrawResetMs = 0;
    drawTelaResetFactory();
    return;
  }

  unsigned long tempoSegurando = millis() - inicioHoldResetMs;
  if (tempoSegurando >= holdResetFactoryMs) {
    drawAnelResetFactory(1.0f);
    drawStatusResetFactory(1.0f, true);
    progressoResetExibido = 1.0f;
    resetarConfiguracaoDeFabrica();
    return;
  }

  float progresso = static_cast<float>(tempoSegurando) / static_cast<float>(holdResetFactoryMs);
  if (millis() - ultimoRedrawResetMs >= 60 || fabsf(progresso - progressoResetExibido) >= 0.02f) {
    drawAnelResetFactory(progresso);
    drawStatusResetFactory(progresso, true);
    progressoResetExibido = progresso;
    ultimoRedrawResetMs = millis();
  }
}

void drawTelaConfiguracaoWifi() {
  String endereco = ipAtualWifi();

  tft.fillScreen(corFundo);
  drawIndicadoresTelaAnimados(0, 0, 3, 0);
  tft.fillCircle(centroTela, centroTela, 92, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 94, corTrilha);

  drawTextoCentralizado("WiFi Setup", 28, 2, corInfo);
  drawTextoCentralizado("Nao configurado", 52, 1, corVermelho);

  drawCartaoArredondado(28, 72, 184, 34, 10, corCartao);
  drawTextoCentralizado(wifiApSsid, 84, 1, corInfo);

  drawTextoCentralizado("1. Conecte no WiFi", 118, 1, corInfo);
  drawTextoCentralizado("2. Abra no celular", 136, 1, corInfo);
  drawTextoCentralizado(endereco, 154, 2, corInfo);
  drawTextoCentralizado("3. Salve a sua rede", 184, 1, corInfoSuave);

  ipExibido = endereco;
}

void drawTelaIp() {
  if (portalConfiguracaoAtivo) {
    drawTelaConfiguracaoWifi();
    return;
  }

  String ipAtual = ipAtualWifi();
  String titulo = "IP";
  String detalhe = String(textoEstadoWifi());

  tft.fillScreen(corFundo);
  drawIndicadoresTela();
  tft.fillCircle(centroTela, centroTela, 74, corPainel);
  tft.drawCircle(centroTela, centroTela, 118, corTrilha);
  tft.drawCircle(centroTela, centroTela, 76, corTrilha);
  drawTextoCentralizado(titulo, 72, 2, corInfo);

  drawCartaoArredondado(32, 102, 176, 36, 10, corCartao);
  drawTextoCentralizado(ipAtual, 110, 2, corInfo);
  drawTextoCentralizado(detalhe, 156, 1, corInfoSuave);
  ipExibido = ipAtual;
}

void drawTelaAtualCompleta() {
  if (portalConfiguracaoAtivo) {
    drawTelaConfiguracaoWifi();
    return;
  }

  if (telaAtual == TelaTemperatura) {
    drawTelaInicial();
  } else if (telaAtual == TelaSensores) {
    drawTelaSensores();
  } else if (telaAtual == TelaIp) {
    drawTelaIp();
  } else {
    drawTelaResetFactory();
  }
}

void animarTrocaTela(TelaAtual novaTela) {
  if (novaTela != TelaResetFactory) {
    resetFactoryEmAndamento = false;
    inicioHoldResetMs = 0;
    ultimoRedrawResetMs = 0;
    progressoResetExibido = -1.0f;
  }

  telaAtual = novaTela;
  drawTelaAtualCompleta();
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

const __FlashStringHelper* renderCssWeb() {
  return FPSTR(CSS_WEB);
}

void handleWifiConfig() {
  int redes = WiFi.scanNetworks(false, true);
  String html;
  html.reserve(4096);
  html += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>");
  html += FPSTR(FONT_AWESOME_HEAD);
  html += renderCssWeb();
  html += F("</head><body><div class='box'><div class='section-copy'><h2>Configurar WiFi</h2><p>Status: ");
  html += textoEstadoWifi();
  html += F("</p>");

  if (portalConfiguracaoAtivo) {
    html += F("<p>Rede do ESP: ");
    html += escapeHtml(wifiApSsid);
    html += F("</p><p>Endereco: ");
    html += WiFi.softAPIP().toString();
    html += F("</p>");
  } else if (wifiTemConfig) {
    html += F("<p>Rede salva: ");
    html += escapeHtml(wifiSsidSalvo);
    html += F("</p>");
  }
  html += F("</div>");

  html += F("<form method='POST' action='/wifi/save'>");
  html += F("<label>Rede encontrada</label><select id='ssid_select' name='ssid_select'>");
  html += F("<option value=''>Adicionar manualmente / rede oculta</option>");
  for (int i = 0; i < redes; i++) {
    String ssidRede = WiFi.SSID(i);
    if (ssidRede.length() == 0) {
      continue;
    }
    html += F("<option value='");
    html += escapeHtml(ssidRede);
    html += F("'>");
    html += escapeHtml(ssidRede);
    html += F(" (");
    html += String(WiFi.RSSI(i));
    html += F(" dBm)</option>");
  }
  html += F("</select>");
  html += F("<div id='ssid_manual_group'>");
  html += F("<label>SSID</label><input id='ssid_manual' name='ssid_manual' maxlength='32'>");
  html += F("</div>");
  html += F("<label>Senha</label><input name='password' type='password' maxlength='64' ");
  html += F("<button class='btn' type='submit'>");
  html += FPSTR(ICON_WIFI);
  html += FPSTR(ICON_SAVE);
  html += F("<span>Salvar e reiniciar</span></button>");
  html += F("</form>");

  html += F("<div class='btn-row'>");
  if (!portalConfiguracaoAtivo) {
    html += F("<a class='btn secondary' href='/'>");
    html += FPSTR(ICON_BACK);
    html += F("<span>Voltar</span></a>");
    html += F("<form method='POST' action='/wifi/reset' onsubmit='return confirm(\"Apagar o WiFi salvo e reiniciar em modo de configuracao?\");'><button class='btn danger' type='submit'>");
    html += FPSTR(ICON_RESET);
    html += F("<span>Apagar WiFi salvo</span></button></form>");
  } else {
    html += F("<a class='btn ghost' href='http://");
    html += WiFi.softAPIP().toString();
    html += F("/wifi'>");
    html += FPSTR(ICON_WIFI);
    html += F("<span>Reabrir portal</span></a>");
  }
  html += F("</div>");

  WiFi.scanDelete();
  html += FPSTR(WIFI_FORM_SCRIPT);
  html += F("</div></body></html>");
  server.send(200, "text/html", html);
}

void handleWifiSave() {
  String ssidManual = server.arg("ssid_manual");
  String ssidSelecionado = server.arg("ssid_select");
  String senha = server.arg("password");
  ssidManual.trim();
  ssidSelecionado.trim();
  String novoSsid = ssidManual.length() ? ssidManual : ssidSelecionado;

  if (novoSsid.length() == 0) {
    sendWebMessage(400, "Informe uma rede WiFi.", "/wifi");
    return;
  }

  salvarConfigWifi(novoSsid, senha);
  sendWebMessage(200, "WiFi salvo. Reiniciando...");
  delay(800);
  ESP.restart();
}

void handleWifiReset() {
  limparConfigWifi();
  sendWebMessage(200, "WiFi apagado. Reiniciando em modo de configuracao...");
  delay(800);
  ESP.restart();
}

void handleCaptivePortal() {
  if (!portalConfiguracaoAtivo) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/wifi", true);
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  if (portalConfiguracaoAtivo) {
    handleCaptivePortal();
    return;
  }

  server.send(404, "text/plain", "Nao encontrado");
}

void handleRoot() {
  if (portalConfiguracaoAtivo) {
    handleWifiConfig();
    return;
  }

  String html;
  html.reserve(4096);
  html += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>");
  html += FPSTR(FONT_AWESOME_HEAD);
  html += renderCssWeb();
  html += FPSTR(ROOT_STATUS_SCRIPT);
  html += F("</head><body><div class='box'><div class='hero'><p class='hero-title'>TERMOMETRO S3</p>");
  html += "<p id='topTemp' class='" + String(sensorOnline ? "hero-reading" : "hero-reading offline") + "'>" + (sensorOnline ? String(tempAtual, 1) : String("--.-")) + " C</p><p class='hero-subtitle'>Sensor principal</p>";
  html += "<p id='topStatus' class='hero-status " + String(sensorOnline ? "online" : "offline") + "'>" + String(sensorOnline ? "Online" : "Offline") + "</p></div>";
  html += "<div class='grid'>";
  html += "<div class='card'><div class='label'>Sensor principal</div><div id='mainCardStatus' class='" + String(sensorOnline ? "value online" : "value offline") + "'>" + String(sensorOnline ? "Online" : "Offline") + "</div><div class='label' style='margin-top:12px;'>Temperatura</div><div id='mainCardTemp' class='" + String(sensorOnline ? "value" : "value muted") + "'>" + (sensorOnline ? String(tempAtual, 1) : String("--.-")) + " C</div></div>";
  html += "<div class='card'><div class='label'>DHT11 - temperatura</div><div id='dhtCardStatus' class='" + String(temperaturaDhtOnline ? "value online" : "value offline") + "'>" + String(temperaturaDhtOnline ? "Online" : "Offline") + "</div><div class='label' style='margin-top:12px;'>Temperatura</div><div id='dhtCardTemp' class='" + String(temperaturaDhtOnline ? "value" : "value muted") + "'>" + (temperaturaDhtOnline ? String(temperaturaDhtAtual, 1) : String("--.-")) + " C</div></div>";
  html += "<div class='card'><div class='label'>DHT11 - umidade</div><div id='humCardStatus' class='" + String(umidadeOnline ? "value online" : "value offline") + "'>" + String(umidadeOnline ? "Online" : "Offline") + "</div><div class='label' style='margin-top:12px;'>Umidade</div><div id='humCardValue' class='" + String(umidadeOnline ? "value" : "value muted") + "'>" + (umidadeOnline ? String(static_cast<int>(roundf(umidadeAtual))) : String("--")) + " %</div></div>";
  html += "</div>";
  html += "<p>WiFi: <span id='wifiState'>" + String(textoEstadoWifi()) + "</span></p>";
  html += "<p>IP atual: <span id='ipValue'>" + ipAtualWifi() + "</span></p>";
  html += "<p>Rede via DHCP</p>";
  html += "<p><a href='/wifi'>Configurar WiFi</a></p>";
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
  json += "\"sensor_online\":" + String(sensorOnline ? "true" : "false") + ",";
  json += "\"temperature_c\":";
  json += sensorOnline ? String(tempAtual, 2) : "null";
  json += ",";
  json += "\"humidity_online\":" + String(umidadeOnline ? "true" : "false") + ",";
  json += "\"humidity_percent\":";
  json += umidadeOnline ? String(umidadeAtual, 0) : "null";
  json += ",";
  json += "\"dht11_online\":" + String(temperaturaDhtOnline ? "true" : "false") + ",";
  json += "\"dht11_temperature_c\":";
  json += temperaturaDhtOnline ? String(temperaturaDhtAtual, 1) : "null";
  json += ",";
  json += "\"unit\":\"C\",";
  json += "\"wifi_configured\":" + String(wifiTemConfig ? "true" : "false") + ",";
  json += "\"wifi_portal_active\":" + String(portalConfiguracaoAtivo ? "true" : "false") + ",";
  json += "\"wifi_state\":\"" + String(textoEstadoWifi()) + "\",";
  json += "\"ip\":\"" + ipAtualWifi() + "\",";
  json += "\"uptime_ms\":" + String(millis());
  json += "}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleTelegramChatId() {
  String token = server.arg("telegram_token");
  token.trim();

  String chatId;
  String erro;
  if (obterTelegramChatIdDoBot(token, chatId, erro)) {
    String json = "{\"ok\":true,\"chat_id\":\"" + escapeJson(chatId) + "\"}";
    server.send(200, "application/json", json);
    return;
  }

  String json = "{\"ok\":false,\"error\":\"" + escapeJson(erro) + "\"}";
  server.send(WiFi.status() == WL_CONNECTED ? 400 : 503, "application/json", json);
}

void handleAlertas() {
  String html;
  html.reserve(8192);
  html += F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0' charset='UTF-8'>");
  html += FPSTR(FONT_AWESOME_HEAD);
  html += renderCssWeb();
  html += F("</head><body><div class='box'><div class='section-copy'><h2>Tipo de Midia</h2></div>");
  html += F("<form id='alertas_form' method='POST' action='/saveAlertas'>");
  html += F("<div class='media-switcher'>");
  html += F("<button class='media-tab active' type='button' data-target='panel-discord'>");
  html += FPSTR(ICON_BELL);
  html += F("<span>Discord</span></button>");
  html += F("<button class='media-tab' type='button' data-target='panel-telegram'>");
  html += FPSTR(ICON_TELEGRAM);
  html += F("<span>Telegram</span></button>");
  html += F("<button class='media-tab' type='button' data-target='panel-whatsapp'>");
  html += FPSTR(ICON_WHATSAPP);
  html += F("<span>WhatsApp</span></button></div>");
  html += F("<div class='config-grid'>");
  html += F("<div id='panel-discord' class='config-card media-panel active'><h3>Discord</h3>");
  html += F("<label class='checkbox-row'><input type='checkbox' name='discord_on' ");
  if (alertaDiscordHabilitado) html += F("checked ");
  html += F("><span>Habilitar Discord</span></label>");
  html += F("<label>Webhook URL</label><input id='discord_url' type='text' name='discord_url' value='");
  html += escapeHtml(discordWebhookUrl);
  html += F("'></div>");
  html += F("<div id='panel-telegram' class='config-card media-panel'><h3>Telegram</h3>");
  html += F("<label class='checkbox-row'><input type='checkbox' name='telegram_on' ");
  if (alertaTelegramHabilitado) html += F("checked ");
  html += F("><span>Habilitar Telegram</span></label>");
  html += F("<label>Bot token</label><input id='telegram_token' type='text' name='telegram_token' value='");
  html += escapeHtml(telegramBotToken);
  html += F("'>");
  html += F("<label>Chat ID</label><div class='input-action'><input id='telegram_chat_id' type='text' name='telegram_chat_id' value='");
  html += escapeHtml(telegramChatId);
  html += F("'><div class='input-action-buttons'><button id='telegram_fetch_chat_id' class='action-button' type='button'>Obter</button><button id='telegram_open_api' class='action-button icon-only' type='button' title='Abrir API do bot'>");
  html += FPSTR(ICON_EYE);
  html += F("</button></div></div>");
  html += F("<div id='telegram_fetch_status' class='status-inline'>Clique em Obter e envie uma mensagem ao bot para capturar o chat ID.</div></div>");
  html += F("<div id='panel-whatsapp' class='config-card media-panel'><h3>WhatsApp</h3>");
  html += F("<label class='checkbox-row'><input type='checkbox' name='whatsapp_on' ");
  if (alertaWhatsappHabilitado) html += F("checked ");
  html += F("><span>Habilitar WhatsApp</span></label>");
  html += F("<label>URL da API</label><input id='whatsapp_url' type='text' name='whatsapp_url' value='");
  html += escapeHtml(whatsappApiUrl);
  html += F("' placeholder='http://dominio/api/external/send-message'>");
  html += F("<label>Chave API</label><input id='whatsapp_key' type='text' name='whatsapp_key' value='");
  html += escapeHtml(whatsappApiKey);
  html += F("'>");
  html += F("<label>Contatos ou grupos</label><textarea id='whatsapp_destinos' name='whatsapp_destinos' placeholder='11999999999&#10;120363403568204860'>");
  html += escapeHtml(whatsappDestinos);
  html += F("</textarea></div></div>");
  html += F("<label>Temperatura de alerta</label><input type='number' step='0.1' name='temp_alerta' value='");
  html += String(temperaturaAlertaDiscord, 1);
  html += F("'>");
  html += F("<label>Temperatura de resolvido</label><input type='number' step='0.1' name='temp_ok' value='");
  html += String(temperaturaResolvidoDiscord, 1);
  html += F("'>");
  html += F("<label>Mensagem de alerta</label><textarea name='msg_alerta'>");
  html += escapeHtml(mensagemAlertaDiscord);
  html += F("</textarea>");
  html += F("<label>Mensagem de resolvido</label><textarea name='msg_ok'>");
  html += escapeHtml(mensagemResolvidoDiscord);
  html += F("</textarea>");
  html += F("<p class='hint'>Use <code>{{temp}}</code> e <code>{{ip}}</code> para inserir valores dinamicos na mensagem.</p>");
  html += F("<p class='hint'>WhatsApp: informe telefone brasileiro com DDD, como <code>11999999999</code>, ou apenas o ID do grupo, como <code>120363403568204860</code>. O firmware completa o formato automaticamente.</p>");
  html += F("<div class='btn-row'>");
  html += F("<button id='alertas_save' class='btn' type='button'>");
  html += FPSTR(ICON_BELL);
  html += FPSTR(ICON_SAVE);
  html += F("<span>Salvar alertas</span></button>");
  html += F("<button id='alertas_test' class='btn secondary' type='button'>");
  html += FPSTR(ICON_TEST);
  html += F("<span>Enviar teste</span></button>");
  html += F("<a class='btn ghost' href='/'>");
  html += FPSTR(ICON_BACK);
  html += F("<span>Voltar</span></a></div></form>");
  html += FPSTR(TELEGRAM_CHAT_SCRIPT);
  html += F("</div></body></html>");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "text/html", html);
}

bool atualizarConfigAlertasDoRequest(String& erro) {
  float novoTempAlerta = server.arg("temp_alerta").toFloat();
  float novoTempOk = server.arg("temp_ok").toFloat();
  String novaMensagemAlerta = server.hasArg("msg_alerta") ? server.arg("msg_alerta") : mensagemAlertaDiscord;
  String novaMensagemOk = server.hasArg("msg_ok") ? server.arg("msg_ok") : mensagemResolvidoDiscord;
  String novaWebhook = server.hasArg("discord_url") ? server.arg("discord_url") : discordWebhookUrl;
  String novoTelegramToken = server.hasArg("telegram_token") ? server.arg("telegram_token") : telegramBotToken;
  String novoTelegramChatId = server.hasArg("telegram_chat_id") ? server.arg("telegram_chat_id") : telegramChatId;
  String novaWhatsappUrl = server.hasArg("whatsapp_url") ? server.arg("whatsapp_url") : whatsappApiUrl;
  String novaWhatsappKey = server.hasArg("whatsapp_key") ? server.arg("whatsapp_key") : whatsappApiKey;
  String novosWhatsappDestinos = server.hasArg("whatsapp_destinos") ? server.arg("whatsapp_destinos") : whatsappDestinos;
  novaWebhook.trim();
  novoTelegramToken.trim();
  novoTelegramChatId.trim();
  novaWhatsappUrl.trim();
  novaWhatsappKey.trim();
  novosWhatsappDestinos.trim();

  if (novoTempAlerta <= 0.0f || novoTempOk <= 0.0f || novoTempOk > novoTempAlerta) {
    erro = "Temperaturas invalidas. A temperatura de resolvido nao pode ser maior que a de alerta.";
    return false;
  }

  bool discordAtivo = server.hasArg("discord_on");
  bool telegramAtivo = server.hasArg("telegram_on");
  bool whatsappAtivo = server.hasArg("whatsapp_on");

  if (discordAtivo && novaWebhook.length() == 0) {
    erro = "Informe o webhook do Discord.";
    return false;
  }

  if (telegramAtivo && (novoTelegramToken.length() == 0 || novoTelegramChatId.length() == 0)) {
    erro = "Informe o bot token e o chat ID do Telegram.";
    return false;
  }

  if (whatsappAtivo && (novaWhatsappUrl.length() == 0 || novaWhatsappKey.length() == 0 || novosWhatsappDestinos.length() == 0)) {
    erro = "Informe a URL, a chave e pelo menos um destino do WhatsApp.";
    return false;
  }

  alertaDiscordHabilitado = discordAtivo;
  alertaTelegramHabilitado = telegramAtivo;
  alertaWhatsappHabilitado = whatsappAtivo;
  discordWebhookUrl = novaWebhook;
  telegramBotToken = novoTelegramToken;
  telegramChatId = novoTelegramChatId;
  whatsappApiUrl = novaWhatsappUrl;
  whatsappApiKey = novaWhatsappKey;
  whatsappDestinos = novosWhatsappDestinos;
  temperaturaAlertaDiscord = novoTempAlerta;
  temperaturaResolvidoDiscord = novoTempOk;
  mensagemAlertaDiscord = novaMensagemAlerta.length() ? novaMensagemAlerta : "Alerta de temperatura: {{temp}} C°  ";
  mensagemResolvidoDiscord = novaMensagemOk.length() ? novaMensagemOk : "Temperatura normalizada: {{temp}} C°";
  alertaDiscordDisparado = false;
  alertaTelegramDisparado = false;
  alertaWhatsappDisparado = false;
  ultimoEnvioDiscordMs = 0;
  ultimoEnvioTelegramMs = 0;
  ultimoEnvioWhatsappMs = 0;
  limparPendenciasAlerta();
  salvarConfigAlertas();
  carregarConfigAlertas();
  erro = "";
  return true;
}

void handleSaveAlertas() {
  String erro;
  if (!atualizarConfigAlertasDoRequest(erro)) {
    sendWebMessage(400, erro, "/alertas");
    return;
  }
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Location", "/alertas");
  server.send(303, "text/plain", "");
}

void handleTestAlertas() {
  if (server.hasArg("temp_alerta")) {
    String erro;
    if (!atualizarConfigAlertasDoRequest(erro)) {
      sendWebMessage(400, erro, "/alertas");
      return;
    }
  } else {
    carregarConfigAlertas();
  }

  if (!alertaDiscordHabilitado && !alertaTelegramHabilitado && !alertaWhatsappHabilitado) {
    sendWebMessage(400, "Ative pelo menos um canal de alerta para testar.", "/alertas");
    return;
  }

  bool testeEmAndamentoLocal = testeAlertasEmAndamento;
  if (mutexTesteAlertas != nullptr && xSemaphoreTake(mutexTesteAlertas, pdMS_TO_TICKS(50)) == pdTRUE) {
    testeEmAndamentoLocal = testeAlertasEmAndamento;
    xSemaphoreGive(mutexTesteAlertas);
  }

  if (testeEmAndamentoLocal) {
    sendWebMessage(409, "Ja existe um teste de alertas em andamento.", "/alertas");
    return;
  }

  if (mutexTesteAlertas != nullptr && xSemaphoreTake(mutexTesteAlertas, pdMS_TO_TICKS(50)) == pdTRUE) {
    testeAlertasEmAndamento = true;
    detalheTesteDiscord = "desativado";
    detalheTesteTelegram = "desativado";
    detalheTesteWhatsapp = "desativado";
    testeDiscordOk = false;
    testeTelegramOk = false;
    testeWhatsappOk = false;
    resultadoTesteAlertas = "Teste em andamento...";
    xSemaphoreGive(mutexTesteAlertas);
  } else {
    testeAlertasEmAndamento = true;
    detalheTesteDiscord = "desativado";
    detalheTesteTelegram = "desativado";
    detalheTesteWhatsapp = "desativado";
    testeDiscordOk = false;
    testeTelegramOk = false;
    testeWhatsappOk = false;
    resultadoTesteAlertas = "Teste em andamento...";
  }

  detalheTesteDiscord = "desativado";
  detalheTesteTelegram = "desativado";
  detalheTesteWhatsapp = "desativado";
  testeDiscordOk = false;
  testeTelegramOk = false;
  testeWhatsappOk = false;
  mensagemTesteAlertas = renderMensagemAlerta(mensagemAlertaDiscord, tempAtual);
  etapaTesteAlertas = TesteEtapaDiscord;
  tarefaTesteAlertasHandle = nullptr;
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Location", "/alertas");
  server.send(303, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  tftSpi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
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
  carregarConfigAlertas();
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

  if (resetWifiSolicitadoNoBoot()) {
    limparConfigWifi();
  }

  mutexTesteAlertas = xSemaphoreCreateMutex();
  mutexEnvioAlertas = xSemaphoreCreateMutex();

  server.on("/", handleRoot);
  server.on("/readTemp", handleTemp);
  server.on("/api/temperature", HTTP_GET, handleApiTemperature);
  server.on("/wifi", HTTP_GET, handleWifiConfig);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/gen_204", HTTP_GET, handleCaptivePortal);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server.on("/library/test/success.html", HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  server.on("/redirect", HTTP_GET, handleCaptivePortal);
  server.on("/canonical.html", HTTP_GET, handleCaptivePortal);
  server.on("/success.txt", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);
  server.on("/alertas", HTTP_GET, handleAlertas);
  server.on("/saveAlertas", HTTP_POST, handleSaveAlertas);
  server.on("/testAlertas", HTTP_POST, handleTestAlertas);
  server.on("/telegram/chatid", HTTP_POST, handleTelegramChatId);
  server.onNotFound(handleNotFound);

  iniciarWifi();

  unsigned long startAttemptTime = millis();
  while (estadoWifi == WifiConectando && WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTempoConexaoMs) {
    delay(100);
  }
  atualizarWifi();

  if (portalConfiguracaoAtivo) {
    telaAtual = TelaIp;
  }

  server.begin();

  drawTelaAtualCompleta();
}

void loop() {
  atualizarWifi();
  server.handleClient();
  processarConclusaoEnvioCanal();
  processarFilaAlertas();
  processarTesteAlertasPendente();
  atualizarProgressoResetFactory();

  int clkEstado = digitalRead(ENC_CLK);
  if (clkEstado != ultimoClkEstado && clkEstado == LOW) {
    if (millis() - ultimoGiroMs > debounceGiroMs) {
      if (resetFactoryEmAndamento) {
        ultimoClkEstado = clkEstado;
        return;
      }
      int direcao = (digitalRead(ENC_DT) != clkEstado) ? 1 : -1;
      if (direcao != 0) {
        int indiceTela = static_cast<int>(telaAtual) + direcao;
        if (indiceTela < 0) indiceTela = 3;
        if (indiceTela > 3) indiceTela = 0;
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
      processarAlertaTemperatura(leitura);
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
      alertaDiscordDisparado = false;
      alertaTelegramDisparado = false;
      alertaWhatsappDisparado = false;
      ultimoEnvioDiscordMs = 0;
      ultimoEnvioTelegramMs = 0;
      ultimoEnvioWhatsappMs = 0;
      limparPendenciasAlerta();
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
    String ipAtual = ipAtualWifi();
    if (ipAtual != ipExibido) {
      drawTelaIp();
    }
  }
}
