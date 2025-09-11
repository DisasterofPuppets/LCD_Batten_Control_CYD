/*


Example update for on serial receive

//--------------------------- YOUR MESSAGE HANDLING HERE ---------------------------
// Parse comma-separated values and call function
if (actualMessage.indexOf(',') > 0) {
  int commaIndex = actualMessage.indexOf(',');
  
  String val1Str = actualMessage.substring(0, commaIndex);
  String val2Str = actualMessage.substring(commaIndex + 1);
  
  // Trim whitespace and convert to integers
  val1Str.trim();
  val2Str.trim();
  
  int var1 = val1Str.toInt();
  int var2 = val2Str.toInt();
  
  // Call your function with parsed values
  myfunction(var1, var2);
  
  Serial.printf("Called myfunction(%d, %d)\n", var1, var2);
}
//--------




    ESP-NOW Combined Master/Slave
    Dynamic role switching based on user input
    
    All devices run the same code. When user enters input via Serial,
    that device broadcasts to all others in the network.
*/

#include "ESP32_NOW.h"
#include "WiFi.h"
#include <esp_mac.h>
#include <vector>

/* Definitions */
#define ESPNOW_WIFI_CHANNEL 6
#define DEVICE_NAME_PREFIX "ESP_ONE"  // Change this for each device

// Heartbeat timing constants
const unsigned long DISCOVERY_PERIOD = 120000;  // 2 minutes
const unsigned long FAST_HEARTBEAT = 3000;      // 3 seconds
const unsigned long SLOW_HEARTBEAT = 45000;     // 45 seconds

/* Broadcast Peer Class */
class ESP_NOW_Broadcast_Peer : public ESP_NOW_Peer {
public:
  ESP_NOW_Broadcast_Peer(uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) 
    : ESP_NOW_Peer(ESP_NOW.BROADCAST_ADDR, channel, iface, lmk) {}
  
  ~ESP_NOW_Broadcast_Peer() { remove(); }
  
  bool begin() {
    if (!add()) {
      log_e("Failed to register broadcast peer");
      return false;
    }
    return true;
  }
  
  bool send_message(const uint8_t *data, size_t len) {
    if (!send(data, len)) {
      log_e("Failed to broadcast message");
      return false;
    }
    return true;
  }
};

/* Peer Class for receiving */
class ESP_NOW_Peer_Class : public ESP_NOW_Peer {
public:
  ESP_NOW_Peer_Class(const uint8_t *mac_addr, uint8_t channel, wifi_interface_t iface, const uint8_t *lmk) 
    : ESP_NOW_Peer(mac_addr, channel, iface, lmk) {}
  
  ~ESP_NOW_Peer_Class() {}
  
  bool add_peer() {
    if (!add()) {
      log_e("Failed to register peer");
      return false;
    }
    return true;
  }
  
  void onReceive(const uint8_t *data, size_t len, bool broadcast);
};

/* Global Variables */
String deviceName;
uint32_t msg_count = 0;
std::vector<ESP_NOW_Peer_Class *> peers;
std::vector<String> discoveredDevices;

// Heartbeat timing
unsigned long lastHeartbeat = 0;
unsigned long startTime = 0;

///REMOVE ME AFTER TESTING -------------------------------------------------
unsigned long sendtest = 0;
///REMOVE ME AFTER TESTING -------------------------------------------------

// Global Objects
ESP_NOW_Broadcast_Peer broadcast_peer(ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);

//--------------------------- YOUR GLOBAL VARIABLES HERE ---------------------------
// Add your custom variables, structs, enums, etc.
// Example: int sensorValue = 0;
// Example: bool ledState = false;
// Example: String lastCommand = "";
//-------------------------------------------------------------------------------

/* Function Definitions */
void displayDeviceList() {
  Serial.println("\n=== Discovered Devices ===");
  Serial.printf("Total devices: %zu\n", discoveredDevices.size());
  for (size_t i = 0; i < discoveredDevices.size(); i++) {
    Serial.printf("  %zu. %s\n", i + 1, discoveredDevices[i].c_str());
  }
  Serial.println("=========================\n");
}

void addDiscoveredDevice(String deviceName) {
  // Check if device already in list
  for (const String& device : discoveredDevices) {
    if (device == deviceName) {
      return; // Already exists
    }
  }
  discoveredDevices.push_back(deviceName);
  Serial.printf("New device discovered: %s\n", deviceName.c_str());
  displayDeviceList();
}

void sendHeartbeat() {
  String heartbeatMsg = deviceName + ":HEARTBEAT";
  Serial.printf("Heartbeat: %s\n", heartbeatMsg.c_str());
  if (!broadcast_peer.send_message((uint8_t *)heartbeatMsg.c_str(), heartbeatMsg.length() + 1)) {
    Serial.println("ERROR: Failed to send heartbeat");
  } else {
    Serial.println("Heartbeat sent successfully");
  }
}

void broadcastMessage(String userMessage) {
  //--------------------------- YOUR MESSAGE PROCESSING HERE ---------------------------
  // Add custom logic before sending (validation, formatting, etc.)
  // Example: if (userMessage.startsWith("LED")) { /* handle LED command */ }
  // Example: userMessage = processCommand(userMessage);
  //-------------------------------------------------------------------------------------
  
  // Format: "DEVICE_NAMEMSG:message"
  String fullMessage = deviceName + "MSG:" + userMessage;
  
  Serial.printf("Broadcasting: %s\n", userMessage.c_str());
  
  if (!broadcast_peer.send_message((uint8_t *)fullMessage.c_str(), fullMessage.length() + 1)) {
    Serial.println("Failed to broadcast message");
  } else {
    msg_count++;
  }
}

/* ESP_NOW_Peer_Class onReceive implementation */
void ESP_NOW_Peer_Class::onReceive(const uint8_t *data, size_t len, bool broadcast) {
  String message = String((char *)data);
  
  // Extract sender info from message
  int separatorIndex = message.indexOf(':');
  if (separatorIndex > 0) {
    String senderName = message.substring(0, separatorIndex);
    String content = message.substring(separatorIndex + 1);
    
    // Filter by prefix and ignore messages from ourselves
    if (senderName.startsWith(DEVICE_NAME_PREFIX) && senderName != deviceName) {
      // Add to discovered devices list
      addDiscoveredDevice(senderName);
      
      if (content == "HEARTBEAT") {
        // Silent heartbeat processing - just confirms peer is alive
        return;
      } else if (content.startsWith("MSG:")) {
        String actualMessage = content.substring(4); // Remove "MSG:" prefix
        Serial.printf("Message from %s: %s\n", senderName.c_str(), actualMessage.c_str());
        
        //--------------------------- YOUR MESSAGE HANDLING HERE ---------------------------
        // Process received messages from other devices
        // Example: if (actualMessage == "LED_ON") { digitalWrite(LED_PIN, HIGH); }
        // Example: if (actualMessage.startsWith("SENSOR:")) { processSensorData(actualMessage); }
        // Example: handleReceivedCommand(senderName, actualMessage);
        //-----------------------------------------------------------------------------------
      }
    }
  }
}

/* Callbacks */
void register_new_peer(const esp_now_recv_info_t *info, const uint8_t *data, int len, void *arg) {
  if (memcmp(info->des_addr, ESP_NOW.BROADCAST_ADDR, 6) == 0) {
    // Check if peer already exists
    for (size_t i = 0; i < peers.size(); i++) {
      if (memcmp(peers[i]->addr(), info->src_addr, 6) == 0) {
        return; // Peer already registered
      }
    }
    
    // Extract device name from first message
    String message = String((char *)data);
    String senderName = "";
    
    if (message.startsWith("HEARTBEAT ")) {
      senderName = message.substring(10); // Remove "HEARTBEAT "
    } else {
      int msgIndex = message.indexOf(" MSG:");
      if (msgIndex > 0) {
        senderName = message.substring(0, msgIndex);
      }
    }
    
    Serial.printf("New peer " MACSTR, MAC2STR(info->src_addr));
    if (senderName.length() > 0) {
      Serial.printf(" (%s)", senderName.c_str());
    }
    Serial.println(" detected");
    
    ESP_NOW_Peer_Class *new_peer = new ESP_NOW_Peer_Class(info->src_addr, ESPNOW_WIFI_CHANNEL, WIFI_IF_STA, nullptr);
    if (!new_peer->add_peer()) {
      Serial.println("Failed to register new peer");
      delete new_peer;
      return;
    }
    peers.push_back(new_peer);
    Serial.printf("Registered peer (total: %zu)\n", peers.size());
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize device name after WiFi starts
  WiFi.mode(WIFI_STA);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) {
    delay(100);
  }
  
  deviceName = DEVICE_NAME_PREFIX + WiFi.macAddress();
  
  Serial.println("ESP-NOW Combined Master/Slave");
  Serial.println("Device: " + deviceName);
  Serial.println("MAC: " + WiFi.macAddress());
  Serial.printf("Channel: %d\n", ESPNOW_WIFI_CHANNEL);
  
  // Initialize ESP-NOW
  if (!ESP_NOW.begin()) {
    Serial.println("Failed to initialize ESP-NOW");
    delay(5000);
    ESP.restart();
  }
  
  // Register broadcast peer for sending
  if (!broadcast_peer.begin()) {
    Serial.println("Failed to initialize broadcast peer");
    delay(5000);
    ESP.restart();
  }
  
  // Register callback for new peers
  ESP_NOW.onNewPeer(register_new_peer, nullptr);
  
  Serial.printf("ESP-NOW version: %d, max data length: %d\n", ESP_NOW.getVersion(), ESP_NOW.getMaxDataLen());
  Serial.println("Setup complete. Enter messages to broadcast:");
  Serial.println("Usage: Type any message and press Enter");
  Serial.println("Commands: 'list' - show discovered devices");
  Serial.println("          'status' - show device status");
  Serial.println("          'restart' - restart device");
  
  // Initialize timing
  startTime = millis();
  lastHeartbeat = millis();

///REMOVE ME AFTER TESTING -------------------------------------------------
  sendtest = millis();
///REMOVE ME AFTER TESTING -------------------------------------------------

  
  // Send initial heartbeat
  sendHeartbeat();
  
  //--------------------------- YOUR SETUP CODE HERE ---------------------------
  // Add your custom initialization (pins, sensors, displays, etc.)
  // Example: pinMode(LED_PIN, OUTPUT);
  // Example: display.begin();
  // Example: sensor.init();
  // Example: initializeCustomVariables();
  //--------------------------------------------------------------------------
}

void loop() {
  // Non-blocking heartbeat
  unsigned long currentTime = millis();
  unsigned long heartbeatInterval = (currentTime - startTime < DISCOVERY_PERIOD) ? FAST_HEARTBEAT : SLOW_HEARTBEAT;
  
  if (currentTime - lastHeartbeat >= heartbeatInterval) {
    lastHeartbeat = currentTime;
    sendHeartbeat();
  }
  
///REMOVE ME AFTER TESTING -------------------------------------------------  
  //simulated variable parse 
  if (currentTime - sendtest >= DISCOVERY_PERIOD) {
    sendtest = currentTime;
    int temperature = 10;
    int humidity = 20;
    int pressure = 30;
    int battery = 40;
    String TestMsg = String(temperature) + "," + String(humidity) + "," + String(pressure) + "," + String(battery);   //use this for the live stuff
    broadcastMessage(TestMsg);
  }
///REMOVE ME AFTER TESTING -------------------------------------------------

  //--------------------------- YOUR PERIODIC TASKS HERE ---------------------------
  // Add your custom periodic tasks (sensor reading, display updates, etc.)
  // Example: if (millis() - lastSensorRead > 1000) { readSensors(); }
  // Example: updateDisplay();
  // Example: checkButtons();
  //--------------------------------------------------------------------------
  
  // Handle Serial input
  if (Serial.available()) {
    String userInput = Serial.readStringUntil('\n');
    userInput.trim();
    
    if (userInput.length() > 0) {
      if (userInput.equalsIgnoreCase("list")) {
        displayDeviceList();
      } else {
        //--------------------------- YOUR INPUT PROCESSING HERE ---------------------------
        // Add custom command processing before broadcasting
        // Example: if (userInput.startsWith("/cmd ")) { handleLocalCommand(userInput); return; }
        // Example: userInput = preprocessInput(userInput);
        // Example: if (validateInput(userInput)) { broadcastMessage(userInput); }
        //-----------------------------------------------------------------------------------
        
        broadcastMessage(userInput);
      }
    }
  }
  
  //--------------------------- YOUR ADDITIONAL INPUT SOURCES HERE ---------------------------
  // Add other input sources (buttons, sensors, timers, etc.)
  // Example: if (digitalRead(BUTTON_PIN) == LOW) { broadcastMessage("BUTTON_PRESSED"); }
  // Example: if (motionDetected()) { broadcastMessage("MOTION:" + String(millis())); }
  // Example: handleWebServerRequests();
  //-------------------------------------------------------------------------------------------
  
  // Debug info every 60 seconds
  static unsigned long lastDebug = 0;
  if (currentTime - lastDebug > 60000) {
    lastDebug = currentTime;
    Serial.printf("\n--- Status Update ---\n");
    Serial.printf("Peers: %zu, Messages sent: %lu\n", peers.size(), msg_count);
    if (discoveredDevices.size() > 0) {
      Serial.printf("Active devices: ");
      for (size_t i = 0; i < discoveredDevices.size(); i++) {
        Serial.printf("%s", discoveredDevices[i].c_str());
        if (i < discoveredDevices.size() - 1) Serial.printf(", ");
      }
      Serial.println();
    }
    Serial.println("--------------------\n");
  }
  
  delay(50);  // Reduced from 100ms for better responsiveness
}

//--------------------------- YOUR CUSTOM FUNCTIONS HERE ---------------------------
// Add your custom functions below
// Example:
// void handleLocalCommand(String cmd) { }
// void readSensors() { }
// void updateDisplay() { }
// bool validateInput(String input) { return true; }
// void processSensorData(String data) { }
//-----------------------------------------------------------------------------------
