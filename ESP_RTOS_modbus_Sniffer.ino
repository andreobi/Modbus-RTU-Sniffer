// trigger pattern is not synchronised between different browsers 

#include "driver/uart.h"
#include "driver/gpio.h"
#include <Preferences.h>

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <set>
#include "html.h"

// --- EXTERNE WEBSEITE EINBINDEN ---
extern const char index_html[];

#define MAX_LOG_ENTRIES 100

#define LED_PIN       2
#define INIT_MODE_PIN 22
// ??? trigger pin  #define PATTERN_PIN 23
#define TEST_PIN      23

// --- Hardware- und Modbus-Konfiguration ---
#define MODBUS_UART_NUM UART_NUM_2
#define RX_PIN 16
#define TX_PIN 17
#define RX_TRIGGER_PIN 4       // Physisch mit RX_PIN (16) verbinden!
#define BAUD_RATE 19200
// 1.5 Symbols glitches: not implemented
// 3.5 Symbols indicates Frame End
#define UART_TIMEOUT_SYMBOLS  2  
#define RESPONSE_TIMEOUT      500
#define WEB_UPDATE_PERIOD     2000

uint16_t timeOffset = (1000000 * UART_TIMEOUT_SYMBOLS * 11) / BAUD_RATE;

// WiFi config
Preferences preferences;

// --- SERVER ---
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

bool apMode = false;
uint32_t lastWebUpdateTime;
int32_t webUpdatePeriod = WEB_UPDATE_PERIOD;

// --- LAUFZEIT CONFIGURATION ---
uint32_t currentBaudrate = 19200;
String currentConfigStr = "SERIAL_8E1";
uint32_t currentConfigType = SERIAL_8E1;

// --- Datenstruktur für die Queue ---
typedef struct {
  uint8_t payload[256];  // Max. Modbus RTU Framegröße
  size_t length;         // Tatsächliche Byte-Länge
  uint64_t startTime;    // Zeitstempel erstes Bit (aus ISR)
  uint64_t endTime;      // Zeitstempel Frame-Ende (Timeout)
} RxRawDataFrame_t;

// Datenstruktur für die Übergabe an die loop()
typedef struct {
  uint8_t payload[256];   // Der feste 256-Byte-Datenpuffer
  size_t length;          // empfangene packet länge
  uint64_t startTime;     // Zeitstempel erstes Bit (aus ISR)
  uint64_t endTime;       // Zeitstempel Frame-Ende (Timeout)
  uint32_t counter;       // Nachrichten-Counter
} LoopData_t;
LoopData_t loopRxDaten;

// --- Globale FreeRTOS Variablen ---
QueueHandle_t rxRawData_Queue;      // Data from Hardware RX driver
QueueHandle_t rxData_Queue;         // transfer RX data to realtime mangagement
QueueHandle_t toLoopData_Queue;     // transfer data from realtime manager to main loop

// -- interrupt Variablen ---
volatile uint64_t g_startTime = 0;
volatile bool neuesFrameStartFlag = false;

uint64_t previousEndTime;

uint32_t noframesCnt;
uint32_t crcerrorCnt;

enum RegisterNumber { RN_COIL =0, RN_DISCRETE, RN_HOLDING, RN_INPUT, RN_CMD, RN_MAX };
const char* labels[] = {"Coil", "Discrete", "Holding", "Input", "OTHER" };

enum RegisterType { RT_COIL, RT_DISCRETE, RT_INPUT, RT_HOLDING, RT_CMD, RT_UNKOWN, RT_MAX };
String regTypeName[] = {"COIL", "DISCRETE", "INPUT", "HOLDING", "CMD", "EXCEPTION", "UNKNOWN" };

enum PacketType { PTM_INVALID, PTM_REQUEST, PTM_RESPONSE, PTM_EXCEPTION, PTM_UNCERTAIN, PTM_MAX };
String packTypeName[] = {"INVALID", "REQUEST", "RESPONSE", "EXCEPTION", "UNCERTAIN"};

enum PacketStatus { PS_OK = 0x01, PS_ILENGTH = 0x02, PS_IQUANTITY = 0x04, PS_IADDRESS = 0x08, PS_NOREQUEST = 0x10, PS_MAX = 0x20};
String statusName[] = { "OK", "ILENGTH", "IQUANTITY", "IADDRESS", "NOREQUEST"};

enum DataDirection { DD_CMD, DD_READ, DD_WRITE, DD_STATUS, DD_MAX };
String dirctionName[] = {"CMD", "READ", "WRITE", "STATUS" };

typedef struct {
  uint8_t  device;  	        // modbus device ID
  uint8_t  fc;                // function code
  RegisterType regNameP;      // register type: coil, input, holding ...
  PacketType packetTypeP;     // request, rsponse ... exception
  uint8_t status;             // any kind of format violation
  bool hasAddress;
  uint16_t address;           // register address
  bool hasQuantity;
  uint16_t quantity;          // data length
  DataDirection direction;    // cmd , read, write, status
  
  uint16_t length;            // packet length
  uint16_t dataStart;         // dataa start position in frame
  uint64_t timestamp;
  uint32_t distance;          // time to previous frame
  uint32_t duration;
  bool timeout;
  uint8_t payload[256];   // Der feste 256-Byte-Datenpuffer
} WorkPacket;

WorkPacket last2P;
WorkPacket lastP;
WorkPacket currentP;

uint32_t timeoutDistance = RESPONSE_TIMEOUT; // timeout for Response

// --- SYSTEM-STATUS ---
bool isRecording = false;
bool isArmed = false;
uint8_t triggerValue[16] = {0x00};
uint8_t triggerMask[16] = {0xFF};

// --- DATENSTRUKTUREN MATRIX ---
struct RegisterStats {
  uint32_t regReads = 0;
  uint32_t regWrites = 0;
  uint32_t errors = 0;
  std::set<String> uniqueAddresses; 
};
RegisterStats matrix[RN_MAX];

struct PacketStatusStruct {
  uint32_t crcErrors;
  uint32_t fragments;
  uint32_t request;
  uint32_t response;
  std::set<String> devices; 
};
PacketStatusStruct pstatus;

struct FunctionView {
  uint32_t timestamp;
  uint8_t fcvDevice = 1;
  uint8_t fcvFc = 4;
  uint16_t fcvAddress;
  String fcvData;
};
FunctionView fcvMatrix[4];
int fcvMatch;

uint8_t targetDeviceFilter = 0;
uint64_t resetTimestamp;

// --- CHRONOLOGISCHER LOG ---
struct LogEntry {
  uint64_t timestamp;
  uint32_t distance;
  uint32_t duration;

  uint8_t  device;
  String fc;
  String address;
  String quantity;
  String packetTypeS; 
  String regTypeS;
  String directionS; 
  String status;

  String payloadHex;
  uint16_t length;

  bool timeout;
};
LogEntry logBuffer[MAX_LOG_ENTRIES];
int logCount = 0;

unsigned long ledOffTime = 0;
bool ledActive = false;
unsigned long lastApBlink = 0;


//
// --- LED --------
//
void triggerLedFlash() {
  digitalWrite(LED_PIN, LOW);
  ledOffTime = millis() + 20;
  ledActive = true;
}
  
  
void checkLedTimeout() {
  if (ledActive && millis() >= ledOffTime) {
    digitalWrite(LED_PIN, HIGH);
    ledActive = false;
  }
}
  
  
void handleApLedBlink() {
  if (millis() - lastApBlink >= 1000) {
    lastApBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN)); 
  } 
}


//
// --- WEB Services -----------------------------------
//
void addLogEntry(WorkPacket &pid) {
  if (!isRecording || logCount >= MAX_LOG_ENTRIES) return;

  logBuffer[logCount].timestamp = pid.timestamp;
  logBuffer[logCount].distance = pid.distance;
  logBuffer[logCount].duration = pid.duration;
  logBuffer[logCount].length = pid.length;
  logBuffer[logCount].device = pid.device;
  
  String hexFc = "";
  uint8_t b = pid.fc;
  if (b < 0x10) hexFc += "0";
  hexFc += String(b, HEX) + " ";
  logBuffer[logCount].fc = hexFc;
  
  if(pid.hasAddress) {
    logBuffer[logCount].address = String(pid.address);
  } else {
    logBuffer[logCount].address = "";
  }

  if(pid.hasQuantity) {
    logBuffer[logCount].quantity = String(pid.quantity);
  } else {
    logBuffer[logCount].quantity = "";
  }

  logBuffer[logCount].packetTypeS = packTypeName[(uint8_t)pid.packetTypeP];
  logBuffer[logCount].regTypeS = regTypeName[(uint8_t)pid.regNameP];
  
  if((uint8_t)pid.direction < DD_MAX) {
    logBuffer[logCount].directionS = dirctionName[(uint8_t)pid.direction];
  }
  String status = "";
  if(pid.status == PS_OK) {
    status = statusName[0];
  } else {
    uint8_t j = 0;
    for(uint8_t i=0x02; i<PS_MAX; i<<=1) {
      j++;
      uint8_t mask = i & pid.status;
      if(mask) {
        status += statusName[j];
      }
    }
  }
  logBuffer[logCount].status = status;

  String hexStr = "";
  for (size_t i = 0; i < pid.length; i++) {
    if (i == 0) {
      hexStr += "CMD: ";
    } else
    if (i == pid.dataStart && i != pid.length -2) {
      hexStr += "- Data: ";
    } else
    if (i == pid.length -2) {
      hexStr += "- CRC: ";
    }
    uint8_t b = pid.payload[i]; 
    if (b < 0x10) hexStr += "0";
    hexStr += String(b, HEX) + " ";
  }
  hexStr.trim();
  hexStr.toUpperCase();
  logBuffer[logCount].payloadHex = hexStr;

  logCount++;
}


//
//--- generate WS Data / Data for Save CMD -------
String generateJSON() {
  JsonDocument doc;
  doc["filter"] = targetDeviceFilter;
  doc["isRecording"] = isRecording;
  doc["isArmed"] = isArmed;
  doc["baudrate"] = currentBaudrate;
  doc["serialConfig"] = currentConfigStr;
  
  JsonArray matrixArr = doc["matrix"].to<JsonArray>();
  for(int i=0; i<RN_MAX; i++) {
    JsonObject row = matrixArr.add<JsonObject>();
    row["type"] = labels[i];
    JsonArray regsArr = row["regs"].to<JsonArray>();
    for (auto addr : matrix[i].uniqueAddresses) {
      regsArr.add(addr);
    }
    row["req"] = matrix[i].regReads;
    row["res"] = matrix[i].regWrites;
    row["err"] = matrix[i].errors;
  }
  
  doc["status"]["frag"] = pstatus.fragments;
  doc["status"]["crc"] = pstatus.crcErrors;
  doc["status"]["resp"] = pstatus.response;
  doc["status"]["requ"] = pstatus.request;
  JsonArray devsArr = doc["status"]["devs"].to<JsonArray>();
  for (auto sdevice : pstatus.devices) {
    devsArr.add(sdevice);
  }

  JsonArray fcvmArr = doc["fcvm"].to<JsonArray>();
  for(int i=0; i<4; i++) {
    JsonObject row = fcvmArr.add<JsonObject>();
    row["time"] = fcvMatrix[i].timestamp;
    row["dev"] = fcvMatrix[i].fcvDevice;
    row["fc"] = fcvMatrix[i].fcvFc;
    row["adr"] = fcvMatrix[i].fcvAddress;
    row["data"] = fcvMatrix[i].fcvData;
  } 
  
  JsonArray logArr = doc["log"].to<JsonArray>();
  for(int i=0; i<logCount; i++) {
    JsonObject entry = logArr.add<JsonObject>();
    entry["time"] = logBuffer[i].timestamp;
    entry["dist"] = logBuffer[i].distance;
    entry["tout"] = logBuffer[i].timeout;
    entry["devi"] = logBuffer[i].device;
    entry["func"] = logBuffer[i].fc;
    entry["addr"] = logBuffer[i].address;
    entry["quan"] = logBuffer[i].quantity;
    entry["drw"] = logBuffer[i].directionS;
    entry["pType"] = logBuffer[i].packetTypeS;
    entry["regType"] = logBuffer[i].regTypeS; 
    entry["status"] = logBuffer[i].status;
    entry["len"] = logBuffer[i].length;
    entry["data"] = logBuffer[i].payloadHex;
    //  uint32_t duration;
  }
  
  String output;
  serializeJson(doc, output);
  return output;
}


void sendUpdate() {
  String jsonStr = generateJSON();
  webSocket.broadcastTXT(jsonStr);
  lastWebUpdateTime = millis();
}


void resetData() {
  logCount = 0;
  for(int i=0; i<RN_MAX; i++) {
    matrix[i].regReads = 0;
    matrix[i].regWrites = 0;
    matrix[i].errors = 0;
    matrix[i].uniqueAddresses.clear();
  }
  
  pstatus.crcErrors = 0;
  pstatus.fragments = 0;
  pstatus.request = 0;
  pstatus.response = 0;
  pstatus.devices.clear();

  noframesCnt = 0;
  crcerrorCnt = 0;
  resetTimestamp = millis();
}


void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_TEXT) { 
    JsonDocument doc;
    deserializeJson(doc, payload);

// Serial config
    bool needsSerialRestart = false;
    if (doc["setBaud"].is<int>()) {
      currentBaudrate = doc["setBaud"];
      needsSerialRestart = true;
    }
    if (doc["setConfig"].is<const char*>()) {
      currentConfigStr = doc["setConfig"].as<String>();
      needsSerialRestart = true;
    }
    if (needsSerialRestart) {
      updateHardwareSerial();
    }
    
// capture 
    if (doc["setFilter"].is<int>()) {
      targetDeviceFilter = doc["setFilter"];
    }
    if (doc["cmd"].is<const char*>()) {
      String cmd = doc["cmd"].as<String>();
      if (cmd == "start") { isRecording = true; isArmed = false; }
      if (cmd == "stop")  { isRecording = false; isArmed = false; }
      if (cmd == "reset") { resetData(); }
    }
    
// function code view set device and code    
    if (doc["setFcvId"].is<String>() && doc["setFcvVal"].is<int>()) {
      String b = doc["setFcvId"].as<String>();
      int v =  doc["setFcvVal"].as<int>();
      if(b.length() == 7) {
        uint8_t num = int(b[6]-'0');
        if (num  <4) {
          if (b.substring(0,6).compareTo("fcvsel") == 0) {
            fcvMatrix[num].fcvFc = v;
          } else if(b.substring(0,6).compareTo("fcvdev") == 0) {
            fcvMatrix[num].fcvDevice = v;
          } else if(b.substring(0,6).compareTo("fcvadr") == 0) {
            fcvMatrix[num].fcvAddress = v;
          }
          fcvMatrix[num].fcvData = "";    // clear data on filter change
        }
      }
    }
    
// Trigger
    if (doc["setTrigger"].is<JsonArray>() && doc["setMask"].is<JsonArray>()) {
      JsonArray hexArray = doc["setTrigger"];
      if (hexArray.size() == 16) {
        for (size_t i = 0; i < hexArray.size(); i++) {
          if (hexArray[i].is<const char*>()) {
            const char* hexStr = hexArray[i].as<const char*>();
            long intVal = strtol(hexStr, NULL, 16);
            triggerValue[i] = (uint8_t)intVal;
          }
        } 
      }
      hexArray.clear();
      hexArray = doc["setMask"];
      if (hexArray.size() == 16) {
        for (size_t i = 0; i < hexArray.size(); i++) {
          if (hexArray[i].is<const char*>()) {
            const char* hexStr = hexArray[i].as<const char*>();
            long intVal = strtol(hexStr, NULL, 16);
            triggerMask[i] = (uint8_t)intVal;
          }
        } 
      }
      isArmed = true;
    }
    
// send update
    sendUpdate();
  }
}


//
// --- Setup WiFi --------------------
//
void startAccessPoint() {
  apMode = true;
  WiFi.softAP("MB-Sniffer", "12345678#ao", 1, false, 1);
  Serial.println("local AccessPoint \nconnect to MB-Sniffer : 12345678#ao");
}


void setupNetwork(void) {
  preferences.begin("modbus-sniffer", true);
  String ssid = preferences.getString("ssid", "");
  String password = preferences.getString("password", "");
  String hn = preferences.getString("hostname", "modbus-sniffer");
  preferences.end();
  
  if (digitalRead(INIT_MODE_PIN) == LOW || ssid == "") {
    startAccessPoint();
  } else {
    WiFi.setHostname(hn.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t < 20) {
      delay(500);
      Serial.print(".");
      t++;
      handleApLedBlink();
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {
      startAccessPoint(); 
    }
    Serial.println("local IP  : " + WiFi.localIP().toString());
    Serial.println("Host-Name : " + hn);
  }
}


void handleSaveConfig(void) {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    preferences.begin("modbus-sniffer", false);
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("password", server.arg("password"));
    preferences.end();
    server.send(200, "text/plain", "Gespeichert. Neustart...");
    delay(1500);
    ESP.restart();
  }
 server.send(400, "text/plain", "Fehler");
}


void handleConfig(void) {
  server.send(200, "text/html", config_html);
}


void setupWEB(void) {
  server.on("/", []() { server.send(200, "text/html", index_html); });
  server.on("/download-json", []() { server.send(200, "application/json", generateJSON()); });
  server.on("/setWiFi", handleConfig);
  server.on("/save-config", HTTP_POST, handleSaveConfig);
  
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  Serial.print("local IP : ");
  Serial.println(WiFi.localIP());
}


//
// --- INTERRUPT SERVICE ROUTINE (ISR) --------------------
//
void IRAM_ATTR rx_start_bit_isr(void* arg) {
  gpio_intr_disable((gpio_num_t)RX_TRIGGER_PIN); // nur frame start erkennen: Interrupt abschalten
  g_startTime = esp_timer_get_time();
  neuesFrameStartFlag = true;
}


//
// --- print current Packet -------------------
//
void printPacket(void) {
  Serial.printf("[Loop] Counter: %lu\n", loopRxDaten.counter);
  Serial.printf("[Loop] Nutzlänge: %d Bytes\n", loopRxDaten.length);

  Serial.print("[Loop] t: ");
  Serial.print(currentP.timestamp);
  Serial.print(" - distance: ");
  Serial.print(currentP.distance);
  Serial.print(" - duration: ");
  Serial.println(currentP.duration);

  Serial.print(currentP.device);
  
  Serial.print(" ");
  Serial.print(currentP.fc);
  
  Serial.print(" ");
  if((uint8_t)currentP.regNameP < RT_MAX) {
    Serial.print(regTypeName[(uint8_t)currentP.regNameP]);
  } else {
    Serial.print((uint8_t)currentP.regNameP);
  }
  
  Serial.print(" ");
  if((uint8_t)currentP.packetTypeP < PTM_MAX) {
    Serial.print(packTypeName[(uint8_t)currentP.packetTypeP]);
  } else {
    Serial.print((uint8_t)currentP.packetTypeP);
  }
  
  if(currentP.hasAddress) {
    Serial.print(" adr ");
    Serial.print(currentP.address);  
  }
  
  if(currentP.hasQuantity) {
    Serial.print(" quantity ");
    Serial.print(currentP.quantity);  
  }
  
  Serial.print(" ");
  if((uint8_t)currentP.direction < DD_MAX) {
    Serial.println(dirctionName[(uint8_t)currentP.direction]);
  }
  
  if(currentP.dataStart < currentP.length -2) {
    Serial.print(" Data: ");
    for (uint8_t i = currentP.dataStart; i < loopRxDaten.length -2; i++) {
      Serial.printf("%02X ", loopRxDaten.payload[i]);
    }  
  }

  Serial.print(" ");
  if(currentP.status == PS_OK) {
    Serial.print(statusName[0]);
  } else {
    uint8_t j = 0;
    for(uint8_t i=0x02; i<PS_MAX; i<<=1) {
      j++;
      uint8_t mask = i & currentP.status;
      if(mask) {
        Serial.print(statusName[j]);
      }
    }
  }
  
  Serial.println();
  
  for (size_t i = 0; i < loopRxDaten.length; i++) {
    Serial.printf("%02X ", loopRxDaten.payload[i]);
  }
  
  Serial.println();
  Serial.print("No frames: ");
  Serial.print(noframesCnt);
  Serial.print("   CRC error: ");
  Serial.println(crcerrorCnt);
  
  Serial.println("\n-------------------------------------------");
}


// ??? response nach timeout? auch wenn das format passt ?
// ??? was ist wenn response adresse != request adresse ist


//
// --- check if response fits request and format !!!!!!!
//
void check_exception(void) {
  currentP.packetTypeP = PTM_EXCEPTION;
  currentP.dataStart = 2;
  if(currentP.length == 5) {
    currentP.packetTypeP = PTM_EXCEPTION;
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != (currentP.fc &0x7f) || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST; 
    }
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }
}


void checkType1_2(void) {
  if(currentP.length == 8) {
    if((currentP.address >> 8) != 3) {     // response quantity is only high byte in address
      currentP.packetTypeP = PTM_REQUEST;
    } else { // fit expectation 
      if(lastP.packetTypeP == PTM_REQUEST && lastP.fc == currentP.fc && lastP.device == currentP.device) {
        if(currentP.timeout) {
          currentP.packetTypeP = PTM_REQUEST;
        } else {
          currentP.packetTypeP = PTM_RESPONSE;
          currentP.dataStart = 3;
        }
      } else {
        currentP.packetTypeP = PTM_REQUEST;
      }
    }
  } else if(currentP.length > 5) { // response
    currentP.packetTypeP = PTM_RESPONSE;
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }
  
  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    if(currentP.quantity == 0 ||  currentP.quantity > 2000) {
      currentP.status |= PS_IQUANTITY;
    }
  } else 
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasQuantity = true;
    currentP.dataStart = 3;
    currentP.direction = DD_READ;
    currentP.quantity = currentP.address >> 8;      // response quantity is only high byte
    if(currentP.length != currentP.quantity + 5) {
      currentP.status |= PS_IQUANTITY;
    }
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST; 
    }
  }
}


void checkType3_4(void) {
  if(currentP.length == 8) {
    currentP.packetTypeP = PTM_REQUEST;
  } else if(currentP.length %2 == 1 && currentP.length > 6) {
    currentP.packetTypeP = PTM_RESPONSE;
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }

  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    if(currentP.quantity == 0 ||  currentP.quantity > 125) {
      currentP.status |= PS_IQUANTITY;
    }
  } else
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasQuantity = true;
    currentP.dataStart = 3;
    currentP.direction = DD_READ;
    currentP.quantity = currentP.address >> 8;      // response quantity is only high byte
    if(currentP.length != (currentP.quantity + 5)) {
      currentP.status |= PS_IQUANTITY;
    }
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST; 
    }
  }
}


void checkType5(void) {
  if(currentP.length == 8) {
    currentP.packetTypeP = PTM_REQUEST;
  } else if(currentP.length == 6) {
    currentP.packetTypeP = PTM_RESPONSE;
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }
  
  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.dataStart = 4;
    currentP.direction = DD_WRITE;
  } else
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasAddress = true;
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST;  
    }
  }
}


void checkType6(void) {
  if(currentP.length == 8) {
    if(lastP.packetTypeP == PTM_REQUEST && lastP.fc == currentP.fc && lastP.device == currentP.device && lastP.address == currentP.address) {
      if(currentP.timeout) {
        currentP.packetTypeP = PTM_REQUEST;
      } else {
        currentP.packetTypeP = PTM_RESPONSE;
      }
    } else {
      currentP.packetTypeP = PTM_REQUEST;
    }
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }

  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.dataStart = 4;
    currentP.direction = DD_WRITE;
  } else
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasAddress = true;
    currentP.dataStart = 4;
    currentP.direction = DD_READ;
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST;  
    }
  }
}


void checkType15(void) {
  if(currentP.length > 9) {
    currentP.packetTypeP = PTM_REQUEST;
    currentP.dataStart = 7;
  } else if(currentP.length == 8) {
    currentP.packetTypeP = PTM_RESPONSE;
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }

  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    currentP.dataStart = 6;
    currentP.direction = DD_WRITE;
    if(currentP.quantity == 0 ||  currentP.quantity > 1968) {
      currentP.status |= PS_IQUANTITY;
    }
    if(currentP.length != (currentP.quantity + 8)) {
      currentP.status |= PS_IQUANTITY;
    }
  } else
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    if(currentP.quantity == 0 ||  currentP.quantity > 1968) {
      currentP.status |= PS_IQUANTITY;
    }
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST; 
    }
  }
}


void checkType16(void) {
  if(currentP.length > 10) {
    currentP.packetTypeP = PTM_REQUEST;
  } else if(currentP.length == 8) {
    currentP.packetTypeP = PTM_RESPONSE;
  } else {
    currentP.packetTypeP = PTM_INVALID;
    currentP.status |= PS_ILENGTH;
  }

  if(currentP.packetTypeP == PTM_REQUEST) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    currentP.dataStart = 7;
    currentP.direction = DD_WRITE;
    if(currentP.quantity == 0 ||  currentP.quantity > 123) {
      currentP.status |= PS_IQUANTITY;
    }
    if(currentP.length != (currentP.quantity * 2 + 9)) {
      currentP.status |= PS_IQUANTITY;
    }
  } else
  if(currentP.packetTypeP == PTM_RESPONSE) {
    currentP.hasAddress = true;
    currentP.hasQuantity = true;
    if(currentP.quantity == 0 ||  currentP.quantity > 123) {
      currentP.status |= PS_IQUANTITY;
    }
    if(lastP.packetTypeP != PTM_REQUEST || lastP.fc != currentP.fc || lastP.device != currentP.device) {
      currentP.status |= PS_NOREQUEST; 
    }
  }
}


void identifyRxData(void) {
// allway detect new devices
  pstatus.devices.insert(String(loopRxDaten.payload[0])); // add to device list

// check Device Filter
  if (targetDeviceFilter != 0 && loopRxDaten.payload[0] != targetDeviceFilter) return;

  last2P = lastP;
  lastP = currentP;
  for(size_t i =0; i <loopRxDaten.length; i++) {
    currentP.payload[i] = loopRxDaten.payload[i];    
  }
  currentP.length = loopRxDaten.length;
  currentP.device = loopRxDaten.payload[0];
  currentP.fc = loopRxDaten.payload[1];
  currentP.hasAddress = false;
  currentP.hasQuantity = false;
  currentP.status = PS_OK;                          // assume it will be OK, clear all other flags
  currentP.address = ((uint16_t)loopRxDaten.payload[2] << 8) | loopRxDaten.payload[3];
  currentP.quantity = ((uint16_t)loopRxDaten.payload[4] << 8) | loopRxDaten.payload[5];
  currentP.dataStart = loopRxDaten.length -2;       // kein datenfeld ohne CRC
  currentP.direction = DD_CMD;
  
  currentP.timestamp = loopRxDaten.startTime /1000 - resetTimestamp;
  currentP.distance = (loopRxDaten.startTime - previousEndTime + timeOffset) / 1000; // symbol-time to detect frameend
  previousEndTime = loopRxDaten.endTime;
  currentP.duration = (previousEndTime - loopRxDaten.startTime - timeOffset) / 1000;

  if(currentP.distance > timeoutDistance) {
    currentP.timeout = true;
  } else {
    currentP.timeout = false;
  }

  if(currentP.fc & 0x80) {
    check_exception();
    switch(currentP.fc &0x7f) {
      case 1:  
        currentP.regNameP = RT_COIL;  // r
        matrix[RN_COIL].errors++;
        break;
      case 2: 
        currentP.regNameP = RT_DISCRETE; // r
        matrix[RN_DISCRETE].errors++;
        break;
      case 3:
        currentP.regNameP = RT_HOLDING; // r
        matrix[RN_HOLDING].errors++;
        break;
      case 4: 
        currentP.regNameP = RT_INPUT; // r
        matrix[RN_INPUT].errors++;
        break;
      case 5: 
        currentP.regNameP = RT_COIL; // w
        matrix[RN_COIL].errors++;
        break;
      case 6: 
        currentP.regNameP = RT_HOLDING; // w
        matrix[RN_HOLDING].errors++;
        break;
      case 15: 
        currentP.regNameP = RT_COIL; // MW
        matrix[RN_COIL].errors++;
        break;
      case 16: 
        currentP.regNameP = RT_HOLDING; // MW
        matrix[RN_HOLDING].errors++;
        break;
      default: 
        currentP.packetTypeP = PTM_UNCERTAIN;
        currentP.regNameP = RT_UNKOWN;
        break;
    }
  } else {
    switch(currentP.fc) {
      case 1:  
        checkType1_2();
        currentP.regNameP = RT_COIL;  // r
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_COIL].regReads++;
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"R";
          matrix[RN_COIL].uniqueAddresses.insert(s);
        }
        break;
      case 2: 
        checkType1_2();
        currentP.regNameP = RT_DISCRETE; // r
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_DISCRETE].regReads++;
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"R";
          matrix[RN_DISCRETE].uniqueAddresses.insert(s);
        }
        break;
      case 3:
        checkType3_4();
        currentP.regNameP = RT_HOLDING; // r
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_HOLDING].regReads++;
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"R";
          matrix[RN_HOLDING].uniqueAddresses.insert(s);
        }
        break;
      case 4: 
        checkType3_4();
        currentP.regNameP = RT_INPUT; // r
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_INPUT].regReads++;
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"R";
          matrix[RN_INPUT].uniqueAddresses.insert(s);
        }
        break;
      case 5: 
        checkType5();
        currentP.regNameP = RT_COIL; // w
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_COIL].regWrites++;
          String s = String(currentP.address) + ":1W";
          matrix[RN_COIL].uniqueAddresses.insert(s);
        }
        break;
      case 6: 
        checkType6();
        currentP.regNameP = RT_HOLDING; // w
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_HOLDING].regWrites++;
          String s = String(currentP.address) + ":1W";
          matrix[RN_HOLDING].uniqueAddresses.insert(s);
        }
        break;
      case 15: 
        checkType15();
        currentP.regNameP = RT_COIL; // MW
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_COIL].regWrites++;
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"W";
          matrix[RN_COIL].uniqueAddresses.insert(s);
        }
        break;
      case 16: 
        checkType16();
        currentP.regNameP = RT_HOLDING; // MW
        if(currentP.packetTypeP == PTM_REQUEST) {
          matrix[RN_HOLDING].regWrites++;     // ??? + quatity
          String s = String(currentP.address) + ":" + String(currentP.quantity)+"W";
          matrix[RN_HOLDING].uniqueAddresses.insert(s);
        }
        break;
      default: 
        currentP.packetTypeP = PTM_UNCERTAIN;
        currentP.regNameP = RT_UNKOWN;
        currentP.dataStart = 2;
        matrix[RN_CMD].regWrites++;
        String hexFc = "0x";
        if (currentP.fc < 0x10) hexFc += "0";
        hexFc += String(currentP.fc, HEX) + " ";
        matrix[RN_CMD].uniqueAddresses.insert(hexFc);
        break;
    }
  }

// protocol flow
  if(lastP.packetTypeP == PTM_REQUEST && currentP.packetTypeP == PTM_REQUEST) {
// ??? respones missing !!
//Serial.println("Respone missing");
    pstatus.response++;
  }
  if(lastP.packetTypeP != PTM_REQUEST && currentP.packetTypeP == PTM_RESPONSE) { // timeout ???
// ??? respones missing !!
//Serial.println("Request missing");
    pstatus.request++;
  }

// take data from task / date are not protected 
  pstatus.crcErrors = crcerrorCnt;
  pstatus.fragments = noframesCnt;
  
// monitor Trigger
  uint8_t trigLength = (loopRxDaten.length < 16 ? loopRxDaten.length :16);
  bool trigFound = true;
  for(uint8_t i=0; i<trigLength; i++) {
    if((loopRxDaten.payload[i] & triggerMask[i]) != (triggerValue[i] & triggerMask[i])){
      trigFound = false;
      break;
    }
  }
  if(trigFound) {
    Serial.println("\n\n-----> trigger\n");
    if ((!isRecording) && isArmed) {                // do it only the first time
      isRecording = true;
      isArmed = false;
      addLogEntry(last2P);  
      addLogEntry(lastP);  
    }
  }

// monitor dev,fc,address
  for(uint8_t i=0; i<4; i++) {
    if(fcvMatrix[i].fcvFc == currentP.fc && fcvMatrix[i].fcvDevice == currentP.device ) {
      if(currentP.packetTypeP == PTM_REQUEST) {
        if(currentP.fc > 0 && currentP.fc < 5 ) {
          if(fcvMatrix[i].fcvAddress == currentP.address) {
            fcvMatch = i + 1;                // read address found
            break;
          }
        } else if(currentP.fc == 5 || currentP.fc == 6 ) {
          // write address & data found
          fcvMatrix[i].timestamp = currentP.timestamp;
          String hexStr = "";
          for (size_t i = currentP.dataStart; i < currentP.length -2; i++) {
            uint8_t b =  loopRxDaten.payload[i]; 
            if (b < 0x10) hexStr += "0";
            hexStr += String(b, HEX) + " ";
          }
          hexStr.trim();
          hexStr.toUpperCase();
          fcvMatrix[i].fcvData = hexStr;
          break;
        } else if(currentP.fc == 15 || currentP.fc == 16 ) {
          // write address and multi Data, quantity found
          fcvMatrix[i].timestamp = currentP.timestamp;
          String hexStr = "";
          for (size_t i = currentP.dataStart; i < currentP.length -2; i++) {
            uint8_t b =  loopRxDaten.payload[i]; 
            if (b < 0x10) hexStr += "0";
            hexStr += String(b, HEX) + " ";
          }
          hexStr.trim();
          hexStr.toUpperCase();
          fcvMatrix[i].fcvData = hexStr;
          break;
        } else if (i == 3) {
          fcvMatch = 0;              // reset match address
        }
      } else if(currentP.packetTypeP == PTM_RESPONSE && fcvMatrix[i].fcvFc == currentP.fc && fcvMatch == i + 1) {
          // read data found
        fcvMatch = 0;              // reset match address
        fcvMatrix[i].timestamp = currentP.timestamp;

        String hexStr = "";
        for (size_t i = currentP.dataStart; i < currentP.length -2; i++) {
          uint8_t b =  loopRxDaten.payload[i]; 
          if (b < 0x10) hexStr += "0";
          hexStr += String(b, HEX) + " ";
        }
        hexStr.trim();
        hexStr.toUpperCase();
        fcvMatrix[i].fcvData = hexStr;
        break;
      } else if (i == 3) {
        fcvMatch = 0;              // reset match address
      }
    }
  }

// gernrate Output
  printPacket();
  addLogEntry(currentP);
}


//
// --- CRC 16 ---------------
//
uint16_t berechneCRC16 (const uint8_t *nData, uint16_t wLength)
{
static const uint16_t wCRCTable[] = {
0X0000, 0XC0C1, 0XC181, 0X0140, 0XC301, 0X03C0, 0X0280, 0XC241,
0XC601, 0X06C0, 0X0780, 0XC741, 0X0500, 0XC5C1, 0XC481, 0X0440,
0XCC01, 0X0CC0, 0X0D80, 0XCD41, 0X0F00, 0XCFC1, 0XCE81, 0X0E40,
0X0A00, 0XCAC1, 0XCB81, 0X0B40, 0XC901, 0X09C0, 0X0880, 0XC841,
0XD801, 0X18C0, 0X1980, 0XD941, 0X1B00, 0XDBC1, 0XDA81, 0X1A40,
0X1E00, 0XDEC1, 0XDF81, 0X1F40, 0XDD01, 0X1DC0, 0X1C80, 0XDC41,
0X1400, 0XD4C1, 0XD581, 0X1540, 0XD701, 0X17C0, 0X1680, 0XD641,
0XD201, 0X12C0, 0X1380, 0XD341, 0X1100, 0XD1C1, 0XD081, 0X1040,
0XF001, 0X30C0, 0X3180, 0XF141, 0X3300, 0XF3C1, 0XF281, 0X3240,
0X3600, 0XF6C1, 0XF781, 0X3740, 0XF501, 0X35C0, 0X3480, 0XF441,
0X3C00, 0XFCC1, 0XFD81, 0X3D40, 0XFF01, 0X3FC0, 0X3E80, 0XFE41,
0XFA01, 0X3AC0, 0X3B80, 0XFB41, 0X3900, 0XF9C1, 0XF881, 0X3840,
0X2800, 0XE8C1, 0XE981, 0X2940, 0XEB01, 0X2BC0, 0X2A80, 0XEA41,
0XEE01, 0X2EC0, 0X2F80, 0XEF41, 0X2D00, 0XEDC1, 0XEC81, 0X2C40,
0XE401, 0X24C0, 0X2580, 0XE541, 0X2700, 0XE7C1, 0XE681, 0X2640,
0X2200, 0XE2C1, 0XE381, 0X2340, 0XE101, 0X21C0, 0X2080, 0XE041,
0XA001, 0X60C0, 0X6180, 0XA141, 0X6300, 0XA3C1, 0XA281, 0X6240,
0X6600, 0XA6C1, 0XA781, 0X6740, 0XA501, 0X65C0, 0X6480, 0XA441,
0X6C00, 0XACC1, 0XAD81, 0X6D40, 0XAF01, 0X6FC0, 0X6E80, 0XAE41,
0XAA01, 0X6AC0, 0X6B80, 0XAB41, 0X6900, 0XA9C1, 0XA881, 0X6840,
0X7800, 0XB8C1, 0XB981, 0X7940, 0XBB01, 0X7BC0, 0X7A80, 0XBA41,
0XBE01, 0X7EC0, 0X7F80, 0XBF41, 0X7D00, 0XBDC1, 0XBC81, 0X7C40,
0XB401, 0X74C0, 0X7580, 0XB541, 0X7700, 0XB7C1, 0XB681, 0X7640,
0X7200, 0XB2C1, 0XB381, 0X7340, 0XB101, 0X71C0, 0X7080, 0XB041,
0X5000, 0X90C1, 0X9181, 0X5140, 0X9301, 0X53C0, 0X5280, 0X9241,
0X9601, 0X56C0, 0X5780, 0X9741, 0X5500, 0X95C1, 0X9481, 0X5440,
0X9C01, 0X5CC0, 0X5D80, 0X9D41, 0X5F00, 0X9FC1, 0X9E81, 0X5E40,
0X5A00, 0X9AC1, 0X9B81, 0X5B40, 0X9901, 0X59C0, 0X5880, 0X9841,
0X8801, 0X48C0, 0X4980, 0X8941, 0X4B00, 0X8BC1, 0X8A81, 0X4A40,
0X4E00, 0X8EC1, 0X8F81, 0X4F40, 0X8D01, 0X4DC0, 0X4C80, 0X8C41,
0X4400, 0X84C1, 0X8581, 0X4540, 0X8701, 0X47C0, 0X4680, 0X8641,
0X8201, 0X42C0, 0X4380, 0X8341, 0X4100, 0X81C1, 0X8081, 0X4040 };

uint8_t nTemp;
uint16_t wCRCWord = 0xFFFF;

   while (wLength--) {
      nTemp = *nData++ ^ wCRCWord;
      wCRCWord >>= 8;
      wCRCWord ^= wCRCTable[nTemp];
   }
   return wCRCWord;
}


// Prüft ein empfangenes Modbus-Frame (inklusive der angehängten CRC-Bytes)
bool pruefeModbusCRC(const uint8_t *buffer, size_t length) {
    if (length < 4) return false; 
    return (berechneCRC16(buffer, length) == 0x0000);
}


// ==========================================
// RTOS HINTERGRUND-TASK (PRODUCER)
// ==========================================

void getRxData_Task(void *pvParameters) {
  uart_event_t event;
  RxRawDataFrame_t temporaeresFrame;
  
  while(1) {
    // Falls die Queue im Setup noch nicht ganz bereit ist, kurz warten
    if (rxRawData_Queue == NULL) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Wartet hocheffizient (0% CPU-Last) auf Hardware-Events
    if (xQueueReceive(rxRawData_Queue, (void *)&event, portMAX_DELAY)) {
      // Wert 0 entspricht UART_DATA (Daten wurden empfangen!)
      if (event.type == 0) { 
        // prüfen, ob der Treiber meldet, dass nach diesen Daten
        // der Hardware-Timeout zugeschlagen hat (timeout_flag == true)
        if (event.timeout_flag == true) {
          size_t verfuegbareBytes = 0;
          uart_get_buffered_data_len(MODBUS_UART_NUM, &verfuegbareBytes);
          
          if (verfuegbareBytes >= 4 && verfuegbareBytes <= 256) {   // frames kleiner 5 Bytes als Störung verwerfen
            // Bytes direkt abholen (Timeout 0, da sie schon physisch im RAM liegen)
            int geleseneBytes = uart_read_bytes(MODBUS_UART_NUM, temporaeresFrame.payload, verfuegbareBytes, 0);
            
            if (geleseneBytes > 0) {
              temporaeresFrame.endTime = esp_timer_get_time();
              temporaeresFrame.startTime = g_startTime;     // Aus Flanken-ISR
              temporaeresFrame.length = geleseneBytes;
              xQueueSend(rxData_Queue, &temporaeresFrame, 0);   // send data to realtime manager
            }
          } else {
            noframesCnt++;
          }
          // Puffer bereinigen und Interrupt für nächstes Frame scharf schalten
          uart_flush_input(MODBUS_UART_NUM);
          neuesFrameStartFlag = false;
          gpio_intr_enable((gpio_num_t)RX_TRIGGER_PIN);
        }
      }
      // Schutz vor Bus-Störungen oder Puffer-Überläufen (Event 5 und 6)
      else if (event.type == 5 || event.type == 6) {
        uart_flush_input(MODBUS_UART_NUM);
        xQueueReset(rxRawData_Queue);
        neuesFrameStartFlag = false;
        gpio_intr_enable((gpio_num_t)RX_TRIGGER_PIN);
      }
    }
  }
}


//
// --- Realtime Manger z.B für schnelle Reaktionen und Senden (Priorität 5) ---
//
void realtimeRX_Process_Task(void *pvParameters) {
  RxRawDataFrame_t empfangenesFrame;

//  uint8_t antwortBuffer[256];
  
  LoopData_t datenFuerLoop;
  uint32_t lokalerSendeCounter = 0;

  while(1) {
    // Dieser Task schläft (0% CPU), bis ein fertiges Frame in der Queue liegt
    if (xQueueReceive(rxData_Queue, &empfangenesFrame, portMAX_DELAY) == pdTRUE) {
      bool crcGueltig = pruefeModbusCRC(empfangenesFrame.payload, empfangenesFrame.length);
//Serial.print(empfangenesFrame.length);Serial.println(" rt");

// act in realtime on packet content 
      if (crcGueltig) {
  /*
        int antwortlength = 0;
        
        if (empfangenesFrame.payload[1] == 0x03) { // Funktion 03: Read Holding Registers
          antwortBuffer[0] = empfangenesFrame.payload[0]; // Slave ID
          antwortBuffer[1] = 0x03;                     // Funktion
          antwortBuffer[2] = 2;                        // Byte Count
          antwortBuffer[3] = 0x00;                     // High Byte data
          antwortBuffer[4] = 0x2A;                     // Low Byte data (Wert 42)
          // ... CRC anhängen ...
          antwortlength = 7;
        }

        // ANTWORT SENDEN
        if (antwortlength > 0) {
          // Falls du einen RS485-Richtungs-Pin manuell steuerst:
          // digitalWrite(RS485_TX_ENABLE_PIN, HIGH);
          
          // Daten auf den Bus schreiben
          uart_write_bytes(MODBUS_UART_NUM, (const char*)antwortBuffer, antwortlength);
          
          // Warten, bis die Hardware alle Bytes physikalisch fertig gesendet hat!
          // Das ist extrem wichtig bei Modbus, bevor man den Richtungs-Pin wieder auf LOW zieht.
          
          uart_wait_tx_done(MODBUS_UART_NUM, pdMS_TO_TICKS(10)); //!!!!!!!!! ist abhängig von der Baudraten  !!!!
          
          // Richtungs-Pin wieder auf Empfang:
          // digitalWrite(RS485_TX_ENABLE_PIN, LOW);
          
          Serial.printf("[ProcessTask] Antwort mit %d Bytes gesendet.\n", antwortlength);
        }
        
        uint16_t antwortCrc = berechneCRC16(antwortBuffer, 5);
          
          // Modbus verlangt: Low-Byte zuerst, dann High-Byte [1]
          antwortBuffer[5] = (uint8_t)(antwortCrc & 0xFF);         // Low Byte [1]
          antwortBuffer[6] = (uint8_t)((antwortCrc >> 8) & 0xFF);  // High Byte [1]
          
          antwortlength = 7;
*/

        lokalerSendeCounter++; // Zähler erhöhen
        datenFuerLoop.counter = lokalerSendeCounter;
        size_t bytesToCopy = (empfangenesFrame.length > 256) ? 256 : empfangenesFrame.length;
        memcpy(datenFuerLoop.payload, empfangenesFrame.payload, bytesToCopy);
        datenFuerLoop.length = bytesToCopy;
        datenFuerLoop.startTime = empfangenesFrame.startTime;
        datenFuerLoop.endTime = empfangenesFrame.endTime;

        xQueueSend(toLoopData_Queue, &datenFuerLoop, 0);
      } else {
        crcerrorCnt++;
      }
    }
  }
}


void updateHardwareSerial() {
  delay(10);
  
  if (currentConfigStr == "SERIAL_8N1") currentConfigType = SERIAL_8N1;
  else if (currentConfigStr == "SERIAL_8E1") currentConfigType = SERIAL_8E1;
  else if (currentConfigStr == "SERIAL_8O1") currentConfigType = SERIAL_8O1;
 
/*  
// 1. Baudrate zur Laufzeit ändern (z. B. auf 9600)
uart_set_baudrate(MODBUS_UART_NUM, currentBaudrate);

// 2. Parität ändern (z. B. auf KEINE Parität)
uart_set_parity(MODBUS_UART_NUM, UART_PARITY_DISABLE); 
// Alternativen: UART_PARITY_EVEN, UART_PARITY_ODD

// 3. Stoppbits ändern (z. B. auf 2 Stoppbits)
uart_set_stop_bits(MODBUS_UART_NUM, UART_STOP_BITS_2);
// Alternativen: UART_STOP_BITS_1, UART_STOP_BITS_1_5

// 4. Datenbits ändern (z. B. auf 7 Bits)
uart_set_word_length(MODBUS_UART_NUM, UART_DATA_7_BITS);
// Alternativen: UART_DATA_5_BITS, UART_DATA_6_BITS, UART_DATA_8_BITS
oder 
*/

//  5:Stopp ,  32:Data +1   10:parity 0, 2, 3
  uart_parity_t new_parity = (uart_parity_t)(currentConfigType & 0x00000003);
  uart_word_length_t new_data = (uart_word_length_t)((currentConfigType >> 2) & 0x00000003);
  uart_stop_bits_t new_stopp = (uart_stop_bits_t)((currentConfigType >> 4) & 0x00000003);

  uart_config_t neue_config = {
    .baud_rate = (int)currentBaudrate,
    .data_bits = new_data,
    .parity    = new_parity,
    .stop_bits = new_stopp,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };

  uart_param_config(MODBUS_UART_NUM, &neue_config);

  timeOffset = (1000000 * UART_TIMEOUT_SYMBOLS * 11) / currentBaudrate;
}


//
// --- SETUP --------------------------------
//
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(INIT_MODE_PIN, INPUT_PULLUP);
  
  pinMode(TEST_PIN, OUTPUT);
  digitalWrite(TEST_PIN, 1);

  // Modbus-UART Konfiguration (19200, 8E1)
  uart_config_t uart_config = {
    .baud_rate = BAUD_RATE,
    .data_bits = UART_DATA_8_BITS,
    .parity    = UART_PARITY_EVEN,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
  };
  
  // UART Treiber installieren.
  uart_driver_install(MODBUS_UART_NUM, 1024, 0, 20, &rxRawData_Queue, 0);
  uart_param_config(MODBUS_UART_NUM, &uart_config);
  uart_set_pin(MODBUS_UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  
  // Hardware-Timeout auf 2 Symbole (Modbus T35 Erkennung)
  uart_set_rx_timeout(MODBUS_UART_NUM, 2); 
  uart_set_always_rx_timeout(MODBUS_UART_NUM, true);

  // RX GPIO-Interrupt für die Hardware-Brücke (RX_TRIGGER_PIN)
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << RX_TRIGGER_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE   // Fallende Flanke für das Startbit
  };

  gpio_config(&io_conf);
  gpio_install_isr_service(0);
  gpio_isr_handler_add((gpio_num_t)RX_TRIGGER_PIN, rx_start_bit_isr, (void*)RX_TRIGGER_PIN);

  // Puffer-Queue erstellen
  rxData_Queue = xQueueCreate(5, sizeof(RxRawDataFrame_t));
  toLoopData_Queue = xQueueCreate(10, sizeof(LoopData_t));

  // Verarbeitungs-Task starten (Mittlere Priorität: 5)
  xTaskCreatePinnedToCore(realtimeRX_Process_Task, "ModbusProcess", 4096, NULL, 5, NULL, 1);

  // Empfangs-Task starten (Höchste Priorität: 10)
  xTaskCreatePinnedToCore(getRxData_Task, "ModbusRx", 4096, NULL, 10, NULL, 1);

  Serial.println("Sniffer initialisiert. Warte auf Modbus...");
  
  setupNetwork();
  setupWEB();
  Serial.println("WEB ready");
}


//
// --- MAIN LOOP ----------------------------
//
void loop() {
  
  server.handleClient();
  webSocket.loop();

  while (xQueueReceive(toLoopData_Queue, &loopRxDaten, 0) == pdTRUE) {
    identifyRxData();
    triggerLedFlash();
  }
  if((int32_t)(millis() - lastWebUpdateTime) > webUpdatePeriod) {
    sendUpdate();
  }

  if (apMode) {
    handleApLedBlink();
  }
  
  
  // Hier kann langsamer Code (WLAN, SD-Karte, Display) stehen

  digitalWrite(TEST_PIN, 1);
  delay(2);
    digitalWrite(TEST_PIN, 0);
  delay(2);
  
  checkLedTimeout();
}

