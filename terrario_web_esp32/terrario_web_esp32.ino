/**
 * Sistema de Monitoramento e Controle Térmico com Interface Web
 * - ESP32 DevKit V1
 * - Temperatura: DS18B20 (controle)
 * - Umidade: DHT22 (monitoramento)
 * - LCD I2C 16x2
 * - AP + STA com configuração de Wi-Fi pela página web
 * - Sincronização de data/hora via NTP quando houver internet
 * - Histórico de 7 dias em SPIFFS (CSV) com Data + Hora
 */

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <time.h>

/* --------- Hardware --------- */
constexpr uint8_t PIN_DHT      = 5;
constexpr uint8_t PIN_DS18B20  = 18;
constexpr uint8_t PIN_LAMP     = 27;
constexpr uint8_t DHTTYPE      = DHT22;

/* --------- Controle térmico --------- */
float TEMP_ALVO = 30.0f;                    // carregado de arquivo
constexpr float HISTERESIS_INF = 2.0f;
constexpr float HISTERESIS_SUP = 2.0f;

// faixa de operação segura
constexpr float TEMP_MIN_SEGURA = 25.0f;
constexpr float TEMP_MAX_SEGURA = 35.0f;

constexpr unsigned long INTERVALO_LEITURA_MS = 2000;

/* Filtro EMA */
float tempFiltrada = NAN;
constexpr float ALFA = 0.25f;

/* Histórico (1 ponto/min, 7 dias) */
constexpr int MINUTOS_DIA = 1440;
constexpr int DIAS_HIST   = 7;

struct RegistroMinuto {
    bool  valido;
    float temp;
    float umid;
    char  data[11]; // "YYYY-MM-DD"
    char  hora[6];  // "HH:MM"
};

RegistroMinuto dadosDia[MINUTOS_DIA];
int indiceMinuto = -1;
int indiceDia    = 0;
unsigned long ultimoLogMinuto = 0;

/* --------- Wi-Fi / NTP --------- */
const char* AP_SSID = "TerrarioESP";
const char* AP_PASS = "12345678";

String wifiSsid;
String wifiPass;
bool   staConfigurado    = false;
bool   staConectado      = false;
bool   tempoSincronizado = false;

unsigned long ultimoTentativaWiFi = 0;
constexpr unsigned long WIFI_RETRY_MS = 60000;

bool configTimeChamado = false;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;   // ajuste de fuso se precisar
const int  daylightOffset_sec = 0;

/* --------- Objetos --------- */
DHT dht(PIN_DHT, DHTTYPE);
OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);
LiquidCrystal_I2C* lcd = nullptr;

WebServer server(80);

/* --------- Variáveis de leitura --------- */
unsigned long ultimoTempoLeitura = 0;

bool  lampadaLigada = false;
float tempDS        = NAN;
float tempMedia     = NAN;
float umidade       = NAN;

/* --------- Funções auxiliares --------- */

uint8_t detectarEnderecoLCD() {
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (addr == 0x27 || addr == 0x3F) return addr;
        }
    }
    return 0x27;
}

String nomeArquivoDia(int d) {
    char buf[16];
    snprintf(buf, sizeof(buf), "/dia%d.csv", d);
    return String(buf);
}

void limparDadosDiaEmRAM() {
    for (int i = 0; i < MINUTOS_DIA; i++) {
        dadosDia[i].valido = false;
    }
}

/* --------- Persistência de setpoint --------- */

void salvarSetpointEmArquivo() {
    File f = SPIFFS.open("/setpoint.cfg", FILE_WRITE);
    if (!f) {
        Serial.println("[ERRO] Nao foi possivel abrir setpoint.cfg para escrita");
        return;
    }
    f.printf("%.2f\n", TEMP_ALVO);
    f.close();
}

void carregarSetpointDeArquivo() {
    if (!SPIFFS.exists("/setpoint.cfg")) {
        Serial.println("[INFO] setpoint.cfg nao existe, usando padrao");
        salvarSetpointEmArquivo();
        return;
    }
    File f = SPIFFS.open("/setpoint.cfg", FILE_READ);
    if (!f) {
        Serial.println("[ERRO] Nao foi possivel abrir setpoint.cfg para leitura");
        return;
    }
    String line = f.readStringUntil('\n');
    TEMP_ALVO = line.toFloat();
    f.close();
    Serial.print("[INFO] Setpoint carregado: ");
    Serial.println(TEMP_ALVO);
}

/* --------- Persistência de Wi-Fi --------- */

void salvarWifiConfig(const String& ssid, const String& pass) {
    File f = SPIFFS.open("/wifi.cfg", FILE_WRITE);
    if (!f) {
        Serial.println("[ERRO] Nao foi possivel abrir wifi.cfg para escrita");
        return;
    }
    f.println("SSID=" + ssid);
    f.println("PASS=" + pass);
    f.close();
    wifiSsid = ssid;
    wifiPass = pass;
    staConfigurado = (wifiSsid.length() > 0);
}

void carregarWifiConfig() {
    if (!SPIFFS.exists("/wifi.cfg")) {
        Serial.println("[INFO] wifi.cfg nao existe, Wi-Fi STA nao configurado");
        staConfigurado = false;
        return;
    }
    File f = SPIFFS.open("/wifi.cfg", FILE_READ);
    if (!f) {
        Serial.println("[ERRO] Nao foi possivel abrir wifi.cfg para leitura");
        staConfigurado = false;
        return;
    }
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("SSID=")) {
            wifiSsid = line.substring(5);
        } else if (line.startsWith("PASS=")) {
            wifiPass = line.substring(5);
        }
    }
    f.close();
    staConfigurado = (wifiSsid.length() > 0);
    Serial.print("[INFO] Wi-Fi configurado para SSID: ");
    Serial.println(wifiSsid);
}

/* --------- Controle térmico --------- */

void atualizarControleTermico(float temperatura) {
    if (isnan(temperatura)) {
        lampadaLigada = false;
        digitalWrite(PIN_LAMP, LOW);
        return;
    }

    const float limiteLiga    = TEMP_ALVO - HISTERESIS_INF;
    const float limiteDesliga = TEMP_ALVO + HISTERESIS_SUP;

    if (!lampadaLigada && temperatura <= limiteLiga) {
        lampadaLigada = true;
    } else if (lampadaLigada && temperatura >= limiteDesliga) {
        lampadaLigada = false;
    }

    digitalWrite(PIN_LAMP, lampadaLigada ? HIGH : LOW);
}

/* --------- Leitura de sensores e LCD --------- */

void atualizarSensoresEControle() {
    const unsigned long agora = millis();
    if (agora - ultimoTempoLeitura < INTERVALO_LEITURA_MS) return;
    ultimoTempoLeitura = agora;

    umidade = dht.readHumidity();
    bool erroUmidade = isnan(umidade);

    ds18b20.requestTemperatures();
    tempDS = ds18b20.getTempCByIndex(0);
    bool erroTemperatura = (tempDS == DEVICE_DISCONNECTED_C);

    if (erroTemperatura) {
        Serial.println("[ERRO] Falha no DS18B20");
        lcd->clear();
        lcd->setCursor(0, 0);
        lcd->print("ERRO TEMP!");
        lcd->setCursor(0, 1);
        lcd->print("DS18B20 FALHOU");

        lampadaLigada = false;
        digitalWrite(PIN_LAMP, LOW);
        return;
    }

    if (isnan(tempFiltrada))
        tempFiltrada = tempDS;
    else
        tempFiltrada = ALFA * tempDS + (1.0f - ALFA) * tempFiltrada;

    tempMedia = tempFiltrada;

    bool alarme = false;
    String msg;

    if (tempMedia < TEMP_MIN_SEGURA) {
        alarme = true;
        msg = "FRIO EXTREMO";
    } else if (tempMedia > TEMP_MAX_SEGURA) {
        alarme = true;
        msg = "CALOR EXTREMO";
    }

    if (alarme) {
        lampadaLigada = false;
        digitalWrite(PIN_LAMP, LOW);

        lcd->clear();
        lcd->setCursor(0, 0);
        lcd->print("ALARME!");
        lcd->setCursor(0, 1);
        lcd->print(msg);

        Serial.print("[ALERTA] ");
        Serial.println(msg);
        return;
    }

    atualizarControleTermico(tempMedia);

    Serial.print("Temp: ");
    Serial.print(tempMedia);
    Serial.print(" | Umid: ");
    if (!erroUmidade) Serial.print(umidade);
    else Serial.print("ERR");
    Serial.print(" | Lamp: ");
    Serial.println(lampadaLigada ? "ON" : "OFF");

    lcd->clear();
    lcd->setCursor(0, 0);
    lcd->print("T:");
    lcd->print(tempMedia, 1);
    lcd->print("C");

    lcd->setCursor(10, 0);
    lcd->print(lampadaLigada ? "ON " : "OFF");

    lcd->setCursor(0, 1);
    lcd->print("U:");

    if (!erroUmidade) {
        lcd->print(umidade, 1);
        lcd->print("%");
    } else {
        lcd->print("ERR");
    }
}

/* --------- NTP / hora atual --------- */

void tentarSincronizarTempo() {
    if (!staConectado || tempoSincronizado) return;

    if (!configTimeChamado) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        configTimeChamado = true;
        Serial.println("[INFO] configTime chamado");
    }

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        tempoSincronizado = true;
        Serial.println("[INFO] Hora sincronizada via NTP");
    }
}

String obterDataHoraString() {
    if (!tempoSincronizado) return String("");
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) return String("");
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &timeinfo);
    return String(buf);
}

/* --------- Histórico em RAM + CSV --------- */

void registrarMinutoEmHistorico() {
    unsigned long agora = millis();
    if (ultimoLogMinuto == 0 || agora - ultimoLogMinuto >= 60000UL) {
        ultimoLogMinuto = agora;

        indiceMinuto++;
        if (indiceMinuto >= MINUTOS_DIA) {
            indiceMinuto = 0;
            indiceDia = (indiceDia + 1) % DIAS_HIST;

            limparDadosDiaEmRAM();
            String nome = nomeArquivoDia(indiceDia);
            SPIFFS.remove(nome);
            File f = SPIFFS.open(nome, FILE_WRITE);
            if (f) {
                f.println("data,hora,temp,umid");
                f.close();
            }
        }

        if (isnan(tempMedia) || isnan(umidade)) return;

        char dataStr[11];
        char horaStr[6];

        if (tempoSincronizado) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                strftime(dataStr, sizeof(dataStr), "%Y-%m-%d", &timeinfo);
                strftime(horaStr, sizeof(horaStr), "%H:%M", &timeinfo);
            } else {
                strncpy(dataStr, "0000-00-00", sizeof(dataStr));
                int h = indiceMinuto / 60;
                int m = indiceMinuto % 60;
                snprintf(horaStr, sizeof(horaStr), "%02d:%02d", h, m);
            }
        } else {
            strncpy(dataStr, "0000-00-00", sizeof(dataStr));
            int h = indiceMinuto / 60;
            int m = indiceMinuto % 60;
            snprintf(horaStr, sizeof(horaStr), "%02d:%02d", h, m);
        }

        dadosDia[indiceMinuto].valido = true;
        dadosDia[indiceMinuto].temp   = tempMedia;
        dadosDia[indiceMinuto].umid   = umidade;
        strncpy(dadosDia[indiceMinuto].data, dataStr, sizeof(dadosDia[indiceMinuto].data));
        strncpy(dadosDia[indiceMinuto].hora, horaStr, sizeof(dadosDia[indiceMinuto].hora));
        dadosDia[indiceMinuto].data[10] = '\0';
        dadosDia[indiceMinuto].hora[5] = '\0';

        String nome = nomeArquivoDia(indiceDia);
        File f = SPIFFS.open(nome, FILE_APPEND);
        if (f) {
            f.printf("%s,%s,%.2f,%.2f\n", dataStr, horaStr, tempMedia, umidade);
            f.close();
        }
    }
}

/* --------- Wi-Fi STA --------- */

void tentarConectarSTA(bool forcarAgora = false) {
    if (!staConfigurado) return;

    unsigned long agora = millis();

    // Espera entre tentativas
    if (!forcarAgora && (agora - ultimoTentativaWiFi < WIFI_RETRY_MS))
        return;

    wl_status_t st = WiFi.status();

    // Se estiver conectando, não pode mexer ainda
    if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED || st == WL_CONNECTION_LOST) {
        Serial.println("[INFO] Resetando WiFi STA...");
        WiFi.disconnect(true, true);
        delay(250);
    }

    if (st != WL_CONNECTED) {
        Serial.print("[INFO] (re)conectando em ");
        Serial.println(wifiSsid);

        ultimoTentativaWiFi = agora;

        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
}


void gerenciarWiFi() {
    wl_status_t st = WiFi.status();
    bool antes = staConectado;
    staConectado = (st == WL_CONNECTED);

    if (staConectado && !antes) {
        Serial.print("[INFO] Conectado na rede STA, IP: ");
        Serial.println(WiFi.localIP());
    }

    if (!staConectado && staConfigurado) {
        tentarConectarSTA(false);
    }

    if (staConectado && !tempoSincronizado) {
        tentarSincronizarTempo();
    }
}

/* --------- HTTP handlers --------- */

void handleRoot() {
    File f = SPIFFS.open("/index.html", FILE_READ);
    if (!f) {
        server.send(500, "text/plain", "index.html nao encontrado");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

void handleStaticFile(const char* path, const char* contentType) {
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        server.send(404, "text/plain", "Arquivo nao encontrado");
        return;
    }
    server.streamFile(f, contentType);
    f.close();
}

void handleApiState() {
    float temp = tempMedia;
    float hum  = umidade;

    String datetime = obterDataHoraString();

    String json = "{";
    json += "\"temp\":" + String(temp, 2) + ",";
    json += "\"umid\":" + String(hum, 2) + ",";
    json += "\"alvo\":" + String(TEMP_ALVO, 2) + ",";
    json += "\"lampada\":" + String(lampadaLigada ? 1 : 0) + ",";
    json += "\"dia\":" + String(indiceDia) + ",";
    json += "\"minuto\":" + String(indiceMinuto) + ",";
    json += "\"online\":" + String(staConectado ? 1 : 0) + ",";
    json += "\"timeValid\":" + String(tempoSincronizado ? 1 : 0) + ",";
    json += "\"datetime\":\"" + datetime + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

void handleApiToday() {
    String json = "[";
    bool first = true;

    for (int i = 0; i < MINUTOS_DIA; i++) {
        if (!dadosDia[i].valido) continue;

        if (!first) json += ",";
        first = false;

        json += "{";
        json += "\"data\":\"" + String(dadosDia[i].data) + "\",";
        json += "\"hora\":\"" + String(dadosDia[i].hora) + "\",";
        json += "\"temp\":" + String(dadosDia[i].temp, 2) + ",";
        json += "\"umid\":" + String(dadosDia[i].umid, 2);
        json += "}";
    }

    json += "]";
    server.send(200, "application/json", json);
}

void handleApiSetpoint() {
    if (!server.hasArg("valor")) {
        server.send(400, "text/plain", "Parametro 'valor' obrigatorio");
        return;
    }
    String s = server.arg("valor");
    TEMP_ALVO = s.toFloat();
    salvarSetpointEmArquivo();
    server.send(200, "text/plain", "OK");
}

void handleDownloadCsv() {
    int d = 0;
    if (server.hasArg("dia")) {
        d = server.arg("dia").toInt();
    }
    if (d < 0 || d >= DIAS_HIST) d = 0;

    String nome = nomeArquivoDia(d);
    if (!SPIFFS.exists(nome)) {
        server.send(404, "text/plain", "Arquivo nao encontrado");
        return;
    }

    File f = SPIFFS.open(nome, FILE_READ);
    String header = "attachment; filename=\"dia" + String(d) + ".csv\"";
    server.sendHeader("Content-Type", "text/csv");
    server.sendHeader("Content-Disposition", header);
    server.sendHeader("Connection", "close");
    while (f.available()) {
        String linha = f.readStringUntil('\n');
        server.sendContent(linha + "\n");
    }
    f.close();
}

void handleApiWifiConfig() {
    if (!server.hasArg("ssid") || !server.hasArg("senha")) {
        server.send(400, "text/plain", "Parametros 'ssid' e 'senha' obrigatorios");
        return;
    }

    String ssid  = server.arg("ssid");
    String senha = server.arg("senha");

    ssid.trim();
    senha.trim();

    if (ssid.length() == 0) {
        server.send(400, "text/plain", "SSID vazio");
        return;
    }

    salvarWifiConfig(ssid, senha);
    tempoSincronizado = false;
    configTimeChamado = false;

    tentarConectarSTA(true);

    server.send(200, "text/plain", "WiFi configurado");
}

void handleApiNetStatus() {
    String datetime = obterDataHoraString();
    String ipSta = staConectado ? WiFi.localIP().toString() : "";
    String ipAp  = WiFi.softAPIP().toString();

    String json = "{";
    json += "\"ap_ssid\":\"" + String(AP_SSID) + "\",";
    json += "\"ssid\":\"" + wifiSsid + "\",";
    json += "\"sta_configured\":" + String(staConfigurado ? 1 : 0) + ",";
    json += "\"sta_connected\":" + String(staConectado ? 1 : 0) + ",";
    json += "\"ip_sta\":\"" + ipSta + "\",";
    json += "\"ip_ap\":\"" + ipAp + "\",";
    json += "\"time_valid\":" + String(tempoSincronizado ? 1 : 0) + ",";
    json += "\"datetime\":\"" + datetime + "\"";
    json += "}";

    server.send(200, "application/json", json);
}

void handleApiNtpSync() {
    tempoSincronizado = false;
    configTimeChamado = false;
    if (staConectado) {
        tentarSincronizarTempo();
    }
    server.send(200, "text/plain", "OK");
}

/* --------- Setup / Loop --------- */

void setup() {
    Serial.begin(115200);
    delay(300);

    Wire.begin(21, 22);
    uint8_t end = detectarEnderecoLCD();
    lcd = new LiquidCrystal_I2C(end, 16, 2);
    lcd->init();
    lcd->backlight();

    lcd->setCursor(0, 0);
    lcd->print("Iniciando...");
    delay(1000);
    lcd->clear();

    dht.begin();
    ds18b20.begin();

    pinMode(PIN_LAMP, OUTPUT);
    digitalWrite(PIN_LAMP, LOW);

    if (!SPIFFS.begin(true)) {
        Serial.println("[ERRO] Falha ao montar SPIFFS");
    } else {
        Serial.println("[INFO] SPIFFS montado");
    }

    limparDadosDiaEmRAM();
    carregarSetpointDeArquivo();
    carregarWifiConfig();

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(true);
    delay(200);

    bool okAp = WiFi.softAP(AP_SSID, AP_PASS);
    if (okAp) {
        Serial.print("[INFO] AP iniciado: ");
        Serial.println(AP_SSID);
        Serial.print("[INFO] IP AP: ");
        Serial.println(WiFi.softAPIP());
    } else {
        Serial.println("[ERRO] Falha ao iniciar AP");
    }

    if (staConfigurado) {
        tentarConectarSTA(true);
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/index.html", HTTP_GET, handleRoot);
    server.on("/style.css", HTTP_GET, [](){ handleStaticFile("/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, [](){ handleStaticFile("/script.js", "application/javascript"); });

    server.on("/api/state", HTTP_GET, handleApiState);
    server.on("/api/today", HTTP_GET, handleApiToday);
    server.on("/api/setpoint", HTTP_POST, handleApiSetpoint);
    server.on("/download", HTTP_GET, handleDownloadCsv);
    server.on("/api/wifi", HTTP_POST, handleApiWifiConfig);
    server.on("/api/netstatus", HTTP_GET, handleApiNetStatus);
    server.on("/api/ntpsync", HTTP_GET, handleApiNtpSync);

    server.onNotFound([]() {
        server.send(404, "text/plain", "Rota nao encontrada");
    });

    server.begin();
    Serial.println("[INFO] Servidor HTTP iniciado");
}

void loop() {
    server.handleClient();
    gerenciarWiFi();
    atualizarSensoresEControle();
    registrarMinutoEmHistorico();
}
