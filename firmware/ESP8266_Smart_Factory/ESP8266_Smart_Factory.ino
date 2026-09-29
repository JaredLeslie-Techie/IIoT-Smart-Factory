#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "secrets.h"

#define DHT11_PIN D8
#define DHTTYPE DHT11

DHT dht(DHT11_PIN, DHTTYPE);

// Local credentials are defined in secrets.h (see secrets.example.h).
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* MQTT_username = MQTT_USERNAME;
const char* MQTT_password = MQTT_PASSWORD;
const char* mqtt_server = MQTT_SERVER;

// HiveMQ Cloud TLS on port 8883
const int mqtt_port = 8883;

const char* TOPIC_TEMPERATURE    = "room/temperature";
const char* TOPIC_HUMIDITY       = "room/humidity";
const char* TOPIC_MOTOR_STATE    = "room/MotorState";
const char* TOPIC_START_ROBOT    = "room/StartRobot";
const char* TOPIC_STOP_ROBOT     = "room/StopRobot";
const char* TOPIC_START_BUTTON   = "room/StartButton";
const char* TOPIC_STOP_BUTTON    = "room/StopButton";
const char* TOPIC_LIGHT_INTENSITY = "room/LightIntensity";

// MQTT command topics for remote control
const char* TOPIC_CMD_ROBOT_START    = "factory/room1/robot/start";
const char* TOPIC_CMD_ROBOT_STOP     = "factory/room1/robot/stop";
const char* TOPIC_CMD_CONVEYOR_START = "room/StartButton";
const char* TOPIC_CMD_CONVEYOR_STOP  = "room/StopButton";

// Secure client for TLS connection to HiveMQ Cloud
WiFiClientSecure espClient;
PubSubClient client(espClient);

// L298N Motor A
int motor1Pin1 = D0;
int motor1Pin2 = D1;

// PushButtons
int startButton = D2;
int stopButton = D3;

// LDR & ROBOT
const int ldrPin = A0;
const int StartRobot = D4;
const int StopRobot = D5;
const int RobotOn = D6;     // Yellow LED: robot operating
const int RobotOff = D7;    // Blue LED: robot idle

// Motor state
bool motorRunning = false;

int lightintensity = 0;

// Timers for readings, publishing, and MQTT reconnection
unsigned long previousLdrMillis = 0;
unsigned long previousDhtPublishMillis = 0;
unsigned long previousMqttReconnectMillis = 0;

const unsigned long ldrInterval = 500;
const unsigned long dhtPublishInterval = 2000;
const unsigned long reconnectInterval = 5000;

unsigned long lastStartButtonTime = 0;
unsigned long lastStopButtonTime = 0;
unsigned long lastStartRobotTime = 0;
unsigned long lastStopRobotTime = 0;
const unsigned long debounceDelay = 50;

int previousStartButtonState = HIGH;
int previousStopButtonState = HIGH;
int previousStartRobotState = HIGH;
int previousStopRobotState = HIGH;

// Returns true for common MQTT ON payloads.
bool mqttCommandIsTrue(const byte* payload, unsigned int length) {
  String command;
  command.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    command += (char)payload[i];
  }

  command.trim();
  command.toLowerCase();

  return command == "true" || command == "1" || command == "on" ||
         command == "start" || command == "pressed";
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  bool activated = mqttCommandIsTrue(payload, length);

  Serial.print("MQTT received [");
  Serial.print(topic);
  Serial.print("]: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  // Only an activated/true command changes an output.
  if (!activated) {
    return;
  }

  if (strcmp(topic, TOPIC_CMD_ROBOT_START) == 0) {
    digitalWrite(RobotOff, LOW);
    digitalWrite(RobotOn, HIGH);

    publishMessage(TOPIC_START_ROBOT, "1");
    publishMessage(TOPIC_STOP_ROBOT, "0");
  }
  else if (strcmp(topic, TOPIC_CMD_ROBOT_STOP) == 0) {
    digitalWrite(RobotOff, HIGH);
    digitalWrite(RobotOn, LOW);

    publishMessage(TOPIC_STOP_ROBOT, "1");
    publishMessage(TOPIC_START_ROBOT, "0");
  }
  else if (strcmp(topic, TOPIC_CMD_CONVEYOR_START) == 0) {
    motorRunning = true;

    publishMessage(TOPIC_MOTOR_STATE, "1");
  }
  else if (strcmp(topic, TOPIC_CMD_CONVEYOR_STOP) == 0) {
    motorRunning = false;

    publishMessage(TOPIC_MOTOR_STATE, "0");
  }
}

// Function to handle connection to the Wi-Fi network
void setup_wifi() {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("Wi-Fi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Non-blocking MQTT reconnect attempt
void reconnect() {
  if (client.connected()) {
    return;
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMqttReconnectMillis >= reconnectInterval) {
    previousMqttReconnectMillis = currentMillis;

    Serial.print("Attempting MQTT connection...");

    // Pass username and password - HiveMQ Cloud requires authentication
    String clientId = "NodeMCUClient-" + String(ESP.getChipId(), HEX);
    if (client.connect(clientId.c_str(), MQTT_username, MQTT_password)) {
      Serial.println("connected");

      bool robotStartSubscribed = client.subscribe(TOPIC_CMD_ROBOT_START);
      bool robotStopSubscribed = client.subscribe(TOPIC_CMD_ROBOT_STOP);
      bool conveyorStartSubscribed = client.subscribe(TOPIC_CMD_CONVEYOR_START);
      bool conveyorStopSubscribed = client.subscribe(TOPIC_CMD_CONVEYOR_STOP);

      Serial.print("Robot start subscription: ");
      Serial.println(robotStartSubscribed ? "OK" : "FAILED");
      Serial.print("Robot stop subscription: ");
      Serial.println(robotStopSubscribed ? "OK" : "FAILED");
      Serial.print("Conveyor start subscription: ");
      Serial.println(conveyorStartSubscribed ? "OK" : "FAILED");
      Serial.print("Conveyor stop subscription: ");
      Serial.println(conveyorStopSubscribed ? "OK" : "FAILED");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" trying again in 5 seconds");
    }
  }
}

void publishMessage(const char* topic, const char* payload) {
  if (client.connected()) {
    client.publish(topic, payload);
  }
}

void publishFloatValue(const char* topic, float value) {
  if (!client.connected()) {
    return;
  }

  char valueStr[12];
  dtostrf(value, 6, 2, valueStr);
  client.publish(topic, valueStr);
}

void publishIntValue(const char* topic, int value) {
  if (!client.connected()) {
    return;
  }

  char valueStr[12];
  snprintf(valueStr, sizeof(valueStr), "%d", value);
  client.publish(topic, valueStr);
}

void publishPeriodicStates() {
  if (!client.connected()) {
    return;
  }

  publishMessage(TOPIC_MOTOR_STATE, motorRunning ? "1" : "0");
  publishIntValue(TOPIC_LIGHT_INTENSITY, lightintensity);
}

void setup() {
  Serial.begin(115200);

  // ROBOT CONTROL
  pinMode(StartRobot, INPUT_PULLUP);
  pinMode(StopRobot, INPUT_PULLUP);

  pinMode(RobotOff, OUTPUT);
  pinMode(RobotOn, OUTPUT);

  digitalWrite(RobotOff, LOW);
  digitalWrite(RobotOn, LOW);

  // Motor
  pinMode(motor1Pin1, OUTPUT);
  pinMode(motor1Pin2, OUTPUT);

  // Button pins with internal pull-ups
  pinMode(startButton, INPUT_PULLUP);
  pinMode(stopButton, INPUT_PULLUP);

  // Start with motor off
  digitalWrite(motor1Pin1, LOW);
  digitalWrite(motor1Pin2, LOW);

  // Initialize DHT and MQTT after the motor outputs are off
  dht.begin();
  setup_wifi();

  espClient.setInsecure();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // Read the LDR at the configured interval
  if (currentMillis - previousLdrMillis >= ldrInterval) {
    previousLdrMillis = currentMillis;

    int ldrValue = analogRead(ldrPin);

    // Mapping the LDR value to 0–100
    lightintensity = map(ldrValue, 120, 890, 0, 100);

    // Prevent values outside the range
    lightintensity = constrain(lightintensity, 0, 100);

    Serial.print("Light Intensity: ");
    Serial.println(lightintensity);
  }

  // Read button states (Active LOW because of INPUT_PULLUP)
  int currentStartButtonState = digitalRead(startButton);
  int currentStopButtonState = digitalRead(stopButton);

  // StartButton publishes immediately only on a new press
  if (currentStartButtonState == LOW && previousStartButtonState == HIGH && currentMillis - lastStartButtonTime >= debounceDelay) {
    lastStartButtonTime = currentMillis;
    motorRunning = true;

    publishMessage(TOPIC_START_BUTTON, "1");
    publishMessage(TOPIC_MOTOR_STATE, "1");
    client.loop();
  }

  // StopButton publishes immediately only on a new press
  if (currentStopButtonState == LOW && previousStopButtonState == HIGH && currentMillis - lastStopButtonTime >= debounceDelay) {
    lastStopButtonTime = currentMillis;
    motorRunning = false;

    publishMessage(TOPIC_STOP_BUTTON, "1");
    publishMessage(TOPIC_MOTOR_STATE, "0");
    client.loop();
  }

  previousStartButtonState = currentStartButtonState;
  previousStopButtonState = currentStopButtonState;

  // Control the motor based on state
  if (motorRunning) {
    digitalWrite(motor1Pin1, HIGH);
    digitalWrite(motor1Pin2, LOW);
  } else {
    digitalWrite(motor1Pin1, LOW);
    digitalWrite(motor1Pin2, LOW);
  }

  int currentStartRobotState = digitalRead(StartRobot);
  int currentStopRobotState = digitalRead(StopRobot);

  // StartRobot activates the yellow LED and publishes on a new press
  if (currentStartRobotState == LOW && previousStartRobotState == HIGH && currentMillis - lastStartRobotTime >= debounceDelay) {
    lastStartRobotTime = currentMillis;
    digitalWrite(RobotOff, LOW);   // Blue OFF
    digitalWrite(RobotOn, HIGH);   // Yellow ON

    publishMessage(TOPIC_START_ROBOT, "1");
    publishMessage(TOPIC_STOP_ROBOT, "0");
    client.loop();
  }

  // StopRobot activates the blue LED and publishes on a new press
  if (currentStopRobotState == LOW && previousStopRobotState == HIGH && currentMillis - lastStopRobotTime >= debounceDelay) {
    lastStopRobotTime = currentMillis;
    digitalWrite(RobotOff, HIGH);  // Blue ON
    digitalWrite(RobotOn, LOW);    // Yellow OFF

    publishMessage(TOPIC_STOP_ROBOT, "1");
    publishMessage(TOPIC_START_ROBOT, "0");
    client.loop();
  }

  previousStartRobotState = currentStartRobotState;
  previousStopRobotState = currentStopRobotState;

  // Read DHT and publish periodic sensor values
  if (currentMillis - previousDhtPublishMillis >= dhtPublishInterval) {
    previousDhtPublishMillis = currentMillis;

    float Temperature = dht.readTemperature();
    float Humidity = dht.readHumidity();

    if (isnan(Temperature) || isnan(Humidity)) {
      Serial.println(F("Failed to read from DHT sensor!"));
    } else {
      Serial.print("Temperature = ");
      Serial.print(Temperature);
      Serial.println("°C ");
      Serial.print("Humidity = ");
      Serial.println(Humidity);

      publishFloatValue(TOPIC_TEMPERATURE, Temperature);
      publishFloatValue(TOPIC_HUMIDITY, Humidity);
    }

    // These publish periodically. StartButton and StopButton are NOT published here.
    // They only publish inside their button-press input blocks above.
    publishPeriodicStates();
  }
}
