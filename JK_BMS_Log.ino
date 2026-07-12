#include <FS.h> 
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEUtils.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include "WifiSetupPage.h"
#include <DNSServer.h>       

DNSServer dnsServer;        
const byte DNS_PORT = 53;    

const unsigned long UPLOAD_CYCLE = 30000;   
const unsigned long SNAPSHOT_LEAD = 10000;  

// กำหนดขาปุ่ม BOOT บน ESP32-S3 สำหรับทำ Factory Reset
#define BOOT_BUTTON 0

String bmsMac = ""; 
String wifiSsid = "";
String wifiPass = "";
String webappUrl = "";
WebServer server(80);
Preferences preferences; 

SemaphoreHandle_t dataMutex;

// Global Variables (Core 1)
int g_soc = 0;
float g_totalVoltage = 0.0;
float g_current = 0.0;
float g_power = 0.0;
float g_capacityRemain = 0.0;
float g_capacityFull = 0.0; 
float g_mosTemp = 0.0;
float g_temp1 = 0.0;
float g_temp2 = 0.0;
float g_maxV = 0.0;
float g_minV = 5.0;
float g_volDiff = 0.0;
float g_cellVoltage[8] = {0.0};
bool g_hasValidCell = false;

// Temp Variables (Core 0)
int t_soc = 0;
float t_totalVoltage = 0.0;
float t_current = 0.0;
float t_power = 0.0;
float t_capacityRemain = 0.0;
float t_capacityFull = 0.0;
float t_mosTemp = 0.0;
float t_temp1 = 0.0;
float t_temp2 = 0.0;
float t_maxV = 0.0;
float t_minV = 5.0;
float t_volDiff = 0.0;
float t_cellVoltage[8] = {0.0};
bool t_hasSnapshot = false; 

static BLEUUID SERVICE_UUID((uint16_t)0xFFE0);
static BLEUUID CHAR_UUID((uint16_t)0xFFE1);
BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pChar = nullptr;

std::vector<uint8_t> frameBuffer;
bool connected = false;
unsigned long lastNotifyTime = 0;
unsigned long lastSend = 0;
unsigned long lastReconnectAttempt = 0;
int failCount = 0; 

TFT_eSPI tft = TFT_eSPI(); 
#define TFT_BL 45

void connectToBMS();
void drawStaticUI();

uint16_t getU16(std::vector<uint8_t>& d, size_t i) { return (uint16_t)d[i] | ((uint16_t)d[i+1]<<8); }
uint32_t getU32(std::vector<uint8_t>& d, size_t i) { return getU16(d,i) | ((uint32_t)getU16(d,i+2)<<16); }
int16_t getS16(std::vector<uint8_t>& d, size_t i) { return (int16_t)getU16(d,i); }
int32_t getS32(std::vector<uint8_t>& d, size_t i) { return (int32_t)getU32(d,i); }

uint8_t simpleChecksum(uint8_t* data, size_t len) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; i++) sum += data[i];
  return (uint8_t)(sum & 0xFF);
}

void sendCommand(uint8_t command) {
  uint8_t frame[20] = {0};
  frame[0] = 0xAA; frame[1] = 0x55; frame[2] = 0x90; frame[3] = 0xEB;
  frame[4] = command;
  frame[19] = simpleChecksum(frame, 19);
  if (pChar) pChar->writeValue(frame, 20, true);
}

uint16_t getTempColor(float temp) {
  if (temp >= 55.0) return TFT_RED;      
  if (temp >= 40.0) return TFT_YELLOW;   
  return TFT_CYAN;                       
}

uint16_t getSocColor(uint8_t soc) {
  if (soc < 20) return TFT_RED;         
  if (soc < 50) return TFT_ORANGE;      
  if (soc < 70) return TFT_YELLOW;      
  if (soc >= 90) return TFT_GREEN;      
  return TFT_GREENYELLOW;               
}

void updateTopBar(uint16_t barColor, String text) {
  tft.fillRect(0, 0, 320, 25, barColor);
  tft.setTextColor(TFT_WHITE, barColor);
  tft.drawCentreString(text, 160, 4, 2);
}

void drawStaticUI() {
  tft.fillScreen(TFT_BLACK);
  String topStr = "Connected | IP: " + WiFi.localIP().toString();
  updateTopBar(TFT_DARKGREEN, topStr);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Vol.", 5, 35, 4);     
  tft.drawString("SOC", 5, 70, 4);     
  tft.drawString("Cap", 5, 105, 4);    
  tft.drawString("  A.", 160, 35, 4);   
  tft.drawString("Diff", 160, 70, 4);  
  tft.drawString("T1:", 5, 142, 2);   
  tft.drawString("T2:", 75, 142, 2);  
  tft.drawString("MOS:", 145, 142, 2); 
  tft.drawLine(0, 165, 320, 165, TFT_DARKGREY);
}

void decodeCellInfo(std::vector<uint8_t>& d) {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    g_totalVoltage = getU32(d, 150) * 0.001f;
    g_current      = getS32(d, 158) * 0.001f;
    g_soc           = d[173];
    g_capacityRemain   = getU32(d, 174) * 0.001f;
    g_mosTemp   = getS16(d, 144) * 0.1f;
    g_temp1     = getS16(d, 162) * 0.1f;
    g_temp2     = getS16(d, 164) * 0.1f;
    
    float parsedFullCap = getU32(d, 178) * 0.001f;
    if(parsedFullCap > 10.0) g_capacityFull = parsedFullCap; 

    g_power = g_totalVoltage * g_current;
    g_maxV = 0; g_minV = 5.0;
    g_hasValidCell = false;

    for (int i = 0; i < 8; i++) {
      g_cellVoltage[i] = getU16(d, 6 + i*2) * 0.001f;
      if (g_cellVoltage[i] > 0.5) { 
        g_hasValidCell = true;
        if (g_cellVoltage[i] > g_maxV) g_maxV = g_cellVoltage[i];
        if (g_cellVoltage[i] < g_minV) g_minV = g_cellVoltage[i];
      }
    }
    g_volDiff = g_hasValidCell ? (g_maxV - g_minV) : 0;
    
    xSemaphoreGive(dataMutex);
  }

  char buf[25]; 
  uint16_t socColor = getSocColor(g_soc);
  
  tft.setTextColor(socColor, TFT_BLACK); 
  sprintf(buf, "%.2fV  ", g_totalVoltage); tft.drawString(buf, 65, 35, 4);
  tft.setTextColor((g_current < 0) ? TFT_RED : TFT_GREEN, TFT_BLACK); 
  sprintf(buf, "%.2fA  ", g_current); tft.drawString(buf, 220, 35, 4);

  tft.setTextColor(socColor, TFT_BLACK); 
  sprintf(buf, "%d%%  ", g_soc); tft.drawString(buf, 65, 70, 4);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  sprintf(buf, "%.3fV  ", g_volDiff); tft.drawString(buf, 220, 70, 4); 

  tft.setTextColor(socColor, TFT_BLACK); 
  sprintf(buf, "%d/%dAh  ", (int)g_capacityRemain, (int)g_capacityFull); 
  tft.drawString(buf, 65, 108, 4); 

  tft.setTextColor(getTempColor(g_temp1), TFT_BLACK);
  sprintf(buf, "%.1fC ", g_temp1); tft.drawString(buf, 28, 142, 2);
  tft.setTextColor(getTempColor(g_temp2), TFT_BLACK);
  sprintf(buf, "%.1fC ", g_temp2); tft.drawString(buf, 100, 142, 2);
  tft.setTextColor(getTempColor(g_mosTemp), TFT_BLACK);
  sprintf(buf, "%.1fC   ", g_mosTemp); tft.drawString(buf, 185, 142, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int startY = 175;
  for (int i = 0; i < 8; i++) {
    int col = i % 4; int row = i / 4;
    int x = 10 + (col * 75); int y = startY + (row * 25);
    if (g_hasValidCell && g_cellVoltage[i] == g_maxV) tft.setTextColor(TFT_RED, TFT_BLACK);
    else if (g_hasValidCell && g_cellVoltage[i] == g_minV) tft.setTextColor(TFT_BLUE, TFT_BLACK);
    else tft.setTextColor(TFT_WHITE, TFT_BLACK);
    sprintf(buf, "%d. %.3fV  ", i+1, g_cellVoltage[i]); tft.drawString(buf, x, y, 2);
  }
}

void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  lastNotifyTime = millis();
  frameBuffer.insert(frameBuffer.end(), pData, pData + length);

  while (frameBuffer.size() >= 4) {
    if (frameBuffer[0] == 0x55 && frameBuffer[1] == 0xAA && frameBuffer[2] == 0xEB && frameBuffer[3] == 0x90) {
      if (frameBuffer.size() < 250) return;
      if (frameBuffer[4] == 0x02) decodeCellInfo(frameBuffer);
      size_t frameLen = 300;
      if (frameBuffer.size() >= frameLen) frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + frameLen);
      else frameBuffer.clear();
    } else {
      frameBuffer.erase(frameBuffer.begin());
    }
  }
  if (frameBuffer.size() > 512) frameBuffer.clear();
}

void connectToBMS() {
  pChar = nullptr;
  if (pClient->isConnected()) pClient->disconnect();

  String ipStr = "Connecting BMS | IP: " + WiFi.localIP().toString();
  updateTopBar(TFT_NAVY, ipStr);

  BLEAddress BMS_ADDRESS(bmsMac.c_str());

  if (pClient->connect(BMS_ADDRESS)) {
    failCount = 0; 
    String successStr = "BMS Connected | IP: " + WiFi.localIP().toString();
    updateTopBar(TFT_DARKGREEN, successStr);

    pClient->setMTU(255);
    BLERemoteService* pService = pClient->getService(SERVICE_UUID);
    if (pService != nullptr) {
      pChar = pService->getCharacteristic(CHAR_UUID);
      if (pChar && pChar->canNotify()) pChar->registerForNotify(notifyCallback);
    }

    connected = true;
    lastNotifyTime = lastSend = millis();
    lastReconnectAttempt = millis();
    
    delay(1000); 
    sendCommand(0x90); delay(800);
    sendCommand(0x97); delay(800);
    sendCommand(0x96);
  } else {
    failCount++; 
    
    if (failCount >= 3) {
      updateTopBar(TFT_MAROON, "BLE Error! Rebooting...");
      delay(1500); 
      ESP.restart(); 
    } else {
      char failMsg[45];
      sprintf(failMsg, "BMS Fail (%d) | IP: %s", failCount, WiFi.localIP().toString().c_str());
      updateTopBar(TFT_MAROON, failMsg);
    }
    connected = false;
    lastReconnectAttempt = millis();
  }
}

void uploadTask(void * pvParameters) {
  unsigned long cycleStartTime = millis();
  bool snapshotTakenInThisCycle = false;

  for(;;) {
    unsigned long elapsed = millis() - cycleStartTime;

    if (elapsed >= (UPLOAD_CYCLE - SNAPSHOT_LEAD) && !snapshotTakenInThisCycle) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_hasValidCell) {
          t_soc = g_soc;
          t_totalVoltage = g_totalVoltage;
          t_current = g_current;
          t_power = g_power;
          t_capacityRemain = g_capacityRemain;
          t_capacityFull = g_capacityFull;
          t_mosTemp = g_mosTemp;
          t_temp1 = g_temp1;
          t_temp2 = g_temp2;
          t_maxV = g_maxV;
          t_minV = g_minV;
          t_volDiff = g_volDiff;
          
          for(int i = 0; i < 8; i++) t_cellVoltage[i] = g_cellVoltage[i];
          
          t_hasSnapshot = true;
          Serial.println("[Core 0] Temp Snapshot Taken Successfully!");
        }
        xSemaphoreGive(dataMutex);
      }
      snapshotTakenInThisCycle = true;
    }

    if (elapsed >= UPLOAD_CYCLE) {
      cycleStartTime = millis(); 
      snapshotTakenInThisCycle = false; 

      if (WiFi.status() == WL_CONNECTED && t_hasSnapshot) {
        Serial.println("[Core 0] Initializing HTTPS Upload Process...");
        
        String cellVStr = "";
        for (int i = 0; i < 8; i++) {
          cellVStr += String(t_cellVoltage[i], 3);
          if (i < 7) cellVStr += ",";
        }

        String url = webappUrl + "?soc=" + String(t_soc) +
                     "&totalVoltage=" + String(t_totalVoltage, 2) +
                     "&current=" + String(t_current, 2) +
                     "&power=" + String(t_power, 2) +
                     "&capacityRemain=" + String(t_capacityRemain, 2) +
                     "&capacityFull=" + String(t_capacityFull, 2) +
                     "&mosTemp=" + String(t_mosTemp, 1) +
                     "&temp1=" + String(t_temp1, 1) +
                     "&temp2=" + String(t_temp2, 1) +
                     "&maxV=" + String(t_maxV, 3) +
                     "&minV=" + String(t_minV, 3) +
                     "&deltaV=" + String(t_volDiff, 3) +
                     "&cellV=" + cellVStr; 

        WiFiClientSecure client;
        client.setInsecure(); 
        
        HTTPClient http;
        http.begin(client, url);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
        
        int httpCode = http.GET();
        if (httpCode > 0) {
          Serial.printf("[Core 0] Google Sheets Response Code: %d\n", httpCode);
        } else {
          Serial.printf("[Core 0] HTTPS Error: %s\n", http.errorToString(httpCode).c_str());
        }
        http.end();

        t_soc = 0; t_totalVoltage = 0; t_current = 0; t_power = 0;
        t_capacityRemain = 0; t_capacityFull = 0; t_mosTemp = 0; t_temp1 = 0; t_temp2 = 0;
        t_maxV = 0; t_minV = 5.0; t_volDiff = 0;
        memset(t_cellVoltage, 0, sizeof(t_cellVoltage));
        
        t_hasSnapshot = false; 
        Serial.println("[Core 0] Temp Variables Cleared.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // ตั้งค่าปุ่ม BOOT สำหรับ Reset
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  dataMutex = xSemaphoreCreateMutex();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);         
  tft.invertDisplay(true);    

  preferences.begin("bms_store", false);
  
  bmsMac = preferences.getString("bms_mac", ""); 
  wifiSsid = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");
  webappUrl = preferences.getString("webapp_url", "");

  if (bmsMac == "" || bmsMac.length() < 17 || wifiSsid == "" || webappUrl == "") {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString("Creating Hotspot...", 10, 50, 4);

    const char* ap_ssid = "JK_BMS_Setup";
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid); 
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Hotspot Active!", 10, 25, 4);
    
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("1. Connect Wi-Fi:", 10, 60, 2); 
    tft.setTextColor(TFT_CYAN, TFT_BLACK); 
    tft.drawString(ap_ssid, 10, 80, 4);    

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("2. Auto Login Webpage", 10, 115, 2); 
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Waiting for Config...", 10, 135, 4); 

    server.on("/", handleRoot);
    server.onNotFound(handleRoot); 
    server.on("/save", handleSave);
    server.begin();

    while (true) {
      dnsServer.processNextRequest(); 
      server.handleClient();
      delay(1);
    }
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("Connecting Wi-Fi...", 10, 50, 4);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  
  // ระบบ 1: จับเวลา 15 วินาที ถ้าเชื่อม Wi-Fi ไม่สำเร็จ ให้ลบข้อมูลแล้วเด้งไปหน้า Hotspot
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED && counter < 30) {  // 30 รอบ * 500ms = 15 วินาที
    delay(500);
    Serial.print(".");
    counter++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("Wi-Fi Failed!", 160, 80, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Resetting to Hotspot...", 160, 120, 2);
    delay(2000);
    
    preferences.clear(); // ล้างข้อมูล Wi-Fi ที่เซฟไว้ผิดพลาดทิ้ง
    ESP.restart();       // รีสตาร์ทเพื่อเข้าสู่โหมดหน้าจอสีเขียว (Hotspot)
  }

  drawStaticUI();             

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();

  for (int i = 3; i > 0; i--) {
    String countdownStr = "Init BLE Stack... " + String(i) + "s | IP: " + WiFi.localIP().toString();
    updateTopBar(TFT_NAVY, countdownStr);
    delay(1000); 
  }

  BLEDevice::init("");
  pClient = BLEDevice::createClient(); 
  connectToBMS();

  xTaskCreatePinnedToCore(
    uploadTask,        
    "UploadTask",      
    8192,              
    NULL,              
    1,                 
    NULL,              
    0                  
  );
}

void loop() {
  // ระบบ 2: กดปุ่ม BOOT ค้างไว้ 5 วินาที เพื่อทำ Factory Reset ข้อมูลทั้งหมด
  static unsigned long buttonPressTime = 0;
  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (buttonPressTime == 0) {
      buttonPressTime = millis();
    } else if (millis() - buttonPressTime > 5000) {
      tft.fillScreen(TFT_RED);
      tft.setTextColor(TFT_WHITE, TFT_RED);
      tft.drawCentreString("FACTORY RESET", 160, 100, 4);
      delay(1500);
      preferences.clear();
      ESP.restart();
    }
  } else {
    buttonPressTime = 0;
  }

  server.handleClient();

  // หาก Wi-Fi หลุดระหว่างทำงาน (ไม่ใช่ตอนบูต) จะแค่ดักจับแล้วพยายามต่อใหม่เงียบๆ
  if (WiFi.status() != WL_CONNECTED && bmsMac != "") {
    static unsigned long lastWifiRetry = 0;
    if (millis() - lastWifiRetry > 20000) { 
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
      lastWifiRetry = millis();
    }
  }

  if (!connected) {
    if (millis() - lastReconnectAttempt > 6000) {
      connectToBMS();
    }
    delay(100);
    return;
  }

  if (millis() - lastSend > 5000) { 
    lastSend = millis();
    sendCommand(0x96);
  }

  if (millis() - lastNotifyTime > 20000) { 
    String closeStr = "BMS Disconnect! Retry | IP: " + WiFi.localIP().toString();
    updateTopBar(TFT_MAROON, closeStr);
    
    connected = false;
    if (pClient->isConnected()) pClient->disconnect();
    lastReconnectAttempt = millis();
    frameBuffer.clear(); 
  }
  
  delay(10); 
}