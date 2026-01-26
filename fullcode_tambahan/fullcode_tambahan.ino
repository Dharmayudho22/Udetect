#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

// --- LED Status ---
#define LED_WIFI 16 // D0 / GPIO16

// --- TCS3200 ---
#define S0 14 // D5
#define S1 12 // D6
#define S2 13 // D7
#define S3 15 // D8
#define sensorOut 0 // D3

// --- WiFi & Server ---
const char* ssid = "Mak";
const char* password = "sehatselalu";
const char* serverName = "http://10.112.96.183:3000/api/data";

// --- ADS1115 ---
Adafruit_ADS1115 ads;

// --- State Machine ---
enum State { MEASURE_COLOR, WAIT_SWAP, MEASURE_CHEM, SEND_PACKET };
State state = MEASURE_COLOR;

// Timeout yang diperpanjang
const unsigned long COLOR_TIMEOUT = 12000; // 12 detik
const unsigned long CHEM_TIMEOUT = 12000; // 12 detik
const int NWIN = 10;

int rBuf[NWIN], gBuf[NWIN], bBuf[NWIN], idxRGB = 0, filledRGB = 0;
float pHBuf[NWIN], tdsBuf[NWIN], idxChem = 0, filledChem = 0;

unsigned long stateStart = 0;

struct RGB { int r, g, b; };

// --- Function Prototypes ---
RGB readRGB();
void readChem(float& pH, float& tds);
bool isRGBStable();
bool isChemStable();
void ledBlinkSlow();
void ledBlinkFast();
void ledOn();
void ledOff();

void setup() {
    Serial.begin(9600);
    Wire.begin(4, 5); // SDA=D2, SCL=D1

    pinMode(LED_WIFI, OUTPUT);
    pinMode(S0, OUTPUT);
    pinMode(S1, OUTPUT);
    pinMode(S2, OUTPUT);
    pinMode(S3, OUTPUT);
    pinMode(sensorOut, INPUT);
    digitalWrite(S0, HIGH);
    digitalWrite(S1, LOW);

    // WiFi
    WiFi.begin(ssid, password);
    Serial.print("Menyambung ke WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        digitalWrite(LED_WIFI, LOW);
        delay(200);
        digitalWrite(LED_WIFI, HIGH);
        delay(200);
        yield();
    }
    Serial.println("\nWiFi Terhubung");
    digitalWrite(LED_WIFI, LOW);

    // ADS1115
    if (!ads.begin()) {
        Serial.println("ADS1115 tidak terdeteksi!");
        while (1);
    } else {
        Serial.println("ADS1115 OK");
    }
}

void loop() {
    static RGB lastRGB {
        0, 0, 0
    };
    static float lastPH = 0, lastTDS = 0;

    switch (state) {
        case MEASURE_COLOR: {
            ledOff(); // Sesuai permintaan: Lampu mati saat pembacaan warna
            if (stateStart == 0) stateStart = millis();

            RGB c = readRGB();
            rBuf[idxRGB] = c.r;
            gBuf[idxRGB] = c.g;
            bBuf[idxRGB] = c.b;
            idxRGB = (idxRGB + 1) % NWIN;
            if (filledRGB < NWIN) filledRGB++;

            Serial.printf("[COLOR] R:%d G:%d B:%d\n", c.r, c.g, c.b);

            if (isRGBStable() || (millis() - stateStart > COLOR_TIMEOUT)) {
                lastRGB = c;
                filledChem = 0;
                idxChem = 0;
                state = WAIT_SWAP;
                stateStart = 0;
            }
            break;
        }

        case WAIT_SWAP: {
            ledBlinkFast();
            if (stateStart == 0) {
                stateStart = millis();
                Serial.println(">> Masukkan probe pH & TDS sekarang...");
            }
            if (millis() - stateStart > 10000) { // 10 detik
                state = MEASURE_CHEM;
                stateStart = 0;
            }
            break;
        }

        case MEASURE_CHEM: {
            ledOn();
            if (stateStart == 0) stateStart = millis();

            float p, t;
            readChem(p, t);
            pHBuf[(int)idxChem] = p;
            tdsBuf[(int)idxChem] = t;
            idxChem = fmod(idxChem + 1, (float)NWIN);
            if (filledChem < NWIN) filledChem++;

            Serial.printf("[CHEM] pH:%.2f TDS:%.1f\n", p, t);

            if (isChemStable() || (millis() - stateStart > CHEM_TIMEOUT)) {
                lastPH = p;
                lastTDS = t;
                state = SEND_PACKET;
                stateStart = 0;
            }
            break;
        }

        case SEND_PACKET: {
            // --- klasifikasi warna/analisis (pakai rule kamu yang sekarang) ---
            String warnaDasar = "Tidak diketahui";
            int redColor = lastRGB.r, greenColor = lastRGB.g, blueColor = lastRGB.b;

            if ((redColor > greenColor + 40) && (redColor > blueColor + 30)) warnaDasar = "MERAH";
            else if ((greenColor > redColor + 30) && (greenColor > blueColor + 20)) warnaDasar = "HIJAU";
            else if ((blueColor > redColor + 40) && (blueColor > greenColor + 30)) warnaDasar = "BIRU";
            else if (redColor > 230 && greenColor > 230 && blueColor > 220) warnaDasar = "PUTIH";
            else if (redColor > 230 && greenColor > 230 && blueColor > 180) warnaDasar = "KUNING";

            String analisis = "Tidak dikenali";
            if (abs(redColor - greenColor) < 20 && abs(greenColor - blueColor) < 20 && lastTDS < 50) {
                analisis = "AIR BENING (BUKAN URINE)";
            } else if (redColor > 235 && greenColor > 235 && blueColor > 235) {
                analisis = "BENING / TRANSPARAN";
            }
            // Logika baru untuk 'BENING JERNIH'
            else if ((redColor >= 100 && redColor <= 115) && (greenColor >= 90 && greenColor <= 105) && (blueColor >= 130 && blueColor <= 150)) {
                analisis = "BENING JERNIH";
            }
            // Logika baru untuk 'KUNING AGAK KERUH' berdasarkan data Anda
            else if ((redColor >= 114 && redColor <= 117) && (greenColor >= 91 && greenColor <= 94) && (blueColor >= 127 && blueColor <= 128)) {
                analisis = "KUNING AGAK KERUH";
            }
            // Logika lama untuk 'BENING JERNIH' tetap dipertahankan
            else if (redColor > 220 && greenColor > 220 && blueColor > 200) {
                analisis = "BENING JERNIH";
            } else if (redColor > 200 && greenColor > 200 && blueColor > 160) {
                analisis = "BENING KERUH";
            } else if (redColor >= 120 && redColor <= 160 && greenColor >= 100 && greenColor <= 140 && blueColor >= 130 && blueColor <= 160) {
                analisis = "KUNING JERNIH";
            } else if (redColor > 140 && greenColor > 100 && blueColor < 120) {
                analisis = "KUNING KERUH";
            } else if (redColor > 210 && greenColor > 200 && blueColor > 170) {
                analisis = "KUNING GELAP / ORANYE";
            } else if (redColor > 160 && greenColor < 130 && blueColor < 170) {
                analisis = "ADA DARAH / MERAH";
            } else if (redColor < 100 && greenColor < 160 && blueColor > 200) {
                analisis = "KEMUNGKINAN ADA OBAT / BIRU";
            } else if (greenColor > 230 && redColor > 170 && blueColor > 170) {
                analisis = "PUTIH KEHIJAUAN";
            }

            Serial.println("=== PAKET TERKIRIM ===");
            Serial.printf("RGB: %d,%d,%d | pH: %.2f | TDS: %.1f | %s | %s\n",
                          redColor, greenColor, blueColor, lastPH, lastTDS,
                          warnaDasar.c_str(), analisis.c_str());

            // --- kirim ke server ---
            if (WiFi.status() == WL_CONNECTED) {
                WiFiClient client;
                HTTPClient http;
                http.begin(client, serverName);
                http.addHeader("Content-Type", "application/json");

                String jsonData = "{";
                jsonData += "\"ph\":" + String(lastPH, 2) + ",";
                jsonData += "\"tds\":" + String(lastTDS, 2) + ",";
                jsonData += "\"red\":" + String(redColor) + ",";
                jsonData += "\"green\":" + String(greenColor) + ",";
                jsonData += "\"blue\":" + String(blueColor) + ",";
                jsonData += "\"warnaDasar\":\"" + warnaDasar + "\",";
                jsonData += "\"analisis\":\"" + analisis + "\",";
                jsonData += "\"mode\":\"sequential\",\"sequence\":\"color_then_probe\"";
                jsonData += "}";

                int httpCode = http.POST(jsonData);
                Serial.print("HTTP Response: ");
                Serial.println(httpCode);
                http.end();
            }

            // reset ke siklus berikutnya dengan notifikasi LED dan jeda
            digitalWrite(LED_WIFI, LOW);
            delay(50); // Kedip
            digitalWrite(LED_WIFI, HIGH);
            delay(50);
            digitalWrite(LED_WIFI, LOW);
            delay(50);
            digitalWrite(LED_WIFI, HIGH);

            for (int i = 0; i < NWIN; i++) {
                rBuf[i] = gBuf[i] = bBuf[i] = 0;
                pHBuf[i] = tdsBuf[i] = 0;
            }
            filledRGB = filledChem = idxRGB = idxChem = 0;
            
            delay(5000); // jeda 5 detik
            
            state = MEASURE_COLOR;
            stateStart = 0;
            break;
        }
    }
    yield();
}

// --- FUNGSI PEMBANTU ---

RGB readRGB() {
    // delay yang diperpanjang
    int redFreq = pulseIn(sensorOut, LOW, 100000);
    yield();
    digitalWrite(S2, LOW);
    digitalWrite(S3, LOW);
    redFreq = pulseIn(sensorOut, LOW, 100000);
    int r = map(constrain(redFreq, 30, 300), 30, 300, 255, 0);
    delay(50); // Diperpanjang
    yield();

    digitalWrite(S2, HIGH);
    digitalWrite(S3, HIGH);
    int greenFreq = pulseIn(sensorOut, LOW, 100000);
    int g = map(constrain(greenFreq, 30, 300), 30, 300, 255, 0);
    delay(50); // Diperpanjang
    yield();

    digitalWrite(S2, LOW);
    digitalWrite(S3, HIGH);
    int blueFreq = pulseIn(sensorOut, LOW, 100000);
    int b = map(constrain(blueFreq, 30, 300), 30, 300, 255, 0);
    delay(50); // Diperpanjang
    yield();

    return { r, g, b };
}

void readChem(float& pH, float& tds) {
    int adcPh = ads.readADC_SingleEnded(0);
    yield();
    float voltPh = adcPh * 0.1875 / 1000.0;
    pH = -6.10 * voltPh + 22.5;
    delay(50); // Delay tambahan
    yield();

    int adcTds = ads.readADC_SingleEnded(1);
    yield();
    float voltTds = adcTds * 0.1875 / 1000.0;
    tds = (133.42 * pow(voltTds, 3) - 255.86 * pow(voltTds, 2) + 857.39 * voltTds) * 0.5;
    delay(50); // Delay tambahan
    yield();
}

bool isRGBStable() {
    if (filledRGB < NWIN) return false;
    int rMin = 999, rMax = -1, gMin = 999, gMax = -1, bMin = 999, bMax = -1;
    for (int i = 0; i < NWIN; i++) {
        rMin = min(rMin, rBuf[i]);
        rMax = max(rMax, rBuf[i]);
        gMin = min(gMin, gBuf[i]);
        gMax = max(gMax, gBuf[i]);
        bMin = min(bMin, bBuf[i]);
        bMax = max(bMax, bBuf[i]);
    }
    return (rMax - rMin < 6) && (gMax - gMin < 6) && (bMax - bMin < 6);
}

bool isChemStable() {
    if (filledChem < NWIN) return false;
    float pMin = 1e9, pMax = -1e9, tMin = 1e9, tMax = -1e9;
    for (int i = 0; i < NWIN; i++) {
        pMin = min(pMin, pHBuf[i]);
        pMax = max(pMax, pHBuf[i]);
        tMin = min(tMin, tdsBuf[i]);
        tMax = max(tMax, tdsBuf[i]);
    }
    return (pMax - pMin < 0.05) && (tMax - tMin < 10.0);
}

void ledBlinkSlow() {
    digitalWrite(LED_WIFI, millis() / 600 % 2);
}

void ledBlinkFast() {
    digitalWrite(LED_WIFI, millis() / 150 % 2);
}

void ledOn() {
    digitalWrite(LED_WIFI, HIGH);
}

void ledOff() {
    digitalWrite(LED_WIFI, LOW);
}
