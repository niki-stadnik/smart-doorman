#include <Arduino.h>
#include <WiFi.h>
#include "WebSocketsClient.h"
#include "StompClient.h"
#include "SudoJSON.h"
#include <ArduinoOTA.h>
#include <SPI.h>
#include <MFRC522.h>

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
const int doorOpenPin = 26;
const int doorButtonPin = 27;
const int bellPin = 4;

boolean doorOpen = false;
boolean doorLock = false;
boolean doorButton = false;
boolean bell = false;
boolean zeroFlagButt = false;
boolean zeroFlagBell = false;
const char* rfid = nullptr;


unsigned long sendtimeing = 0;

//MFRC522 setup
#define RST_PIN   22
#define SS_PIN    5
MFRC522 mfrc522(SS_PIN, RST_PIN);
const byte targetBlock = BLOCK; 
MFRC522::MIFARE_Key sectorKey;



void setup() {
  //GPIO setup
  pinMode(doorOpenPin, INPUT_PULLDOWN);
  pinMode(doorButtonPin, INPUT_PULLDOWN);
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
  SPI.begin();
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max); // ADD THIS LINE TO BOOST RADIO POWER
  Serial.println(F("Secure Reader Ready. Scan card..."));
  // Create an array using your defined bytes
  const byte customKey[6] = KEYCARD; 
  for (byte i = 0; i < 6; i++) {
    sectorKey.keyByte[i] = customKey[i];
  }
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

  if(millis() >= sendtimeing + 20000){
    sendData();
    sendtimeing = millis();
  }

  //RFID
  if ( ! mfrc522.PICC_IsNewCardPresent() || ! mfrc522.PICC_ReadCardSerial() ) {
    return;
  }
  debugln("Card detected. Authenticating sector...");
  // STEP 1: Authenticate using Key A
  MFRC522::StatusCode status;
  status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, targetBlock, &sectorKey, &(mfrc522.uid));
  if (status != MFRC522::STATUS_OK) {
    debug(F("Authentication failed: "));
    debugln(mfrc522.GetStatusCodeName(status));
    mfrc522.PICC_HaltA();
    return;
  }
  // STEP 2: Read data from the block
  byte buffer[18]; // Buffer must be at least 18 bytes to hold the 16-byte block + 2 bytes CRC
  byte size = sizeof(buffer);
  status = mfrc522.MIFARE_Read(targetBlock, buffer, &size);
  if (status != MFRC522::STATUS_OK) {
    debug(F("Reading block failed: "));
    debugln(mfrc522.GetStatusCodeName(status));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    return;
  }
  // STEP 3: Check if the data matches the secret password
  bool passwordMatch = true;
  for (uint8_t i = 0; i < 15; i++) { // Compare the first 15 characters
    if ((char)buffer[i] != PASSWORD[i]) {
      passwordMatch = false;
      break;
    }
  }
  // STEP 4: Grant or Deny Access
  if (passwordMatch) {
    debugln(F("🔒 PASSWORD MATCH! Access Granted."));
    rfid = KEYNAME;
    sendData();
  } else {
    debugln(F("❌ WRONG PASSWORD. Access Denied."));
    sendData();
    // Print what was actually found on the card for debugging
    debug(F("Found data: "));
    for (uint8_t i = 0; i < 16; i++) {
      debugwr(buffer[i]);
    }
    debugln();
  }
  // STEP 5: Clean up radio communications and encryption
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
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
  json.addPair("doorButton", doorButton);
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