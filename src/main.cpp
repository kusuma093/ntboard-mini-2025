#define MQTT_MAX_PACKET_SIZE 512

#include <Arduino.h>

#include <Ethernet.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>
#define mDNSUpdate() \
  do                 \
  {                  \
    (void)0;         \
  } while (0)

#include <AutoConnect.h>
#include <ArduinoJson.h>
#include <Artron_RTC.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ezLED.h>

#include "FileSystemOperations.h"

#define RESET_PIN (0)

#define JUMPER_PIN (5)

#define NETLED_PIN (32)

#define W5500_RST_PIN (25)
#define W5500_CS_PIN (26)

#define RS485_RX1 (35)
#define RS485_TX1 (15)
#define RS485_DIR1 (14)

#define RS485_RX2 (16)
#define RS485_TX2 (17)
#define RS485_DIR2 (27)

#define MODE_SEND HIGH
#define MODE_RECV LOW

#define TOTAL_REG 10
#define REG_SUBSET_SIZE 5 // Number of registers to read each round

#define REG_ERG 2699
#define REG_CUR 3009
#define REG_VLN 3035
#define REG_POW 3059
#define REG_CUW 3017
#define REG_PFT 3083
#define REG_VLLL 3025
#define REG_CURA 2999
#define REG_CURB 3001
#define REG_CURC 3003

// #define REG_FQ 3109--

// #define REG_AEDA 2811
// #define REG_AEDB 2813
// #define REG_AEDC 2815
// #define REG_VLAB 3019

// #define REG_VLBC 3021
// #define REG_VLCA 3023

// #define REG_VLAN 3027
// #define REG_VLBN 3029
// #define REG_VLCN 3031

// #define REG_CURN 3005
// #define REG_CURG 3007
// #define REG_APA 3053
// #define REG_APB 3055
// #define REG_APC 3057

EthernetUDP ethUdp;
WiFiUDP wifiUdp;
NTPClient timeClient(wifiUdp, "0.asia.pool.ntp.org", 25200, 3600);

EthernetClient ethClient;
WiFiClient wifiClient;
PubSubClient mqttClient;
AutoConnect Portal;
AutoConnectConfig Config;

Preferences myPreferences;
Artron_RTC rtc(&Wire);

ModbusMaster node1;
ModbusMaster node2;

ezLED netLed(NETLED_PIN);

bool resetRequired = false;
unsigned long lastResetCheckTime = 0;
const unsigned long resetCheckInterval = 1000; // Check reset button every 1 second

const unsigned long sensorInterval = 300 * 1000; // Check every 5 minutes
const int XYMD02_buadRate = 9600;
const int PM2230_buadrate = 9600;
float dataMeter[TOTAL_REG];
uint16_t regAddr[TOTAL_REG] = {REG_ERG, REG_CUR, REG_VLN, REG_POW, REG_VLLL, REG_CURA,
                               REG_CURB, REG_CURC, REG_PFT, REG_CUW};

const char *logName = "/log.txt";
String dataMessage;

bool isEthernetConnected = false;
unsigned long lastConnectionCheckTime = 0;
const unsigned long connectionCheckInterval = 15 * 1000; // Check every 15 seconds
const int maxUnknownOrOffCount = 5;                      // Maximum number of consecutive unknown status before restart
int unknownOrOffCount = 0;                               // Counter for consecutive unknown status
const int maxRetries = 5;
const int retryDelay = 15 * 1000;
const char *testHost = "www.google.com";

const char *mqtt_broker = "broker.ntplc.co.th";
const char *mqtt_username = "admin";
const char *mqtt_password = "nt@sandb0x";
const int mqtt_port = 21883;
String pub_topic;
String sub_topic;
String clientId;
String mdnsName;

uint8_t baseMac[6];
char macAddress[18];
char mdnsMac[13];

char currentDateTime[18];
int ntpMday, ntpMonth, ntpYear, ntpHour, ntpMin, ntpSec;
RTC_DATA_ATTR int readingID = 0;
String dayStamp;
String timeStamp;

void readEfuseMac()
{
  uint64_t chipid = ESP.getEfuseMac();

  for (int i = 0; i < 6; i++)
  {
    baseMac[i] = (chipid >> (8 * (5 - i))) & 0xFF;
  }

  snprintf(macAddress, sizeof(macAddress), 
            "%02X:%02X:%02X:%02X:%02X:%02X", 
            baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  
  snprintf(mdnsMac, sizeof(mdnsMac), 
            "%02X%02X%02X%02X%02X%02X", 
            baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
}

void getTimeStamp()
{
  while (!timeClient.update())
  {
    timeClient.forceUpdate();
  }
  String formattedDate = timeClient.getFormattedDate();
  int firstDash = formattedDate.indexOf("-");
  ntpYear = formattedDate.substring(0, firstDash).toInt();
  Serial.println(ntpYear);

  int secondDash = formattedDate.indexOf("-", firstDash + 1);
  ntpMday = formattedDate.substring(secondDash + 1).toInt();
  Serial.println(ntpMday);

  ntpMonth = formattedDate.substring(firstDash + 1, secondDash).toInt();
  Serial.println(ntpMonth);

  // Extract date
  int splitT = formattedDate.indexOf("T");
  dayStamp = formattedDate.substring(0, splitT);
  Serial.println(dayStamp);
  // Extract time
  timeStamp = formattedDate.substring(splitT + 1, formattedDate.length() - 1);
  Serial.println(timeStamp);
}

void setupRTC()
{
  while (!rtc.begin())
  {
    Serial.println("Init RTC fail !!!");
    delay(500);
  }

  struct tm timeinfo_write = {
      .tm_sec = timeClient.getSeconds(),
      .tm_min = timeClient.getMinutes(),
      .tm_hour = timeClient.getHours(),
      .tm_mday = ntpMday,
      .tm_mon = ntpMonth,
      .tm_year = ntpYear,
  };

  while (!rtc.write(&timeinfo_write))
  {
    Serial.println("RTC write fail !!!");
    delay(500);
  }
  Serial.println("RTC writed.");
}

void readRTC()
{
  struct tm timeinfo_read = {0};
  if (!rtc.read(&timeinfo_read))
  {
    Serial.println("RTC read fail !!!");
    delay(500);
  }
  Serial.printf("[RTC] %d/%d/%d %02d:%02d:%02d\n", timeinfo_read.tm_mday, timeinfo_read.tm_mon, timeinfo_read.tm_year + 1900, timeinfo_read.tm_hour, timeinfo_read.tm_min, timeinfo_read.tm_sec);
  int rtcMday = timeinfo_read.tm_mday;
  int rtcMonth = timeinfo_read.tm_mon;
  int rtcYear = timeinfo_read.tm_year + 1900;
  int rtcHour = timeinfo_read.tm_hour;
  int rtcMin = timeinfo_read.tm_min;
  int rtcSec = timeinfo_read.tm_sec;
  snprintf_P(currentDateTime, sizeof(currentDateTime), PSTR("%d-%d-%d %02d:%02d:%02d"), rtcYear, rtcMonth, rtcMday, rtcHour, rtcMin, rtcSec);
}

void resetWireless()
{
  pinMode(RESET_PIN, INPUT_PULLUP);
  int resetState = digitalRead(RESET_PIN);
  Serial.print("Reset State: ");
  Serial.println(resetState);
  if (resetState == LOW)
  {
    resetRequired = true;
  }
}

void performReset()
{
  if (resetRequired)
  {
    Serial.println("Resetting WiFi...");
    dataMessage = currentDateTime;
    dataMessage += ", Resetting WiFi.\r\n";
    appendFile(SD, logName, dataMessage.c_str());
    WiFi.disconnect(true);
    ESP.restart();
  }
}

bool initEthernet()
{
  for (int i = 0; i < maxRetries; i++)
  {
    Serial.println("Attempting to connect to Ethernet...");

    if (Ethernet.begin(baseMac) == 0)
    {
      Serial.println("Failed to configure Ethernet using DHCP");
      continue; // Try again
    }

    delay(1000); // Give the Ethernet shield a second to initialize

    if (Ethernet.linkStatus() == LinkON)
    {
      IPAddress ip = Ethernet.localIP();
      Serial.print("IP address assigned: ");
      Serial.println(ip);

      // Test internet connectivity
      EthernetClient client;
      if (client.connect(testHost, 80))
      {
        Serial.println("Internet connection verified!");
        client.stop();
        return true;
      }
      else
      {
        Serial.println("Internet connection test failed. Retrying...");
      }
    }
    else
    {
      Serial.println("Ethernet link is down. Retrying...");
    }

    delay(retryDelay);
  }

  Serial.println("Failed to connect to Ethernet after multiple attempts.");
  return false;
}

bool reconnectToInternet()
{
  for (int i = 0; i < maxRetries; i++)
  {
    readRTC();
    dataMessage = currentDateTime;
    dataMessage += ", Attempting to reconnect to Internet.";
    Serial.println("Attempting to reconnect Ethernet...");

    Ethernet.begin(baseMac); // Attempt to start the Ethernet connection

    if (Ethernet.linkStatus() == LinkON)
    {
      delay(1000); // Wait a bit for DHCP to complete

      IPAddress ip = Ethernet.localIP();
      if (ip != INADDR_NONE)
      {
        Serial.print("IP address assigned: ");
        Serial.println(ip);

        // Test internet connectivity
        if (Ethernet.maintain() == 0)
        {
          EthernetClient client;
          if (client.connect(testHost, 80))
          {
            Serial.println("Internet connection verified!");
            client.stop();
            return true;
          }
          else
          {
            dataMessage += ", Internet connection test failed. Retrying...\r\n";
            Serial.println("Internet connection test failed. Retrying...");
          }
        }
        else
        {
          dataMessage += ", DHCP error. Retrying...\r\n";
          Serial.println("DHCP error. Retrying...");
        }
      }
      else
      {
        dataMessage += ", No IP address assigned. Retrying...\r\n";
        Serial.println("Link is up, but no IP address assigned. Retrying...");
      }
    }
    else
    {
      dataMessage += ", Ethernet link is down. Retrying...\r\n";
      Serial.println("Ethernet link is down. Retrying...");
    }
    appendFile(SD, logName, dataMessage.c_str());
    delay(retryDelay);
  }
  Serial.println("Failed to reconnect Ethernet after multiple attempts.");
  return false;
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  Serial.printf("Message arrived on topic: %s, length: %d\r\n", topic, length);
}

void mqttReconnect()
{
  // Connect to MQTT broker
  while (!mqttClient.connected())
  {
    Serial.print("Attempting MQTT connection...");
    dataMessage = currentDateTime;
    dataMessage += ", Attempting MQTT connection...";
    if (mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password))
    {
      Serial.println("connected");
      dataMessage += ", connected\r\n";
      // Subscribe to topics here
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      dataMessage += ", failed, rc=";
      dataMessage += mqttClient.state();
      dataMessage += "\r\n";
      delay(5000);
    }
    appendFile(SD, logName, dataMessage.c_str());
  }
}

void publishData(const char *portID, int fromSlaveID, const char *sensorType)
{
  readRTC();
  mqttReconnect();
  if (strcmp(sensorType, "PM2230") == 0)
  {
    int batchSize = REG_SUBSET_SIZE;                          // Change this value to the desired batch size
    int numBatches = (TOTAL_REG + batchSize - 1) / batchSize; // Calculate the number of batches

    for (int batch = 0; batch < numBatches; batch++)
    {
      StaticJsonDocument<200> doc;
      int startIdx = batch * batchSize;
      int endIdx = min(startIdx + batchSize, TOTAL_REG);

      for (int i = startIdx; i < endIdx; i++)
      {
        String key = String(regAddr[i] + 1);
        doc[key] = (int)(dataMeter[i] * 100 + 0.5) / 100.0;
        ;
      }

      // Measure the size of the JSON document
      size_t jsonSize = measureJson(doc);
      Serial.print("JSON size: ");
      Serial.println(jsonSize);

      if (jsonSize > MQTT_MAX_PACKET_SIZE)
      {
        Serial.println("Error: JSON size exceeds maximum packet size");
        continue; // Skip this batch if it exceeds the maximum packet size
      }

      std::string str = std::to_string(fromSlaveID);
      const char *slaveId = str.c_str();
      pub_topic = "ntsandbox/main/energy/";
      pub_topic += macAddress;
      pub_topic += portID;
      pub_topic += slaveId;
      pub_topic += "/ems/";
      pub_topic += sensorType;
      pub_topic += "/test";
      Serial.print("Publish topic: ");
      Serial.println(pub_topic);

      size_t topicLength = pub_topic.length();
      // Check if combined length exceeds maximum packet size
      size_t totalLength = topicLength + jsonSize + 2; // +2 for MQTT overhead
      Serial.print("Topic length: ");
      Serial.println(topicLength);
      Serial.print("Total length: ");
      Serial.println(totalLength);

      if (totalLength > MQTT_MAX_PACKET_SIZE)
      {
        Serial.println("Error: Topic and JSON combined size exceeds maximum packet size");
        continue; // Skip this batch if it exceeds the maximum packet size
      }

      char buffer[200];
      size_t temp = serializeJson(doc, buffer);
      Serial.print("Publish message: ");
      Serial.println(buffer);
      if (mqttClient.publish(pub_topic.c_str(), buffer, temp) == true)
      {
        // Serial.println(F("Success sending message."));
        Serial.printf("Success sending message from %s\r\n", pub_topic.c_str());
        dataMessage = currentDateTime;
        dataMessage += ", Success sending message from ";
        dataMessage += pub_topic.c_str();
        dataMessage += "\r\n";
        appendFile(SD, logName, dataMessage.c_str());
      }
      else
      {
        Serial.print("Fail sending message. rc=");
        Serial.println(mqttClient.state());
        dataMessage = currentDateTime;
        dataMessage += ", Fail sending message.\r\n";
        appendFile(SD, logName, dataMessage.c_str());
      }
    }
  }
  else
  {
    StaticJsonDocument<200> doc;
    if (strcmp(sensorType, "XYMD02") == 0)
    {
      doc["temp"] = dataMeter[0];
      doc["humid"] = dataMeter[1];
    }
    else
    {
      doc["val01"] = dataMeter[0];
      doc["val02"] = dataMeter[1];
      doc["val03"] = dataMeter[2];
      doc["val04"] = dataMeter[3];
      doc["val05"] = dataMeter[4];
    }
    std::string str = std::to_string(fromSlaveID);
    const char *slaveId = str.c_str();
    pub_topic = "ntsandbox/main/energy/";
    pub_topic += macAddress;
    pub_topic += portID;
    pub_topic += slaveId;
    pub_topic += "/temphum/";
    pub_topic += sensorType;
    pub_topic += "/test";
    Serial.print("Publish topic: ");
    Serial.println(pub_topic);

    char buffer[200];
    size_t temp = serializeJson(doc, buffer);
    Serial.print("Publish message: ");
    Serial.println(buffer);
    if (mqttClient.publish(pub_topic.c_str(), buffer, temp) == true)
    {
      // Serial.println(F("Success sending message."));
      Serial.printf("Success sending message from %s\r\n", pub_topic.c_str());
      dataMessage = currentDateTime;
      dataMessage += ", Success sending message from ";
      dataMessage += pub_topic.c_str();
      dataMessage += "\r\n";
      appendFile(SD, logName, dataMessage.c_str());
    }
    else
    {
      Serial.print("Fail sending message. rc=");
      Serial.println(mqttClient.state());
      dataMessage = currentDateTime;
      dataMessage += ", Fail sending message.\r\n";
      appendFile(SD, logName, dataMessage.c_str());
    }
  }
}

void loggingData(const char *sensorType)
{
  readRTC();
  dataMessage = currentDateTime;
  dataMessage += ", ";
  dataMessage += sensorType;
  dataMessage += ", ";
  dataMessage += dataMeter[0];
  dataMessage += ", ";
  dataMessage += dataMeter[1];
  dataMessage += ", ";
  dataMessage += dataMeter[2];
  dataMessage += ", ";
  dataMessage += dataMeter[3];
  dataMessage += ", ";
  dataMessage += dataMeter[4];
  dataMessage += ", ";
  dataMessage += dataMeter[5];
  dataMessage += ", ";
  dataMessage += dataMeter[6];
  dataMessage += ", ";
  dataMessage += dataMeter[7];
  dataMessage += ", ";
  dataMessage += dataMeter[8];
  dataMessage += ", ";
  dataMessage += dataMeter[9];
  dataMessage += ", ";
  dataMessage += dataMeter[10];
  dataMessage += ", ";
  dataMessage += dataMeter[11];
  dataMessage += ", ";
  dataMessage += dataMeter[12];
  dataMessage += ", ";
  dataMessage += dataMeter[13];
  dataMessage += ", ";
  dataMessage += dataMeter[14];
  dataMessage += ", ";
  dataMessage += dataMeter[15];
  dataMessage += ", ";
  dataMessage += dataMeter[16];
  dataMessage += ", ";
  dataMessage += dataMeter[17];
  dataMessage += ", ";
  dataMessage += dataMeter[18];
  dataMessage += ", ";
  dataMessage += dataMeter[19];
  dataMessage += ", ";
  dataMessage += dataMeter[20];
  dataMessage += ", ";
  dataMessage += dataMeter[21];
  dataMessage += ", ";
  dataMessage += dataMeter[22];
  dataMessage += "\r\n";
  appendFile(SD, logName, dataMessage.c_str());
}

// RS485 Port One
void preTransmissionOne()
{
  digitalWrite(RS485_DIR1, 1);
}

void postTransmissionOne()
{
  digitalWrite(RS485_DIR1, 0);
}

// RS485 Port Two
void preTransmissionTwo()
{
  digitalWrite(RS485_DIR2, 1);
}

void postTransmissionTwo()
{
  digitalWrite(RS485_DIR2, 0);
}

// Schneider Processes
float HexToFloat(uint32_t x)
{
  return (*(float *)&x);
}

uint32_t FloatToHex(float x)
{
  return (*(uint32_t *)&x);
}

bool readDataFromPortOne(int slaveID)
{
  Serial.println("Reading data from port one for slave ID: " + String(slaveID));
  pinMode(RS485_DIR1, OUTPUT);
  digitalWrite(RS485_DIR1, MODE_RECV);
  delay(500);
  // Initialize virtual serial ports
  Serial1.begin(XYMD02_buadRate, SERIAL_8N1, RS485_RX1, RS485_TX1);
  delay(500);
  // Attempt to initialize Modbus port 1
  node1.begin(slaveID, Serial1);
  node1.preTransmission(preTransmissionOne);
  node1.postTransmission(postTransmissionOne);
  Serial.println("Modbus port 1 (Serial1) initialized");
  delay(500);

  uint8_t result1 = node1.readInputRegisters(0x0001, 0x0002);

  if (result1 == node1.ku8MBSuccess)
  {
    Serial.println("Data read from Modbus port 1 (Serial1)");

    uint16_t data0 = node1.getResponseBuffer(0);
    uint16_t data1 = node1.getResponseBuffer(1);

    // Process and print data
    Serial.print("Data 0: ");
    Serial.println(data0);
    Serial.print("Data 1: ");
    Serial.println(data1);
    dataMeter[0] = data0;
    dataMeter[1] = data1;

    return true;
  }
  else
  {
    Serial.println("Modbus port 1 is unavailable");
    return false;
    // Handle the case when both ports are unavailable
  }
}

float ReadMeterFloat(int slaveID, uint16_t reg, float &result2)
{
  uint8_t j, resultCode;
  uint16_t data[2];
  uint32_t value = 0;

  pinMode(RS485_DIR2, OUTPUT);
  digitalWrite(RS485_DIR2, MODE_RECV);
  delay(500);
  // Initialize virtual serial ports
  Serial2.begin(PM2230_buadrate, SERIAL_8E1, RS485_RX2, RS485_TX2);
  delay(500);
  // Attempt to initialize Modbus port 2
  node2.begin(slaveID, Serial2);
  node2.preTransmission(preTransmissionTwo);
  node2.postTransmission(postTransmissionTwo);
  Serial.println("Modbus port 2 (Serial2) initialized");
  delay(500);

  resultCode = node2.readHoldingRegisters(reg, 2);
  delay(500);

  // Check results and switch between ports based on availability
  if (resultCode == node2.ku8MBSuccess)
  {
    Serial.println("Data read from Modbus port 2 (Serial2)");
    for (j = 0; j < 2; j++)
    {
      data[j] = node2.getResponseBuffer(j);
    }
    value = data[0];
    value = value << 16;
    value = value + data[1];
    if (isnan(HexToFloat(value)))
    {
      result2 = 0.0;
    }
    else
    {
      result2 = HexToFloat(value);
    }
    return true;
    // Process data from Modbus port 2
  }
  else
  {
    Serial.println("Modbus port 2 is unavailable");
    Serial.print("Connect to modbus fail. REG >>> ");
    Serial.println(reg);
    delay(500);
    return false;
    // Handle the case when both ports are unavailable
  }
}

bool readDataFromPortTwo(int slaveID)
{
  Serial.println("Reading data from port two for slave ID: " + String(slaveID));
  bool success = true;
  for (char i = 0; i < TOTAL_REG; i++)
  {
    float data;
    if (!ReadMeterFloat(slaveID, regAddr[i], data))
    {
      success = false;
      break;
    }
    dataMeter[i] = data;
  }
  Serial.printf("[1]: %.2f, [2]: %.2f, [3]: %.2f, [4]: %.2f, [5]: %.2f\n", dataMeter[0], dataMeter[1], dataMeter[2], dataMeter[3], dataMeter[4]);
  return success;
}

void readDataFromSlave()
{
  const int slaveIDs[] = {1, 2, 3, 4, 5};
  const int numSlaves = sizeof(slaveIDs) / sizeof(int);
  static int currentIndex = 0; // Start index for each round

  for (int i = 0; i < numSlaves; i++)
  {
    int currentSlaveID = slaveIDs[i];
    // Read data from port one
    if (readDataFromPortOne(currentSlaveID))
    {
      Serial.println("Successfully read from port one for slave ID: " + String(currentSlaveID));
      loggingData("XYMD02");
      publishData("01", currentSlaveID, "XYMD02");
    }
    else
    {
      Serial.println("Error reading from port one for slave ID: " + String(currentSlaveID));
    }
    // Read data from port two
    if (readDataFromPortTwo(currentSlaveID))
    {
      Serial.println("Successfully read from port two for slave ID: " + String(currentSlaveID));
      loggingData("PM2230");
      publishData("02", currentSlaveID, "PM2230");
    }
    else
    {
      Serial.println("Error reading from port two for slave ID: " + String(currentSlaveID));
    }
  }
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

  Wire.begin(21, 22, 100E3);
  SPI.begin(18, 19, 23, -1);

  readEfuseMac();
  Serial.print("MAC Address: ");
  Serial.println(macAddress);
  Serial.print("MAC Address for MDNS: ");
  Serial.println(mdnsMac);

  // esp_read_mac(baseMac, ESP_MAC_WIFI_SOFTAP);
  // sprintf(macAddress, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  // Serial.print("MAC Address: ");
  // Serial.println(macAddress);
  // sprintf(mdnsMac, "%02X%02X%02X%02X%02X%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  // Serial.print("MAC Address for MDNS: ");
  // Serial.println(mdnsMac);

  clientId = "ntiot65-";
  clientId += macAddress;
  Serial.print("Client Id: ");
  Serial.println(clientId);

  mdnsName = "ntiot65-";
  mdnsName += mdnsMac;
  Serial.print("MDNS Id: ");
  Serial.println(mdnsName);

  initializeSDCard();
  checkExists(SD, logName);
  delay(500);

  pinMode(JUMPER_PIN, INPUT);
  int jumperState = digitalRead(JUMPER_PIN);
  Serial.print("Jumper State: ");
  Serial.println(jumperState);
  if (jumperState == HIGH)
  {
    // Ethernet interface
    Ethernet.init(W5500_CS_PIN);
    if (initEthernet())
    {
      Serial.println("Ethernet initialized successfully!");
      Serial.print("Ethernet connection established., IP address: ");
      Serial.println(Ethernet.localIP());

      timeClient = NTPClient(ethUdp, "0.asia.pool.ntp.org", 25200, 3600);
      timeClient.begin();
      mqttClient.setClient(ethClient);
    }
    else
    {
      Serial.println("Failed to initialize Ethernet. Halting setup.");
      ESP.restart();
    }
  }
  else
  {
    // WiFi interface
    Config.autoReconnect = true;   // Reconnect to known access points.
    Config.reconnectInterval = 10; // Reconnection attempting interval is 5[min].
    Config.apid = "ntiot65";
    Config.psk = "66665555";
    Config.apip = IPAddress(10, 3, 2, 1);
    Config.gateway = IPAddress(10, 3, 2, 1);
    Config.netmask = IPAddress(255, 255, 255, 0);
    Portal.config(Config);
    if (Portal.begin())
    {
      if (MDNS.begin(mdnsName.c_str()))
      {
        MDNS.addService("http", "tcp", 80);
      }
    }
    timeClient = NTPClient(wifiUdp, "0.asia.pool.ntp.org", 25200, 3600);
    timeClient.begin();
    mqttClient.setClient(wifiClient);
  }

  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setServer(mqtt_broker, mqtt_port);

  getTimeStamp();
  delay(500);

  setupRTC();
  delay(500);
}

void loop()
{
  // put your main code here, to run repeatedly:
  int jumperState = digitalRead(JUMPER_PIN);
  if (jumperState == HIGH)
  {
    unsigned long currentTime = millis();
    if (currentTime - lastConnectionCheckTime >= connectionCheckInterval)
    {
      lastConnectionCheckTime = currentTime;
      if (!reconnectToInternet())
      {
        ESP.restart();
      }
    }
  }
  else
  {
    Portal.handleClient();
  }

  // Check MQTT connection
  if (!mqttClient.connected())
  {
    mqttReconnect();
  }

  // Handle MQTT client
  mqttClient.loop();

  // Main Program Loop code goes here:
  unsigned long currentMillis = millis();
  if (currentMillis - lastResetCheckTime >= resetCheckInterval)
  {
    lastResetCheckTime = currentMillis;
    resetWireless();
  }

  performReset(); // Check and perform reset if needed

  readDataFromSlave();
  delay(sensorInterval);
}