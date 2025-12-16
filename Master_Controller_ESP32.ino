/*
 * 🦾 ESP32 Host - V24.3 (Smart Silence + 3s Buffer)
 * 修改：
 * 1. [Silence] 增加 disableHeartbeatUntil 機制。
 * 2. [Buffer] 移動指令後，額外增加 3000ms (3秒) 的勿擾緩衝時間。
 * 3. [Benefit] 徹底避免 C3 在動作剛結束時因接收心跳包而產生的不穩。
 */

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>

// OLED 設定 (I2C)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 22, 21);
// C3 通訊腳位
#define C3_RX_PIN 16 
#define C3_TX_PIN 17
HardwareSerial C3Serial(2);

// 控制腳位
#define DETECT_PIN 4 
#define RELAY_PIN 23 

// WiFi 設定
const char *ssid = "ROG Phone 6D";
const char *password = "qwertyuiopasdfghjklzxcvbnm";
WebSocketsServer webSocket = WebSocketsServer(81);
JsonDocument doc;

// 系統狀態變數
int maxID = 0;
int lastKnownCount = 0;
bool systemSafe = false;     
bool requireRescan = false;  
bool isInitializing = false; 
bool lastDetectState = HIGH;

bool isAppConnected = false;
String currentStatusText = "Init...";

// 位置記憶
int motorPositions[13]; 

// 時間控制變數
unsigned long lastHeartbeatTime = 0;
unsigned long lastAnimTime = 0;
int animFrame = 0;

bool waitingForResponse = false;     
unsigned long responseTimer = 0;
// 安全參數
const int HEARTBEAT_INTERVAL = 3000; 
const int RESPONSE_TIMEOUT = 6000;   

// ★★★ 新增：勿擾模式計時器 ★★★
unsigned long disableHeartbeatUntil = 0;

// --- OLED 顯示函式 ---
void updateDisplay() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  
  // 第一行 IP
  u8g2.setCursor(0, 10); u8g2.print("IP: "); 
  if (WiFi.status() == WL_CONNECTED) u8g2.print(WiFi.localIP());
  
  // 第二行 狀態
  u8g2.setCursor(0, 25);
  u8g2.print("N:"); 
  if (isInitializing) u8g2.print("--"); else u8g2.print(maxID);
  
  u8g2.print("  App:"); 
  if (isAppConnected) u8g2.print("OK"); else u8g2.print("--");
  
  // 第三行 主訊息
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.setCursor(0, 50); 
  
  if (currentStatusText == "CONNECTING") {
      String dots = "";
      for(int i=0; i<=animFrame; i++) dots += ".";
      u8g2.print("CONNECTING" + dots);
  } else {
      u8g2.print(currentStatusText);
  }
  
  // 電源指示燈
  if (digitalRead(RELAY_PIN) == HIGH) u8g2.drawDisc(124, 58, 3);
  else u8g2.drawCircle(124, 58, 3);
  
  u8g2.sendBuffer();
}

// --- 錯誤觸發與鎖定 ---
void triggerError(String reason) {
  if (requireRescan && !isInitializing) return; 
  
  Serial.println("System Lock: " + reason); 
  digitalWrite(RELAY_PIN, LOW); // 斷電
  
  systemSafe = false;
  requireRescan = true;
  isInitializing = false;
  waitingForResponse = false;
  lastKnownCount = 0;
  webSocket.broadcastTXT("{\"status\":\"ERROR\",\"msg\":\"" + reason + "\"}");
  currentStatusText = "LOCKED!";
  updateDisplay();
}


// --- 解析 C3 回傳資料 ---
void parseInput(String data) {
  Serial.println("[C3] " + data);
  
  if (data.startsWith("EOS")) {
      waitingForResponse = false; 
      int comma = data.indexOf(',');
      int currentCount = data.substring(comma + 1).toInt();
      
      // A. 初始化階段 (掃描完成)
      if (isInitializing) {
          if (currentCount > 0) {
              Serial.printf("Init Success! Count: %d\n", currentCount);
              maxID = currentCount;
              lastKnownCount = currentCount;
              
              systemSafe = true;
              requireRescan = false;
              isInitializing = false;
              
              // --- 智慧復歸邏輯 (Smart Restore) ---
              Serial.println("Restoring positions...");
              for(int i=1; i<=maxID; i++) {
                  int targetPos = 500; // 預設歸零
                  
                  // 檢查是否有記憶
                  if (motorPositions[i] != -1) {
                      targetPos = motorPositions[i];
                      Serial.printf("ID %d -> Restore to %d\n", i, targetPos);
                  } else {
                      // 新模組 -> 歸零並寫入記憶
                      motorPositions[i] = 500;
                      Serial.printf("ID %d -> Init to 500\n", i);
                  }
                  
                  // 發送移動指令 (給 2000ms 慢速歸位，避免嚇到人)
                  C3Serial.print("<" + String(i) + "," + String(targetPos) + ",2000>");
                  delay(50); // 指令間隔
              }
              
              // 通知 APP 解鎖
              webSocket.broadcastTXT("{\"type\":\"UNLOCK\", \"total\":" + String(maxID) + "}");
              
              // --- ★★★ 狀態顯示邏輯 ★★★ ---
              if (isAppConnected) {
                  // 情境 1: APP 已連線 -> 顯示 READY，兩秒後變 WORKING
                  currentStatusText = "READY";
                  updateDisplay();
                  delay(2000); 
                  currentStatusText = "WORKING";
              } else {
                  // 情境 2: APP 未連線 -> 直接顯示 OFFLINE
                  currentStatusText = "OFFLINE";
              }
              
              lastHeartbeatTime = millis(); 
              updateDisplay(); // 更新最終顯示狀態
              
          } else {
              triggerError("Scan Empty");
          }
          return;
      }

      // B. 運作監控 (檢查數量是否變動)
      if (systemSafe) {
          if (currentCount != lastKnownCount) {
              triggerError("Module Count Changed");
          }
      }
  }
}

// --- 啟動掃描流程 ---
void startRescanProcess() {
  Serial.println("\n=== Start Rescan Process ===");
  if (digitalRead(DETECT_PIN) == HIGH) {
     webSocket.broadcastTXT("{\"status\":\"ERROR\",\"msg\":\"Connector Open\"}");
     if (!isAppConnected) currentStatusText = "CONNECTING";
     return;
  }
  
  isInitializing = true; 
  requireRescan = true; 
  maxID = 0; 
  
  currentStatusText = "Init Power...";
  updateDisplay();
  
  digitalWrite(RELAY_PIN, LOW);
  delay(500); 
  digitalWrite(RELAY_PIN, HIGH); 
  Serial.println("Power ON..."); 
  
  delay(3000); // 等待 C3 啟動
  
  currentStatusText = "Scanning...";
  updateDisplay();
  
  C3Serial.print("<SET_ID,1>");
  waitingForResponse = false; 
}

// --- WebSocket 事件處理 ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      isAppConnected = false;
      Serial.println("App Disconnected");
      
      currentStatusText = "OFFLINE"; 
      updateDisplay();
      delay(1000);
      
      currentStatusText = "OFFLINE"; 
      break;
    case WStype_CONNECTED:
      isAppConnected = true;
      Serial.println("App Connected");
      // 連線同步 (Sync)
      if (systemSafe && !requireRescan) {
         webSocket.sendTXT(num, "{\"total\":" + String(maxID) + "}");
         delay(100);
         
         String json = "{\"sync\":true, \"data\":[";
         bool first = true;
         for(int i=1; i<=maxID; i++) {
             if (motorPositions[i] != -1) {
                 if(!first) json += ",";
                 json += "{\"id\":" + String(i) + ",\"pos\":" + String(motorPositions[i]) + "}";
                 first = false;
             }
         }
         json += "]}";
         webSocket.sendTXT(num, json);
      }
      
      if (currentStatusText == "OFFLINE" ) {currentStatusText = "WORKING";}
      break;
      
    case WStype_TEXT:
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        if (doc["cmd"] == "RESCAN") { startRescanProcess(); return; }
        
        if (doc["cmd"] == "STATUS") { 
            if(systemSafe) webSocket.sendTXT(num, "{\"total\":" + String(maxID) + "}");
            return; 
        }
        
        if (requireRescan || !systemSafe) return;
        
        lastHeartbeatTime = millis(); // 有操作就重置心跳計時

        // 馬達指令
        if (doc.containsKey("id")) {
          int id = doc["id"];
          int pos = doc["pos"];
          int t = doc.containsKey("time") ? doc["time"] : 500;
          
          if(id >= 1 && id <= 12) motorPositions[id] = pos;
          C3Serial.print("<" + String(id) + "," + String(pos) + "," + String(t) + ">");
          
          // ★★★ 核心修改：設定勿擾時間 (動作時間 + 5000ms 緩衝) ★★★
          unsigned long finishTime = millis() + t + 5000; 
          if (finishTime > disableHeartbeatUntil) {
              disableHeartbeatUntil = finishTime;
          }
        }
        
        // LED 指令
        if (doc.containsKey("led")) {
          int on = doc["on"] ? 1 : 0;
          C3Serial.print("<LED," + String(doc["led"]) + "," + String(on) + ">");
        }
      }
      break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(DETECT_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 

  u8g2.begin();
  C3Serial.begin(9600, SERIAL_8N1, C3_RX_PIN, C3_TX_PIN);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  for(int i=0; i<13; i++) motorPositions[i] = -1;

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 10) { delay(500); Serial.print("."); retry++; }
  Serial.println("\nWiFi: " + WiFi.localIP().toString()); 

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  lastDetectState = digitalRead(DETECT_PIN); 
  
  if (lastDetectState == LOW) {
      startRescanProcess();
  } else {
      systemSafe = false;
      requireRescan = true; 
      currentStatusText = "CONNECTING";
  }
}

String rxBuffer = "";

void loop() {
  webSocket.loop();
  
  // OLED 動畫
  if (millis() - lastAnimTime > 300) {
      lastAnimTime = millis();
      animFrame = (animFrame + 1) % 4; 
      updateDisplay();
  }

  while (C3Serial.available()) {
    char c = (char)C3Serial.read();
    if (c == '<') rxBuffer = "";
    else if (c == '>') { parseInput(rxBuffer); rxBuffer = ""; } 
    else rxBuffer += c;
  }

  // 熱插拔監控
  bool currentDetectState = digitalRead(DETECT_PIN);
  if (currentDetectState != lastDetectState) {
    if (currentDetectState == LOW) {
        lastDetectState = LOW;
        triggerError("Connection Changed (Plugged)"); 
    } else {
        lastDetectState = HIGH;
        triggerError("Connection Changed (Unplugged)");
    }
  }

  // ★★★ 心跳監控 (含智慧勿擾) ★★★
  if (systemSafe && !requireRescan && !isInitializing) {
      if (millis() - lastHeartbeatTime > HEARTBEAT_INTERVAL) {
          
          // 如果還在「勿擾時間」內 (動作時間 + 3秒)
          if (millis() < disableHeartbeatUntil) {
             // 默默更新計時器，保持暫停，且預留結束後 1 秒的緩衝
             lastHeartbeatTime = millis() - HEARTBEAT_INTERVAL + 1000; 
          } 
          else if (!waitingForResponse) {
              lastHeartbeatTime = millis();
              C3Serial.print("<SET_ID,1>"); 
              waitingForResponse = true;
              responseTimer = millis();
          }
      }
  }

  // Watchdog 逾時
  if (waitingForResponse && !isInitializing) {
      if (millis() - responseTimer > RESPONSE_TIMEOUT) {
          triggerError("Timeout (Line Cut?)");
      }
  }
}