/**
 * McGarager.cpp -- Simple MQTT garage door opener with endstop detection (reed sensor)
 * https://github.com/toblum/McGarager
 *
 * Copyright (C) 2022 Tobias Blum <mcgarager@tobiasblum.de>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <PubSubClient.h>
#include <uptime_formatter.h>
# include <ESP8266HTTPUpdateServer.h>

// -- Pins
#define RELAY_PIN 12
#define OPENED_SENSOR_PIN 5 // PIN 5 == D1
#define CLOSED_SENSOR_PIN 4 // PIN 4 == D2

bool sensorOpened = false;
bool sensorClosed = false;

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "McGarager";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "GaragerMc";

#define STRING_LEN 128
#define NUMBER_LEN 32

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "mcg1"

// -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to buld an AP. (E.g. in case of lost password)
#define CONFIG_PIN D2

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

// -- Method declarations.
void handleRoot();
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper);
IRAM_ATTR void handlePinChangeInterrupt();
void publishStatus();

DNSServer dnsServer;
WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// -- MQTT client setup
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttConnectionAttempt = 0;
unsigned long pinChangeDebounceTimeout = 0;
bool needsStatusUpdate = false;
bool connectMqtt();
void mqttCallback(char *topic, byte *payload, unsigned int length);

// -- IotWebConf setup
char mqttServerHostValue[STRING_LEN];
char mqttServerPortValue[NUMBER_LEN];
char mqttServerUserValue[STRING_LEN];
char mqttServerPassValue[STRING_LEN];
char mqttServerTopicValue[STRING_LEN];

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfTextParameter mqttServerHostParam = IotWebConfTextParameter("MQTT hostname", "mqttHost", mqttServerHostValue, STRING_LEN, "somehost.fritz.box");
IotWebConfNumberParameter mqttServerPortParam = IotWebConfNumberParameter("MQTT port", "mqttPort", mqttServerPortValue, NUMBER_LEN, "1883");
IotWebConfTextParameter mqttServerUserParam = IotWebConfTextParameter("MQTT username", "mqttUser", mqttServerUserValue, STRING_LEN);
IotWebConfTextParameter mqttServerPassParam = IotWebConfTextParameter("MQTT password", "mqttPass", mqttServerPassValue, STRING_LEN);
IotWebConfTextParameter mqttServerTopicParam = IotWebConfTextParameter("MQTT topic", "mqttTopic", mqttServerTopicValue, STRING_LEN, "mc_garager");
IotWebConfParameterGroup group1 = IotWebConfParameterGroup("groupMQTT", "MQTT");

// *************************************
// * setup()
// *************************************
void setup()
{
	Serial.begin(115200);
	Serial.println();
	Serial.println("Starting up...");

	pinMode(RELAY_PIN, OUTPUT);
	pinMode(OPENED_SENSOR_PIN, INPUT_PULLUP);
	pinMode(CLOSED_SENSOR_PIN, INPUT_PULLUP);

	attachInterrupt(digitalPinToInterrupt(OPENED_SENSOR_PIN), handlePinChangeInterrupt, CHANGE);
	attachInterrupt(digitalPinToInterrupt(CLOSED_SENSOR_PIN), handlePinChangeInterrupt, CHANGE);

	group1.addItem(&mqttServerHostParam);
	group1.addItem(&mqttServerPortParam);
	group1.addItem(&mqttServerUserParam);
	group1.addItem(&mqttServerPassParam);
	group1.addItem(&mqttServerTopicParam);

	iotWebConf.setStatusPin(STATUS_PIN);
	iotWebConf.setConfigPin(CONFIG_PIN);
	// iotWebConf.addSystemParameter(&stringParam);
	iotWebConf.addParameterGroup(&group1);
	iotWebConf.setConfigSavedCallback(&configSaved);
	iotWebConf.setFormValidator(&formValidator);
	iotWebConf.getApTimeoutParameter()->visible = true;

	iotWebConf.setupUpdateServer(
		[](const char *updatePath)
		{ httpUpdater.setup(&server, updatePath); },
		[](const char *userName, char *password)
		{ httpUpdater.updateCredentials(userName, password); });

	// -- Initializing the configuration.
	iotWebConf.init();

	// -- Set up required URL handlers on the web server.
	server.on("/", handleRoot);
	server.on("/config", []
			  { iotWebConf.handleConfig(); });
	server.onNotFound([]()
					  { iotWebConf.handleNotFound(); });

	// -- MQTT setup
	mqttClient.setServer(mqttServerHostValue, (uint16_t)atoi(mqttServerPortValue));
	mqttClient.setCallback(mqttCallback);

	Serial.println("setup() ready :-)");
}

// *************************************
// * loop()
// *************************************
void loop()
{
	// -- doLoop should be called as frequently as possible.
	iotWebConf.doLoop();
	mqttClient.loop();

	if ((iotWebConf.getState() == iotwebconf::OnLine) && (!mqttClient.connected()))
	{
		Serial.println("MQTT (re-)connect");
		connectMqtt();
	}

	if (needsStatusUpdate && pinChangeDebounceTimeout < millis()) {
		publishStatus();
		needsStatusUpdate = false;
	}
}

/**
 * Interrupt: Pin (open/closed state) changed
 */
IRAM_ATTR void handlePinChangeInterrupt() { 
    Serial.println("Interrupt Detected");
	pinChangeDebounceTimeout = millis() + 1000;
	needsStatusUpdate = true;
}

/**
 * Relay: Pulse relay
 */
void pulseRelay()
{
	digitalWrite(RELAY_PIN, HIGH);
	delay(250);
	digitalWrite(RELAY_PIN, LOW);
}

/**
 * Helper: Update sensor state
 */
void updateSensorState()
{
	sensorOpened = (digitalRead(OPENED_SENSOR_PIN) == LOW);
	sensorClosed = (digitalRead(CLOSED_SENSOR_PIN) == LOW);
}

/**
 * Publish status message via MQTT
 */
void publishStatus()
{
	updateSensorState();

	String opened = (sensorOpened) ? "true" : "false";
	String closed = (sensorClosed) ? "true" : "false";
	long rssi = WiFi.RSSI();
	String status = "{\"rssi\" : \"" + String(rssi) + "\", \"memory\" : \"" + String(ESP.getFreeHeap()) + "\", \"uptime\" : \"" + uptime_formatter::getUptime() + "\", \"sensor_opened\" : \"" + opened + "\", \"sensor_closed\" : \"" + closed + "\"}";
	// Serial.println(status);

	char message_buff[256];
	status.toCharArray(message_buff, status.length() + 1);

	char topicStatus[STRING_LEN + 7];
	sprintf(topicStatus, "%s/status", mqttServerTopicValue);
	mqttClient.publish(topicStatus, message_buff);
}

/**
 * (Re-)connect to MQTT broker
 */
bool connectMqtt()
{
	unsigned long now = millis();
	if (5000 > now - lastMqttConnectionAttempt)
	{
		// Do not repeat within 5 sec.
		return false;
	}
	Serial.println("Connecting to MQTT server...");

	String clientId = "McGaragerClient-";
	clientId += String(random(0xffff), HEX);

	if (mqttClient.connect(clientId.c_str()))
	{
		Serial.println("Connected!");

		char topicCmnd[STRING_LEN + 5];
		sprintf(topicCmnd, "%s/cmnd", mqttServerTopicValue);

		mqttClient.subscribe(topicCmnd);
		return true;
	}
	else
	{
		Serial.print("MQTT connection failed, rc=");
		Serial.print(mqttClient.state());
		Serial.println(" try again in 5 seconds");
		lastMqttConnectionAttempt = now;
		return false;
	}
}

/**
 * Callback on MQTT message
 */
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
	Serial.print("Message arrived [");
	Serial.print(topic);
	Serial.print("] ");
	for (unsigned int i = 0; i < length; i++)
	{
		Serial.print((char)payload[i]);
	}
	Serial.println();

	if (!strncmp((char *)payload, "trigger", length))
	{
		Serial.println("Received trigger");
		pulseRelay();
	}
	if (!strncmp((char *)payload, "status", length))
	{
		Serial.println("Received status");
		needsStatusUpdate = true;
	}
}

/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
	// -- Let IotWebConf test and handle captive portal requests.
	if (iotWebConf.handleCaptivePortal())
	{
		// -- Captive portal request were already served.
		return;
	}

	String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
	s += "<title>McGarager v2</title></head><body style=\"font-family:Arial;\"><h1>McGarager v2</h1>";
	s += "<ul>";
	s += "<li>MQTT hostname: ";
	s += mqttServerHostValue;
	s += "<li>MQTT port: ";
	s += atoi(mqttServerPortValue);
	s += "<li>MQTT username: ";
	s += mqttServerUserValue;
	s += "<li>MQTT password: ";
	s += mqttServerPassValue;
	s += "<li>MQTT topic: ";
	s += mqttServerTopicValue;
	s += "<li>Uptime: ";
	s += uptime_formatter::getUptime();
	s += "<li>Free memory: ";
	s += String(ESP.getFreeHeap());
	s += " Bytes";

	s += "</ul>";
	s += "<p>Go to <a href='config'>configure page</a> to change values.</p>";
	s += "<p>GitHub: <a href='https://github.com/toblum/McGarager' target='_blank'>https://github.com/toblum/McGarager</a></p>";
	s += "</body></html>\n";

	server.send(200, "text/html", s);
}

/**
 * Callback when IotWebConf config is saved.
 */
void configSaved()
{
	Serial.println("Configuration was updated.");
}

/**
 * Callback for IotWebConf config validation.
 */
bool formValidator(iotwebconf::WebRequestWrapper *webRequestWrapper)
{
	Serial.println("Validating form.");
	bool valid = true;

	/*
	  int l = webRequestWrapper->arg(stringParam.getId()).length();
	  if (l < 3)
	  {
		stringParam.errorMessage = "Please provide at least 3 characters for this test!";
		valid = false;
	  }
	*/
	return valid;
}