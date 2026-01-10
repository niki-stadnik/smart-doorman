#include <Arduino.h>
#include <WiFi.h>
#include "WebSocketsClient.h"
#include "StompClient.h"
#include "SudoJSON.h"

//debug
#define DEBUG 0 //1 = debug messages ON; 0 = debug messages OFF

#if DEBUG == 1
#define debugStart(x) Serial.begin(x)
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#else
#define debugStart(x)
#define debug(x)
#define debugln(x)
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
boolean rfid = false;


unsigned long sendtimeing = 0;



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
  stomper.onConnect(subscribe);
  stomper.onError(error);


 // Start the StompClient
  if (useWSS) {
    stomper.beginSSL();
  } else {
    stomper.begin();
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
  if(millis() >= keepAlive + 60000){  //if no messages are recieved in 1min - restart esp
    ESP.restart();
    keepAlive = millis();
  }

  
  if(millis() >= sendtimeing + 250){

    sendData();

    sendtimeing = millis();
  }

  webSocket.loop();
}


void sendData(){
   
  doorOpen = digitalRead(doorOpenPin);
  debug("Status, doorOpen: ");
  debugln(doorOpen);
  doorButton = digitalRead(doorButtonPin);
  debug("Status, doorButton: ");
  debugln(doorButton);
  bell = digitalRead(bellPin);
  debug("Status, bell: ");
  debugln(bell);

  
  // Construct the STOMP message
  SudoJSON json;
  json.addPair("doorOpen", doorOpen);
  json.addPair("doorLock", doorLock);
  json.addPair("doorButton", doorButton);
  json.addPair("bell", bell);
  json.addPair("rfid", rfid);
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