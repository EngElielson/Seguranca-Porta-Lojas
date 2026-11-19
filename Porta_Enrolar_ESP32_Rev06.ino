// ============================================================================
//  SISTEMA DE MONITORAMENTO E BLOQUEIO - PORTA DE ENROLAR
//  REV: 2026-10-22s
//    - FIX: Prioridade: Bypass > Fechado > Manutenção(Aberto) > Aberto
//    - FIX: Cinza ajustado para #a6a6a6
// ============================================================================

#define WEBSOCKETS_MAX_DATA_SIZE 1500

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <time.h>
#include "esp_mac.h"

#define SENSOR_PADRAO_NF

#define PINO_TRAVA_ESQUERDA   32
#define PINO_TRAVA_DIREITA    33
#define PINO_BOTAO            25
#define PINO_RELE             26
#define PINO_LED_VERDE        27
#define PINO_LED_VERMELHO     14
#define PINO_DFPLAYER_RX      16
#define PINO_DFPLAYER_TX      17
#define PINO_BOTAO_RESET_WIFI 4
#define PINO_BOTAO_REBOOT     13
#define PINO_BYPASS           23

const char* AP_SSID_DEFAULT = "PortaEnrolar";
const char* AP_SENHA_DEFAULT = "senha123";

const unsigned long INTERVALO_AUDIO = 5000;
const unsigned long INTERVALO_RECONEXAO_WIFI = 30000;
const unsigned long TIMEOUT_TENTATIVA_WIFI  = 15000;
const unsigned long TEMPO_RESET_WIFI_FISICO = 3000;
const unsigned long TEMPO_REBOOT_FISICO     = 3000;
const unsigned long INTERVALO_NTP_RETRY     = 10000;
const unsigned long DEBOUNCE_TRAVA_MS       = 80;
const unsigned long DELAY_POS_RELE_MS       = 50;
const unsigned long DEBOUNCE_BYPASS_MS      = 200;

#define MAX_REDES 5
#define MAX_LOGS  200
#define TAM_MSG_LOG     64
#define TAM_TIPO_LOG    10
#define TAM_TEMPO_LOG   20

#define TRAVA_NA 0
#define TRAVA_NF 1

struct LogEntry {
  char tempo[TAM_TEMPO_LOG];
  char tipo[TAM_TIPO_LOG];
  char mensagem[TAM_MSG_LOG];
};

LogEntry logsSistema[MAX_LOGS];
int totalLog = 0;

char ultimoEventoTempo[TAM_TEMPO_LOG] = "";
char ultimoEventoTipo[TAM_TIPO_LOG]   = "";
char ultimoEventoMsg[TAM_MSG_LOG]     = "";

AsyncWebServer servidorHTTP(80);
WebSocketsServer servidorWS(81);
HardwareSerial serialDFPlayer(2);
DFRobotDFPlayerMini dfPlayer;
Preferences preferencias;

bool travaEsquerdaAberta = false;
bool travaDireitaAberta  = false;
bool modoManutencao      = false;
bool bypassAtivo         = false;
unsigned long ultimoAudio = 0;

int  tipoTravaEsq        = TRAVA_NF;
int  tipoTravaDir        = TRAVA_NF;

bool travaEsqLeitura = false;
bool travaDirLeitura = false;
bool travaEsqEstavel = false;
bool travaDirEstavel = false;
unsigned long travaEsqMudouEm = 0;
unsigned long travaDirMudouEm = 0;

bool bypassLeitura            = false;
bool bypassEstavel            = false;
unsigned long bypassMudouEm   = 0;
bool bypassUltimoEstadoFisico = false;

bool broadcastPendente = false;
unsigned long broadcastAgendado = 0;

String senhaAdmin        = "admin";
String senhaVisualizador = "visualizador";
String nomePorta         = "Porta de Enrolar";
String macAddress        = "";
String ssidAPDinamico    = "";
String prefixoAP         = "";   
String senhaAP           = AP_SENHA_DEFAULT; 

struct RedeSalva { String ssid; String senha; };
RedeSalva redesSalvas[MAX_REDES];
int totalRedes = 0;

String wifiSSID        = "";
String wifiSenha       = "";
String wifiIP          = "";
bool   wifiConectado   = false;
bool   tentandoConectar = false;
bool   wifiHabilitado  = false;
bool   ntpSincronizado = false;
time_t bootUnixTime    = 0;
int    tentativasNTP   = 0;
int    tentativasHTTP  = 0;
unsigned long tempoInicioWiFi    = 0;
unsigned long ultimaTentativaWiFi = 0;
unsigned long ultimaTentativaNTP  = 0;

int reiniciarComMotivo = 0;
unsigned long quandoReiniciar = 0;
unsigned long tempoApertouReset = 0;

unsigned long tempoApertouReboot = 0;
unsigned long ultimoPiscaReboot = 0;
bool ledRebootEstado = false;

// ============================================================================
//  PERSISTENCIA
// ============================================================================
void carregarRedes() {
  preferencias.begin("credenciais", true);
  totalRedes = preferencias.getInt("numRedes", 0);
  if (totalRedes > MAX_REDES) totalRedes = MAX_REDES;
  for (int i = 0; i < totalRedes; i++) {
    char k1[16], k2[16];
    snprintf(k1, sizeof(k1), "rede%d_ssid", i);
    snprintf(k2, sizeof(k2), "rede%d_senha", i);
    redesSalvas[i].ssid  = preferencias.getString(k1, "");
    redesSalvas[i].senha = preferencias.getString(k2, "");
  }
  preferencias.end();
}

void salvarRedes() {
  preferencias.begin("credenciais", false);
  preferencias.putInt("numRedes", totalRedes);
  for (int i = 0; i < MAX_REDES; i++) {
    char k1[16], k2[16];
    snprintf(k1, sizeof(k1), "rede%d_ssid", i);
    snprintf(k2, sizeof(k2), "rede%d_senha", i);
    if (i < totalRedes) {
      preferencias.putString(k1, redesSalvas[i].ssid);
      preferencias.putString(k2, redesSalvas[i].senha);
    } else { preferencias.remove(k1); preferencias.remove(k2); }
  }
  preferencias.end();
}

void adicionarRede(String ssid, String senha) {
  for (int i = 0; i < totalRedes; i++) {
    if (redesSalvas[i].ssid == ssid) {
      for (int j = i; j < totalRedes - 1; j++) redesSalvas[j] = redesSalvas[j + 1];
      totalRedes--;
      break;
    }
  }
  int lim = (totalRedes < MAX_REDES) ? totalRedes : MAX_REDES - 1;
  for (int i = lim; i > 0; i--) redesSalvas[i] = redesSalvas[i - 1];
  redesSalvas[0].ssid = ssid;
  redesSalvas[0].senha = senha;
  if (totalRedes < MAX_REDES) totalRedes++; else totalRedes = MAX_REDES;
  salvarRedes();
}

bool removerRede(int idx) {
  if (idx < 0 || idx >= totalRedes) return false;
  for (int i = idx; i < totalRedes - 1; i++) redesSalvas[i] = redesSalvas[i + 1];
  totalRedes--;
  salvarRedes();
  return true;
}

void carregarConfiguracoes() {
  preferencias.begin("credenciais", true);
  senhaAdmin        = preferencias.getString("senhaAdmin", "admin");
  senhaVisualizador = preferencias.getString("senhaVis", "visualizador");
  nomePorta         = preferencias.getString("nomePorta", "Porta de Enrolar");
  bypassAtivo       = preferencias.getBool("bypassAtivo", false);
  prefixoAP         = preferencias.getString("prefixoAP", AP_SSID_DEFAULT);
  senhaAP           = preferencias.getString("senhaAP", AP_SENHA_DEFAULT);
  
  if (senhaAP.length() < 8) senhaAP = AP_SENHA_DEFAULT;

#ifdef SENSOR_PADRAO_NF
  tipoTravaEsq = preferencias.getInt("tipoTravaEsq", TRAVA_NF);
  tipoTravaDir = preferencias.getInt("tipoTravaDir", TRAVA_NF);
#else
  tipoTravaEsq = preferencias.getInt("tipoTravaEsq", TRAVA_NA);
  tipoTravaDir = preferencias.getInt("tipoTravaDir", TRAVA_NA);
#endif
  if (tipoTravaEsq != TRAVA_NA && tipoTravaEsq != TRAVA_NF) tipoTravaEsq = TRAVA_NF;
  if (tipoTravaDir != TRAVA_NA && tipoTravaDir != TRAVA_NF) tipoTravaDir = TRAVA_NF;
  if (prefixoAP.length() == 0 || prefixoAP.length() > 24) prefixoAP = AP_SSID_DEFAULT;
  preferencias.end();
  carregarRedes();
}

void salvarConfiguracoes() {
  preferencias.begin("credenciais", false);
  preferencias.putString("senhaAdmin", senhaAdmin);
  preferencias.putString("senhaVis", senhaVisualizador);
  preferencias.putString("nomePorta", nomePorta);
  preferencias.putBool("bypassAtivo", bypassAtivo);
  preferencias.putInt("tipoTravaEsq", tipoTravaEsq);
  preferencias.putInt("tipoTravaDir", tipoTravaDir);
  preferencias.putString("prefixoAP", prefixoAP);
  preferencias.putString("senhaAP", senhaAP);
  preferencias.end();
}

void marcarResetWiFi(int motivo) {
  preferencias.begin("credenciais", false);
  preferencias.putInt("resetWiFi", motivo);
  preferencias.end();
}

int lerEResetarFlagReset() {
  preferencias.begin("credenciais", false);
  int m = preferencias.getInt("resetWiFi", 0);
  if (m != 0) preferencias.putInt("resetWiFi", 0);
  preferencias.end();
  return m;
}

// ============================================================================
//  LOGS
// ============================================================================
void converterLogsAntigos() {
  if (bootUnixTime == 0) return;
  int convertidos = 0;
  for (int i = 0; i < totalLog; i++) {
    if (strncmp(logsSistema[i].tempo, "boot ", 5) == 0) {
      int h = 0, m = 0, s = 0;
      if (sscanf(logsSistema[i].tempo, "boot %d:%d:%d", &h, &m, &s) == 3) {
        unsigned long secs = h * 3600UL + m * 60UL + s;
        time_t logLocal = bootUnixTime + secs - (3 * 3600);
        struct tm ti; gmtime_r(&logLocal, &ti);
        char buf[TAM_TEMPO_LOG];
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &ti);
        strncpy(logsSistema[i].tempo, buf, TAM_TEMPO_LOG - 1);
        logsSistema[i].tempo[TAM_TEMPO_LOG - 1] = 0;
        convertidos++;
      }
    }
  }
  if (convertidos > 0) { Serial.printf("[NTP] Convertidos %d logs\n", convertidos); }
}

void adicionarLog(const char* tipo, const char* msg) {
  if (totalLog >= MAX_LOGS) { Serial.print("[LOG] BUFFER CHEIO: "); Serial.println(msg); return; }

  time_t agora = time(nullptr);
  if (agora > 1700000000) {
    if (!ntpSincronizado) { ntpSincronizado = true; bootUnixTime = agora - (millis() / 1000); converterLogsAntigos(); }
    time_t local = agora - (3 * 3600);
    struct tm ti; gmtime_r(&local, &ti);
    strftime(logsSistema[totalLog].tempo, TAM_TEMPO_LOG, "%d/%m/%Y %H:%M:%S", &ti);
  } else if (bootUnixTime > 0) {
    time_t logT = bootUnixTime + (millis() / 1000) - (3 * 3600);
    struct tm ti; gmtime_r(&logT, &ti);
    strftime(logsSistema[totalLog].tempo, TAM_TEMPO_LOG, "%d/%m/%Y %H:%M:%S", &ti);
  } else {
    unsigned long seg = millis() / 1000, min = seg / 60, hr = min / 60;
    seg %= 60; min %= 60;
    snprintf(logsSistema[totalLog].tempo, TAM_TEMPO_LOG, "boot %02lu:%02lu:%02lu", hr, min, seg);
  }

  strncpy(logsSistema[totalLog].tipo, tipo, TAM_TIPO_LOG - 1);
  logsSistema[totalLog].tipo[TAM_TIPO_LOG - 1] = 0;
  strncpy(logsSistema[totalLog].mensagem, msg, TAM_MSG_LOG - 1);
  logsSistema[totalLog].mensagem[TAM_MSG_LOG - 1] = 0;

  bool ehAcessoOuAlarme = (strcmp(tipo, "ACESSO") == 0) || (strcmp(tipo, "ALARME") == 0);
  bool ehDFPlayer = (strstr(msg, "DFPlayer") != NULL);
  bool ehTimeoutWiFi = (strstr(msg, "Timeout Wi-Fi externo") != NULL); 
  
  if (ehAcessoOuAlarme && !ehDFPlayer && !ehTimeoutWiFi) {
    strncpy(ultimoEventoTempo, logsSistema[totalLog].tempo, TAM_TEMPO_LOG - 1);
    ultimoEventoTempo[TAM_TEMPO_LOG - 1] = 0;
    strncpy(ultimoEventoTipo, tipo, TAM_TIPO_LOG - 1);
    ultimoEventoTipo[TAM_TIPO_LOG - 1] = 0;
    strncpy(ultimoEventoMsg, msg, TAM_MSG_LOG - 1);
    ultimoEventoMsg[TAM_MSG_LOG - 1] = 0;

    if (servidorWS.connectedClients() > 0) {
      StaticJsonDocument<256> ev;
      ev["evento"]     = "novo_acesso_alarme";
      ev["tempo"]      = ultimoEventoTempo;
      ev["tipo"]       = ultimoEventoTipo;
      ev["mensagem"]   = ultimoEventoMsg;
      String j; serializeJson(ev, j);
      servidorWS.broadcastTXT(j);
    }
  }

  Serial.printf("[LOG] %s %s: %s\n", logsSistema[totalLog].tempo, tipo, msg);
  totalLog++;
}

void escreverEscapado(AsyncResponseStream *resp, const char *s) {
  while (*s) {
    char c = *s;
    if (c == '"') resp->print("\\\"");
    else if (c == '\\') resp->print("\\\\");
    else if (c == '\n') resp->print("\\n");
    else if (c == '\r') resp->print("\\r");
    else if (c == '\t') resp->print("\\t");
    else if ((unsigned char)c < 32) { }
    else resp->print(c);
    s++;
  }
}

// ============================================================================
//  AUXILIARES
// ============================================================================
void tocarAudio(int n) {
  if (millis() - ultimoAudio > INTERVALO_AUDIO) { dfPlayer.play(n); ultimoAudio = millis(); }
}

bool motorLiberado() {
  return (travaEsquerdaAberta && travaDireitaAberta && !modoManutencao) || bypassAtivo;
}

void aplicarHardware() {
  bool lib = motorLiberado();
  digitalWrite(PINO_RELE, lib ? HIGH : LOW);
  digitalWrite(PINO_LED_VERDE, lib ? HIGH : LOW);
  digitalWrite(PINO_LED_VERMELHO, lib ? LOW : HIGH);
}

void broadcastCompleto() {
  if (servidorWS.connectedClients() == 0) return;
  StaticJsonDocument<896> doc;
  doc["trava_esquerda"]  = travaEsquerdaAberta ? "aberta" : "fechada";
  doc["trava_direita"]   = travaDireitaAberta  ? "aberta" : "fechada";
  doc["motor_liberado"]  = motorLiberado();
  doc["modo_manutencao"] = modoManutencao;
  doc["bypass_ativo"]    = bypassAtivo;
  doc["tipo_trava_esq"]  = tipoTravaEsq;
  doc["tipo_trava_dir"]  = tipoTravaDir;
  doc["nome_porta"]      = nomePorta;
  doc["wifi_conectado"]  = wifiConectado;
  doc["wifi_ip"]         = wifiIP;
  doc["wifi_ssid"]       = wifiSSID;
  doc["total_redes"]     = totalRedes;
  doc["mac"]             = macAddress;
  doc["ssid_ap"]         = ssidAPDinamico;
  doc["prefixo_ap"]      = prefixoAP;
  doc["ultimo_evento_tempo"] = ultimoEventoTempo;
  doc["ultimo_evento_tipo"]  = ultimoEventoTipo;
  doc["ultimo_evento_msg"]   = ultimoEventoMsg;
  String j; serializeJson(doc, j);
  servidorWS.broadcastTXT(j);
}

void broadcastTravas() {
  if (servidorWS.connectedClients() == 0) return;
  StaticJsonDocument<320> doc;
  doc["trava_esquerda"]  = travaEsquerdaAberta ? "aberta" : "fechada";
  doc["trava_direita"]   = travaDireitaAberta  ? "aberta" : "fechada";
  doc["motor_liberado"]  = motorLiberado();
  doc["modo_manutencao"] = modoManutencao;
  doc["bypass_ativo"]    = bypassAtivo;
  String j; serializeJson(doc, j);
  servidorWS.broadcastTXT(j);
}

void atualizarSaidas() {
  aplicarHardware();
  broadcastCompleto();
}

void enviarAlertaTrava(const char* lado) {
  StaticJsonDocument<128> d;
  d["evento"] = "alerta_trava";
  d["lado"] = lado;
  String j; serializeJson(d, j);
  servidorWS.broadcastTXT(j);
}

bool lerTravaComoAberta(int pino, int tipo) {
  int nivel = digitalRead(pino);
  if (tipo == TRAVA_NF) return (nivel == HIGH);
  else                  return (nivel == LOW);
}

void verificarTravas() {
  bool esq = lerTravaComoAberta(PINO_TRAVA_ESQUERDA, tipoTravaEsq);
  bool dir = lerTravaComoAberta(PINO_TRAVA_DIREITA,  tipoTravaDir);

  unsigned long agora = millis();

  if (esq != travaEsqLeitura) { travaEsqLeitura = esq; travaEsqMudouEm = agora; }
  if (travaEsqLeitura != travaEsqEstavel && (agora - travaEsqMudouEm) >= DEBOUNCE_TRAVA_MS) {
    travaEsqEstavel = travaEsqLeitura;
  }

  if (dir != travaDirLeitura) { travaDirLeitura = dir; travaDirMudouEm = agora; }
  if (travaDirLeitura != travaDirEstavel && (agora - travaDirMudouEm) >= DEBOUNCE_TRAVA_MS) {
    travaDirEstavel = travaDirLeitura;
  }

  if (travaEsqEstavel != travaEsquerdaAberta || travaDirEstavel != travaDireitaAberta) {
    travaEsquerdaAberta = travaEsqEstavel;
    travaDireitaAberta  = travaDirEstavel;

    aplicarHardware();

    broadcastPendente = true;
    broadcastAgendado = agora + DELAY_POS_RELE_MS;
  }
}

void processarBroadcastPendente() {
  if (!broadcastPendente) return;
  if ((long)(millis() - broadcastAgendado) < 0) return;
  broadcastPendente = false;
  broadcastTravas();
}

bool usuarioEhAdmin(AsyncWebServerRequest *r) {
  if (!r->hasHeader("Cookie")) return false;
  return r->getHeader("Cookie")->value().indexOf("sessao=admin") >= 0;
}

bool usuarioEstaLogado(AsyncWebServerRequest *r) {
  if (!r->hasHeader("Cookie")) return false;
  String c = r->getHeader("Cookie")->value();
  return c.indexOf("sessao=admin") >= 0 || c.indexOf("sessao=visualizador") >= 0;
}

// ============================================================================
//  NTP VIA HTTP FALLBACK
// ============================================================================
bool obterHoraHTTP() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  client.setTimeout(2);
  if (!client.connect("worldtimeapi.org", 80)) return false;
  client.println("GET /api/ip HTTP/1.0");
  client.println("Host: worldtimeapi.org");
  client.println("User-Agent: ESP32");
  client.println("Connection: close");
  client.println();

  unsigned long t0 = millis();
  while (client.available() == 0 && millis() - t0 < 2000) { delay(10); yield(); }
  if (client.available() == 0) { client.stop(); return false; }

  String response = "";
  while (client.available() && response.length() < 4096 && millis() - t0 < 4000) {
    response += (char)client.read();
  }
  client.stop();

  int idx = response.indexOf("\"unixtime\":");
  if (idx < 0) return false;
  int start = idx + 12;
  while (start < (int)response.length() && (response[start] == ' ' || response[start] == ':')) start++;
  int end = start;
  while (end < (int)response.length() && response[end] >= '0' && response[end] <= '9') end++;
  if (end <= start) return false;

  long unix = response.substring(start, end).toInt();
  if (unix < 1700000000) return false;
  bootUnixTime = unix - (millis() / 1000);
  ntpSincronizado = true;
  Serial.printf("[HTTP] OK unixtime=%ld\n", unix);
  converterLogsAntigos();
  return true;
}

// ============================================================================
//  WI-FI + NTP
// ============================================================================
void iniciarConexaoWiFi() {
  if (!wifiHabilitado || wifiSSID.length() == 0) return;
  WiFi.begin(wifiSSID.c_str(), wifiSenha.c_str());
  tempoInicioWiFi = millis();
  tentandoConectar = true;
}

void aplicarConfigTime() {
  configTime(-3 * 3600, 0, "216.239.35.0", "216.239.35.4", "129.6.15.28");
  Serial.println("[NTP] configTime");
}

void checarConexaoWiFi() {
  if (!tentandoConectar) return;
  if (WiFi.status() == WL_CONNECTED) {
    tentandoConectar = false;
    wifiConectado = true;
    wifiIP = WiFi.localIP().toString();
    Serial.printf("[WIFI] Conectado: %s\n", wifiIP.c_str());
    aplicarConfigTime();
    ultimaTentativaNTP = millis();
    tentativasNTP = 0; tentativasHTTP = 0;
    time_t agora = time(nullptr);
    if (agora > 1700000000) { ntpSincronizado = true; bootUnixTime = agora - (millis() / 1000); converterLogsAntigos(); }
    String msg = "Wi-Fi conectado (" + wifiSSID + "): " + wifiIP;
    adicionarLog("INFO", msg.c_str());
    atualizarSaidas();
  } else if (millis() - tempoInicioWiFi > TIMEOUT_TENTATIVA_WIFI) {
    tentandoConectar = false;
    wifiConectado = false;
    wifiIP = "";
    adicionarLog("ALARME", "Timeout Wi-Fi externo");
    atualizarSaidas();
  }
}

void checarNTP() {
  if (ntpSincronizado) return;
  if (!wifiConectado) return;
  time_t agora = time(nullptr);
  if (agora > 1700000000) {
    ntpSincronizado = true;
    bootUnixTime = agora - (millis() / 1000);
    converterLogsAntigos();
    adicionarLog("INFO", "NTP sincronizado (UDP)");
    atualizarSaidas();
    return;
  }
  if (millis() - ultimaTentativaNTP < INTERVALO_NTP_RETRY) return;
  ultimaTentativaNTP = millis();
  if (tentativasNTP < 3) {
    tentativasNTP++;
    Serial.printf("[NTP] UDP #%d\n", tentativasNTP);
    aplicarConfigTime();
  } else if (tentativasHTTP < 3) {
    tentativasHTTP++;
    Serial.printf("[HTTP] #%d\n", tentativasHTTP);
    if (obterHoraHTTP()) { adicionarLog("INFO", "Hora via HTTP"); atualizarSaidas(); }
  }
}

void monitorarWiFi() {
  if (wifiConectado && WiFi.status() != WL_CONNECTED) {
    wifiConectado = false;
    wifiIP = "";
    adicionarLog("ALARME", "Wi-Fi externo desconectado");
    atualizarSaidas();
  }
  if (!wifiHabilitado) return;
  if (!wifiConectado && !tentandoConectar && wifiSSID.length() > 0) {
    if (millis() - ultimaTentativaWiFi > INTERVALO_RECONEXAO_WIFI) {
      ultimaTentativaWiFi = millis();
      iniciarConexaoWiFi();
    }
  }
}

// ============================================================================
//  BOTAO DE REBOOT MANUAL (GPIO 13)
// ============================================================================
void verificarBotaoReboot() {
  if (digitalRead(PINO_BOTAO_REBOOT) == LOW) {
    unsigned long agora = millis();

    if (tempoApertouReboot == 0) {
      tempoApertouReboot = agora;
      ultimoPiscaReboot = agora;
      ledRebootEstado = false;
      Serial.println("[REBOOT] Botao pressionado - contando 3s");
    }

    if (agora - ultimoPiscaReboot >= 200) {
      ledRebootEstado = !ledRebootEstado;
      digitalWrite(PINO_LED_VERMELHO, ledRebootEstado ? HIGH : LOW);
      ultimoPiscaReboot = agora;
    }

    if (agora - tempoApertouReboot >= TEMPO_REBOOT_FISICO) {
      Serial.println("[REBOOT] Executando reboot manual");
      adicionarLog("CFG", "Reboot manual via botao GPIO 13");

      digitalWrite(PINO_LED_VERMELHO, HIGH);
      digitalWrite(PINO_LED_VERDE, LOW);
      digitalWrite(PINO_RELE, LOW);

      delay(300);
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  } else {
    if (tempoApertouReboot != 0) {
      Serial.println("[REBOOT] Botao solto antes do tempo - cancelado");
      tempoApertouReboot = 0;
      ledRebootEstado = false;
      aplicarHardware();
    }
  }
}

// ============================================================================
//  BOTAO FISICO DE BYPASS (GPIO 23) - TOGGLE
// ============================================================================
void verificarBotaoBypass() {
  bool fisico = (digitalRead(PINO_BYPASS) == LOW);
  unsigned long agora = millis();

  if (fisico != bypassLeitura) {
    bypassLeitura = fisico;
    bypassMudouEm = agora;
  }

  if (bypassLeitura != bypassEstavel && (agora - bypassMudouEm) >= DEBOUNCE_BYPASS_MS) {
    bypassEstavel = bypassLeitura;

    if (bypassEstavel && !bypassUltimoEstadoFisico) {
      bypassAtivo = !bypassAtivo;
      salvarConfiguracoes();
      aplicarHardware();
      broadcastCompleto();
      adicionarLog("CFG", bypassAtivo ? "Bypass ATIVADO (fisico)" : "Bypass DESATIVADO (fisico)");
      tocarAudio(bypassAtivo ? 4 : 3);
      Serial.printf("[BYPASS] %s via GPIO %d\n", bypassAtivo ? "ON" : "OFF", PINO_BYPASS);
    }
    bypassUltimoEstadoFisico = bypassEstavel;
  }
}

// ============================================================================
//  WEBSOCKET
// ============================================================================
void eventoWS(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (type != WStype_TEXT) return;
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;
  if (doc["comando"].as<String>() == "status") {
    StaticJsonDocument<896> resp;
    resp["trava_esquerda"]  = travaEsquerdaAberta ? "aberta" : "fechada";
    resp["trava_direita"]   = travaDireitaAberta  ? "aberta" : "fechada";
    resp["motor_liberado"]  = motorLiberado();
    resp["modo_manutencao"] = modoManutencao;
    resp["bypass_ativo"]    = bypassAtivo;
    resp["tipo_trava_esq"]  = tipoTravaEsq;
    resp["tipo_trava_dir"]  = tipoTravaDir;
    resp["nome_porta"]      = nomePorta;
    resp["wifi_conectado"]  = wifiConectado;
    resp["wifi_ip"]         = wifiIP;
    resp["wifi_ssid"]       = wifiSSID;
    resp["total_redes"]     = totalRedes;
    resp["mac"]             = macAddress;
    resp["ssid_ap"]         = ssidAPDinamico;
    resp["prefixo_ap"]      = prefixoAP;
    resp["ultimo_evento_tempo"] = ultimoEventoTempo;
    resp["ultimo_evento_tipo"]  = ultimoEventoTipo;
    resp["ultimo_evento_msg"]   = ultimoEventoMsg;
    String j; serializeJson(resp, j);
    servidorWS.sendTXT(num, j);
  }
}

// ============================================================================
//  SVG ICONE
// ============================================================================
const char SVG_ICONE_PORTA[] PROGMEM = R"SVG(
<svg viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg">
  <rect x="8" y="8" width="84" height="84" fill="#0f172a" stroke="#3b82f6" stroke-width="3" rx="6"/>
  <rect x="8" y="8" width="84" height="14" fill="#1e293b" stroke="#3b82f6" stroke-width="2" rx="6"/>
  <rect x="8" y="22" width="6" height="70" fill="#1e293b" stroke="#3b82f6" stroke-width="1"/>
  <rect x="86" y="22" width="6" height="70" fill="#1e293b" stroke="#3b82f6" stroke-width="1"/>
  <g stroke="#3b82f6" stroke-width="0.8">
    <rect x="14" y="26" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="35" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="44" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="53" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="62" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="71" width="72" height="7" fill="#1e3a5f" rx="1"/>
    <rect x="14" y="80" width="72" height="7" fill="#1e3a5f" rx="1"/>
  </g>
  <g fill="#0a0f1a">
    <circle cx="26" cy="29.5" r="1.8"/><circle cx="42" cy="29.5" r="1.8"/><circle cx="58" cy="29.5" r="1.8"/><circle cx="74" cy="29.5" r="1.8"/>
    <circle cx="26" cy="38.5" r="1.8"/><circle cx="42" cy="38.5" r="1.8"/><circle cx="58" cy="38.5" r="1.8"/><circle cx="74" cy="38.5" r="1.8"/>
    <circle cx="26" cy="47.5" r="1.8"/><circle cx="42" cy="47.5" r="1.8"/><circle cx="58" cy="47.5" r="1.8"/><circle cx="74" cy="47.5" r="1.8"/>
    <circle cx="26" cy="56.5" r="1.8"/><circle cx="42" cy="56.5" r="1.8"/><circle cx="58" cy="56.5" r="1.8"/><circle cx="74" cy="56.5" r="1.8"/>
    <circle cx="26" cy="65.5" r="1.8"/><circle cx="42" cy="65.5" r="1.8"/><circle cx="58" cy="65.5" r="1.8"/><circle cx="74" cy="65.5" r="1.8"/>
    <circle cx="26" cy="74.5" r="1.8"/><circle cx="42" cy="74.5" r="1.8"/><circle cx="58" cy="74.5" r="1.8"/><circle cx="74" cy="74.5" r="1.8"/>
    <circle cx="26" cy="83.5" r="1.8"/><circle cx="42" cy="83.5" r="1.8"/><circle cx="58" cy="83.5" r="1.8"/><circle cx="74" cy="83.5" r="1.8"/>
  </g>
</svg>
)SVG";

// ============================================================================
//  CSS
// ============================================================================
const char CSS_BASE[] PROGMEM = R"CSS(
* { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
html, body {
  margin: 0; padding: 0; width: 100%; max-width: 100vw;
  overflow-x: hidden; -webkit-text-size-adjust: 100%;
}
body {
  font-family: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #0f172a; color: #e2e8f0; min-height: 100vh; line-height: 1.5; padding-bottom: 40px;
}
.header { position: fixed; top: 0; left: 0; right: 0; z-index: 100; background: #1e293b; border-bottom: 1px solid #334155; padding: 10px 14px; display: flex; justify-content: space-between; align-items: center; gap: 10px; }
.header-esq { display: flex; align-items: center; gap: 10px; min-width: 0; flex: 1; }
.header-esq svg { width: 32px; height: 32px; flex-shrink: 0; }
.header-info h1 { margin: 0; font-size: 0.95rem; font-weight: 600; color: #f1f5f9; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.header-info .user { font-size: 0.68rem; color: #94a3b8; margin-top: 2px; }
.header-dir { display: flex; gap: 4px; align-items: center; flex-shrink: 0; }

.btn-icon {
  font-family: inherit; font-weight: inherit; line-height: 1; text-align: center;
  text-transform: none; text-indent: 0; letter-spacing: normal; word-spacing: normal;
  white-space: nowrap; -webkit-appearance: none; -moz-appearance: none; appearance: none;
  outline: none; box-shadow: none; border: none; margin: 0; padding: 0; background: none;
  width: 34px; height: 34px; min-width: 34px; min-height: 34px; max-width: 34px; max-height: 34px;
  box-sizing: border-box; border: 1px solid #334155; border-radius: 8px;
  background: #0f172a; color: #cbd5e1; font-size: 0.95rem;
  display: inline-flex; align-items: center; justify-content: center; cursor: pointer;
  text-decoration: none; flex-shrink: 0; flex-grow: 0; overflow: hidden; vertical-align: middle;
  -webkit-user-select: none; user-select: none;
}
.btn-icon:hover { background: #1e293b; border-color: #3b82f6; color: #f1f5f9; }
.btn-icon:active { transform: scale(0.94); }
.btn-icon.settings { font-size: 1.35rem; }
.btn-icon.calendar { color: #38bdf8; }
.btn-icon.maint { background: #1e1b4b; border-color: #4c1d95; color: #a78bfa; }
.btn-icon.maint.ativo { background: #f59e0b; color: #0f172a; border-color: #f59e0b; }

.btn-sair-icon {
  background: #ef4444; border-color: #ef4444; color: #fff;
  width: 34px; height: 34px; min-width: 34px; min-height: 34px; max-width: 34px; max-height: 34px;
}
.btn-sair-icon:hover { background: #dc2626; border-color: #dc2626; color: #fff; }
.btn-sair-icon svg { width: 22px; height: 22px; display: block; pointer-events: none; flex-shrink: 0; }

.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; max-width: 900px; margin: 0 auto 16px; padding: 0 12px; }
.card-status { background: #1e293b; border-radius: 12px; padding: 12px 10px; text-align: center; border: 1px solid #334155; display: flex; flex-direction: column; align-items: center; gap: 4px; min-height: 88px; justify-content: center; }
.card-status .icone { font-size: 1.2rem; display: flex; align-items: center; justify-content: center; height: 22px; }
.card-status .icone svg { width: 22px; height: 22px; display: block; }
.card-status .titulo { font-size: 0.65rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
.card-status .valor { font-size: 1rem; font-weight: 700; color: #f1f5f9; }
.card-status.aberta .valor, .card-status.liberado .valor, .card-status.normal .valor, .card-status.bypass-on .valor { color: #10b981; }
.card-status.fechada .valor, .card-status.bloqueado .valor, .card-status.bypass-off .valor { color: #ef4444; }
.card-status.manutencao .valor { color: #f59e0b; }
.card-status.aberta, .card-status.liberado, .card-status.normal, .card-status.bypass-on { border-color: #10b981; box-shadow: 0 0 10px rgba(16,185,129,0.15); }
.card-status.fechada, .card-status.bloqueado, .card-status.bypass-off { border-color: #ef4444; box-shadow: 0 0 10px rgba(239,68,68,0.15); }
.card-status.manutencao { border-color: #f59e0b; box-shadow: 0 0 10px rgba(245,158,11,0.15); }
@media (min-width: 480px) { .card-status { padding: 16px 12px; min-height: 110px; } .card-status .icone { font-size: 1.6rem; height: 28px; } .card-status .icone svg { width: 28px; height: 28px; } .card-status .valor { font-size: 1.3rem; } }

.icone-contato svg { transition: all 0.35s ease; }
.icone-contato.aberto { color: #64748b; }
.icone-contato.fechado { color: #10b981; }
.icone-contato.fechado svg { filter: drop-shadow(0 0 4px rgba(16,185,129,0.7)); }

.painel {
  width: calc(100% - 24px); max-width: 900px; background: #1e293b;
  border-radius: 12px; padding: 16px; border: 1px solid #334155; margin: 0 auto 16px; overflow: hidden;
}
@media (min-width: 924px) { .painel { width: 100%; } }
.painel h3 { margin: 0 0 12px; font-size: 0.95rem; color: #f1f5f9; display: flex; align-items: center; gap: 8px; }
.painel.wifi h3 { color: #38bdf8; }
.linha { margin-bottom: 12px; padding-bottom: 12px; border-bottom: 1px solid #334155; }
.linha:last-child { border-bottom: none; margin-bottom: 0; padding-bottom: 0; }
.linha label { display: block; font-size: 0.7rem; color: #94a3b8; margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px; }

.ultimo-evento {
  width: calc(100% - 24px); max-width: 900px; margin: 0 auto 12px;
  background: #1e293b; border: 1px solid #334155; border-left: 4px solid #334155;
  border-radius: 10px; padding: 10px 14px; display: flex; align-items: center;
  gap: 10px; min-height: 62px; height: 62px; overflow: hidden;
}
@media (min-width: 924px) { .ultimo-evento { width: 100%; } }
.ultimo-evento.acesso { border-left-color: #10b981; }
.ultimo-evento.alarme { border-left-color: #ef4444; }
.ultimo-evento .ue-icone { font-size: 1.2rem; flex-shrink: 0; width: 24px; text-align: center; }
.ultimo-evento.acesso .ue-icone { color: #10b981; }
.ultimo-evento.alarme .ue-icone { color: #ef4444; }
.ultimo-evento .ue-conteudo { flex: 1; min-width: 0; overflow: hidden; }
.ultimo-evento .ue-topo { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 2px; flex-wrap: nowrap; }
.ultimo-evento .ue-tipo { font-size: 0.58rem; font-weight: 700; letter-spacing: 0.8px; text-transform: uppercase; padding: 2px 6px; border-radius: 4px; background: #334155; color: #cbd5e1; flex-shrink: 0; }
.ultimo-evento.acesso .ue-tipo { background: #10b981; color: #0f172a; }
.ultimo-evento.alarme .ue-tipo { background: #ef4444; color: #fff; }
.ultimo-evento .ue-tempo { font-size: 0.62rem; color: #94a3b8; font-family: monospace; flex-shrink: 0; white-space: nowrap; }
.ultimo-evento .ue-msg { font-size: 0.82rem; color: #e2e8f0; word-break: break-word; line-height: 1.35; display: -webkit-box; -webkit-line-clamp: 1; -webkit-box-orient: vertical; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ultimo-evento .ue-msg.vazio { color: #64748b; font-style: italic; }

input, select { width: 100%; padding: 10px 12px; border-radius: 8px; border: 1px solid #334155; background: #0f172a; color: #f1f5f9; font-size: 0.95rem; }
input:focus, select:focus { outline: none; border-color: #3b82f6; }
select { appearance: none; background-image: url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'><path fill='%2394a3b8' d='M6 9L1 4h10z'/></svg>"); background-repeat: no-repeat; background-position: right 12px center; padding-right: 32px; cursor: pointer; }

button.acao { padding: 10px 16px; background: #3b82f6; color: #fff; border: none; border-radius: 8px; font-weight: 600; font-size: 0.85rem; cursor: pointer; min-height: 40px; white-space: nowrap; }
button.acao.verde { background: #10b981; }
button.acao.vermelho { background: #ef4444; }
button.acao.laranja { background: #f59e0b; color: #0f172a; }

a.acao-link { display: block; text-align: center; text-decoration: none; padding: 10px 16px; border-radius: 8px; font-weight: 600; font-size: 0.85rem; margin-top: 6px; }
a.acao-link.vermelho { background: #ef4444; color: #fff; }
a.acao-link.laranja { background: #f59e0b; color: #0f172a; }
a.acao-link.azul { background: #3b82f6; color: #fff; }

.wifi-status-linha { display: flex; justify-content: space-between; align-items: center; padding: 6px 0; font-size: 0.82rem; gap: 10px; }
.wifi-status-linha .lbl { color: #94a3b8; }
.wifi-status-linha .val { font-weight: 600; color: #f1f5f9; font-family: monospace; word-break: break-all; text-align: right; max-width: 60%; }

.dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; vertical-align: middle; }
.dot.verde { background: #10b981; box-shadow: 0 0 6px #10b981; }
.dot.vermelho { background: #ef4444; box-shadow: 0 0 6px #ef4444; }
.dot.amarelo { background: #f59e0b; box-shadow: 0 0 6px #f59e0b; }

.toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(100px); background: #10b981; color: #0f172a; padding: 10px 18px; border-radius: 8px; font-weight: 600; font-size: 0.85rem; box-shadow: 0 4px 20px rgba(0,0,0,0.5); transition: transform 0.3s; z-index: 200; max-width: 90vw; text-align: center; }
.toast.visivel { transform: translateX(-50%) translateY(0); }
.toast.erro { background: #ef4444; color: #fff; }

.nova-rede-box { background: #0f172a; border: 1px solid #334155; border-radius: 8px; padding: 12px; margin-top: 8px; }
.btn-voltar { display: inline-flex; align-items: center; gap: 6px; padding: 6px 12px; border-radius: 8px; background: #334155; color: #f1f5f9; text-decoration: none; font-size: 0.8rem; font-weight: 600; min-height: 34px; }

.tabs { display: flex; gap: 3px; max-width: 900px; margin: 76px auto 12px; padding: 0 8px; overflow: hidden; }
.tab { flex: 1; min-width: 0; padding: 8px 2px; border-radius: 6px; border: 1px solid #334155; background: #1e293b; color: #94a3b8; font-weight: 600; font-size: 0.65rem; cursor: pointer; white-space: nowrap; text-align: center; }
.tab.ativa { background: #3b82f6; color: #fff; border-color: #3b82f6; }

.door-painel { padding: 20px 14px !important; margin-top: 76px !important; }
.door-wrapper { position: relative; width: 100%; max-width: 380px; margin: 0 auto; padding: 0 56px; }
@media (max-width: 360px) { .door-wrapper { padding: 0 44px; } }
@media (max-width: 300px) { .door-wrapper { padding: 0 36px; } }
.door-frame { position: relative; width: 100%; height: 140px; background: #0a0f1a; border: 2px solid #ef4444; border-radius: 8px; overflow: hidden; box-shadow: 0 0 15px rgba(239,68,68,0.35), inset 0 0 20px rgba(0,0,0,0.5); transition: border-color 0.4s, background-color 0.4s; }
.door-frame.pronta { border-color: #10b981; box-shadow: 0 0 20px rgba(16,185,129,0.4), inset 0 0 20px rgba(0,0,0,0.5); }

.door-box { position: absolute; top: 0; left: 0; right: 0; height: 16px; background: linear-gradient(to bottom, #1e293b, #0f172a); border-bottom: 2px solid #334155; z-index: 2; }
.door-slats { position: absolute; top: 16px; left: 0; right: 0; height: calc(100% - 16px); background: repeating-linear-gradient(to bottom, #3b82f6 0px, #3b82f6 11px, #1e3a5f 11px, #1e3a5f 13px); background-size: 100% 13px; transform: translateY(0); transition: transform 1.4s cubic-bezier(0.4, 0, 0.2, 1); }
.door-slats.aberta { transform: translateY(calc(-100% + 3px)); }

.manutencao-overlay {
  position: absolute; top: 0; left: 0; right: 0; bottom: 0;
  background: transparent !important; /* FORÇA TRANSPARENTE PARA NÃO ALTERAR O FUNDO */
  display: none; align-items: center; justify-content: center;
  z-index: 10; pointer-events: none;
}
.manutencao-overlay.ativo { display: flex; }
.manutencao-overlay svg { 
  width: 85%; 
  max-width: 240px; 
  height: auto; 
  filter: drop-shadow(0 2px 4px rgba(0,0,0,0.2));
  transform: translateY(-2%);
}

.sensor-group { position: absolute; bottom: 8px; display: flex; align-items: center; gap: 3px; z-index: 3; }
.sensor-group-esq { left: 0; flex-direction: row-reverse; }
.sensor-group-dir { right: 0; }
.sensor-lateral { width: 22px; height: 22px; border-radius: 50%; background: #475569; border: 2px solid #64748b; display: flex; align-items: center; justify-content: center; flex-shrink: 0; }
.sensor-lateral::after { content: ''; width: 8px; height: 8px; border-radius: 50%; background: #0a0f1a; }
.sensor-label { font-size: 0.65rem; font-weight: 700; color: #64748b; text-transform: uppercase; white-space: nowrap; }
.sensor-group.verde .sensor-label { color: #10b981; }
.sensor-group.vermelho .sensor-label { color: #ef4444; }
.sensor-lateral.verde { background: #10b981; border-color: #34d399; box-shadow: 0 0 14px #10b981; animation: blinkVerde 3s ease-in-out infinite; }
.sensor-lateral.verde::after { background: #d1fae5; }
.sensor-lateral.vermelho { background: #ef4444; border-color: #f87171; box-shadow: 0 0 14px #ef4444; animation: blinkVermelho 3s ease-in-out infinite; }
.sensor-lateral.vermelho::after { background: #fee2e2; }
.sensor-lateral.alerta { animation: blinkAlerta 0.96s ease-in-out infinite !important; }
.sensor-lateral.alerta::after { background: #fff !important; }
@keyframes blinkVerde { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
@keyframes blinkVermelho { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
@keyframes blinkAlerta { 0%, 100% { opacity: 1; transform: scale(1.15); } 50% { opacity: 0.3; transform: scale(0.85); } }

.motor-icon {
  position: absolute; right: 0; top: -2px; width: 40px; height: 40px;
  display: flex; align-items: center; justify-content: center;
  color: #ef4444; z-index: 5; transition: color 0.35s ease, filter 0.35s ease;
  cursor: default; user-select: none; background: transparent; border: none; padding: 0;
}
.motor-icon svg { width: 100%; height: 100%; display: block; overflow: visible; }
.motor-icon.liberado { color: #10b981; filter: drop-shadow(0 0 6px rgba(16,185,129,0.85)); }
.motor-icon.bloqueado { color: #ef4444; filter: drop-shadow(0 0 6px rgba(239,68,68,0.85)); }
.motor-icon.liberado.pulsando { animation: pulseMotor 1.4s ease-in-out infinite; }
@keyframes pulseMotor { 0%, 100% { filter: drop-shadow(0 0 6px rgba(16,185,129,0.85)); } 50% { filter: drop-shadow(0 0 14px rgba(16,185,129,1)); } }

.btn-limpar { display: block; width: calc(100% - 24px); max-width: 900px; margin: 16px auto 20px; padding: 12px; background: #ef4444; color: #fff; border: none; border-radius: 8px; font-weight: 700; font-size: 0.85rem; cursor: pointer; }
.btn-limpar.por-categoria { background: #f59e0b; color: #0f172a; }

.cal-painel { padding: 12px !important; max-width: 380px !important; margin: 0 auto 16px !important; }
.cal-painel h3 { margin: 0 0 10px !important; font-size: 0.9rem !important; }
.cal-nav { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; gap: 4px; }
.cal-nav button { background: #334155; color: #fff; border: none; padding: 6px 10px; border-radius: 6px; cursor: pointer; font-size: 1rem; font-weight: 700; line-height: 1; min-width: 32px; height: 32px; display: flex; align-items: center; justify-content: center; }
.cal-nav button:hover { background: #475569; }
.cal-nav h2 { margin: 0; color: #38bdf8; font-size: 0.85rem; text-align: center; flex: 1; min-width: 0; }
.btn-hoje-nav { background: #3b82f6 !important; color: #fff !important; font-size: 0.72rem !important; font-weight: 600 !important; padding: 0 10px !important; min-width: 48px !important; height: 32px !important; }

.cal-dias-semana { display: grid; grid-template-columns: repeat(7, 1fr); gap: 3px; margin-bottom: 3px; text-align: center; font-size: 0.6rem; color: #94a3b8; font-weight: 700; text-transform: uppercase; }
.cal-grid { display: grid; grid-template-columns: repeat(7, 1fr); grid-template-rows: repeat(6, 46px); gap: 3px; height: 291px; }
.dia { position: relative; width: 100%; height: 46px; display: flex; align-items: center; justify-content: center; border-radius: 5px; background: #0f172a; border: 1px solid #334155; font-size: 0.78rem; font-weight: 600; color: #cbd5e1; cursor: pointer; transition: all 0.15s; overflow: hidden; }
.dia.vazio { background: transparent; border: none; cursor: default; }
.dia:hover:not(.vazio) { transform: scale(1.04); }
.dia.com-acesso { border-color: #10b981; background: rgba(16,185,129,0.08); color: #10b981; }
.dia.com-alarme { border-color: #ef4444; background: rgba(239,68,68,0.08); color: #ef4444; }
.dia.com-ambos { border-color: #64748b; background: rgba(100,116,139,0.08); color: #cbd5e1; }
.dia.selecionado { box-shadow: 0 0 0 2px #3b82f6; }
.dia .num { line-height: 1; }
.dia .dots { position: absolute; bottom: 2px; left: 0; right: 0; display: flex; justify-content: center; align-items: center; gap: 2px; pointer-events: none; }
.dia .dots span { width: 3px; height: 3px; border-radius: 50%; display: inline-block; }
.dia .dots span.dot-g { background: #10b981; box-shadow: 0 0 3px #10b981; }
.dia .dots span.dot-r { background: #ef4444; box-shadow: 0 0 3px #ef4444; }

.evento-item { background: #0f172a; border-left: 4px solid #334155; border-radius: 8px; padding: 10px 12px; margin-bottom: 6px; }
.evento-item.acesso { border-left-color: #10b981; }
.evento-item.alarme { border-left-color: #ef4444; }
.evento-topo { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 3px; flex-wrap: wrap; }
.evento-tipo { font-size: 0.6rem; font-weight: 700; text-transform: uppercase; padding: 2px 6px; border-radius: 4px; color: #fff; }
.evento-item.acesso .evento-tipo { background: #10b981; color: #0f172a; }
.evento-item.alarme .evento-tipo { background: #ef4444; }
.evento-hora { font-size: 0.65rem; color: #94a3b8; font-family: monospace; }
.evento-msg { font-size: 0.82rem; color: #cbd5e1; }
.legenda-cal { display: flex; gap: 12px; justify-content: center; margin-top: 8px; font-size: 0.65rem; color: #94a3b8; flex-wrap: wrap; }
.legenda-cal span { display: flex; align-items: center; gap: 5px; }
.legenda-cal .cx { width: 10px; height: 10px; border-radius: 2px; display: inline-block; }

.vazio { text-align: center; padding: 40px 20px; color: #64748b; background: #1e293b; border-radius: 8px; }
.aviso-ntp { background: #f59e0b; color: #000; padding: 8px 12px; border-radius: 6px; margin-bottom: 10px; font-size: 0.72rem; font-weight: 600; display: none; }
.aviso-ntp.visivel { display: block; }
.aviso-buffer { background: #f59e0b; color: #000; padding: 8px 14px; border-radius: 8px; margin: 0 auto 12px; max-width: 900px; font-size: 0.75rem; font-weight: 600; display: none; }
.aviso-buffer.visivel { display: block; }
)CSS";

// ============================================================================
//  PAGINA LOGIN
// ============================================================================
const char PAGINA_LOGIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,viewport-fit=cover"><meta http-equiv="Cache-Control" content="no-cache"><meta name="theme-color" content="#0f172a"><title>Login</title>
<style>*{box-sizing:border-box}html,body{height:100%}body{font-family:'Inter',-apple-system,sans-serif;background:linear-gradient(160deg,#0f172a,#1e3a5f);color:#e2e8f0;margin:0;padding:16px;display:flex;justify-content:center;align-items:center;min-height:100vh}.card{background:#1e293b;padding:28px 24px;border-radius:16px;width:100%;max-width:380px;box-shadow:0 8px 30px rgba(0,0,0,0.5);text-align:center;border:1px solid #334155}.icon{display:flex;justify-content:center;margin-bottom:12px}.icon svg{width:80px;height:80px}h2{color:#f1f5f9;margin:8px 0 20px;font-size:1.3rem}input{width:100%;padding:14px 16px;margin:8px 0;border:1px solid #334155;border-radius:10px;background:#0f172a;color:#f1f5f9;font-size:16px}button{width:100%;padding:14px;margin-top:12px;background:#3b82f6;color:#fff;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;min-height:48px}.erro{color:#ef4444;min-height:20px;margin:10px 0 0;font-size:0.9rem}.nome-porta{color:#38bdf8;font-size:0.85rem;margin-top:16px;opacity:0.8}</style></head>
<body><div class="card"><div class="icon">__SVG_ICONE__</div><h2>Acesso ao Sistema</h2><form method="POST" action="/login"><input type="text" name="usuario" placeholder="Usuário" required><input type="password" name="senha" placeholder="Senha" required><button type="submit">Entrar</button></form><p class="erro">__MENSAGEM_ERRO__</p><div class="nome-porta">__NOME_PORTA__</div></div></body></html>
)rawliteral";

// ============================================================================
//  PAGINA PAINEL
// ============================================================================
const char PAGINA_PAINEL[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,viewport-fit=cover"><meta http-equiv="Cache-Control" content="no-cache"><meta name="theme-color" content="#0f172a"><title>Painel</title><style>__CSS__</style></head><body>

<div class="header"><div class="header-esq">__SVG_ICONE__<div class="header-info"><h1 id="titulo-porta">__NOME_PORTA__</h1><div class="user">Logado como <b>__TIPO_USUARIO__</b></div></div></div>
<div class="header-dir">
<a href="/calendario" id="btn-calendario" class="btn-icon calendar" style="display:none;" title="Calendário">&#128197;</a>
<a href="/logs" id="btn-logs" class="btn-icon" style="display:none;" title="Logs">&#128203;</a>
<button id="btn-maint" class="btn-icon maint" onclick="toggleManutencao()" style="display:none;" title="Modo manutenção">&#128679;</button>
<a href="/admin" class="btn-icon settings" title="Configurações">&#9881;</a>
<button class="btn-icon btn-sair-icon" onclick="logout()" title="Sair"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/></svg></button>
</div></div>

<div class="painel door-painel"><div class="door-wrapper">
<div class="sensor-group sensor-group-esq" id="sensor-group-esq"><div class="sensor-lateral" id="sensor-esq"></div><span class="sensor-label">ESQ</span></div>
<div class="door-frame" id="door-frame" data-maint="false" data-bypass="false" data-aberta="false">
  <div class="door-box"></div>
  <div class="door-slats" id="door-slats"></div>
  
  <!-- SVG DE MANUTENÇÃO REFINADO -->
  <div class="manutencao-overlay" id="manutencao-overlay">
    <svg viewBox="0 0 300 200" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <pattern id="listras" width="20" height="20" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
          <rect width="10" height="20" fill="#facc15"/>
          <rect x="10" width="10" height="20" fill="#0f172a"/>
        </pattern>
      </defs>

      <!-- Sombra no chão reduzida -->
      <ellipse cx="150" cy="185" rx="100" ry="8" fill="rgba(0,0,0,0.15)" />

      <!-- Postes (Cavaletes) -->
      <path d="M 75 80 L 85 80 L 95 180 L 65 180 Z" fill="#1e293b" stroke="#0f172a" stroke-width="2" stroke-linejoin="round"/>
      <path d="M 215 80 L 225 80 L 235 180 L 205 180 Z" fill="#1e293b" stroke="#0f172a" stroke-width="2" stroke-linejoin="round"/>

      <!-- Barras Horizontais -->
      <rect x="40" y="100" width="220" height="28" rx="4" fill="url(#listras)" stroke="#0f172a" stroke-width="2"/>
      <rect x="40" y="140" width="220" height="28" rx="4" fill="url(#listras)" stroke="#0f172a" stroke-width="2"/>

      <!-- Luzes de Alerta -->
      <rect x="70" y="80" width="10" height="15" fill="#1e293b"/>
      <circle cx="75" cy="70" r="12" fill="#fff" stroke="#0f172a" stroke-width="3"/>
      <rect x="220" y="80" width="10" height="15" fill="#1e293b"/>
      <circle cx="225" cy="70" r="12" fill="#fff" stroke="#0f172a" stroke-width="3"/>

      <!-- Placa de Aviso -->
      <g transform="translate(210, 120) rotate(10)">
        <polygon points="0,-25 25,20 -25,20" fill="#ef4444" stroke="#fff" stroke-width="3" stroke-linejoin="round"/>
        <rect x="-4" y="-10" width="8" height="16" rx="4" fill="#fff"/>
        <circle cx="0" cy="12" r="4" fill="#fff"/>
      </g>

      <!-- Cone em Pé -->
      <g transform="translate(140, 170)">
        <polygon points="0,-20 12,10 -12,10" fill="#f97316" stroke="#ea580c" stroke-width="1.5" stroke-linejoin="round"/>
        <polygon points="-3,-5 3,-5 6,5 -6,5" fill="#fff"/>
        <rect x="-15" y="10" width="30" height="6" rx="2" fill="#f97316" stroke="#ea580c" stroke-width="1.5"/>
      </g>

      <!-- Cone Caído -->
      <g transform="translate(80, 175) rotate(-70)">
        <polygon points="0,-20 12,10 -12,10" fill="#f97316" stroke="#ea580c" stroke-width="1.5" stroke-linejoin="round"/>
        <polygon points="-3,-5 3,-5 6,5 -6,5" fill="#fff"/>
        <rect x="-15" y="10" width="30" height="6" rx="2" fill="#f97316" stroke="#ea580c" stroke-width="1.5"/>
      </g>
    </svg>
  </div>
</div>
<div class="sensor-group sensor-group-dir" id="sensor-group-dir"><div class="sensor-lateral" id="sensor-dir"></div><span class="sensor-label">DIR</span></div>
<div class="motor-icon bloqueado" id="motor-icon" title="Estado do motor">
  <svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
    <line x1="14" y1="6"  x2="14" y2="11"/>
    <line x1="19" y1="6"  x2="19" y2="11"/>
    <line x1="24" y1="6"  x2="24" y2="11"/>
    <line x1="29" y1="6"  x2="29" y2="11"/>
    <line x1="34" y1="6"  x2="34" y2="11"/>
    <rect x="10" y="11" width="28" height="18" rx="2"/>
    <line x1="14" y1="11" x2="14" y2="29"/>
    <line x1="34" y1="11" x2="34" y2="29"/>
    <line x1="14" y1="15" x2="34" y2="15"/>
    <line x1="14" y1="20" x2="34" y2="20"/>
    <line x1="14" y1="25" x2="34" y2="25"/>
    <line x1="14" y1="29" x2="14" y2="34"/>
    <line x1="19" y1="29" x2="19" y2="34"/>
    <line x1="24" y1="29" x2="24" y2="34"/>
    <line x1="29" y1="29" x2="29" y2="34"/>
    <line x1="34" y1="29" x2="34" y2="34"/>
    <line x1="38" y1="20" x2="42" y2="20"/>
    <circle cx="42.5" cy="20" r="2"/>
    <line x1="6" y1="20" x2="10" y2="20"/>
    <line x1="12" y1="34" x2="12" y2="40"/>
    <line x1="36" y1="34" x2="36" y2="40"/>
    <line x1="8"  y1="40" x2="40" y2="40"/>
  </svg>
</div>
</div></div>

<div class="ultimo-evento" id="ultimo-evento">
  <div class="ue-icone" id="ue-icone">&#9888;</div>
  <div class="ue-conteudo">
    <div class="ue-topo">
      <span class="ue-tipo" id="ue-tipo">--</span>
      <span class="ue-tempo" id="ue-tempo">--</span>
    </div>
    <div class="ue-msg vazio" id="ue-msg">Nenhum acesso ou alarme registrado ainda.</div>
  </div>
</div>

<div class="grid">
<div class="card-status" id="card-esq"><div class="icone">&#128274;</div><div class="titulo">Trava Esquerda</div><div class="valor" id="trava-esq">--</div></div>
<div class="card-status" id="card-dir"><div class="icone">&#128274;</div><div class="titulo">Trava Direita</div><div class="valor" id="trava-dir">--</div></div>
<div class="card-status" id="card-bypass">
  <div class="icone icone-contato aberto" id="bypass-contato" title="Bypass desativado">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <circle cx="6" cy="18" r="2.5"/>
      <circle cx="18" cy="18" r="2.5"/>
      <path d="M8 16.5 L16 8"/>
    </svg>
  </div>
  <div class="titulo">Bypass</div>
  <div class="valor" id="bypass-valor">--</div>
</div>
<div class="card-status" id="card-modo"><div class="icone">&#9881;</div><div class="titulo">Modo</div><div class="valor" id="modo">NORMAL</div></div>
</div>

<div class="painel wifi"><h3>&#128246; Configuração Wi-Fi</h3><div class="linha">
<div class="wifi-status-linha"><span class="lbl">Status</span><span class="val" id="wifi-status"><span class="dot vermelho"></span>Desconectado</span></div>
<div class="wifi-status-linha"><span class="lbl">Rede ativa</span><span class="val" id="wifi-ssid-config">--</span></div>
<div class="wifi-status-linha"><span class="lbl">IP externo (STA)</span><span class="val" id="wifi-ip">--</span></div>
<div class="wifi-status-linha"><span class="lbl">IP do Access Point</span><span class="val" id="wifi-ap-ip">192.168.4.1</span></div>
<div class="wifi-status-linha"><span class="lbl">MAC Address (AP)</span><span class="val" id="mac-address">--</span></div>
</div></div>

<div class="toast" id="toast"></div>
<script>
var TIPO_USUARIO="__TIPO_USUARIO__";
var isAdmin=(TIPO_USUARIO==='admin');
if(isAdmin){
  document.getElementById('btn-logs').style.display='inline-flex';
  document.getElementById('btn-maint').style.display='inline-flex';
  document.getElementById('btn-calendario').style.display='inline-flex';
}
document.getElementById('wifi-ap-ip').textContent="__AP_IP__";
document.getElementById('mac-address').textContent="__MAC__";

var SVG_CONTATO_ABERTO = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="18" r="2.5"/><circle cx="18" cy="18" r="2.5"/><path d="M8 16.5 L16 8"/></svg>';
var SVG_CONTATO_FECHADO = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="18" r="2.5"/><circle cx="18" cy="18" r="2.5"/><path d="M8 18 L16 18"/></svg>';

var toastTimer=null;
function toast(msg,erro){var el=document.getElementById('toast');el.textContent=msg;el.className='toast visivel'+(erro?' erro':'');clearTimeout(toastTimer);toastTimer=setTimeout(function(){el.className='toast';},3000);}

function setText(el, txt) { if (el && el.textContent !== txt) el.textContent = txt; }
function setClass(el, cls) { if (el && el.className !== cls) el.className = cls; }

// ============================================================================
//  CONTROLE DE CORES DO FRAME (VIA JS INLINE)
//  Prioridade: Bypass > Fechado > Manutenção(Aberto) > Aberto
// ============================================================================
function atualizarFundoFrame() {
  var frame = document.getElementById('door-frame');
  if(!frame) return;

  var isMaint = frame.dataset.maint === 'true';
  var isBypass = frame.dataset.bypass === 'true';
  var isAberta = frame.dataset.aberta === 'true';

  if (isBypass) {
    frame.style.setProperty('background-color', '#E0E0E0', 'important'); // Branco
  } else if (!isAberta) {
    frame.style.setProperty('background-color', '#0a0f1a', 'important'); // Preto
  } else if (isMaint) {
    frame.style.setProperty('background-color', '#a6a6a6', 'important'); // Cinza
  } else {
    frame.style.setProperty('background-color', '#E0E0E0', 'important'); // Branco
  }
}

var ultimoEventoTempoAtual = "";
var ultimoEventoTipoAtual  = "";
function atualizarUltimoEvento(tempo, tipo, msg){
  if(!tempo || !tipo){ return; }
  if(tempo === ultimoEventoTempoAtual && tipo === ultimoEventoTipoAtual){ return; }
  ultimoEventoTempoAtual = tempo;
  ultimoEventoTipoAtual = tipo;

  var box  = document.getElementById('ultimo-evento');
  var ic   = document.getElementById('ue-icone');
  var tp   = document.getElementById('ue-tipo');
  var tm   = document.getElementById('ue-tempo');
  var ms   = document.getElementById('ue-msg');
  if(!box) return;

  var cls = (tipo === 'ACESSO') ? 'acesso' : 'alarme';
  box.className = 'ultimo-evento ' + cls;
  ic.innerHTML  = (cls === 'acesso') ? '&#9989;' : '&#9888;';
  tp.textContent = tipo;
  tm.textContent = tempo;
  ms.textContent = msg || '';
  ms.title = msg || '';
  ms.classList.remove('vazio');
}

function aplicarEstado(d){
  if(!d) return;
  try{
    var frame = document.getElementById('door-frame');
    var atualizarFundo = false;

    if(typeof d.trava_esquerda==='string'){
      setText(document.getElementById('trava-esq'), d.trava_esquerda.toUpperCase());
      setClass(document.getElementById('card-esq'), 'card-status '+d.trava_esquerda);
    }
    if(typeof d.trava_direita==='string'){
      setText(document.getElementById('trava-dir'), d.trava_direita.toUpperCase());
      setClass(document.getElementById('card-dir'), 'card-status '+d.trava_direita);
    }
    if(typeof d.motor_liberado==='boolean'){
      var mi=document.getElementById('motor-icon');
      if(mi){
        mi.classList.toggle('liberado', d.motor_liberado);
        mi.classList.toggle('bloqueado', !d.motor_liberado);
        mi.classList.toggle('pulsando', d.motor_liberado);
        mi.title = d.motor_liberado ? 'Motor LIBERADO' : 'Motor BLOQUEADO';
      }
    }
    if(typeof d.modo_manutencao==='boolean'){
      setText(document.getElementById('modo'), d.modo_manutencao?'MANUTENÇÃO':'NORMAL');
      setClass(document.getElementById('card-modo'), 'card-status '+(d.modo_manutencao?'manutencao':'normal'));
      var bm=document.getElementById('btn-maint');
      if(bm) bm.classList.toggle('ativo', d.modo_manutencao);
      var ov=document.getElementById('manutencao-overlay');
      if(ov) ov.classList.toggle('ativo', d.modo_manutencao);
      
      if(frame) { frame.dataset.maint = d.modo_manutencao; atualizarFundo = true; }
    }
    if(typeof d.bypass_ativo==='boolean'){
      var ativo = d.bypass_ativo;
      setText(document.getElementById('bypass-valor'), ativo ? 'ATIVADO' : 'DESATIVADO');
      setClass(document.getElementById('card-bypass'), 'card-status ' + (ativo ? 'bypass-on' : 'bypass-off'));
      var ct=document.getElementById('bypass-contato');
      if(ct){
        ct.className = 'icone icone-contato ' + (ativo ? 'fechado' : 'aberto');
        ct.innerHTML = ativo ? SVG_CONTATO_FECHADO : SVG_CONTATO_ABERTO;
        ct.title = ativo ? 'Bypass ATIVO (contato fechado)' : 'Bypass desativado (contato aberto)';
      }
      
      if(frame) { frame.dataset.bypass = ativo; atualizarFundo = true; }
    }
    if(typeof d.ultimo_evento_tempo === 'string' && d.ultimo_evento_tempo.length > 0){
      atualizarUltimoEvento(d.ultimo_evento_tempo, d.ultimo_evento_tipo, d.ultimo_evento_msg);
    }
    if(typeof d.trava_esquerda==='string'&&typeof d.trava_direita==='string'){
      var esqA=d.trava_esquerda==='aberta', dirA=d.trava_direita==='aberta', ambas=esqA&&dirA;
      var slats=document.getElementById('door-slats');
      if(slats) slats.classList.toggle('aberta',ambas);
      if(frame) {
        frame.classList.toggle('pronta',ambas&&!d.modo_manutencao);
        frame.dataset.aberta = ambas;
        atualizarFundo = true;
      }
      setClass(document.getElementById('sensor-esq'), 'sensor-lateral '+(esqA?'verde':'vermelho'));
      setClass(document.getElementById('sensor-group-esq'), 'sensor-group sensor-group-esq '+(esqA?'verde':'vermelho'));
      setClass(document.getElementById('sensor-dir'), 'sensor-lateral '+(dirA?'verde':'vermelho'));
      setClass(document.getElementById('sensor-group-dir'), 'sensor-group sensor-group-dir '+(dirA?'verde':'vermelho'));
    }
    
    // Aplica a cor com prioridade correta
    if (atualizarFundo) atualizarFundoFrame();

    if(d.wifi_conectado!==undefined || d.wifi_ssid!==undefined){
      var st=document.getElementById('wifi-status');
      if(st){
        if(d.wifi_conectado) st.innerHTML='<span class="dot verde"></span>Conectado';
        else if(d.wifi_ssid) st.innerHTML='<span class="dot amarelo"></span>Desconectado';
        else st.innerHTML='<span class="dot vermelho"></span>Não configurado';
      }
    }
    if(d.wifi_ssid!==undefined){ setText(document.getElementById('wifi-ssid-config'), d.wifi_ssid||'--'); }
    if(d.wifi_ip!==undefined){ setText(document.getElementById('wifi-ip'), d.wifi_ip||'--'); }
    if(d.mac){ setText(document.getElementById('mac-address'), d.mac); }
    if(d.nome_porta){
      var tt=document.getElementById('titulo-porta');
      if(tt&&tt.textContent!==d.nome_porta){ tt.textContent=d.nome_porta; document.title=d.nome_porta; }
    }
  }catch(e){ console.error('aplicarEstado',e); }
}

function aplicarAlerta(lado){
  if(lado==='esq'||lado==='ambos'){var s=document.getElementById('sensor-esq'); if(s){s.classList.add('alerta'); setTimeout(function(){s.classList.remove('alerta');},3000);}}
  if(lado==='dir'||lado==='ambos'){var s=document.getElementById('sensor-dir'); if(s){s.classList.add('alerta'); setTimeout(function(){s.classList.remove('alerta');},3000);}}
}

var ws = null;
var wsState = 0;
var reconectarTimer = null;
var ultimoRecebimento = 0;

function conectarWS(){
  if (wsState === 1 || wsState === 2) return;
  wsState = 1;
  var novoWs;
  try { novoWs = new WebSocket('ws://' + location.hostname + ':81/'); }
  catch(e) { wsState = 0; clearTimeout(reconectarTimer); reconectarTimer = setTimeout(conectarWS, 2000); return; }

  novoWs.onopen = function(){
    if (ws && ws !== novoWs && ws.readyState <= 1) { try { ws.close(); } catch(e){} }
    ws = novoWs;
    wsState = 2;
    ultimoRecebimento = Date.now();
    try { novoWs.send(JSON.stringify({comando:"status"})); } catch(e){}
  };

  novoWs.onmessage = function(event){
    if (ws !== novoWs) return;
    ultimoRecebimento = Date.now();
    try {
      var d = JSON.parse(event.data);
      if (d.evento === 'novo_acesso_alarme') {
        atualizarUltimoEvento(d.tempo, d.tipo, d.mensagem);
        return;
      }
      if (d.evento === 'alerta_trava') { aplicarAlerta(d.lado); return; }
      aplicarEstado(d);
    } catch(e) { console.warn('WS JSON invalido', e); }
  };

  novoWs.onerror = function(){ try { novoWs.close(); } catch(e){} };

  novoWs.onclose = function(){
    if (ws !== novoWs) return;
    ws = null;
    wsState = 0;
    clearTimeout(reconectarTimer);
    reconectarTimer = setTimeout(conectarWS, 2000);
  };
}

conectarWS();

setInterval(function(){
  if (wsState !== 2) { conectarWS(); return; }
  if (Date.now() - ultimoRecebimento > 8000) {
    if (ws) { try { ws.close(); } catch(e){} }
    ws = null; wsState = 0; return;
  }
  try { ws.send(JSON.stringify({comando:"status"})); } catch(e){}
}, 4000);

function toggleManutencao(){fetch('/manutencao',{method:'POST'}).then(r=>r.json()).then(d=>toast(d.mensagem,!d.ok)).catch(()=>toast('Erro',true));}
function logout(){window.location.href='/logout';}
</script></body></html>
)rawliteral";

// ============================================================================
//  PAGINA ADMIN
// ============================================================================
const char PAGINA_ADMIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,viewport-fit=cover"><meta http-equiv="Cache-Control" content="no-cache"><meta name="theme-color" content="#0f172a"><title>Configurações</title><style>__CSS__</style></head><body>
<div class="header"><div class="header-esq">__SVG_ICONE__<div class="header-info"><h1 id="admin-title">Configurações</h1><div class="user">Logado como <b>__TIPO_USUARIO__</b></div></div></div>
<div class="header-dir"><a href="/painel" class="btn-voltar">&#8592; Voltar</a></div></div>
<div style="height:76px;"></div>

<div class="painel" id="admin-only" style="display:none;"><h3>&#9881; Administração</h3>
<div class="linha"><label>Nome da porta</label><div style="display:flex;gap:6px;flex-wrap:wrap;"><input type="text" id="novo-nome-porta" maxlength="40" style="flex:1;min-width:150px;"><button class="acao" onclick="alterarNome()">Salvar</button></div></div>
<div class="linha"><label>Nova senha do Admin</label><div style="display:flex;gap:6px;flex-wrap:wrap;"><input type="password" id="nova-senha-admin" placeholder="Mínimo 4 caracteres" style="flex:1;min-width:150px;"><button class="acao" onclick="alterarSenha('admin')">Alterar</button></div></div>
<div class="linha"><label>Nova senha do Visualizador</label><div style="display:flex;gap:6px;flex-wrap:wrap;"><input type="password" id="nova-senha-vis" placeholder="Mínimo 4 caracteres" style="flex:1;min-width:150px;"><button class="acao" onclick="alterarSenha('visualizador')">Alterar</button></div></div>

<!-- CAIXA UNIFICADA: Prefixo e Senha do AP (Restrita ao Admin) -->
<div class="linha">
  <label>Configurações do Access Point (AP)</label>
  <div style="display:flex; flex-direction:column; gap:8px;">
    <input type="text" id="prefixo-ap" maxlength="24" placeholder="Prefixo do SSID (ex: PortaEnrolar)" style="width:100%;">
    <input type="password" id="nova-senha-ap" maxlength="32" placeholder="Senha do AP (mín. 8 caracteres)" style="width:100%;">
    <button class="acao verde" onclick="salvarConfiguracoesAP()" style="width:100%;">Salvar Configurações do AP</button>
  </div>
  <div style="background:#0f172a;border:1px solid #334155;padding:10px;border-radius:8px;margin-top:8px;font-size:0.72rem;color:#94a3b8;line-height:1.45;">
    O SSID final será <b>PREFIXO-SUFIXOMAC</b> (ex.: <span style="font-family:monospace;">__SSID_AP__</span>).<br>
    <b style="color:#f59e0b;">&#9888;</b> Alterar o prefixo ou a senha <b>requer reinicialização</b> para aplicar.
  </div>
</div>
</div>

<div class="painel" id="sensor-box" style="display:none;">
  <h3>&#128268; Sensores de Trava (NA/NF)</h3>
  <div class="linha">
    <label>Trava Esquerda (GPIO 32)</label>
    <select id="tipo-esq">
      <option value="0">NA — Normalmente Aberto (atua em LOW)</option>
      <option value="1">NF — Normalmente Fechado (atua em HIGH)</option>
    </select>
  </div>
  <div class="linha">
    <label>Trava Direita (GPIO 33)</label>
    <select id="tipo-dir">
      <option value="0">NA — Normalmente Aberto (atua em LOW)</option>
      <option value="1">NF — Normalmente Fechado (atua em HIGH)</option>
    </select>
  </div>
  <div class="linha">
    <button class="acao verde" onclick="salvarTiposTravas()" style="width:100%;">Salvar configurações</button>
  </div>
  <div style="background:#0f172a;border:1px solid #334155;padding:10px;border-radius:8px;font-size:0.72rem;color:#94a3b8;line-height:1.45;">
    <b style="color:#f59e0b;">&#9888; Atenção:</b> a mudança é aplicada imediatamente e persistida em NVS (sobrevive a reboot).<br><br>
    <b>NA:</b> contato fecha para GND quando a trava atua → GPIO vai para LOW = "aberta".<br>
    <b>NF:</b> contato abre do GND quando a trava atua → GPIO vai para HIGH = "aberta".
  </div>
</div>

<div class="painel" id="bypass-box" style="display:none;">
  <h3>&#128273; Bypass do Sistema</h3>
  <div class="linha">
    <div class="wifi-status-linha">
      <span class="lbl">Estado atual</span>
      <span class="val" id="bypass-estado"><span class="dot vermelho"></span>Desativado</span>
    </div>
    <div style="background:#0f172a;border:1px solid #334155;padding:10px;border-radius:8px;margin-top:8px;font-size:0.72rem;color:#94a3b8;line-height:1.45;">
      Quando <b style="color:#f59e0b;">ATIVADO</b>, o motor é liberado independentemente das travas
      (modo serviço). Pode ser alternado pelo <b>botão físico GPIO 23</b> ou por este botão.
    </div>
  </div>
  <div class="linha">
    <button class="acao laranja" id="btn-bypass" onclick="toggleBypass()" style="width:100%;">
      Ativar Bypass
    </button>
  </div>
</div>

<div class="painel wifi"><h3>&#128246; Configuração Wi-Fi</h3>
<div class="linha">
<div class="wifi-status-linha"><span class="lbl">Status</span><span class="val" id="wifi-status"><span class="dot vermelho"></span>Desconectado</span></div>
<div class="wifi-status-linha"><span class="lbl">Rede ativa</span><span class="val" id="wifi-ssid-config">--</span></div>
<div class="wifi-status-linha"><span class="lbl">IP externo (STA)</span><span class="val" id="wifi-ip">--</span></div>
<div class="wifi-status-linha"><span class="lbl">AP SSID </span><span class="val" id="wifi-ssid-ap">--</span></div>
<div class="wifi-status-linha"><span class="lbl">IP do Access Point</span><span class="val" id="wifi-ap-ip">192.168.4.1</span></div>
<div class="wifi-status-linha"><span class="lbl">MAC Address (AP)</span><span class="val" id="mac-address">--</span></div>
</div>

<div class="linha"><label>Redes salvas</label><div style="display:flex;gap:6px;flex-wrap:wrap;">
<select id="lista-redes" onchange="selecionouRede()" style="flex:1;min-width:150px;"></select>
<button class="acao verde" id="btn-conectar" style="display:none;" onclick="conectarSelecionada()">Conectar</button></div>
<a href="#" id="link-esquecer" onclick="esquecerSelecionada(event)" class="acao-link vermelho" style="margin-top:6px;">&#128465; Esquecer rede selecionada</a></div>
<div class="nova-rede-box" id="box-nova" style="display:none;">
<div style="margin-bottom:8px;"><label style="font-size:0.7rem;">SSID</label><input type="text" id="novo-ssid" maxlength="32"></div>
<div style="margin-bottom:8px;"><label style="font-size:0.7rem;">Senha</label><input type="password" id="nova-senha-wifi"></div>
<button class="acao" onclick="adicionarNovaRede()" style="width:100%;">Salvar e conectar</button></div>
<div class="linha" id="bloco-conectado" style="display:none;"><label>Desconectar</label><a href="/api/wifi/desconectar-get" class="acao-link laranja">&#128268; Desconectar do Wi-Fi externo</a></div>
<div style="background:#0f172a;border:1px solid #334155;padding:10px;border-radius:8px;margin-top:10px;font-size:0.7rem;color:#94a3b8;line-height:1.4;">
<b style="color:#f59e0b;">&#9888; Importante:</b> Ao esquecer uma rede, o ESP32 reinicia. O AP <b>__SSID_AP__</b> volta em ~10s.<br><br>
<b style="color:#38bdf8;">&#128161; Botão físico:</b> GPIO 4 por 3s esquece a última rede. GPIO 13 por 3s reinicia a placa. GPIO 23 alterna o bypass.</div>
</div>
<div class="toast" id="toast"></div>
<script>
var TIPO_USUARIO="__TIPO_USUARIO__";var isAdmin=(TIPO_USUARIO==='admin');var listaRedesCache=[];var estadoAtual={wifi_conectado:false,wifi_ssid:''};
var bypassEstadoAtual=false;
if(isAdmin){
  document.getElementById('admin-title').textContent='Painel Administrativo';
  document.getElementById('admin-only').style.display='block';
  document.getElementById('novo-nome-porta').value="__NOME_PORTA__";
  document.getElementById('bypass-box').style.display='block';
  document.getElementById('sensor-box').style.display='block';
  document.getElementById('prefixo-ap').value="__PREFIXO_AP__";
}
document.getElementById('wifi-ap-ip').textContent="__AP_IP__";
document.getElementById('mac-address').textContent="__MAC__";
document.getElementById('wifi-ssid-ap').textContent="__SSID_AP__";
var toastTimer=null;function toast(msg,erro){var el=document.getElementById('toast');el.textContent=msg;el.className='toast visivel'+(erro?' erro':'');clearTimeout(toastTimer);toastTimer=setTimeout(function(){el.className='toast';},3000);}

function atualizarUIBypass(){
  var el = document.getElementById('bypass-estado');
  var b  = document.getElementById('btn-bypass');
  if(!el || !b) return;
  if(bypassEstadoAtual){
    el.innerHTML = '<span class="dot verde"></span>ATIVADO (motor liberado)';
    b.textContent = 'Desativar Bypass';
    b.className = 'acao verde';
  } else {
    el.innerHTML = '<span class="dot vermelho"></span>Desativado (normal)';
    b.textContent = 'Ativar Bypass';
    b.className = 'acao laranja';
  }
}
function toggleBypass(){
  fetch('/bypass', { method:'POST' })
    .then(r => r.json())
    .then(d => {
      toast(d.mensagem, !d.ok);
      if(d.ok){ bypassEstadoAtual = !bypassEstadoAtual; atualizarUIBypass(); }
    })
    .catch(()=>toast('Erro', true));
}

function salvarTiposTravas(){
  var te = parseInt(document.getElementById('tipo-esq').value, 10);
  var td = parseInt(document.getElementById('tipo-dir').value, 10);
  fetch('/sensor-tipo', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({tipo_esq:te, tipo_dir:td})
  })
  .then(r => r.json())
  .then(d => toast(d.mensagem, !d.ok))
  .catch(() => toast('Erro', true));
}

function salvarConfiguracoesAP(){
  var p = document.getElementById('prefixo-ap').value.trim();
  var s = document.getElementById('nova-senha-ap').value;
  if(p.length < 1 || p.length > 24){ toast('Prefixo deve ter 1 a 24 caracteres', true); return; }
  if(s.length < 8 || s.length > 32){ toast('Senha deve ter 8 a 32 caracteres', true); return; }
  if(!confirm('Salvar configurações do AP e REINICIAR a placa?')) return;
  fetch('/alterar-ap-config', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({prefixo:p, senha:s})
  })
  .then(r => r.json())
  .then(d => toast(d.mensagem, !d.ok))
  .catch(() => toast('Erro', true));
}

function atualizarWiFi(d){estadoAtual.wifi_conectado=d.wifi_conectado;estadoAtual.wifi_ssid=d.wifi_ssid;
var st=document.getElementById('wifi-status');var ss=document.getElementById('wifi-ssid-config');var ip=document.getElementById('wifi-ip');var bloco=document.getElementById('bloco-conectado');
if(st){if(d.wifi_conectado)st.innerHTML='<span class="dot verde"></span>Conectado';else if(d.wifi_ssid)st.innerHTML='<span class="dot amarelo"></span>Desconectado';else st.innerHTML='<span class="dot vermelho"></span>Não configurado';}
if(ss)ss.textContent=d.wifi_ssid||'--';if(ip)ip.textContent=d.wifi_ip||'--';if(d.mac)document.getElementById('mac-address').textContent=d.mac;
if(d.ssid_ap)document.getElementById('wifi-ssid-ap').textContent=d.ssid_ap;
if(bloco)bloco.style.display=d.wifi_conectado?'block':'none';atualizarBotaoConectar();
if(typeof d.bypass_ativo === 'boolean' && d.bypass_ativo !== bypassEstadoAtual){
  bypassEstadoAtual = d.bypass_ativo;
  atualizarUIBypass();
}
if(typeof d.tipo_trava_esq === 'number'){
  var te = document.getElementById('tipo-esq');
  if(te && parseInt(te.value,10) !== d.tipo_trava_esq) te.value = String(d.tipo_trava_esq);
}
if(typeof d.tipo_trava_dir === 'number'){
  var td = document.getElementById('tipo-dir');
  if(td && parseInt(td.value,10) !== d.tipo_trava_dir) td.value = String(d.tipo_trava_dir);
}
if(typeof d.prefixo_ap === 'string' && document.getElementById('prefixo-ap')){
  var inp=document.getElementById('prefixo-ap');
  if(document.activeElement !== inp && inp.value !== d.prefixo_ap) inp.value = d.prefixo_ap;
}
}
function carregarLista(){fetch('/api/wifi/lista').then(r=>r.json()).then(d=>{listaRedesCache=d.redes||[];var sel=document.getElementById('lista-redes');var ant=sel.value;sel.innerHTML='';
for(var i=0;i<listaRedesCache.length;i++){var o=document.createElement('option');o.value='r'+i;o.textContent=listaRedesCache[i]+(d.ativo===listaRedesCache[i]?' ✓':'');sel.appendChild(o);}
var on=document.createElement('option');on.value='nova';on.textContent='➕ Adicionar nova rede...';sel.appendChild(on);
if(ant&&ant.startsWith('r')){var ia=parseInt(ant.substring(1));sel.value=(ia<listaRedesCache.length)?ant:(listaRedesCache.length>0?'r0':'nova');}
else if(listaRedesCache.length>0&&d.ativo){var achou=false;for(var i=0;i<listaRedesCache.length;i++){if(listaRedesCache[i]===d.ativo){sel.value='r'+i;achou=true;break;}}if(!achou)sel.value='r0';}
else if(listaRedesCache.length>0)sel.value='r0';else sel.value='nova';
selecionouRede();}).catch(function(){});}
function selecionouRede(){var v=document.getElementById('lista-redes').value;var box=document.getElementById('box-nova');var lk=document.getElementById('link-esquecer');
if(v==='nova'){box.style.display='block';lk.style.display='none';}else{box.style.display='none';lk.style.display='block';}atualizarBotaoConectar();}
function atualizarBotaoConectar(){var v=document.getElementById('lista-redes').value;var b=document.getElementById('btn-conectar');
if(v==='nova'||!v){b.style.display='none';return;}var i=parseInt(v.substring(1));var s=listaRedesCache[i];
b.style.display=(!estadoAtual.wifi_conectado||s!==estadoAtual.wifi_ssid)?'inline-block':'none';}

var ws=null, wsState=0, rt=null, ultimoReceb=0;
function conectarWS(){
  if(wsState===1||wsState===2) return;
  wsState=1;
  var novoWs;
  try{ novoWs=new WebSocket('ws://'+location.hostname+':81/'); }
  catch(e){ wsState=0; clearTimeout(rt); rt=setTimeout(conectarWS,2000); return; }
  novoWs.onopen=function(){
    if(ws && ws!==novoWs && ws.readyState<=1){ try{ ws.close(); }catch(e){} }
    ws=novoWs; wsState=2; ultimoReceb=Date.now();
    try{ novoWs.send(JSON.stringify({comando:"status"})); }catch(e){}
  };
  novoWs.onmessage=function(e){
    if(ws!==novoWs) return;
    ultimoReceb=Date.now();
    try{ var d=JSON.parse(e.data); if(d.evento) return; atualizarWiFi(d); }catch(_){}
  };
  novoWs.onerror=function(){ try{ novoWs.close(); }catch(e){} };
  novoWs.onclose=function(){
    if(ws!==novoWs) return;
    ws=null; wsState=0; clearTimeout(rt); rt=setTimeout(conectarWS,2000);
  };
}
conectarWS();
setInterval(function(){
  if(wsState!==2){ conectarWS(); return; }
  if(Date.now()-ultimoReceb>8000){ if(ws){ try{ ws.close(); }catch(e){} } ws=null; wsState=0; return; }
  try{ ws.send(JSON.stringify({comando:"status"})); }catch(e){}
},4000);
carregarLista();setInterval(carregarLista,15000);

function conectarSelecionada(){var i=parseInt(document.getElementById('lista-redes').value.substring(1));if(isNaN(i)||i<0||i>=listaRedesCache.length){toast('Nenhuma',true);return;}window.location.href='/api/wifi/conectar-rede?idx='+i;}
function esquecerSelecionada(e){if(e)e.preventDefault();var v=document.getElementById('lista-redes').value;if(v==='nova'){toast('Selecione',true);return;}var i=parseInt(v.substring(1));if(!confirm('Esquecer "'+listaRedesCache[i]+'"?'))return;window.location.href='/api/wifi/esquecer-rede?idx='+i;}
function adicionarNovaRede(){var s=document.getElementById('novo-ssid').value.trim();var p=document.getElementById('nova-senha-wifi').value;if(s.length<1){toast('Informe SSID',true);return;}
fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,senha:p})}).then(r=>r.json()).then(d=>{toast(d.mensagem,!d.ok);if(d.ok){document.getElementById('novo-ssid').value='';document.getElementById('nova-senha-wifi').value='';carregarLista();}}).catch(()=>toast('Erro',true));}
function alterarNome(){var n=document.getElementById('novo-nome-porta').value.trim();if(n.length<2){toast('Curto',true);return;}
fetch('/alterar-nome',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({nome:n})}).then(r=>r.json()).then(d=>toast(d.mensagem,!d.ok)).catch(()=>toast('Erro',true));}
function alterarSenha(t){var id=t==='admin'?'nova-senha-admin':'nova-senha-vis';var s=document.getElementById(id).value;if(s.length<4){toast('Curta',true);return;}
fetch('/alterar-senha',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tipo:t,senha:s})}).then(r=>r.json()).then(d=>{toast(d.mensagem,!d.ok);if(d.ok)document.getElementById(id).value='';}).catch(()=>toast('Erro',true));}
</script></body></html>
)rawliteral";

// ============================================================================
//  PAGINA LOGS
// ============================================================================
const char PAGINA_LOGS[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,viewport-fit=cover"><meta http-equiv="Cache-Control" content="no-cache"><meta name="theme-color" content="#0f172a"><title>Logs</title><style>__CSS__</style>
<style>.log-list{max-width:900px;margin:0 auto;padding:0 12px}.log-item{background:#1e293b;border-left:4px solid #334155;border-radius:8px;padding:10px 12px;margin-bottom:6px}.log-item.alarme{border-left-color:#ef4444}.log-item.acesso{border-left-color:#10b981}.log-item.cfg{border-left-color:#f59e0b}.log-item.info{border-left-color:#3b82f6}.log-topo{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:3px;flex-wrap:wrap}.log-tipo{font-size:0.6rem;font-weight:700;text-transform:uppercase;letter-spacing:0.8px;padding:2px 6px;border-radius:4px;background:#334155;color:#f1f5f9}.log-item.alarme .log-tipo{background:#ef4444}.log-item.acesso .log-tipo{background:#10b981;color:#0f172a}.log-item.cfg .log-tipo{background:#f59e0b;color:#0f172a}.log-tempo{font-size:0.65rem;color:#94a3b8;font-family:monospace}.log-msg{font-size:0.82rem;color:#cbd5e1;word-break:break-word}</style></head><body>
<div class="header"><div class="header-esq">__SVG_ICONE__<div class="header-info"><h1>Logs do Sistema</h1><div class="user">__NOME_PORTA__</div></div></div>
<div class="header-dir"><a href="/painel" class="btn-voltar">&#8592; Voltar</a></div></div>
<div class="aviso-buffer" id="aviso-buffer"></div>
<div class="tabs">
<button class="tab ativa" data-tipo="todos" onclick="mudarFiltro('todos',this)">Todos</button>
<button class="tab" data-tipo="ACESSO" onclick="mudarFiltro('ACESSO',this)">Acesso</button>
<button class="tab" data-tipo="ALARME" onclick="mudarFiltro('ALARME',this)">Alarmes</button>
<button class="tab" data-tipo="INFO" onclick="mudarFiltro('INFO',this)">Info</button>
<button class="tab" data-tipo="CFG" onclick="mudarFiltro('CFG',this)">Config</button></div>
<div class="log-list" id="log-list"></div>
<button class="btn-limpar" id="btn-limpar" onclick="limparLogs()">Limpar TODOS os logs</button>
<div class="toast" id="toast"></div>
<script>
var logsCache=[];var filtroAtual='todos';var toastTimer=null;
function toast(msg,erro){var el=document.getElementById('toast');el.textContent=msg;el.className='toast visivel'+(erro?' erro':'');clearTimeout(toastTimer);toastTimer=setTimeout(function(){el.className='toast';},3000);}
function carregarLogs(){fetch('/api/logs').then(r=>{if(!r.ok)throw new Error('negado');return r.json();}).then(d=>{logsCache=d;renderizar();checarBuffer();}).catch(()=>{document.getElementById('log-list').innerHTML='<div class="vazio">Erro ao carregar logs</div>';});}
function checarBuffer(){var av=document.getElementById('aviso-buffer');if(logsCache.length>=200){av.textContent='⚠ Buffer cheio ('+logsCache.length+' logs). Novos logs não serão registrados.';av.classList.add('visivel');}else{av.classList.remove('visivel');}}
function esc(s){if(s===null||s===undefined)return '';return String(s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function renderizar(){var el=document.getElementById('log-list');var f=logsCache.filter(l=>filtroAtual==='todos'||l.tipo===filtroAtual);
if(f.length===0){el.innerHTML='<div class="vazio">Nenhum log registrado nesta categoria</div>';return;}
var html='';for(var i=f.length-1;i>=0;i--){var l=f[i];var c=(l.tipo||'info').toLowerCase();
html+='<div class="log-item '+c+'"><div class="log-topo"><span class="log-tipo">'+esc(l.tipo)+'</span><span class="log-tempo">'+esc(l.tempo)+'</span></div>';
html+='<div class="log-msg">'+esc(l.msg)+'</div></div>';}el.innerHTML=html;}
function atualizarBotaoLimpar(){var b=document.getElementById('btn-limpar');if(filtroAtual==='todos'){b.textContent='Limpar TODOS os logs';b.className='btn-limpar';}else{b.textContent='Limpar apenas '+filtroAtual;b.className='btn-limpar por-categoria';}}
function mudarFiltro(t,el){filtroAtual=t;document.querySelectorAll('.tab').forEach(x=>x.classList.remove('ativa'));el.classList.add('ativa');atualizarBotaoLimpar();renderizar();}
function limparLogs(){var m=(filtroAtual==='todos')?'Apagar TODOS os logs?':'Apagar apenas '+filtroAtual+'?';if(!confirm(m))return;
fetch('/api/logs/limpar',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tipo:filtroAtual})}).then(r=>r.json()).then(d=>{if(d.ok){toast('Apagado!',false);logsCache=[];carregarLogs();}else toast('Erro',true);}).catch(()=>toast('Erro',true));}
carregarLogs();setInterval(carregarLogs,5000);
</script></body></html>
)rawliteral";

// ============================================================================
//  PAGINA CALENDARIO
// ============================================================================
const char PAGINA_CALENDARIO[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="pt-BR"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,viewport-fit=cover"><meta http-equiv="Cache-Control" content="no-cache"><meta name="theme-color" content="#0f172a"><title>Calendário</title><style>__CSS__</style></head><body>
<div class="header"><div class="header-esq">__SVG_ICONE__<div class="header-info"><h1>Calendário</h1><div class="user">__NOME_PORTA__</div></div></div>
<div class="header-dir"><a href="/painel" class="btn-voltar">&#8592; Voltar</a></div></div>
<div style="height:76px;"></div>
<div class="painel cal-painel">
  <h3>&#128197; Histórico</h3>
  <div class="aviso-ntp" id="aviso-ntp">⚠ Relógio não sincronizado. Aguarde NTP ou conecte a uma rede com internet.</div>
  <div class="cal-nav">
    <button onclick="mesAnterior()">&#8249;</button>
    <h2 id="mes-titulo">--</h2>
    <button class="btn-hoje-nav" onclick="irHoje()">Hoje</button>
    <button onclick="proximoMes()">&#8250;</button>
  </div>
  <div class="cal-dias-semana"><span>Dom</span><span>Seg</span><span>Ter</span><span>Qua</span><span>Qui</span><span>Sex</span><span>Sáb</span></div>
  <div class="cal-grid" id="calendario-grid"></div>
  <div class="legenda-cal">
    <span><span class="cx" style="background:#10b981;"></span>Acesso</span>
    <span><span class="cx" style="background:#ef4444;"></span>Alarme</span>
  </div>
</div>
<div class="painel" id="painel-eventos" style="display:none;">
  <h3 id="titulo-eventos">Eventos</h3>
  <div id="lista-eventos"></div>
</div>
<div class="toast" id="toast"></div>
<script>
var eventosPorDia = {};
var mesAtual = new Date().getMonth();
var anoAtual = new Date().getFullYear();
var diaSelecionado = null;
var nomesMeses = ['Janeiro','Fevereiro','Março','Abril','Maio','Junho','Julho','Agosto','Setembro','Outubro','Novembro','Dezembro'];
var toastTimer = null;
function toast(msg, erro) {var el = document.getElementById('toast');el.textContent = msg;el.className = 'toast visivel' + (erro ? ' erro' : '');clearTimeout(toastTimer);toastTimer = setTimeout(function() { el.className = 'toast'; }, 3000);}
function esc(s) {if (s === null || s === undefined) return '';return String(s).replace(/[&<>"']/g, function(c) {return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function verificarNTP() {fetch('/diag').then(r => r.json()).then(d => {var av = document.getElementById('aviso-ntp');if (!d.ntp_sincronizado) av.classList.add('visivel');else av.classList.remove('visivel');}).catch(function() {});}
function parseTempoLog(tempoStr) {if (!tempoStr) return null;if (tempoStr.indexOf('/') < 0) return null;var partes = tempoStr.split(' ');if (partes.length < 2) return null;return { dia: partes[0], hora: partes[1] };}
function carregarEventos() {
  fetch('/api/logs').then(r => {if (!r.ok) throw new Error('HTTP ' + r.status);return r.json();}).then(data => {
    eventosPorDia = {};
    for (var i = 0; i < data.length; i++) {
      var l = data[i];
      if (l.tipo !== 'ACESSO' && l.tipo !== 'ALARME') continue;
      var info = parseTempoLog(l.tempo); if (!info) continue;
      if (!eventosPorDia[info.dia]) eventosPorDia[info.dia] = { acessos: 0, alarmes: 0, eventos: [] };
      if (l.tipo === 'ACESSO') eventosPorDia[info.dia].acessos++; else eventosPorDia[info.dia].alarmes++;
      eventosPorDia[info.dia].eventos.push({ hora: info.hora, tipo: l.tipo, msg: l.msg });
    }
    renderCalendario();
    if (diaSelecionado) mostrarEventos(diaSelecionado);
    verificarNTP();
  }).catch(err => {console.error('Erro:', err);toast('Erro ao carregar eventos', true);});
}
function renderCalendario() {
  document.getElementById('mes-titulo').textContent = nomesMeses[mesAtual] + ' ' + anoAtual;
  var primeiroDia = new Date(anoAtual, mesAtual, 1);
  var ultimoDia = new Date(anoAtual, mesAtual + 1, 0);
  var diaSemanaInicio = primeiroDia.getDay();
  var totalDias = ultimoDia.getDate();
  var html = '';
  for (var i = 0; i < diaSemanaInicio; i++) html += '<div class="dia vazio"></div>';
  for (var d = 1; d <= totalDias; d++) {
    var diaStr = String(d).padStart(2, '0') + '/' + String(mesAtual + 1).padStart(2, '0') + '/' + anoAtual;
    var info = eventosPorDia[diaStr];
    var temAcesso = info && info.acessos > 0;
    var temAlarme = info && info.alarmes > 0;
    var cls = 'dia';
    if (temAcesso && temAlarme) cls += ' com-ambos'; else if (temAlarme) cls += ' com-alarme'; else if (temAcesso) cls += ' com-acesso';
    if (diaSelecionado === diaStr) cls += ' selecionado';
    var dotsHtml = '';
    if (temAcesso || temAlarme) {
      var nGreen = Math.min(info.acessos, 3), nRed = Math.min(info.alarmes, 3);
      dotsHtml = '<div class="dots">';
      for (var k = 0; k < nGreen; k++) dotsHtml += '<span class="dot-g"></span>';
      for (var k = 0; k < nRed; k++) dotsHtml += '<span class="dot-r"></span>';
      dotsHtml += '</div>';
    }
    var tooltip = info ? info.acessos + ' acesso(s), ' + info.alarmes + ' alarme(s)' : 'Sem eventos';
    html += '<div class="' + cls + '" onclick="clicarDia(\'' + diaStr + '\')" title="' + tooltip + '"><span class="num">' + d + '</span>' + dotsHtml + '</div>';
  }
  var celulasFaltando = 42 - (diaSemanaInicio + totalDias);
  for (var i = 0; i < celulasFaltando; i++) html += '<div class="dia vazio"></div>';
  document.getElementById('calendario-grid').innerHTML = html;
}
function clicarDia(diaStr) {diaSelecionado = diaStr;renderCalendario();mostrarEventos(diaStr);}
function mostrarEventos(diaStr) {
  var info = eventosPorDia[diaStr];
  var painel = document.getElementById('painel-eventos'), titulo = document.getElementById('titulo-eventos'), lista = document.getElementById('lista-eventos');
  titulo.textContent = 'Eventos de ' + diaStr; painel.style.display = 'block';
  if (!info || info.eventos.length === 0) {lista.innerHTML = '<div class="vazio">Nenhum acesso ou alarme registrado neste dia</div>';return;}
  var evs = info.eventos.slice(); evs.sort(function(a, b) { return b.hora.localeCompare(a.hora); });
  var html = '<div style="font-size:0.75rem; color:#94a3b8; margin-bottom:8px;">' + info.acessos + ' acesso(s) · ' + info.alarmes + ' alarme(s)</div>';
  for (var i = 0; i < evs.length; i++) {
    var e = evs[i], c = e.tipo.toLowerCase();
    html += '<div class="evento-item ' + c + '"><div class="evento-topo"><span class="evento-tipo">' + e.tipo + '</span><span class="evento-hora">' + e.hora + '</span></div><div class="evento-msg">' + esc(e.msg) + '</div></div>';
  }
  lista.innerHTML = html;
}
function irHoje() {var hoje = new Date();mesAtual = hoje.getMonth();anoAtual = hoje.getFullYear();diaSelecionado = String(hoje.getDate()).padStart(2, '0') + '/' + String(mesAtual + 1).padStart(2, '0') + '/' + anoAtual;renderCalendario();mostrarEventos(diaSelecionado);}
function mesAnterior() {mesAtual--;if (mesAtual < 0) { mesAtual = 11; anoAtual--; }diaSelecionado = null;document.getElementById('painel-eventos').style.display = 'none';renderCalendario();}
function proximoMes() {mesAtual++;if (mesAtual > 11) { mesAtual = 0; anoAtual++; }diaSelecionado = null;document.getElementById('painel-eventos').style.display = 'none';renderCalendario();}
carregarEventos();
setInterval(carregarEventos, 10000);
</script></body></html>
)rawliteral";

// ============================================================================
//  HELPER
// ============================================================================
String montarPagina(const char* tmpl, const char* tipoUsuario) {
  String html;
  html.reserve(40000); 
  html = String(tmpl);
  html.replace("__CSS__", FPSTR(CSS_BASE));
  html.replace("__SVG_ICONE__", FPSTR(SVG_ICONE_PORTA));
  html.replace("__NOME_PORTA__", nomePorta);
  html.replace("__AP_IP__", WiFi.softAPIP().toString());
  html.replace("__MAC__", macAddress);
  html.replace("__SSID_AP__", ssidAPDinamico);
  html.replace("__PREFIXO_AP__", prefixoAP);
  if (tipoUsuario) html.replace("__TIPO_USUARIO__", tipoUsuario);
  return html;
}

void enviarHTMLSemCache(AsyncWebServerRequest *r, String h) {
  AsyncWebServerResponse *resp = r->beginResponse(200, "text/html", h);
  resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  resp->addHeader("Pragma", "no-cache");
  r->send(resp);
}

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== PORTA DE ENROLAR - INICIANDO ===");

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char bufMac[18];
  snprintf(bufMac, sizeof(bufMac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  macAddress = String(bufMac);

  carregarConfiguracoes();

  char sufixoMac[7];
  snprintf(sufixoMac, sizeof(sufixoMac), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  ssidAPDinamico = prefixoAP + "-" + String(sufixoMac);
  Serial.printf("[AP] SSID dinamico: %s\n", ssidAPDinamico.c_str());

  int motivo = lerEResetarFlagReset();
  if (motivo == 2) { if (totalRedes > 0) removerRede(0); wifiSSID = ""; wifiSenha = ""; wifiHabilitado = false; }
  else if (motivo == 1) { wifiSSID = ""; wifiSenha = ""; wifiHabilitado = false; }
  else { if (totalRedes > 0) { wifiSSID = redesSalvas[0].ssid; wifiSenha = redesSalvas[0].senha; wifiHabilitado = true; } else wifiHabilitado = false; }

  pinMode(PINO_TRAVA_ESQUERDA, INPUT_PULLUP);
  pinMode(PINO_TRAVA_DIREITA, INPUT_PULLUP);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_LED_VERMELHO, OUTPUT);
  pinMode(PINO_BOTAO_RESET_WIFI, INPUT_PULLUP);
  pinMode(PINO_BOTAO_REBOOT, INPUT_PULLUP);
  pinMode(PINO_BYPASS, INPUT_PULLUP);
  digitalWrite(PINO_RELE, LOW);
  digitalWrite(PINO_LED_VERDE, LOW);
  digitalWrite(PINO_LED_VERMELHO, HIGH);

  travaEsqLeitura = false; travaDirLeitura = false;
  travaEsqEstavel = false; travaDirEstavel = false;

  bypassLeitura = (digitalRead(PINO_BYPASS) == LOW);
  bypassEstavel = bypassLeitura;
  bypassUltimoEstadoFisico = bypassEstavel;

  adicionarLog("INFO", "Sistema iniciado");
  if (bypassAtivo) adicionarLog("CFG", "Bypass carregado do NVS (ATIVO)");
  {
    char msg[80];
    snprintf(msg, sizeof(msg), "Tipo travas: Esq=%s Dir=%s",
      tipoTravaEsq == TRAVA_NA ? "NA" : "NF",
      tipoTravaDir == TRAVA_NA ? "NA" : "NF");
    adicionarLog("INFO", msg);
  }
  {
    String msgAp = "AP SSID: " + ssidAPDinamico;
    adicionarLog("INFO", msgAp.c_str());
  }

  serialDFPlayer.begin(9600, SERIAL_8N1, PINO_DFPLAYER_RX, PINO_DFPLAYER_TX);
  if (dfPlayer.begin(serialDFPlayer)) { dfPlayer.volume(25); adicionarLog("INFO", "DFPlayer OK"); }
  else adicionarLog("ALARME", "DFPlayer falhou");

  WiFi.mode(WIFI_AP_STA);
  delay(200);
  WiFi.softAP(ssidAPDinamico.c_str(), senhaAP.c_str());
  delay(500);
  adicionarLog("INFO", "Wi-Fi AP iniciado");

  if (wifiHabilitado && wifiSSID.length() > 0) { ultimaTentativaWiFi = millis(); iniciarConexaoWiFi(); }

  servidorHTTP.on("/teste", HTTP_GET, [](AsyncWebServerRequest *r){ r->send(200, "text/plain", "OK"); });

  servidorHTTP.on("/diag", HTTP_GET, [](AsyncWebServerRequest *r){
    AsyncResponseStream *resp = r->beginResponseStream("application/json");
    resp->addHeader("Cache-Control", "no-store");
    resp->print("{\"wifi_conectado\":"); resp->print(wifiConectado ? "true" : "false");
    resp->print(",\"ntp_sincronizado\":"); resp->print(ntpSincronizado ? "true" : "false");
    resp->print(",\"ssid_ativo\":\""); resp->print(wifiSSID); resp->print("\"");
    resp->print(",\"ip_sta\":\""); resp->print(WiFi.localIP().toString()); resp->print("\"");
    resp->print(",\"gateway\":\""); resp->print(WiFi.gatewayIP().toString()); resp->print("\"");
    resp->print(",\"dns\":\""); resp->print(WiFi.dnsIP().toString()); resp->print("\"");
    resp->print(",\"total_logs\":"); resp->print(totalLog);
    resp->print(",\"max_logs\":"); resp->print(MAX_LOGS);
    resp->print(",\"ws_clientes\":"); resp->print(servidorWS.connectedClients());
    resp->print(",\"heap_livre\":"); resp->print(ESP.getFreeHeap());
    resp->print(",\"bypass_ativo\":"); resp->print(bypassAtivo ? "true" : "false");
    resp->print(",\"tipo_trava_esq\":"); resp->print(tipoTravaEsq);
    resp->print(",\"tipo_trava_dir\":"); resp->print(tipoTravaDir);
    resp->print(",\"ssid_ap\":\""); resp->print(ssidAPDinamico); resp->print("\"");
    resp->print(",\"prefixo_ap\":\""); resp->print(prefixoAP); resp->print("\"");
    resp->print(",\"mac\":\""); resp->print(macAddress); resp->print("\"}");
    r->send(resp);
  });

  servidorHTTP.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
    String h = String(PAGINA_LOGIN);
    h.replace("__SVG_ICONE__", FPSTR(SVG_ICONE_PORTA));
    h.replace("__NOME_PORTA__", nomePorta);
    h.replace("__MENSAGEM_ERRO__", "");
    enviarHTMLSemCache(r, h);
  });

  servidorHTTP.on("/login", HTTP_POST, [](AsyncWebServerRequest *r){
    String u = r->hasParam("usuario", true) ? r->getParam("usuario", true)->value() : "";
    String p = r->hasParam("senha", true) ? r->getParam("senha", true)->value() : "";
    String tipo = "";
    if (u == "admin" && p == senhaAdmin) tipo = "admin";
    else if (u == "visualizador" && p == senhaVisualizador) tipo = "visualizador";
    if (tipo != "") {
      AsyncWebServerResponse *resp = r->beginResponse(302);
      resp->addHeader("Location", "/painel");
      resp->addHeader("Set-Cookie", "sessao=" + tipo + "; Path=/; HttpOnly");
      r->send(resp);
      adicionarLog("INFO", (tipo == "admin") ? "Login admin" : "Login visualizador");
    } else {
      String h = String(PAGINA_LOGIN);
      h.replace("__SVG_ICONE__", FPSTR(SVG_ICONE_PORTA));
      h.replace("__NOME_PORTA__", nomePorta);
      h.replace("__MENSAGEM_ERRO__", "Usuário ou senha inválidos");
      enviarHTMLSemCache(r, h);
      String m = "Login falhou (" + u + ")";
      adicionarLog("INFO", m.c_str());
    }
  });

  servidorHTTP.on("/painel", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->redirect("/"); return; }
    enviarHTMLSemCache(r, montarPagina(PAGINA_PAINEL, usuarioEhAdmin(r) ? "admin" : "visualizador"));
  });

  servidorHTTP.on("/admin", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->redirect("/"); return; }
    enviarHTMLSemCache(r, montarPagina(PAGINA_ADMIN, usuarioEhAdmin(r) ? "admin" : "visualizador"));
  });

  servidorHTTP.on("/logs", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEhAdmin(r)) { r->redirect("/painel"); return; }
    enviarHTMLSemCache(r, montarPagina(PAGINA_LOGS, NULL));
  });

  servidorHTTP.on("/calendario", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEhAdmin(r)) { r->redirect("/painel"); return; }
    enviarHTMLSemCache(r, montarPagina(PAGINA_CALENDARIO, NULL));
  });

  servidorHTTP.on("/logout", HTTP_GET, [](AsyncWebServerRequest *r){
    AsyncWebServerResponse *resp = r->beginResponse(302);
    resp->addHeader("Location", "/");
    resp->addHeader("Set-Cookie", "sessao=; Path=/; Max-Age=0");
    r->send(resp);
    adicionarLog("INFO", "Logout");
  });

  servidorHTTP.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"erro\":\"negado\"}"); return; }
    AsyncResponseStream *resp = r->beginResponseStream("application/json");
    resp->addHeader("Cache-Control", "no-store");
    resp->print("[");
    for (int i = 0; i < totalLog; i++) {
      if (i > 0) resp->print(",");
      resp->print("{\"tempo\":\"");
      escreverEscapado(resp, logsSistema[i].tempo);
      resp->print("\",\"tipo\":\"");
      escreverEscapado(resp, logsSistema[i].tipo);
      resp->print("\",\"msg\":\"");
      escreverEscapado(resp, logsSistema[i].mensagem);
      resp->print("\"}");
    }
    resp->print("]");
    r->send(resp);
  });

  servidorHTTP.on("/api/logs/limpar", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<128> d;
      if (len > 0 && data != NULL) deserializeJson(d, data, len);
      String tipo = d["tipo"] | "todos";
      if (tipo == "todos" || tipo.length() == 0) { totalLog = 0; adicionarLog("CFG", "Todos os logs apagados"); }
      else {
        int novo = 0;
        for (int i = 0; i < totalLog; i++) if (String(logsSistema[i].tipo) != tipo) logsSistema[novo++] = logsSistema[i];
        totalLog = novo;
        String m = "Logs de " + tipo + " apagados";
        adicionarLog("CFG", m.c_str());
      }
      r->send(200, "application/json", "{\"ok\":true}");
  });

  servidorHTTP.on("/api/wifi/lista", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->send(403, "application/json", "{}"); return; }
    AsyncResponseStream *resp = r->beginResponseStream("application/json");
    resp->print("{\"redes\":[");
    for (int i = 0; i < totalRedes; i++) {
      if (i > 0) resp->print(",");
      resp->print("\""); escreverEscapado(resp, redesSalvas[i].ssid.c_str()); resp->print("\"");
    }
    resp->print("],\"ativo\":\"");
    if (wifiConectado) escreverEscapado(resp, wifiSSID.c_str());
    resp->print("\"}");
    r->send(resp);
  });

  servidorHTTP.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEstaLogado(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<192> d;
      deserializeJson(d, data, len);
      String ssid = d["ssid"] | "";
      String senha = d["senha"] | "";
      if (ssid.length() < 1 || ssid.length() > 32) { r->send(400, "application/json", "{\"ok\":false}"); return; }
      adicionarRede(ssid, senha);
      wifiSSID = ssid; wifiSenha = senha; wifiHabilitado = true;
      WiFi.disconnect(false, false);
      wifiConectado = false; wifiIP = "";
      ultimaTentativaWiFi = millis();
      iniciarConexaoWiFi();
      adicionarLog("CFG", "Nova rede salva");
      atualizarSaidas();
      r->send(200, "application/json", "{\"ok\":true,\"mensagem\":\"Conectando\"}");
  });

  servidorHTTP.on("/api/wifi/conectar-rede", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->redirect("/"); return; }
    if (!r->hasParam("idx")) { r->redirect("/admin"); return; }
    int idx = r->getParam("idx")->value().toInt();
    if (idx < 0 || idx >= totalRedes) { r->redirect("/admin"); return; }
    String ssid = redesSalvas[idx].ssid, senha = redesSalvas[idx].senha;
    adicionarRede(ssid, senha);
    wifiSSID = ssid; wifiSenha = senha; wifiHabilitado = true;
    WiFi.disconnect(false, false);
    wifiConectado = false; wifiIP = "";
    ultimaTentativaWiFi = millis();
    iniciarConexaoWiFi();
    adicionarLog("CFG", "Conectando rede");
    String h = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='3;url=/admin'></head><body style='font-family:Arial;background:#0f172a;color:#fff;padding:30px;text-align:center'><h2>Conectando...</h2></body></html>";
    AsyncWebServerResponse *resp = r->beginResponse(200, "text/html", h);
    resp->addHeader("Cache-Control", "no-cache");
    r->send(resp);
  });

  servidorHTTP.on("/api/wifi/esquecer-rede", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->redirect("/"); return; }
    if (!r->hasParam("idx")) { r->redirect("/admin"); return; }
    int idx = r->getParam("idx")->value().toInt();
    if (idx < 0 || idx >= totalRedes) { r->redirect("/admin"); return; }
    String ssidRem = redesSalvas[idx].ssid;
    bool eraAtiva = (ssidRem == wifiSSID);
    removerRede(idx);
    if (eraAtiva) { marcarResetWiFi(1); reiniciarComMotivo = 1; quandoReiniciar = millis() + 1500; }
    adicionarLog("CFG", "Rede removida");
    String h = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='15;url=http://192.168.4.1/admin'></head><body style='font-family:Arial;background:#0f172a;color:#fff;padding:30px;text-align:center'><h2>Removida</h2></body></html>";
    AsyncWebServerResponse *resp = r->beginResponse(200, "text/html", h);
    resp->addHeader("Cache-Control", "no-cache");
    r->send(resp);
  });

  servidorHTTP.on("/api/wifi/desconectar-get", HTTP_GET, [](AsyncWebServerRequest *r){
    if (!usuarioEstaLogado(r)) { r->redirect("/"); return; }
    marcarResetWiFi(1); reiniciarComMotivo = 1; quandoReiniciar = millis() + 1500;
    String h = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='15;url=http://192.168.4.1/admin'></head><body style='font-family:Arial;background:#0f172a;color:#fff;padding:30px;text-align:center'><h2>Desconectando...</h2></body></html>";
    AsyncWebServerResponse *resp = r->beginResponse(200, "text/html", h);
    resp->addHeader("Cache-Control", "no-cache");
    r->send(resp);
  });

  servidorHTTP.on("/alterar-nome", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<128> d; deserializeJson(d, data, len);
      String n = d["nome"] | "";
      if (n.length() < 2 || n.length() > 40) { r->send(400, "application/json", "{\"ok\":false}"); return; }
      nomePorta = n; salvarConfiguracoes(); atualizarSaidas();
      adicionarLog("CFG", "Nome alterado");
      r->send(200, "application/json", "{\"ok\":true,\"mensagem\":\"Atualizado\"}");
  });

  servidorHTTP.on("/alterar-senha", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<128> d; deserializeJson(d, data, len);
      String tipo = d["tipo"] | "", s = d["senha"] | "";
      if (s.length() < 4) { r->send(400, "application/json", "{\"ok\":false}"); return; }
      if (tipo == "admin") senhaAdmin = s;
      else if (tipo == "visualizador") senhaVisualizador = s;
      else { r->send(400, "application/json", "{\"ok\":false}"); return; }
      salvarConfiguracoes();
      adicionarLog("CFG", "Senha alterada");
      r->send(200, "application/json", "{\"ok\":true,\"mensagem\":\"Alterada\"}");
  });

  servidorHTTP.on("/alterar-ap-config", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<256> d; deserializeJson(d, data, len);
      
      String p = d["prefixo"] | "";
      String s = d["senha"] | "";
      p.trim();
      
      if (p.length() < 1 || p.length() > 24) {
        r->send(400, "application/json", "{\"ok\":false,\"mensagem\":\"Prefixo deve ter 1-24 caracteres\"}");
        return;
      }
      if (s.length() < 8 || s.length() > 32) {
        r->send(400, "application/json", "{\"ok\":false,\"mensagem\":\"Senha deve ter 8-32 caracteres\"}");
        return;
      }
      
      prefixoAP = p;
      senhaAP = s;
      salvarConfiguracoes();
      
      adicionarLog("CFG", "Configurações do AP alteradas");
      reiniciarComMotivo = 1;
      quandoReiniciar = millis() + 1500;
      r->send(200, "application/json", "{\"ok\":true,\"mensagem\":\"Configurações salvas. Reiniciando...\"}");
  });

  servidorHTTP.on("/manutencao", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
    modoManutencao = !modoManutencao;
    atualizarSaidas();
    adicionarLog("CFG", modoManutencao ? "Manutenção ON" : "Manutenção OFF");
    String j = modoManutencao ? "{\"ok\":true,\"mensagem\":\"ATIVADO\"}" : "{\"ok\":true,\"mensagem\":\"DESATIVADO\"}";
    r->send(200, "application/json", j);
  });

  servidorHTTP.on("/bypass", HTTP_POST, [](AsyncWebServerRequest *r){
    if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
    bypassAtivo = !bypassAtivo;
    salvarConfiguracoes();
    aplicarHardware();
    broadcastCompleto();
    adicionarLog("CFG", bypassAtivo ? "Bypass ATIVADO (web)" : "Bypass DESATIVADO (web)");
    String j = bypassAtivo
      ? "{\"ok\":true,\"mensagem\":\"Bypass ATIVADO\"}"
      : "{\"ok\":true,\"mensagem\":\"Bypass DESATIVADO\"}";
    r->send(200, "application/json", j);
  });

  servidorHTTP.on("/sensor-tipo", HTTP_POST,
    [](AsyncWebServerRequest *r){}, NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      if (!usuarioEhAdmin(r)) { r->send(403, "application/json", "{\"ok\":false}"); return; }
      StaticJsonDocument<192> d; deserializeJson(d, data, len);

      int te = d["tipo_esq"] | -1;
      int td = d["tipo_dir"] | -1;

      if (te != TRAVA_NA && te != TRAVA_NF) { r->send(400, "application/json", "{\"ok\":false,\"mensagem\":\"Tipo esq inválido\"}"); return; }
      if (td != TRAVA_NA && td != TRAVA_NF) { r->send(400, "application/json", "{\"ok\":false,\"mensagem\":\"Tipo dir inválido\"}"); return; }

      tipoTravaEsq = te;
      tipoTravaDir = td;
      salvarConfiguracoes();

      travaEsqLeitura = false; travaDirLeitura = false;
      travaEsqEstavel = false; travaDirEstavel = false;

      aplicarHardware();
      broadcastCompleto();

      char msg[80];
      snprintf(msg, sizeof(msg), "Travas: Esq=%s Dir=%s",
        tipoTravaEsq == TRAVA_NA ? "NA" : "NF",
        tipoTravaDir == TRAVA_NA ? "NA" : "NF");
      adicionarLog("CFG", msg);

      String j = String("{\"ok\":true,\"mensagem\":\"") + msg + "\"}";
      r->send(200, "application/json", j);
  });

  servidorHTTP.begin();
  servidorWS.begin();
  servidorWS.onEvent(eventoWS);
  servidorWS.enableHeartbeat(5000, 2000, 1);

  Serial.println("=== SISTEMA INICIADO ===");
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
  servidorWS.loop();
  verificarTravas();
  processarBroadcastPendente();
  verificarBotaoReboot();
  verificarBotaoBypass();
  checarConexaoWiFi();
  monitorarWiFi();
  checarNTP();

  if (reiniciarComMotivo != 0 && millis() >= quandoReiniciar) { Serial.flush(); delay(200); ESP.restart(); }

  if (digitalRead(PINO_BOTAO_RESET_WIFI) == LOW) {
    if (tempoApertouReset == 0) tempoApertouReset = millis();
    else if (millis() - tempoApertouReset > TEMPO_RESET_WIFI_FISICO) { marcarResetWiFi(2); delay(200); ESP.restart(); }
  } else tempoApertouReset = 0;

  if (digitalRead(PINO_BOTAO) == LOW) {
    delay(50);
    if (digitalRead(PINO_BOTAO) == LOW) {
      if (bypassAtivo) {
        tocarAudio(4); adicionarLog("ACESSO", "Acesso liberado via botao (Bypass)");
      } else if (!travaEsquerdaAberta && !travaDireitaAberta) {
        tocarAudio(3); adicionarLog("ALARME", "Acesso negado - ambas travas fechadas"); enviarAlertaTrava("ambos");
      } else if (!travaEsquerdaAberta) {
        tocarAudio(1); adicionarLog("ALARME", "Acesso negado - trava ESQUERDA fechada"); enviarAlertaTrava("esq");
      } else if (!travaDireitaAberta) {
        tocarAudio(2); adicionarLog("ALARME", "Acesso negado - trava DIREITA fechada"); enviarAlertaTrava("dir");
      } else {
        tocarAudio(4); adicionarLog("ACESSO", "Acesso liberado via botao");
      }
      unsigned long tPress = millis();
      while (digitalRead(PINO_BOTAO) == LOW && (millis() - tPress) < 3000) { delay(10); yield(); }
    }
  }
  delay(50);
}