/* 
Description:
Connect a clearcore device to MQ broker and subscribe to the following topics:
- input/StackLight
- input/state
- output/state
- output/analog1
- output/analog2

Set the following outputs based on the data received from the above topics:
- input/StackLight - Set the stack light color and frequency based on the following values:
    - RED - Set the stack light to red
    - GREEN - Set the stack light to green
    - YELLOW - Set the stack light to yellow
    - BLUE - Set the stack light to blue
    - ALL - Set the stack light to all colors

    - FAST - Set the stack light to blink fast
    - SLOW - Set the stack light to blink slow 
    - ON - Set the stack light to be on
    - OFF - Set the stack light to be off
    - FIRE - Set the stack light to red slow and yellow fast

- output/state - Publish the state of the machine based on the following values:
    - NEED_MAINT - Publish that the state of the machine is NEED_MAINT. This will be 
    published when the local input is pressed
    - Connected - Publish the state of the machine as Connected (the default state). This will be
    published when the device connects to the MQTT broker
    - reconnected - Set the state of the machine to Connected (the default state). This will be 
    published when the device reconnects to the MQTT broker.

Read analog input and publish to the following topic:
    - output/Analog1 - the analog value of Analog1
    - output/Analog2 - the analog value of Analog2
Display output/Analog on the corresponding serial monitor

The device also has a local input that when pressed will publish the following values to the 
input/StackLight and output/state topics:
- input/stacksight - "ALL,OFF" and "BLUE,SLOW"
- output/state - "NEED_MAINT"

The device will also reset when the input/state topic receives the value "MASTER_RESET"
- The device will publish "RESETTING" to the output/state topic
- The device will reset after 1 second

The device will also print debug messages to the serial monitor if the debug_mode is set to true
-  debug_mode - Set to true to enable debug messages

The device will also connect to the MQTT broker using the following settings:
- mqtt_server - The IP address of the MQTT broker
- mqtt_user - The username of the MQTT broker
- mqtt_password - The password of the MQTT broker
- mqtt_client - The client name of the MQTT broker

Author: Buzzy Lee and Github Pilot
Date: 2024 MAR 26
*/

#include <ClearCore.h>
#include <PubSubClient.h>
#include <Ethernet.h>
#include <SPI.h>
#include <SD.h>

// Global Variables

// Set debug_mode to true to enable debug messages
bool debug_mode = true; // Change this to true to enable debug_mode output

//Initialize the SD card
File myFile;

//Define pins
#define RED IO0
#define GREEN IO1
#define BLUE IO2
#define ALARM IO4
#define INPUT_PIN_1 IO3
#define ANALOG_PIN_1 A12
#define ANALOG_PIN_2 A11


// Define debounce time
#define DEBOUNCE_TIME 250

// Last time the button was pressed
unsigned long lastButtonPress = 0;

// MQTT server settings
// These are now defined in the settings.ini file
//const char* mqtt_server = "10.10.12.215";
//const char* mqtt_user = "BBMMCC001";
//const char* mqtt_password = "BB69420";
//const char* mqtt_client = "ClearCore";

// Declare your variables
char machine_location[2];
char asset_number[5];
char mqtt_server[16];
char mqtt_user[10];
char mqtt_password[10];
char mqtt_client[15];
char StackLightInputTopic[20];
char stateInputTopic[20];
char analog1OutputTopic[20];
char analog2OutputTopic[20];
char currentStateTopic[20];
int fastOnTime;
int fastOffTime;
int slowOnTime;
int slowOffTime;

// Machine settings

// These are now defined in the settings.ini file
// Update these with values suitable for your machine!
//const char* machine_location = "T"; //T for building 533 Q for building 440
//const char* asset_number = "0001";  //Omit the preceding T or Q and use only the last 4 digit nummber, e.g. 0173


/*
// MQTT subscribe topics
String mqtt_subtopic_StackLight_str = String(machine_location) + "/" + asset_number + StackLightInputTopic;
const char* mqtt_subtopic_StackLight = mqtt_subtopic_StackLight_str.c_str();

String mqtt_subtopic_state_str = String(machine_location) + "/" + asset_number + stateInputTopic;
const char* mqtt_subtopic_state = mqtt_subtopic_state_str.c_str();

// MQTT publish topics
String mqtt_pubtopic_analog_str1 = String(machine_location) + "/" + asset_number + analog1OutputTopic;
const char* mqtt_pubtopic_analog1 = mqtt_pubtopic_analog_str1.c_str();

String mqtt_pubtopic_analog_str2 = String(machine_location) + "/" + asset_number + analog2OutputTopic;
const char* mqtt_pubtopic_analog2 = mqtt_pubtopic_analog_str2.c_str();

String mqtt_pubtopic_state_str = String(machine_location) + "/" + asset_number + currentStateTopic;
const char* mqtt_pubtopic_state = mqtt_pubtopic_state_str.c_str();
*/

// MQTT subscribe topics
String mqtt_subtopic_StackLight_str;
const char* mqtt_subtopic_StackLight;

String mqtt_subtopic_state_str;
const char* mqtt_subtopic_state;

// MQTT publish topics
String mqtt_pubtopic_analog_str1;
const char* mqtt_pubtopic_analog1;

String mqtt_pubtopic_analog_str2;
const char* mqtt_pubtopic_analog2;

String mqtt_pubtopic_state_str;
const char* mqtt_pubtopic_state;


// StackLight mode settings
enum StackLightMode {
    OFF,
    FAST,
    SLOW,
    ON,
};

StackLightMode stackLightMode[4] = {OFF, OFF, OFF, OFF};

// These are now defined in the settings.ini file
// Define on and off times for FAST and SLOW modes
//unsigned long fastOnTime = 350; // 350ms
//unsigned long fastOffTime = 150; // 150ms
//unsigned long slowOnTime = 750; // 750ms
//unsigned long slowOffTime = 250; // 250ms


// StackLight Color settings
enum StackLightColor {
    COLOR_NONE = 0,
    COLOR_RED = 1,
    COLOR_GREEN = 2,
    COLOR_BLUE = 4,
    COLOR_YELLOW = COLOR_RED | COLOR_GREEN, // New color
    COLOR_MAGENTA = COLOR_RED | COLOR_BLUE, // New color
    COLOR_CYAN = COLOR_GREEN | COLOR_BLUE, // New color
    COLOR_WHITE = COLOR_RED | COLOR_GREEN | COLOR_BLUE, // New color
    COLOR_ALL = COLOR_RED | COLOR_GREEN | COLOR_BLUE,
    COLOR_ALARM = 8
};

StackLightColor stackLightColor[4] = {COLOR_NONE, COLOR_NONE, COLOR_NONE, COLOR_NONE};

// FIREMODE ENUM
enum FireModeState {
    FIRE_RED,
    FIRE_YELLOW,
    FIRE_ORANGE
};

// Firemode State
FireModeState fireModeState = FIRE_RED;


// ClearCore settings
const int analogPin1 = A12;
int analogValue1 = 0;
int analogValueOld1 = 0;
const int analogPin2 = A11;
int analogValue2 = 0;
int analogValueOld2 = 0;
unsigned long lastPublishTime = 0;


// Ethernet settings
// Update these with values suitable for your network.
byte mac[]    = {  0x24, 0x15, 0x10, 0xb0, 0x06, 0x68 };
IPAddress ip(10, 10, 12, 205);
IPAddress server(10, 10, 12, 215);

// Initialize the Ethernet client object
EthernetClient ethClient;
PubSubClient client(ethClient);

// Debug print function
void debugPrint(String message) {
    if (debug_mode) {
        Serial.println(message);
    }
}



// Callback function for MQTT messages
void callback(char* topic, byte* payload, unsigned int length) {
    payload[length] = '\0'; // Null-terminate the payload
    String strTopic = String(topic);
    String strPayload = String((char*)payload);
    debugPrint("Message arrived [" + strTopic + "] " + strPayload);

    // Handle the message based on the topic
    if (strTopic == mqtt_subtopic_StackLight) {
        int commaIndex = strPayload.indexOf(',');
        String color = strPayload.substring(0, commaIndex);
        String mode = strPayload.substring(commaIndex + 1);

      // Set the StackLight color and mode based on the received values
        if (color == "ALL" && mode == "FIRE") {
            // Cycle through RED, YELLOW, and ORANGE
            if (fireModeState == FIRE_RED) {
                // Set RED light to SLOW
                stackLightColor[0] = COLOR_RED;
                stackLightMode[0] = StackLightMode::SLOW;

                // Set other lights to OFF
                stackLightColor[1] = COLOR_GREEN;
                stackLightMode[1] = StackLightMode::OFF;
                stackLightColor[2] = COLOR_BLUE;
                stackLightMode[2] = StackLightMode::OFF;

                // Move to the next state
                fireModeState = FIRE_YELLOW;
            } else if (fireModeState == FIRE_YELLOW) {
                // Set YELLOW light to SLOW
                stackLightColor[0] = COLOR_RED;
                stackLightColor[1] = COLOR_GREEN;
                stackLightMode[0] = StackLightMode::SLOW;
                stackLightMode[1] = StackLightMode::SLOW;

                // Set other lights to OFF
                stackLightColor[2] = COLOR_BLUE;
                stackLightMode[2] = StackLightMode::OFF;

                // Move to the next state
                fireModeState = FIRE_ORANGE;
            } else if (fireModeState == FIRE_ORANGE) {
                // Set ORANGE light to SLOW
                stackLightColor[0] = COLOR_RED;
                stackLightColor[1] = COLOR_GREEN;
                stackLightMode[0] = StackLightMode::SLOW;
                stackLightMode[1] = StackLightMode::SLOW;

                // Set other lights to OFF
                stackLightColor[2] = COLOR_BLUE;
                stackLightMode[2] = StackLightMode::OFF;

                // Reset to the first state
                fireModeState = FIRE_RED;
            }

            // Set ALARM to FAST
            stackLightColor[3] = COLOR_ALARM;
            stackLightMode[3] = StackLightMode::FAST;
        } else {
            StackLightMode newMode;
            if (mode == "FAST") {
                newMode = StackLightMode::FAST;
            } else if (mode == "SLOW") {
                newMode = StackLightMode::SLOW;
            } else if (mode == "ON") {
                newMode = StackLightMode::ON;
            } else if (mode == "OFF") {
                newMode = StackLightMode::OFF;
            }

        if (color == "ALL") {
            for (int i = 0; i < 3; i++) {
                stackLightColor[i] = (StackLightColor)(1 << i);
                stackLightMode[i] = newMode;
            }
             } else {
                int colorIndex = -1;
                if (color == "RED") {
                    colorIndex = 0;
                } else if (color == "GREEN") {
                    colorIndex = 1;
                } else if (color == "BLUE") {
                    colorIndex = 2;
                } else if (color == "YELLOW") { // New color
                    stackLightColor[0] = COLOR_RED; // Red
                    stackLightColor[1] = COLOR_GREEN; // Green
                    stackLightMode[0] = newMode;
                    stackLightMode[1] = newMode;
                    return;
                } else if (color == "MAGENTA") { // New color
                    stackLightColor[0] = COLOR_RED; // Red
                    stackLightColor[2] = COLOR_BLUE; // Blue
                    stackLightMode[0] = newMode;
                    stackLightMode[2] = newMode;
                    return;
                } else if (color == "CYAN") { // New color
                    stackLightColor[1] = COLOR_GREEN; // Green
                    stackLightColor[2] = COLOR_BLUE; // Blue
                    stackLightMode[1] = newMode;
                    stackLightMode[2] = newMode;
                    return;
                } else if (color == "WHITE") { // New color
                    for (int i = 0; i < 3; i++) {
                        stackLightColor[i] = (StackLightColor)(1 << i);
                        stackLightMode[i] = newMode;
                    }
                    return;
                }

                if (colorIndex != -1) {
                    stackLightColor[colorIndex] = (StackLightColor)(1 << colorIndex);
                    stackLightMode[colorIndex] = newMode;
                }
            }
        }
    } 
    else if (strTopic == mqtt_subtopic_state) {
        if (strPayload == "MASTER_RESET") {
            client.publish(mqtt_pubtopic_state, "RESETTING");
            delay(1000);
            masterReset();
        }
    }
    debugPrint("StackLight: " + String(stackLightColor[0]) + ", " + String(stackLightColor[1]) + ", " + String(stackLightColor[2]) + ", " + String(stackLightColor[3]));
}

// Reconnect to the MQTT broker
void reconnect() {
    while (!client.connected()) {
        debugPrint("Attempting MQTT connection...");

        if (client.connect(mqtt_client, mqtt_user, mqtt_password)) {
            debugPrint("connected");
            client.publish(mqtt_pubtopic_state, "Re-connected");
            subscribeToTopics();
        } else {
            debugPrint("failed, rc=" + String(client.state()) + " try again in 5 seconds");
            delay(5000);
        }
    }
}

void masterReset() {
    // Set all stack lights to ON
    for (int i = 0; i < 4; i++) {
        stackLightColor[i] = (StackLightColor)(1 << i);
        stackLightMode[i] = StackLightMode::ON;
    }

    // Reset the board
    SysMgr.ResetBoard();
}

void updateStackLight() {
    static unsigned long lastUpdateTime[4] = {0, 0, 0, 0};
    static bool blinkState[4] = {false, false, false, false};
    unsigned long currentTime = millis();

    // Determine the desired state of the light based on the mode
    bool lightState[4];
    for (int i = 0; i < 4; i++) {
        if (stackLightMode[i] == StackLightMode::ON) {
            lightState[i] = true;
        } else if (stackLightMode[i] == StackLightMode::OFF) {
            lightState[i] = false;
        } else {
            // For FAST and SLOW modes, we blink the light on and off
            unsigned long blinkPeriod;
            if (stackLightMode[i] == StackLightMode::FAST) {
                blinkPeriod = blinkState[i] ? fastOnTime : fastOffTime;
            } else { // SLOW
                blinkPeriod = blinkState[i] ? slowOnTime : slowOffTime;
            }
            if ((currentTime - lastUpdateTime[i]) >= blinkPeriod) {
                blinkState[i] = !blinkState[i];
                lastUpdateTime[i] = currentTime;
            }
            lightState[i] = blinkState[i];
        }
    }

    // Update the light state
    digitalWrite(RED, lightState[0]);
    digitalWrite(GREEN, lightState[1]);
    digitalWrite(BLUE, lightState[2]);
    digitalWrite(ALARM, lightState[3]);
}

// Publish the analog values to the output/Analog1 and output/Analog2 topics
void publishAnalogValue() {
    static unsigned long lastPublishTime = 0;
    unsigned long currentMillis = millis();

    if (currentMillis - lastPublishTime >= 3000) {
        int analogValue1 = analogRead(ANALOG_PIN_1);
        if (analogValue1 != analogValueOld1) {
            char analogValueString1[5];
            sprintf(analogValueString1, "#%04d*", analogValue1);
            client.publish(mqtt_pubtopic_analog1, analogValueString1);
            analogValueOld1 = analogValue1;
            debugPrint("Published analog1 value: " + String(analogValueString1));
            Serial0.print(analogValueString1);
        }
        int analogValue2 = analogRead(ANALOG_PIN_2);
        if (analogValue2 != analogValueOld2) {
            char analogValueString2[5];
            sprintf(analogValueString2, "#%04d*", analogValue2);
            client.publish(mqtt_pubtopic_analog2, analogValueString2);
            analogValueOld2 = analogValue2;
            debugPrint("Published analog2 value: " + String(analogValueString2));
            Serial1.print(analogValueString2);
        }
        lastPublishTime = currentMillis;
    }
}

// Subscribe to the input/StackLight and input/state topics
void subscribeToTopics() {
    client.subscribe(mqtt_subtopic_StackLight);
    client.subscribe(mqtt_subtopic_state);

    //debug print the subscribed topics
    debugPrint("Subscribed to topics: " + String(mqtt_subtopic_StackLight) + ", " + String(mqtt_subtopic_state));
}

void localInput() {
    // Debounce the button press
    if ((millis() - lastButtonPress) > DEBOUNCE_TIME) {
        // Update the last button press time
        lastButtonPress = millis();

        // Publish "ALL,OFF" and "BLUE,ON" to the input/StackLight topic
        client.publish(mqtt_subtopic_StackLight, "ALL,OFF");
        client.publish(mqtt_subtopic_StackLight, "BLUE,SLOW");

        // Publish "NEED_MAINT" to the output/state topic
        client.publish(mqtt_pubtopic_state, "NEED_MAINT");
    }
    //debug print the local input
    debugPrint("Local input pressed");
 }

void setup() {
  // Initialize the serial ports
  if (debug_mode) {
    Serial.begin(9600);
    delay(1000); // Wait for the USB serial port to open
    debugPrint("Serial communication started");
  }   

  Serial0.begin(9600);
  Serial1.begin(9600);

  // Initialize SD card
  if (!SD.begin()) {
    debugPrint("SD card initialization failed!");
    return;
  }
  debugPrint("SD card initialized.");

  // Open the file
  File file = SD.open("settings.ini");
  if (file) {
    debugPrint("Opened settings.ini");
    while (file.available()) {
      // Read the file line by line
      String line = file.readStringUntil('\n');

      // Remove any leading or trailing whitespace
        line.trim();

        // Skip empty lines
        if (line.length() == 0) {
            continue;
        }

        // Skip comments
        if (line.startsWith("#")) {
            continue;
        }

        // Skip lines that don't contain an equals sign
        if (!line.indexOf("=")) {
            continue;
        }

        //Find the semi-colon and remove it, and any trailing characters
        int semiColonIndex = line.indexOf(";");
        if (semiColonIndex != -1) {
            line = line.substring(0, semiColonIndex);
        }
        
      // Print the line to the serial monitor
      debugPrint(line);
      
      // Check if the line contains the key you're looking for and extract the value
      if (line.startsWith("machine_location=")) {
        line.substring(17).toCharArray(machine_location, sizeof(machine_location));
      } else if (line.startsWith("asset_number=")) {
        line.substring(13).toCharArray(asset_number, sizeof(asset_number));
      } else if (line.startsWith("server=")) {
        line.substring(7).toCharArray(mqtt_server, sizeof(mqtt_server));
      } else if (line.startsWith("user=")) {
        line.substring(5).toCharArray(mqtt_user, sizeof(mqtt_user));
      } else if (line.startsWith("password=")) {
        line.substring(9).toCharArray(mqtt_password, sizeof(mqtt_password));
      } else if (line.startsWith("client=")) {
        line.substring(7).toCharArray(mqtt_client, sizeof(mqtt_client));
      } else if (line.startsWith("StackLightInputTopic=")) {
        line.substring(21).toCharArray(StackLightInputTopic, sizeof(StackLightInputTopic));
      } else if (line.startsWith("stateInputTopic=")) {
        line.substring(16).toCharArray(stateInputTopic, sizeof(stateInputTopic));
      } else if (line.startsWith("analog1OutputTopic=")) {
        line.substring(19).toCharArray(analog1OutputTopic, sizeof(analog1OutputTopic));
      } else if (line.startsWith("analog2OutputTopic=")) {
        line.substring(19).toCharArray(analog2OutputTopic, sizeof(analog2OutputTopic));
      } else if (line.startsWith("currentStateTopic=")) {
        line.substring(18).toCharArray(currentStateTopic, sizeof(currentStateTopic));
      } else if (line.startsWith("fastOnTime=")) {
        fastOnTime = line.substring(11).toInt();
      } else if (line.startsWith("fastOffTime=")) {
        fastOffTime = line.substring(12).toInt();
      } else if (line.startsWith("slowOnTime=")) {
        slowOnTime = line.substring(11).toInt();
      } else if (line.startsWith("slowOffTime=")) {
        slowOffTime = line.substring(12).toInt();
      }
    }
    
    // Close the file
    file.close();

    // Set the MQTT subscribe and publish topics
    mqtt_subtopic_StackLight_str = String(machine_location) + asset_number + StackLightInputTopic;
    mqtt_subtopic_StackLight = mqtt_subtopic_StackLight_str.c_str();

    mqtt_subtopic_state_str = String(machine_location) + asset_number + stateInputTopic;
    mqtt_subtopic_state = mqtt_subtopic_state_str.c_str();

    mqtt_pubtopic_analog_str1 = String(machine_location) + asset_number + analog1OutputTopic;
    mqtt_pubtopic_analog1 = mqtt_pubtopic_analog_str1.c_str();

    mqtt_pubtopic_analog_str2 = String(machine_location) + asset_number + analog2OutputTopic;
    mqtt_pubtopic_analog2 = mqtt_pubtopic_analog_str2.c_str();

    mqtt_pubtopic_state_str = String(machine_location) + asset_number + currentStateTopic;
    mqtt_pubtopic_state = mqtt_pubtopic_state_str.c_str();

    // Print the extracted values to the serial monitor using the debugPrint function
    debugPrint("Loaded variables:");
    debugPrint("machine_location: " + String(machine_location));
    debugPrint("asset_number: " + String(asset_number));
    debugPrint("mqtt_server: " + String(mqtt_server));
    debugPrint("mqtt_user: " + String(mqtt_user));
    debugPrint("mqtt_password: " + String(mqtt_password));
    debugPrint("mqtt_client: " + String(mqtt_client));
    debugPrint("StackLightInputTopic: " + String(StackLightInputTopic));
    debugPrint("stateInputTopic: " + String(stateInputTopic));
    debugPrint("analog1OutputTopic: " + String(analog1OutputTopic));
    debugPrint("analog2OutputTopic: " + String(analog2OutputTopic));
    debugPrint("currentStateTopic: " + String(currentStateTopic));
    debugPrint("fastOnTime: " + String(fastOnTime));
    debugPrint("fastOffTime: " + String(fastOffTime));
    debugPrint("slowOnTime: " + String(slowOnTime));
    debugPrint("slowOffTime: " + String(slowOffTime));
    } else {
    debugPrint("error opening settings.ini");
    }

  // Setup the pins
  pinMode(ANALOG_PIN_1, INPUT);
  pinMode(ANALOG_PIN_2, INPUT);
  pinMode(INPUT_PIN_1, INPUT);
  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);
  pinMode(BLUE, OUTPUT);
  pinMode(ALARM, OUTPUT);

  //Attach an interrupt to the input pin
  attachInterrupt(digitalPinToInterrupt(INPUT_PIN_1), localInput, RISING);

  // Initialize the Ethernet connection
  Ethernet.begin(mac, ip);
  delay(1000); // Let the Ethernet module initialize
  
  client.setServer(server, 1883);
  client.setCallback(callback);

  if (!client.connect(mqtt_client, mqtt_user, mqtt_password)) {
      debugPrint("Failed to connect to MQTT broker");
  }

  subscribeToTopics();
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    if (millis() - lastPublishTime >= 500) {
        publishAnalogValue();
        updateStackLight();
        lastPublishTime = millis();
    }

}

