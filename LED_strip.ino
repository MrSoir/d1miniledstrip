#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>

//#define FASTLED_ESP8266_NODEMCU_PIN_ORDER
//#define FASTLED_ESP8266_RAW_PIN_ORDER
#include <FastLED.h>

#include <ArduinoJson.h>

#include <string>
#include <sstream>
#include <vector>
#include <math.h>
#include <tuple>
#include <algorithm>
#include <memory>

#include <StaticFunctions.h>

#include <UnixTimeHandler.h>

#include <Task.h>
#include <OneTimerTask.h>
#include <RecurringTask.h>
#include <ScheduleBD.h>
#include <Scheduler.h>
#include <ScheduleHandler.h>

#include <DigitalSensorHandler.h>
#include <DigitalSensor.h>

#include <LEDstrip.h>
#include <LEDhandler.h>
#include <LEDanimation.h>

using namespace std;
using namespace SF;

//Pin numbers Wemos D1 Mini:
//D0  16  
//D1  5 - led relay 0 - x
//D2  4
//D3  0
//D4  2
//D5  14  - 1st led data pin
//D6  12  - 2nd led data pin
//D7  13  - 1rd movementSensorPin
//D8  15  - 2th movementSensorPin
//TX  1
//RX  3

//------------------forward declarations-------------------

void updateServerData();

//------------------constants and global/static variables----------------------------

#define LED_PIN_0     14
#define LED_PIN_1     12
#define NUM_LEDS_0    60
#define NUM_LEDS_1    60
#define LED_TYPE      NEOPIXEL
uint8_t BRIGHTNESS =  255;
unsigned long ledUpdateDelay = 10;

int LED_RELAY_PIN = 5;


CRGB leds0[NUM_LEDS_0];
CRGB leds1[NUM_LEDS_1];

std::vector<int> dataPins  = {LED_PIN_0, LED_PIN_1};
std::vector<int> ledCounts = {NUM_LEDS_0, NUM_LEDS_1};


int PORT = 80;
String WIFI_NAME = "WIFI_SSID";
String WIFI_PWD = "WIFI_PASSWORD";
String DNS_NAME = "d1miniledstrip0";

std::shared_ptr<String> SERVER_MDNS = std::make_shared<String>("raspberrypi");
std::shared_ptr<String> ARDUINO_ID  = std::make_shared<String>("ARDUINO_LED_STRIP");
std::shared_ptr<String> SERVER_PORT = std::make_shared<String>("8080");


std::shared_ptr<int>           UNIX_DAY_OFFSET = std::make_shared<int>(3);
std::shared_ptr<unsigned long> UNIX_TIME       = std::make_shared<unsigned long>(0);


MDNSResponder mdns;
std::shared_ptr<ESP8266WebServer> server = std::make_shared<ESP8266WebServer>(PORT);

std::shared_ptr<LEDHandler> ledHndlr;
ScheduleHandler       scheduleHndlr(server, SERVER_MDNS, SERVER_PORT, ARDUINO_ID, UNIX_TIME, UNIX_DAY_OFFSET, LED_RELAY_PIN, "/ledstrip");
UnixTimeHandler       unixTimeHndlr(server, SERVER_MDNS, SERVER_PORT, ARDUINO_ID, UNIX_TIME, UNIX_DAY_OFFSET);
DigitalSensorHandler  movementSensors;

unsigned long LED_SENSOR_ACTIVATION_DURATION = 5 * 60;

std::vector<int> MOVEMENT_SENSOR_PINS = {13,15};
bool MOVEMENT_SENSORS_ACTIVATED = true;

//------------------main functions----------------------------

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP started");
  
  WiFi.begin(WIFI_NAME.c_str(), WIFI_PWD.c_str());

  Serial.print("connecting to WiFi...");
  while(WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nconnected to WiFi!");
  Serial.print("D1 mini ip: ");
  Serial.print(WiFi.localIP());
  Serial.print("  on Port: ");
  Serial.println(PORT);

  if(mdns.begin(DNS_NAME, WiFi.localIP()))
  {
    Serial.print("SOC listening on DNS name: ");
    Serial.println(DNS_NAME);
  }

  MDNS.addService("http", "tcp", PORT);

  server->onNotFound([](){
    server->send(404, "text/plain", "link not found!");
  });

  server->on("/", [](){
    server->send(200, "text/plain", "ESP landing page!");
  });

  server->on("/setServerIPAndPort",           receiveAndSetServerIpAndPort);
  server->on("/setSensorActivationDuration",  receiveAndSetSensorActivationDuration);
  server->on("/setBrightness",                receiveAndSetBrightness);
  server->on("/activateMotionSensors",        activateMotionSensors);
  server->on("/deactivateMotionSensors",      deactivateMotionSensors);
  
  // example usage in browser:
//  http://192.168.2.110/receiveAndSetServerIpAndPort?ip=raspberrypi&ip=8080
  
  server->begin();

  //------------------------------------------------------------
    
  // set default movementSensor:
  for(const auto movmntSnsrPin: MOVEMENT_SENSOR_PINS)
  {
    movementSensors.addSensor(movmntSnsrPin);
  }

  pinMode(LED_RELAY_PIN, OUTPUT);
//  digitalWrite(LED_RELAY_PIN, HIGH);


  ledHndlr = std::make_shared<LEDHandler>( dataPins, ledCounts, server );
  ledHndlr->setServerURLs();
  const auto& ledColors = ledHndlr->getColors();
  
  FastLED.addLeds<LED_TYPE, LED_PIN_0>(ledColors[0], NUM_LEDS_0);
  FastLED.addLeds<LED_TYPE, LED_PIN_1>(ledColors[1], NUM_LEDS_1);
  FastLED.setBrightness(  BRIGHTNESS );
  FastLED.show();

  //------------------------------------------------------------

  // setup/innit data by requesting server-data:
  
  Serial.println("setup/innit data by requesting server-data");
  Serial.println("requesting UNIX-time from server");
 
//  updateServerData();

  //------------------------------------------------------------
}

void loop(){
  server->handleClient();

  if(MOVEMENT_SENSORS_ACTIVATED)
  {
    checkMovementSensors();
  }

  if( scheduleHndlr.scheduler.isRunning(*UNIX_TIME) ){
    ledHndlr->update(ledUpdateDelay);
  }

  unixTimeHndlr.updateUnixTime();
  scheduleHndlr.update();

  updateServerData();

//  delay(1000);
  delay(ledUpdateDelay);
}

//----------------------------------------------------------------------------

void activateLEDs()
{
  scheduleHndlr.scheduler.start( *UNIX_TIME + LED_SENSOR_ACTIVATION_DURATION );
}

//----------------------------------------------------------------------------

void updateServerData()
{
  unixTimeHndlr.requestDailyTimeFromServerIfNotAlreadyDone();

  // led strip shall not request server plan
//  if(unixTimeHndlr.successfullyReceivedUnixtTimeToday())
//  {
//    scheduleHndlr.requestDailyScheduleFromServerIfNotAlreadyDone();
//  }
}

void receiveAndSetServerIpAndPort()
{
  String serverIP = server->arg("ip");
  String serverPort = server->arg("port");
  if( serverIP.length() > 0 && serverPort.length() > 0 )
  {   
    *SERVER_MDNS = serverIP;
    *SERVER_PORT = serverPort;

    Serial.print("SERVER_MDNS: ");
    Serial.println(*SERVER_MDNS);
    Serial.print("serverPort: ");
    Serial.println(*SERVER_PORT);

    String msg("server-ip and server-port set to: IP: ");
    msg += *SERVER_MDNS;
    msg += String("   port: ");
    msg += *SERVER_PORT;
    
    Serial.println(msg);
    server->send(200, "text/plain", msg);
  }else{
    String msg("invalid server-ip and/or server-port received!");
    Serial.println(msg);
    server->send(400, "text/plain", msg);
  }
}

void receiveAndSetBrightness()
{
  float brghtnsFlt = server->arg("brightness").toFloat();
  setBrightness(brghtnsFlt);
}
void setBrightness(float brghtnsFlt)
{
  uint8_t brghtns = 5 + static_cast<uint8_t>((255.0f - 5.0f) * brghtnsFlt);
  
  BRIGHTNESS = brghtns;
  FastLED.setBrightness( BRIGHTNESS );
  FastLED.show();
  
  String msg("successfully received animation -> brightness set to: ");
  msg.concat(brghtnsFlt);
  msg.concat("!");
  Serial.println(msg);
  server->send(200, "text/plain", msg);
}

void activateMotionSensors()
{
  MOVEMENT_SENSORS_ACTIVATED = true;
  String msg("activated motion sensors!");
  server->send(200, "text/plain", msg);
}
void deactivateMotionSensors()
{
  MOVEMENT_SENSORS_ACTIVATED = false;
  String msg("deactivated motion sensors!");
  server->send(200, "text/plain", msg);
}


//-----------------------Sensors code-----------------------

void checkMovementSensors()
{
  if(movementSensors.anySensorActive())
  {
    activateLEDs();
  }
}

void setSensorActivationDuration(unsigned long dur)
{
  LED_SENSOR_ACTIVATION_DURATION = dur;
}
void receiveAndSetSensorActivationDuration()
{
  auto dur = server->arg("duration").toInt();
  if( dur > 0 )
  {
    setSensorActivationDuration(dur);
    
    String msg("SensorActivationDuration recceived: ");
    msg.concat(dur);
    server->send(200, "text/plain", msg);
  }else{
    String msg("invalid SensorActivationDuration received: ");
    msg.concat(dur);
    msg.concat("!!!");
    server->send(400, "text/plain", msg);
  }
}
