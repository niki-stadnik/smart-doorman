#include <Arduino.h>
#include <WiFi.h>
#include "WebSocketsClient.h"
#include "StompClient.h"
#include "SudoJSON.h"
#include <ArduinoOTA.h>
#include <HardwareSerial.h>
#include <Adafruit_PN532.h>

#define NAME "smart-doorman"

//debug
#define DEBUG 0 //1 = debug messages ON; 0 = debug messages OFF

#if DEBUG == 1
#define debugStart(x) Serial.begin(x)
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define debugwr(x) Serial.write(x)
#else
#define debugStart(x)
#define debug(x)
#define debugln(x)
#define debugwr(x)
#endif


#include "config.h"
const char* wlan_ssid = WIFI;
const char* wlan_password =  PASS;
const char * ws_host = HOSTPI;
const uint16_t ws_port = PORT;
const char* ws_baseurl = URL; 
bool useWSS = USEWSS;
const char * key = KEY;


// VARIABLES
WebSocketsClient webSocket;
Stomp::StompClient stomper(webSocket, ws_host, ws_port, ws_baseurl, true);
unsigned long keepAlive = 0;


const int relayPin = 25;
const int doorButtonPin = 26;
const int bellPin = 4;

boolean doorOpen = false;
boolean doorLock = false;
boolean doorButton = true;
boolean bell = false;
boolean zeroFlagButt = true;
boolean zeroFlagBell = false;
const char* rfid = nullptr;


unsigned long sendtimeing = 0;

// --- PIN CONFIGURATION (UART/HSU) ---
#define PN532_RX_PIN 16  // Connects to TX on PN532
#define PN532_TX_PIN 17  // Connects to RX on PN532
Adafruit_PN532 nfc(255, &Serial2);

const byte targetBlock = BLOCK; 
const byte customKey[6] = KEYCARD;

// Variables to track the currently present card to prevent spamming
uint8_t lastScannedUid[7] = { 0 };
uint8_t lastScannedUidLength = 0;
bool cardWasPresentLastLoop = false;



void setup() {
  //GPIO setup
  pinMode(doorButtonPin, INPUT);
  pinMode(bellPin, INPUT_PULLDOWN);

  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);


  // setup serial
  debugStart(115200);
  // flush it - ESP Serial seems to start with rubbish
  debugln();

  // connect to WiFi
  debugln("Logging into WLAN: " + String(wlan_ssid));
  debug(" ...");
  WiFi.begin(wlan_ssid, wlan_password);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED) {
    i++;
    delay(200);
    debug(".");
    if (i >= 25) ESP.restart(); // 5s / 200ms = 25
  }
  debugln(" success.");
  debug("IP: "); debugln(WiFi.localIP());

    //OTA upload
  ArduinoOTA.setHostname(NAME); // shows up with this name
  ArduinoOTA.setPassword("admin"); // optional security
  ArduinoOTA.onStart([]() {
    debugln("OTA update starting...");
  });
  ArduinoOTA.onEnd([]() {
    debugln("\nOTA update complete!");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    debugln("Progress: " + (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    debugln("Error" + error);
  });

  ArduinoOTA.begin();
  debugln("OTA ready");


  stomper.onConnect(subscribe);
  stomper.onError(error);


 // Start the StompClient
  if (useWSS) {
    stomper.beginSSL();
  } else {
    stomper.begin();
  }

  //RFID setup
  // FORCE ESP32 Hardware Serial 2 to use pins 16 and 17 at the PN532's standard baud rate
  Serial2.begin(115200, SERIAL_8N1, PN532_RX_PIN, PN532_TX_PIN);
  delay(10);
  nfc.begin();
  Serial2.setTimeout(15);
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    debugln("Didn't find PN532 board. Check wiring!");
    ESP.restart();
  }
  // Configure board to read RFID tags
  nfc.SAMConfig();
  debugln("PN532 Reader Ready. Scan card...");
}


// Once the Stomp connection has been made, subscribe to a topic

void subscribe(Stomp::StompCommand cmd) {
  debugln("Connected to STOMP broker");
  stomper.subscribe("/topic/doorman", Stomp::CLIENT, handleMessage);    //this is the @MessageMapping("/test") anotation so /topic must be added
  stomper.subscribe("/topic/doorman/selfReboot", Stomp::CLIENT, selfReboot);
  stomper.subscribe("/topic/lightSwitch/reboot", Stomp::CLIENT, handleRebootDev);
  stomper.subscribe("/topic/keepAlive", Stomp::CLIENT, handleKeepAlive);
}

Stomp::Stomp_Ack_t handleMessage(const Stomp::StompCommand cmd) {
  debugln(cmd.body);
  keepAlive = millis();
  getData(cmd.body);
  return Stomp::CONTINUE;
}
Stomp::Stomp_Ack_t selfReboot(const Stomp::StompCommand cmd) {
  debugln(cmd.body);
  ESP.restart();
  return Stomp::CONTINUE;
}
Stomp::Stomp_Ack_t handleRebootDev(const Stomp::StompCommand cmd) {
  debugln(cmd.body);
  keepAlive = millis();
  rebootDev();
  return Stomp::CONTINUE;
}
Stomp::Stomp_Ack_t handleKeepAlive(const Stomp::StompCommand cmd) {
  debugln(cmd.body);
  keepAlive = millis();
  return Stomp::CONTINUE;
}

void error(const Stomp::StompCommand cmd) {
  debugln("ERROR: " + cmd.body);
  ESP.restart();
}



void loop() {
  webSocket.loop();
  ArduinoOTA.handle();

  if(millis() >= keepAlive + 60000){  //if no messages are recieved in 1min - restart esp
    ESP.restart();
    keepAlive = millis();
  }

  doorButton = digitalRead(doorButtonPin);
  bell = digitalRead(bellPin);
  bool shouldSend = false;
  if (doorButton != zeroFlagButt) {
    zeroFlagButt = doorButton;
    shouldSend = true;
  }
  if (bell != zeroFlagBell) {
    zeroFlagBell = bell;
    shouldSend = true;
  }
  if (shouldSend) {
    sendData();
  }
/*
  if(millis() >= sendtimeing + 20000){
    sendData();
    sendtimeing = millis();
  }
*/
  //RFID
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uidLength;                        // Length of the UID (4 or 7 bytes depending on ISO14443A card type)

  // Health check: every 5s (when no card is on the reader) verify the PN532 still answers
  static unsigned long lastHealth = 0;
  static uint8_t healthFails = 0;
  if (!cardWasPresentLastLoop && millis() - lastHealth > 10000) {
    lastHealth = millis();
    while (Serial2.available()) Serial2.read();
    if (nfc.getFirmwareVersion()) {
      healthFails = 0;
      rfid = "ok";
      sendData();
    } else if (++healthFails >= 2) {
      debugln("PN532 unresponsive, re-initializing");
      rfid = "unresponsive";
      sendData();
      //if (!initReader()) ESP.restart();   // last resort
      healthFails = 0;
    }
  }
  while (Serial2.available()) Serial2.read();   // flush before each poll

  // Look for passive RFID tags
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);
  
  if (success) {
    // Check if this is the exact same card we processed on the previous loop turn
    bool isSameCard = false;
    if (cardWasPresentLastLoop && uidLength == lastScannedUidLength) {
      isSameCard = true;
      for (uint8_t i = 0; i < uidLength; i++) {
        if (uid[i] != lastScannedUid[i]) {
          isSameCard = false;
          break;
        }
      }
    }

    // If it's the same card still sitting on the reader, skip processing entirely!
    if (isSameCard) {
      return; 
    }

    // New card detected! Save its ID immediately so we don't process it again next loop
    cardWasPresentLastLoop = true;
    lastScannedUidLength = uidLength;
    memcpy(lastScannedUid, uid, uidLength);

    debugln("New card detected. Authenticating...");


    // Authenticate Sector 1, Block 0 (Absolute Block 4) using Key A
    // We cast our customKey array to a non-const uint8_t pointer required by the library
    success = nfc.mifareclassic_AuthenticateBlock(uid, uidLength, BLOCK, 0, (uint8_t*)customKey);
    
    if (!success) {
      debugln("Authentication failed. Wrong Key!");
      // --- FIX: Reset state machine on failure so it can try again ---
      cardWasPresentLastLoop = false;
      lastScannedUidLength = 0;
      memset(lastScannedUid, 0, sizeof(lastScannedUid));

      // Reset the chip state on authentication failure
      nfc.SAMConfig();

      return;
    }
    
    // Read the 16-byte block
    uint8_t buffer[16];
    success = nfc.mifareclassic_ReadDataBlock(BLOCK, buffer);
    
    if (success) {
      // Compare the data to our PASSWORD macro
      bool passwordMatch = true;
      const char* passwordString = PASSWORD;
      
      for (uint8_t i = 0; i < 15; i++) {
        if ((char)buffer[i] != passwordString[i]) {
          passwordMatch = false;
          break;
        }
      }
      
      if (passwordMatch) {
        debugln("🔒 PASSWORD MATCH! Access Granted.");
        rfid = KEYNAME;
        sendData();
      } else {
        debugln("❌ WRONG PASSWORD. Access Denied.");
      }
    } else {
      debugln("Failed to read block data.");
      // --- FIX: Reset state machine if the data read fails mid-stream ---
      cardWasPresentLastLoop = false;
      lastScannedUidLength = 0;
      memset(lastScannedUid, 0, sizeof(lastScannedUid));
    }

    // --- FIX: Break the crypto lock by resetting the chip state ---
    nfc.SAMConfig();

  }else {
    // If readPassiveTargetID returns false, no card is on the reader.
    // Reset our tracker variables so the card can be scanned again when brought back.
    if (cardWasPresentLastLoop) {
      debugln("Card removed from reader.");
      cardWasPresentLastLoop = false;
      lastScannedUidLength = 0;
      memset(lastScannedUid, 0, sizeof(lastScannedUid));
    }
  }
}


void sendData(){
   
  //doorButton = digitalRead(doorButtonPin);
  debug("Status, doorButton: ");
  debugln(doorButton);
  //bell = digitalRead(bellPin);
  debug("Status, bell: ");
  debugln(bell);

  
  // Construct the STOMP message
  SudoJSON json;
  json.addPair("doorButton", !doorButton);
  json.addPair("bell", bell);
  if(rfid != nullptr){
    json.addPair("rfid", rfid);
  }
  rfid = nullptr;
  // Send the message to the STOMP server
  stomper.sendMessage("/app/device/doorman", json.retrive());   //this is the @SendTo anotation
}

void getData(String input){
  SudoJSON json = SudoJSON(input);


  
}

void rebootDev(){
  digitalWrite(relayPin, HIGH);
  delay(2000);
  digitalWrite(relayPin, LOW);
}