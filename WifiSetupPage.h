#ifndef WIFI_SETUP_PAGE_H
#define WIFI_SETUP_PAGE_H

#include <Arduino.h>
#include <WebServer.h>
#include <Preferences.h>

extern WebServer server;
extern String bmsMac;
extern String wifiSsid;
extern String wifiPass;
extern String webappUrl;

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background:#f0f2f5;}";
  html += ".card{background:white;padding:25px;border-radius:12px;box-shadow:0 4px 6px rgba(0,0,0,0.1);max-width:400px;margin:auto;}";
  html += "h2{color:#0056b3;text-align:center;margin-bottom:20px;}";
  html += "label{font-weight:bold;display:block;margin-top:15px;color:#333;}";
  html += "input{width:100%;padding:12px;margin-top:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:6px;font-size:16px;}";
  html += ".pass-container{position:relative;}";
  html += ".toggle-btn{position:absolute;right:10px;top:18px;background:none;border:none;color:#0056b3;font-size:14px;font-weight:bold;cursor:pointer;user-select:none;}";
  html += "button.submit-btn{width:100%;padding:14px;background:#28a745;color:white;border:none;border-radius:6px;font-size:16px;font-weight:bold;margin-top:25px;cursor:pointer;}";
  html += "button.submit-btn:hover{background:#218838;}";
  html += "</style></head><body>";
  
  html += "<div class='card'><h2>JK BMS Settings Panel</h2>";
  html += "<form action='/save' method='POST'>";
  
  html += "<label>1. MAC Address ของ JK BMS:</label>";
  html += "<input type='text' name='mac' value='" + bmsMac + "' placeholder='aa:bb:cc:11:22:33' required>";
  
  html += "<label>2. ชื่อ Wi-Fi (SSID):</label>";
  html += "<input type='text' name='ssid' value='" + wifiSsid + "' placeholder='HomeWiFi_2.4G' required>";
  
  html += "<label>3. รหัสผ่าน Wi-Fi:</label>";
  html += "<div class='pass-container'>";
  html += "<input type='password' id='password' name='password' value='" + wifiPass + "' placeholder='Min 8 Characters'>";
  html += "<span class='toggle-btn' id='togglePass' onclick='togglePasswordVisibility()'>แสดง</span>";
  html += "</div>";
  
  html += "<label>4. Google Web App URL:</label>";
  html += "<input type='text' name='webapp' value='" + webappUrl + "' placeholder='https://script.google.com/macros/s/.../exec' required>";
  
  html += "<button type='submit' class='submit-btn'>บันทึกค่าและรีสตาร์ท</button></form></div>";
  
  // JavaScript สำหรับปุ่มแสดงรหัสผ่าน
  html += "<script>";
  html += "function togglePasswordVisibility() {";
  html += "  var pInput = document.getElementById('password');";
  html += "  var tBtn = document.getElementById('togglePass');";
  html += "  if (pInput.type === 'password') {";
  html += "    pInput.type = 'text';";
  html += "    tBtn.innerText = 'ซ่อน';";
  html += "  } else {";
  html += "    pInput.type = 'password';";
  html += "    tBtn.innerText = 'แสดง';";
  html += "  }";
  html += "}";
  html += "</script>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("mac") && server.hasArg("ssid") && server.hasArg("webapp")) {
    bmsMac = server.arg("mac");
    wifiSsid = server.arg("ssid");
    wifiPass = server.arg("password");
    webappUrl = server.arg("webapp");
    
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body style='text-align:center;font-family:sans-serif;margin-top:50px;'>";
    html += "<h2 style='color:green;'>บันทึกสำเร็จ!</h2><p>ระบบกำลังรีบูตเพื่อทำงานด้วยค่าใหม่ที่คุณตั้งไว้...</p></body></html>";
    server.send(200, "text/html", html);
    
    delay(1500);
    
    extern Preferences preferences;
    preferences.putString("bms_mac", bmsMac);
    preferences.putString("wifi_ssid", wifiSsid);
    preferences.putString("wifi_pass", wifiPass);
    preferences.putString("webapp_url", webappUrl);
    
    ESP.restart(); 
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

#endif