/**
 * Sistema de Monitoramento e Controle Térmico com Interface Web
 * Temperatura: DS18B20 (controle)
 * Umidade: DHT22 (monitoramento)
 * LCD I2C 16x2
 * AP Wi-Fi + servidor HTTP + SPIFFS
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

/* -------------------- PINOS -------------------- */
constexpr uint8_t PIN_DHT      = 5;
constexpr uint8_t PIN_DS18B20  = 18;
constexpr uint8_t PIN_LAMP     = 27;

constexpr uint8_t DHTTYPE = DHT22;

/* -------------------- CONTROLE TÉRMICO -------------------- */
float TEMP_ALVO = 30.0f;                  // será carregado/salvo em arquivo
constexpr float HISTERESIS_INF   = 2.0f;
constexpr float HISTERESIS_SUP   = 2.0f;

constexpr unsigned long INTERVALO_LEITURA_MS = 2000;

/* -------------------- FILTRO EMA & ALARMES -------------------- */
float tempFiltrada = NAN;
constexpr float ALFA = 0.25f;

constexpr float TEMP_MIN_SEGURA    = 15.0f;
constexpr float TEMP_MAX_SEGURA    = 40.0f;

/* -------------------- HISTÓRICO EM MEMÓRIA + CSV -------------------- */
constexpr int MINUTOS_DIA = 1440;
constexpr int DIAS_HIST   = 7;

struct RegistroMinuto {
    bool  valido;
    float temp;
    float umid;
};

RegistroMinuto dadosDia[MINUTOS_DIA];
int indiceMinuto = -1;
int indiceDia    = 0;
unsigned long ultimoLogMinuto = 0;

/* -------------------- Wi-Fi AP -------------------- */
const char* AP_SSID = "TerrarioESP";
const char* AP_PASS = "12345678";

/* -------------------- OBJETOS -------------------- */
DHT dht(PIN_DHT, DHTTYPE);
OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);
LiquidCrystal_I2C* lcd = nullptr;

WebServer server(80);

/* -------------------- VARIÁVEIS -------------------- */
unsigned long ultimoTempoLeitura = 0;

bool  lampadaLigada = false;
float tempDS        = NAN;
float tempMedia     = NAN;
float umidade       = NAN;

/* -------------------- AUXILIARES -------------------- */

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
        dadosDia[i].temp   = NAN;
        dadosDia[i].umid   = NAN;
    }
}

/* -------------------- PERSISTÊNCIA DO SETPOINT -------------------- */

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
        Serial.println("[INFO] setpoint.cfg nao existe, usando valor padrao");
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

/* -------------------- CONTROLE TÉRMICO -------------------- */

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

/* -------------------- LEITURAS & LCD -------------------- */

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
        delay(2000);
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

/* -------------------- LOG EM MEMÓRIA + CSV -------------------- */

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
                f.println("hora,temp,umid");
                f.close();
            }
        }

        if (isnan(tempMedia) || isnan(umidade)) return;

        dadosDia[indiceMinuto].valido = true;
        dadosDia[indiceMinuto].temp   = tempMedia;
        dadosDia[indiceMinuto].umid   = umidade;

        int h = indiceMinuto / 60;
        int m = indiceMinuto % 60;
        char hora[6];
        snprintf(hora, sizeof(hora), "%02d:%02d", h, m);

        String nome = nomeArquivoDia(indiceDia);
        File f = SPIFFS.open(nome, FILE_APPEND);
        if (f) {
            f.printf("%s,%.2f,%.2f\n", hora, tempMedia, umidade);
            f.close();
        }
    }
}

/* -------------------- ROTAS HTTP -------------------- */

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

    if (isnan(temp)) temp = NAN;
    if (isnan(hum))  hum  = NAN;

    String json = "{";
    json += "\"temp\":" + String(temp, 2) + ",";
    json += "\"umid\":" + String(hum, 2) + ",";
    json += "\"alvo\":" + String(TEMP_ALVO, 2) + ",";
    json += "\"lampada\":" + String(lampadaLigada ? 1 : 0) + ",";
    json += "\"dia\":" + String(indiceDia) + ",";
    json += "\"minuto\":" + String(indiceMinuto);
    json += "}";

    server.send(200, "application/json", json);
}

void handleApiToday() {
    String json = "[";
    bool first = true;

    for (int i = 0; i < MINUTOS_DIA; i++) {
        if (!dadosDia[i].valido) continue;

        int h = i / 60;
        int m = i % 60;
        char hora[6];
        snprintf(hora, sizeof(hora), "%02d:%02d", h, m);

        if (!first) json += ",";
        first = false;

        json += "{";
        json += "\"hora\":\"" + String(hora) + "\",";
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
    float novo = s.toFloat();
    TEMP_ALVO = novo;
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

/* -------------------- SETUP -------------------- */

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
    delay(1500);
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

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("[INFO] AP iniciado: ");
    Serial.println(AP_SSID);
    Serial.print("[INFO] IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", HTTP_GET, handleRoot);
    server.on("/index.html", HTTP_GET, handleRoot);
    server.on("/style.css", HTTP_GET, [](){ handleStaticFile("/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, [](){ handleStaticFile("/script.js", "application/javascript"); });

    server.on("/api/state", HTTP_GET, handleApiState);
    server.on("/api/today", HTTP_GET, handleApiToday);
    server.on("/api/setpoint", HTTP_POST, handleApiSetpoint);
    server.on("/download", HTTP_GET, handleDownloadCsv);

    server.onNotFound([](){
        server.send(404, "text/plain", "Rota nao encontrada");
    });

    server.begin();
    Serial.println("[INFO] Servidor HTTP iniciado");
}

/* -------------------- LOOP -------------------- */

void loop() {
    server.handleClient();
    atualizarSensoresEControle();
    registrarMinutoEmHistorico();
}
