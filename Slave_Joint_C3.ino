/*
 * 🦾 ESP32-C3 Module - V24.6 (Pure Linear + LookAhead)
 * 修改：
 * 1. [Remove Curve] 移除所有加減速曲線邏輯，改為全程「線性勻速」(Linear Constant Speed)。
 * 2. [LookAhead] 保留 60ms 預判算法，確保通訊無頓挫。
 * 3. [Stability] 簡化運動邏輯，提升多軸同步時的穩定性。
 */
#include <Arduino.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

const int UP_RX_PIN = 4;
const int UP_TX_PIN = 3; 
const int LED_PIN = 8;    
const int DETECT_PIN = 6;
const int FACE_PINS[5] = {0, 1, 5, 20, 21};

SoftwareSerial UpSerial;
SoftwareSerial Face0; SoftwareSerial Face1;
SoftwareSerial Face2; SoftwareSerial Face3;
SoftwareSerial Face4;
HardwareSerial MotorSerial(1);

int myID = 0;
#define EEPROM_SIZE 4
int faceRoutes[5][2] = {{0,0}, {0,0}, {0,0}, {0,0}, {0,0}};

struct MotionTask {
  bool active;
  int startPos;
  int targetPos;
  unsigned long startTime;
  unsigned long totalTime;
  unsigned long lastTick;
  // bool isLinear; // 移除：現在全部都是 Linear
};

MotionTask motion = {false, 500, 500, 0, 0, 0};
int lastKnownPos = 500;

void blinkLED(int times, int delayTime) {
  for(int i=0; i<times; i++) { digitalWrite(LED_PIN, LOW); delay(delayTime); digitalWrite(LED_PIN, HIGH); delay(delayTime);
  }
}

void setLED(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void sendUpstream(String msg) {
  String packet = "<" + msg + ">";
  UpSerial.print(packet); 
  Serial.println("[Up] " + packet);
}

SoftwareSerial* getFaceSerial(int idx) {
  switch(idx) {
    case 0: return &Face0; case 1: return &Face1;
    case 2: return &Face2; case 3: return &Face3; case 4: return &Face4; default: return NULL;
  }
}
int getFacePin(int idx) {
  switch(idx) {
    case 0: return 0; case 1: return 1;
    case 2: return 5; case 3: return 20; case 4: return 21; default: return -1;
  }
}
byte LobotCheckSum(byte buf[]) { byte i; uint16_t temp = 0;
  for (i = 2; i < buf[3] + 2; i++) temp += buf[i]; return ~(byte)temp;
}

void sendMotorCmd(int pos, int time) {
  if (time < 20) time = 20; 
  byte buf[10]; buf[0]=0x55; buf[1]=0x55; buf[2]=0xFE; buf[3]=7;
  buf[4]=1;
  buf[5]=(uint8_t)pos; buf[6]=(uint8_t)(pos>>8); buf[7]=(uint8_t)time; buf[8]=(uint8_t)(time>>8);
  buf[9]=LobotCheckSum(buf); 
  MotorSerial.write(buf, 10);
}

void startSmartMove(int target, int time) {
  if (target == lastKnownPos) return;
  motion.active = true;
  motion.startPos = lastKnownPos;
  motion.targetPos = target;
  motion.totalTime = time;
  motion.startTime = millis();
  motion.lastTick = millis();
  
  // 移除所有速度判斷與曲線模式選擇
  Serial.println("Mode: Linear (Uniform)");
  
  sendMotorCmd(lastKnownPos, 40); 
}

// ★★★ 核心修改：純線性勻速 + Look-Ahead ★★★
void handleMotion() {
  if (!motion.active) return;
  unsigned long now = millis();

  // 1. 更新頻率 30ms
  if (now - motion.lastTick < 30) return;
  motion.lastTick = now;

  // 2. 預判時間 (60ms)
  int lookAheadTime = 60; 

  // 真實進度 (用於判斷是否結束)
  float realProgress = (float)(now - motion.startTime) / motion.totalTime;
  
  if (realProgress >= 1.0) {
      motion.active = false;
      lastKnownPos = motion.targetPos;
      sendMotorCmd(motion.targetPos, 100); 
      Serial.println("Motion Done");
      return;
  }

  // 3. 計算「未來」進度
  float futureProgress = (float)(now + lookAheadTime - motion.startTime) / motion.totalTime;
  if (futureProgress > 1.0) futureProgress = 1.0; 

  // 4. 純線性計算 (移除所有 if/else 曲線邏輯)
  // 公式：目標 = 起點 + (總距離 * 進度)
  int currentTarget = motion.startPos + (int)((motion.targetPos - motion.startPos) * futureProgress);
  
  // 5. 發送指令
  sendMotorCmd(currentTarget, lookAheadTime);
}

void sendToFace(int faceIdx, String msg) {
  SoftwareSerial* s = getFaceSerial(faceIdx);
  int pin = getFacePin(faceIdx);
  if (s == NULL) return;
  s->begin(9600, SWSERIAL_8N1, pin, pin, false, 256);
  s->enableTx(true);
  s->print("<" + msg + ">"); s->enableTx(false); s->end(); 
  UpSerial.listen(); 
}

int scanAndDistribute(int startID) {
  int currentMaxID = startID;
  for(int i=0; i<5; i++) { faceRoutes[i][0]=0; faceRoutes[i][1]=0; }
  Serial.println("Scan...");
  for (int i = 0; i < 5; i++) {
    int pin = getFacePin(i);
    SoftwareSerial* s = getFaceSerial(i);
    s->begin(9600, SWSERIAL_8N1, pin, pin, false, 256);
    delay(20); 
    int nextID = currentMaxID + 1;
    String cmd = "<SET_ID," + String(nextID) + ">";
    Serial.printf("Ping F%d...", i); 
    s->enableTx(true); s->print(cmd); s->enableTx(false);
    unsigned long waitStart = millis();
    bool isConnected = false;
    String ackBuffer = "";
    while (millis() - waitStart < 300) {
       if (s->available()) {
         char c = (char)s->read();
         if (c == '<') ackBuffer = "";
         else if (c == '>') {
           if (ackBuffer.startsWith("LINK_OK")) { isConnected = true;
           break; }
           ackBuffer = "";
         } else ackBuffer += c;
       }
    }
    bool foundChild = false;
    int childEndID = 0;
    if (isConnected) {
        Serial.println(" ACK"); 
        waitStart = millis();
        String eosBuffer = "";
        while (millis() - waitStart < 6000) { 
           if (s->available()) {
             char c = (char)s->read();
             if (c == '<') eosBuffer = "";
             else if (c == '>') {
               if (eosBuffer.startsWith("EOS")) {
                  int comma = eosBuffer.indexOf(',');
                  if (comma != -1) { childEndID = eosBuffer.substring(comma + 1).toInt(); foundChild = true;
                  }
                  break;
               }
               eosBuffer = "";
             } else eosBuffer += c;
           }
        }
    } else { Serial.println(" No");
    }
    s->end(); 
    if (foundChild) {
      faceRoutes[i][0] = nextID; faceRoutes[i][1] = childEndID;
      currentMaxID = childEndID;
    }
  }
  UpSerial.listen();
  return currentMaxID;
}

void parseCommand(String data, bool fromUpstream) {
  // 1. SET_ID 指令 (最優先)
  if (data.startsWith("SET_ID") && fromUpstream) {
    int comma = data.indexOf(',');
    int nextID = data.substring(comma + 1).toInt();
    if (myID != nextID) { myID = nextID; EEPROM.write(0, myID); EEPROM.commit();
    }
    sendUpstream("LINK_OK"); 
    blinkLED(1, 20);
    if (true) { 
       int finalMaxID = scanAndDistribute(myID);
       delay(50);
       sendUpstream("EOS," + String(finalMaxID));
    }
  }
  // ★★★ 2. LED 指令 (移到這裡，比馬達移動指令優先!) ★★★
  else if (data.startsWith("LED") && fromUpstream) {
      int comma1 = data.indexOf(',');
      int comma2 = data.indexOf(',', comma1 + 1);
      int targetID = data.substring(comma1 + 1, comma2).toInt();
      int on = data.substring(comma2 + 1).toInt();
      
      if (targetID == myID) setLED(on == 1);
      else {
          // 轉發給下游
          for(int i=0; i<5; i++) {
             if (faceRoutes[i][0] != 0 && targetID >= faceRoutes[i][0] && targetID <= faceRoutes[i][1]) {
                 sendToFace(i, data);
                 break; 
             }
          }
      }
  }
  // 3. 馬達移動指令 (最後才檢查這個)
  // 排除掉 SET_ID, EOS, LINK_OK, 還有 LED
  else if (!data.startsWith("SET_ID") && !data.startsWith("EOS") && !data.startsWith("LINK_OK") && fromUpstream) {
      int comma = data.indexOf(',');
      if (comma != -1) {
          int targetID = data.substring(0, comma).toInt(); // 這裡如果是 "LED" 會變成 0，現在不會進來了
          
          if (targetID == myID) {
              int pos = data.substring(comma+1).toInt();
              int c2 = data.indexOf(',', comma+1);
              int time = (c2!=-1) ? data.substring(c2+1).toInt() : 500;
              startSmartMove(pos, time);
          } else {
             for(int i=0; i<5; i++) {
                if (faceRoutes[i][0] != 0 && targetID >= faceRoutes[i][0] && targetID <= faceRoutes[i][1]) {
                    sendToFace(i, data);
                    break; 
                }
             }
          }
      }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nC3 V24.6 (Pure Linear + LookAhead)"); 
  
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(DETECT_PIN, INPUT_PULLUP); 
  
  EEPROM.begin(EEPROM_SIZE);
  myID = EEPROM.read(0);
  if (myID > 255) myID = 1;

  UpSerial.begin(9600, SWSERIAL_8N1, UP_RX_PIN, UP_TX_PIN, false, 256);
  MotorSerial.begin(115200, SERIAL_8N1, -1, 7);
  UpSerial.listen(); 
  blinkLED(1, 200);
}

String upBuffer = "";
void loop() {
  handleMotion();

  if (myID > 1 && digitalRead(DETECT_PIN) == HIGH) { 
      delay(50);
      ESP.restart(); 
  }

  while (UpSerial.available()) {
    char c = (char)UpSerial.read();
    if (c == '<') upBuffer = "";
    else if (c == '>') { parseCommand(upBuffer, true); upBuffer = ""; } 
    else upBuffer += c;
  }
}