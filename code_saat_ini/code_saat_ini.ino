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
const char* serverName = "http://10.157.211.183:3000/api/data";

// --- ADS1115 ---
Adafruit_ADS1115 ads;

// --- KALIBRASI pH ---
const float PH_SLOPE = -3.33;
const float PH_OFFSET = 13.83;

// --- State Machine ---
enum State { MEASURE_COLOR, WAIT_SWAP, MEASURE_CHEM, SEND_PACKET };
State state = MEASURE_COLOR;

const unsigned long COLOR_TIMEOUT = 12000;
const unsigned long CHEM_TIMEOUT = 12000;
const int NWIN = 10;

int rBuf[NWIN], gBuf[NWIN], bBuf[NWIN], idxRGB = 0, filledRGB = 0;
float pHBuf[NWIN], tdsBuf[NWIN], idxChem = 0, filledChem = 0;

unsigned long stateStart = 0;

struct RGB { int r, g, b; };

// Struktur untuk data kekeruhan
struct TurbidityData {
    String level;        // Level kekeruhan (Jernih, Keruh, Sangat Keruh)
    float ntu;         // Estimasi NTU (Nephelometric Turbidity Units)
    String interpretation; // Interpretasi klinis
};

// --- Function Prototypes ---
RGB readRGB();
void readChem(float& pH, float& tds);
bool isRGBStable();
bool isChemStable();
void ledBlinkSlow();
void ledBlinkFast();
void ledOn();
void ledOff();
float tdsToSpecificGravity(float tds);
TurbidityData analyzeTurbidity(float tds, int r, int g, int b);
String analyzeUrine(int r, int g, int b, float pH, float specificGravity, TurbidityData turbidity);

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
    
    Serial.println("=== SISTEM ANALISIS URINE ===");
    Serial.println("Fitur: pH, TDS, Berat Jenis, Kekeruhan");
}

void loop() {
    static RGB lastRGB { 0, 0, 0 };
    static float lastPH = 0, lastTDS = 0;

    switch (state) {
        case MEASURE_COLOR: {
            ledOff();
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
            if (millis() - stateStart > 10000) {
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
            int redColor = lastRGB.r, greenColor = lastRGB.g, blueColor = lastRGB.b;
            
            // Konversi TDS ke berat jenis
            float specificGravity = tdsToSpecificGravity(lastTDS);
            
            // Analisis kekeruhan
            TurbidityData turbidity = analyzeTurbidity(lastTDS, redColor, greenColor, blueColor);
            
            // ========== KLASIFIKASI WARNA DIPERBAIKI ==========
            String warnaDasar = "Tidak diketahui";
            float total = redColor + greenColor + blueColor;
            
            if (total > 0) {
                float rRatio = (float)redColor / total;
                float gRatio = (float)greenColor / total;
                float bRatio = (float)blueColor / total;
                
                // Hitung perbedaan antar channel
                int r_g_diff = redColor - greenColor;
                int r_b_diff = redColor - blueColor;
                int b_r_diff = blueColor - redColor;
                int b_g_diff = blueColor - greenColor;
                
                // PRIORITAS 1: Deteksi BENING/JERNIH (RGB sangat tinggi dan seimbang)
                if (redColor > 200 && greenColor > 200 && blueColor > 200) {
                    warnaDasar = "BENING";
                }
                // PRIORITAS 2: Deteksi KUNING (Red dominan atau seimbang dengan sedikit lebih tinggi)
                // Urine normal kuning: R ≈ G ≈ B atau R sedikit > G,B
                else if (redColor >= 80 && redColor <= 180 && 
                        greenColor >= 70 && greenColor <= 170 && 
                        blueColor >= 80 && blueColor <= 180) {
                    
                    // Cek apakah Blue terlalu dominan (>30 dari Red) → BIRU
                    if (b_r_diff > 30 && b_g_diff > 25) {
                        warnaDasar = "BIRU";
                    }
                    // Cek apakah Red dominan → KUNING TUA/ORANGE
                    else if (r_b_diff > 20 && r_g_diff > 15) {
                        warnaDasar = "KUNING TUA";
                    }
                    // RGB relatif seimbang atau Red sedikit lebih tinggi → KUNING/KUNING MUDA
                    else if (abs(r_b_diff) <= 30 && abs(r_g_diff) <= 20) {
                        // Cek brightness untuk bedakan kuning muda vs kuning
                        if (total < 330) { // RGB rendah = pekat
                            warnaDasar = "KUNING MUDA"; // Kuning pekat
                        } else {
                            warnaDasar = "KUNING"; // Kuning normal
                        }
                    }
                    else {
                        warnaDasar = "KUNING"; // Default untuk range ini
                    }
                }
                // PRIORITAS 3: Deteksi KUNING TERANG (RGB tinggi, R dominan)
                else if (redColor > 180 && greenColor > 150 && blueColor > 120 && 
                        r_b_diff > 10 && rRatio > 0.33) {
                    warnaDasar = "KUNING";
                }
                // PRIORITAS 4: Deteksi BIRU (Blue jelas dominan)
                else if (b_r_diff > 30 && b_g_diff > 20) {
                    warnaDasar = "BIRU";
                }
                // PRIORITAS 5: Deteksi MERAH (Red jelas dominan)
                else if (r_g_diff > 35 && r_b_diff > 30) {
                    warnaDasar = "MERAH";
                }
                // PRIORITAS 6: RGB sangat rendah
                else if (redColor < 80 && greenColor < 80 && blueColor < 80) {
                    warnaDasar = "GELAP";
                }
                // Default
                else {
                    warnaDasar = "CAMPURAN";
                }
            }
            
            // Analisis komprehensif
            String analisis = analyzeUrine(redColor, greenColor, blueColor, lastPH, specificGravity, turbidity);

            Serial.println("\n=== HASIL ANALISIS ===");
            Serial.printf("RGB: R:%d G:%d B:%d\n", redColor, greenColor, blueColor);
            Serial.printf("Perbedaan: R-B=%d, R-G=%d, B-R=%d\n", 
                          redColor-blueColor, redColor-greenColor, blueColor-redColor);
            Serial.printf("pH: %.2f\n", lastPH);
            Serial.printf("TDS: %.1f mg/L\n", lastTDS);
            Serial.printf("Berat Jenis: %.3f\n", specificGravity);
            Serial.printf("Kekeruhan: %s (~%.1f NTU)\n", turbidity.level.c_str(), turbidity.ntu);
            Serial.printf("Warna: %s\n", warnaDasar.c_str());
            Serial.printf("Interpretasi: %s\n", turbidity.interpretation.c_str());
            Serial.printf("Analisis: %s\n", analisis.c_str());
            Serial.println("=====================\n");

            // Kirim ke server
            if (WiFi.status() == WL_CONNECTED) {
                WiFiClient client;
                HTTPClient http;
                http.begin(client, serverName);
                http.addHeader("Content-Type", "application/json");

                String jsonData = "{";
                jsonData += "\"ph\":" + String(lastPH, 2) + ",";
                jsonData += "\"tds\":" + String(lastTDS, 2) + ",";
                jsonData += "\"specificGravity\":" + String(specificGravity, 3) + ",";
                jsonData += "\"turbidityLevel\":\"" + turbidity.level + "\",";
                jsonData += "\"turbidityNTU\":" + String(turbidity.ntu, 1) + ",";
                jsonData += "\"turbidityInterpretation\":\"" + turbidity.interpretation + "\",";
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

            // Reset LED
            digitalWrite(LED_WIFI, LOW);
            delay(50);
            digitalWrite(LED_WIFI, HIGH);
            delay(50);
            digitalWrite(LED_WIFI, LOW);
            delay(50);
            digitalWrite(LED_WIFI, HIGH);

            // Reset buffer
            for (int i = 0; i < NWIN; i++) {
                rBuf[i] = gBuf[i] = bBuf[i] = 0;
                pHBuf[i] = tdsBuf[i] = 0;
            }
            filledRGB = filledChem = idxRGB = idxChem = 0;
            
            delay(5000);
            
            state = MEASURE_COLOR;
            stateStart = 0;
            break;
        }
    }
    yield();
}

// --- FUNGSI PEMBANTU ---

RGB readRGB() {
    digitalWrite(S2, LOW);
    digitalWrite(S3, LOW);
    int redFreq = pulseIn(sensorOut, LOW, 100000);
    int r = map(constrain(redFreq, 30, 300), 30, 300, 255, 0);
    delay(50);
    yield();

    digitalWrite(S2, HIGH);
    digitalWrite(S3, HIGH);
    int greenFreq = pulseIn(sensorOut, LOW, 100000);
    int g = map(constrain(greenFreq, 30, 300), 30, 300, 255, 0);
    delay(50);
    yield();

    digitalWrite(S2, LOW);
    digitalWrite(S3, HIGH);
    int blueFreq = pulseIn(sensorOut, LOW, 100000);
    int b = map(constrain(blueFreq, 30, 300), 30, 300, 255, 0);
    delay(50);
    yield();

    return { r, g, b };
}

void readChem(float& pH, float& tds) {
    // Baca pH
    int adcPh = ads.readADC_SingleEnded(0);
    yield();
    float voltPh = adcPh * 0.1875 / 1000.0;
    pH = PH_SLOPE * voltPh + PH_OFFSET;
    
    if (pH < 0) pH = 0;
    if (pH > 14) pH = 14;
    
    delay(50);
    yield();

    // Baca TDS
    int adcTds = ads.readADC_SingleEnded(1);
    yield();
    float voltTds = adcTds * 0.1875 / 1000.0;
    tds = (133.42 * pow(voltTds, 3) - 255.86 * pow(voltTds, 2) + 857.39 * voltTds) * 0.5;
    
    delay(50);
    yield();
}

// Konversi TDS ke Berat Jenis
float tdsToSpecificGravity(float tds) {
    float sg;
    if (tds < 50) {
        sg = 1.005;
    } else if (tds <= 500) {
        sg = 1.005 + (tds - 50) * 0.000011;
    } else if (tds <= 1500) {
        sg = 1.010 + (tds - 500) * 0.000015;
    } else {
        sg = 1.025 + (tds - 1500) * 0.000003;
    }
    
    if (sg < 1.005) sg = 1.005;
    if (sg > 1.030) sg = 1.030;
    
    return sg;
}

// Analisis Kekeruhan berdasarkan TDS dan RGB - VERSI 3 TINGKAT (FINAL FIX)
TurbidityData analyzeTurbidity(float tds, int r, int g, int b) {
    TurbidityData result;
    
    // Hitung brightness (kecerahan) dari RGB
    float brightness = (r + g + b) / 3.0;
    
    // Hitung variance (variasi) warna - indikator kekeruhan visual
    float avgColor = (r + g + b) / 3.0;
    float variance = (pow(r - avgColor, 2) + pow(g - avgColor, 2) + pow(b - avgColor, 2)) / 3.0;
    
    // Hitung color saturation (tingkat warna vs grayscale)
    int minRGB = min(min(r, g), b);
    int maxRGB = max(max(r, g), b);
    float saturation = (maxRGB - minRGB);
    
    // Hitung color uniformity (seberapa seragam warna RGB)
    float colorUniformity = 1.0 - (saturation / 255.0);
    
    // Estimasi NTU BASE berdasarkan TDS (DIREVISI: lebih rendah untuk jernih)
    float ntu = 0;
    
    if (tds < 300) {
        ntu = tds * 0.01; // Sangat encer (0-3 NTU)
    } else if (tds <= 800) {
        ntu = 3 + (tds - 300) * 0.006; // Normal (3-6 NTU)
    } else if (tds <= 1200) {
        ntu = 6 + (tds - 800) * 0.008; // Pekat tapi jernih (6-9.2 NTU)
    } else {
        ntu = 9.2 + (tds - 1200) * 0.015; // Sangat pekat (>9.2 NTU)
    }
    
    // DETEKSI KHUSUS 1: Urine JERNIH PEKAT (TDS tinggi, RGB gelap tapi konsisten)
    // Ciri: TDS > 1000, brightness rendah, variance rendah, saturation sedang
    bool isJernihPekat = (tds > 1000) && (brightness < 130) && (variance < 200) && (saturation > 15 && saturation < 40);
    
    if (isJernihPekat) {
        // Urine pekat tapi jernih (kuning tua/orange pekat)
        ntu *= 0.5; // Kurangi drastis karena jernih
        Serial.println("[DETEKSI] Jernih Pekat - TDS tinggi tapi tidak keruh");
    }
    
    // DETEKSI KHUSUS 2: Urine KERUH PUTIH (bakteri/pus)
    // Ciri: brightness tinggi, variance rendah, saturation rendah (warna putih seragam)
    bool isKeruhPutih = (brightness > 160) && (variance < 150) && (saturation < 25);
    
    if (isKeruhPutih) {
        ntu *= 2.8;
        ntu += 25; // Tambah NTU besar untuk keruh putih
        Serial.println("[DETEKSI] Keruh Putih - Kemungkinan bakteri/pus");
    }
    
    // DETEKSI KHUSUS 3: Urine JERNIH TERANG (banyak minum air)
    // Ciri: brightness tinggi, variance sedang/tinggi (bukan putih seragam)
    bool isJernihTerang = (brightness > 160) && (variance > 150 || saturation > 25);
    
    if (isJernihTerang && !isKeruhPutih) {
        ntu *= 0.6; // Kurangi karena jernih
        Serial.println("[DETEKSI] Jernih Terang - Hidrasi baik");
    }
    
    // Faktor koreksi brightness umum (jika belum ada deteksi khusus)
    if (!isJernihPekat && !isKeruhPutih && !isJernihTerang) {
        if (brightness > 140) {
            ntu *= 0.85;
        } else if (brightness < 100) {
            ntu *= 1.3; // Gelap = lebih keruh
        }
    }
    
    // Faktor koreksi variance (warna tidak seragam = lebih keruh)
    if (variance > 500) {
        ntu *= 1.4;
    } else if (variance > 300 && !isJernihPekat) {
        ntu *= 1.2;
    }
    
    result.ntu = ntu;
    
    // Klasifikasi level kekeruhan - 3 TINGKAT
    if (ntu <= 10) {
        result.level = "Jernih";
        result.interpretation = "Urine jernih, tidak ada kekeruhan";
    } else if (ntu < 30) {
        result.level = "Agak Keruh";
        result.interpretation = "Urine sedikit keruh";
    } else {
        result.level = "Keruh";
        result.interpretation = "Urine keruh";
    }
    
    return result;
}

// Analisis Urine Komprehensif
String analyzeUrine(int r, int g, int b, float pH, float specificGravity, TurbidityData turbidity) {
    String result = "";
    
    // Deteksi air bening/bukan urine (perbaikan: hapus kondisi yang mematikan analisis)
    if ((r + g + b < 50 && specificGravity < 1.008)) {
        result += "AIR BENING (BUKAN URINE)";
    } else {
      // Analisis warna
      if (r > 230 && g > 230 && b > 220) {
          result += "BENING/JERNIH";
      } else if (r >= 120 && r <= 150 && g >= 110 && g <= 130 && b >= 140 && b <= 160) {
          result += "KUNING NORMAL";
      } else if (r >= 110 && r <= 140 && g >= 100 && g <= 125 && b >= 130 && b <= 150) {
          result += "KUNING MUDA";
      } else if (b > r + 20 && b > g + 15) {
          result += "KEBIRUAN/OBAT";
      } else if (r > g + 30 && r > b + 20) {
          result += "KEMERAHAN/DARAH";
      } else if (r >= 100 && r <= 130 && g >= 90 && g <= 120 && b >= 120 && b <= 150) {
          result += "KUNING KERUH";
      } else {
          result += "WARNA TIDAK NORMAL";
      }
      
      // Analisis pH
      if (pH < 4.5) {
          result += " + pH SANGAT ASAM";
      } else if (pH >= 4.5 && pH < 6.0) {
          result += " + pH ASAM NORMAL";
      } else if (pH >= 6.0 && pH <= 7.5) {
          result += " + pH NORMAL";
      } else if (pH > 7.5 && pH <= 8.5) {
          result += " + pH BASA NORMAL";
      } else {
          result += " + pH SANGAT BASA";
      }
      
      // Analisis berat jenis
      if (specificGravity < 1.008) {
          result += " + HIDRASI BAIK";
      } else if (specificGravity >= 1.008 && specificGravity <= 1.020) {
          result += " + HIDRASI NORMAL";
      } else if (specificGravity > 1.020 && specificGravity <= 1.025) {
          result += " + SEDIKIT PEKAT";
      } else {
          result += " + PEKAT/DEHIDRASI";
      }
      
      // Tambahkan info kekeruhan
      result += " + " + turbidity.level;
      
      // Deteksi kondisi khusus
      if (pH < 5.0 && specificGravity > 1.022 && turbidity.ntu > 30) {
          result += " [PERHATIAN: ASAM+PEKAT+KERUH - Risiko Batu Ginjal]";
      } else if (pH > 8.0 && turbidity.ntu > 40) {
          result += " [PERHATIAN: BASA+KERUH - Kemungkinan Infeksi]";
      } else if (specificGravity < 1.010 && turbidity.ntu > 25) {
          result += " [ANOMALI: Encer tapi Keruh - Periksa Infeksi]";
      }
    }
    
    return result;
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