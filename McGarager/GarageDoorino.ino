#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

//needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h>

const char DEVICE_ID[40] = "McGarager";
const char TOPIC_STATUS[40] = "garage_door/status";
const char TOPIC_CMD[40] = "garage_door/cmd";

char message_buff[128];
enum STATE { IS_OPENED, IS_CLOSING, IS_CLOSED, IS_OPENING };
STATE state = IS_CLOSING;
enum SENSOR_STATE { SENSOR_OPENED, SENSOR_CLOSED, SENSOR_NONE };
SENSOR_STATE sensor_state = SENSOR_NONE;
long stateChangeTimestamp = 0;




// #########################################################
// # WifiManager:
// # Config data, initial values, will be overwritten if they are different in config.json
// #########################################################
char mqtt_server[40] = "raspberrypi2";
char mqtt_port[6] = "1883";
char mqtt_topic[34] = "garage_door";

bool shouldSaveConfig = false;


// #########################################################
// # Pin definition
// #########################################################
#define CONFIGAP_TRIGGER_PIN 16
#define RELAY_PIN 12
#define OPENED_SENSOR_PIN 5   // PIN 5 == D1
#define CLOSED_SENSOR_PIN 4   // PIN 4 == D2


// #########################################################
// # PubSubClient
// #########################################################
WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
long lastReconnectAttempt = 0;



// #########################################################
// # Callback notifying us of the need to save config
// #########################################################
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}



void setup() {
  // Init serial
  Serial.begin(115200);
  Serial.println("");
  Serial.println("****************");

  // Init pins
  pinMode(CONFIGAP_TRIGGER_PIN, INPUT);
  //pinMode(BUILTIN_LED, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(OPENED_SENSOR_PIN, INPUT_PULLUP);
  pinMode(CLOSED_SENSOR_PIN, INPUT_PULLUP);
  

  // #########################################################
  // # Init SPIFFS and read config
  // #########################################################
  //SPIFFS.format();
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      // File exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_topic, json["mqtt_topic"]);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 5);
  WiFiManagerParameter custom_mqtt_topic("topic", "mqtt topic", mqtt_topic, 32);


  // #########################################################
  // # WiFiManager
  // #########################################################
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // Set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Add all your parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_topic);

  // Reset settings - for testing
  //wifiManager.resetSettings();

  if (!wifiManager.autoConnect("McGaragerAP")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }
  
  // If you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  // Read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());

  // Save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("Saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_topic"] = mqtt_topic;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
  }

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());


  // #########################################################
  // # Init MQTT
  // #########################################################
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(mqtt_callback);
  lastReconnectAttempt = 0;
  Serial.println("Ready");
}


// #########################################################
// # Relay: Pulse relay
// #########################################################
void pulseRelay() {
  digitalWrite(RELAY_PIN, HIGH);
  delay(200);
  digitalWrite(RELAY_PIN, LOW);
}


// #########################################################
// # MQTT: Received message
// #########################################################
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  int i = 0;
  for (i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
    message_buff[i] = payload[i];
  }
  message_buff[i] = '\0';
  String msgString = String(message_buff);
  Serial.println();

  if (msgString.equals("status")) {
    publishStatus();
  }
  if (msgString.equals("trigger")) {
    pulseRelay();
  }
}


// #########################################################
// # MQTT: Get status 
// #########################################################
String getStatus() {
  long rssi = WiFi.RSSI();
  String status = "[\"device\" : \"" + String(DEVICE_ID) + "\", \"rssi\" : \"" + String(rssi) + "\", \"state\" : \"" + getStateString() + "\", \"sensor_state\" : \"" + getSensorStateString() + "\"]";
  Serial.println(status);
  return status;
}


// #########################################################
// # MQTT: Reconnect when connection lost
// #########################################################
boolean reconnect() {
  if (client.connect(DEVICE_ID)) {
    // Once connected, publish an announcement...
    publishStatus();

    // ... and resubscribe
    client.subscribe(TOPIC_CMD);
  }
  return client.connected();
}


// #########################################################
// # MQTT: Publish status
// #########################################################
void publishStatus() {
  String pubString = getStatus();
  pubString.toCharArray(message_buff, pubString.length()+1);
  
  client.publish(TOPIC_STATUS, message_buff);
}


// #########################################################
// # Helper: Get descriptive state text
// #########################################################
String getStateString() {
  if (state == IS_OPENED) {
    return "IS_OPENED";
  }
  if (state == IS_CLOSING) {
    return "IS_CLOSING";
  }
  if (state == IS_CLOSED) {
    return "IS_CLOSED";
  }
  if (state == IS_OPENING) {
    return "IS_OPENING";
  }
}


// #########################################################
// # Helper: Get descriptive sonsor state text
// #########################################################
String getSensorStateString() {
  if (sensor_state == SENSOR_NONE) {
    return "SENSOR_NONE";
  }
  if (sensor_state == SENSOR_OPENED) {
    return "SENSOR_OPENED";
  }
  if (sensor_state == SENSOR_CLOSED) {
    return "SENSOR_CLOSED";
  }
}


// #########################################################
// # Helper: Get sensor state
// #########################################################
void getSensorState() {
  sensor_state = SENSOR_NONE;

  bool opened = (digitalRead(OPENED_SENSOR_PIN) == LOW);
  bool closed = (digitalRead(CLOSED_SENSOR_PIN) == LOW);

  if (opened && !closed) {
    sensor_state = SENSOR_OPENED;
  }
  if (closed && !opened) {
    sensor_state = SENSOR_CLOSED;
  }
}


// #########################################################
// # Helper: Central statemachine implementation
// #########################################################
void statemachine() {
  STATE last_state = state;
  getSensorState();

  if (state == IS_OPENED) {
    if (sensor_state == SENSOR_NONE) {
      state = IS_CLOSING;
    }
    if (sensor_state == SENSOR_CLOSED) {
      state = IS_CLOSED;
    }
  }

  if (state == IS_CLOSING || state == IS_OPENING) {
    if (sensor_state == SENSOR_OPENED) {
      state = IS_OPENED;
    }
    if (sensor_state == SENSOR_CLOSED) {
      state = IS_CLOSED;
    }
  }

  if (state == IS_CLOSED) {
    if (sensor_state == SENSOR_NONE) {
      state = IS_OPENING;
    }
    if (sensor_state == SENSOR_OPENED) {
      state = IS_OPENED;
    }
  }

  if (last_state != state) {
    publishStatus();
  }
}



// #########################################################
// # Main loop
// #########################################################
void loop() {
  if ( digitalRead(CONFIGAP_TRIGGER_PIN) == LOW ) {
    Serial.println("CONFIGAP_TRIGGER_PIN CLICKED");
    WiFiManager wifiManager;
    wifiManager.setTimeout(300);
    if (!wifiManager.startConfigPortal("McGaragerAP")) {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
    }
    Serial.println("connected...yeey :)");
  }

  // If not connected reconnect after 5 seconds
  if (!client.connected()) {
    long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      // Attempt to reconnect
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();
  }

  statemachine();
}
