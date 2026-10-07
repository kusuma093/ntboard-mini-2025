#define MQTT_MAX_PACKET_SIZE 512

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_idf_version.h>
#include <time.h>

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
#include "OtaClient.h"


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

//#define REG_FQ 3109--

//#define REG_AEDA 2811
//#define REG_AEDB 2813
//#define REG_AEDC 2815
//#define REG_VLAB 3019

//#define REG_VLBC 3021
//#define REG_VLCA 3023

//#define REG_VLAN 3027
//#define REG_VLBN 3029
//#define REG_VLCN 3031

//#define REG_CURN 3005
//#define REG_CURG 3007
//#define REG_APA 3053
//#define REG_APB 3055
//#define REG_APC 3057

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

const unsigned long sensorInterval = 120UL * 1000; // Wait two minutes after each completed scan
const int XYMD02_buadRate = 9600;
const int PM2230_buadrate = 9600;
float dataMeter[TOTAL_REG];
uint16_t regAddr[TOTAL_REG] = {REG_ERG, REG_CUR, REG_VLN, REG_POW, REG_VLLL, REG_CURA,
                                REG_CURB, REG_CURC, REG_PFT, REG_CUW};

const char *logName = "/log.txt";
String dataMessage;

bool isEthernetConnected = false;
bool sensorScanIdle = false;
bool sensorScanHealthy = false;
bool ethernetMode = false;
bool rtcReady = false;
bool ntpReady = false;
unsigned long lastConnectionCheckTime = 0;
const unsigned long connectionCheckInterval = 15000;
unsigned long lastMqttAttempt = 0;
bool mqttAttempted = false;
const unsigned long mqttRetryInterval = 15000;
unsigned long lastTimeSync = 0;
bool timeSyncAttempted = false;
const unsigned long timeSyncInterval = 60000;
const uint32_t watchdogSeconds = 120;
bool mqttPublishFailed = false;
bool communicationFault = false;
unsigned long communicationFaultSince = 0;
unsigned long lastNetworkRecovery = 0;
unsigned long lastHealthLog = 0;
unsigned long lastPublishOk = 0;
uint32_t publishOkCount = 0, publishFailCount = 0;
uint32_t modbusOkCount[2] = {0, 0}, modbusFailCount[2] = {0, 0};
RTC_DATA_ATTR bool communicationRestart = false;
Preferences restartPrefs;
bool restartPrefsReady = false;
uint32_t lastMidnightDate = 0, observedDate = 0;
unsigned long lastMidnightCheck = 0;

void startWatchdog()
{
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t config = {};
  config.timeout_ms = watchdogSeconds * 1000;
  config.idle_core_mask = 0;
  config.trigger_panic = true;
  esp_err_t result = esp_task_wdt_init(&config);
  if (result == ESP_ERR_INVALID_STATE)
    result = esp_task_wdt_reconfigure(&config);
#else
  esp_err_t result = esp_task_wdt_init(watchdogSeconds, true);
#endif
  ESP_ERROR_CHECK(result);
  if (esp_task_wdt_status(NULL) != ESP_OK)
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
}

void markProgress()
{
  ESP_ERROR_CHECK(esp_task_wdt_reset());
}

bool networkReady()
{
  return ethernetMode ? isEthernetConnected : WiFi.status() == WL_CONNECTED;
}

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

char currentDateTime[24] = "TIME_UNAVAILABLE";
int ntpMday, ntpMonth, ntpYear, ntpHour, ntpMin, ntpSec;
RTC_DATA_ATTR int readingID = 0;
String dayStamp;
String timeStamp;

bool getTimeStamp()
{
  if (!networkReady() || !timeClient.forceUpdate())
    return false;
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
  ntpReady = ntpYear >= 2024 && ntpYear <= 2099;
  return ntpReady;
}

void setupRTC()
{
  if (!rtcReady)
    rtcReady = rtc.begin();
  if (!rtcReady || !ntpReady)
    return; // Keep the battery-backed RTC when NTP is unavailable.

  struct tm value = {};
  value.tm_sec = timeClient.getSeconds();
  value.tm_min = timeClient.getMinutes();
  value.tm_hour = timeClient.getHours();
  value.tm_mday = ntpMday;
  value.tm_mon = ntpMonth; // This RTC library uses months 1..12.
  value.tm_year = ntpYear - 1900;
  value.tm_wday = timeClient.getDay();
  if (!rtc.write(&value))
    Serial.println("RTC write failed; retry on next time sync");
}

void readRTC()
{
  struct tm timeinfo_read = {0};
  if (!rtcReady || !rtc.read(&timeinfo_read))
  {
    snprintf(currentDateTime, sizeof(currentDateTime), "TIME_UNAVAILABLE");
    return;
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

void serviceMidnightRestart()
{
  if (millis() - lastMidnightCheck < 1000UL) return;
  lastMidnightCheck = millis();
  struct tm local = {};
  if (ntpReady) {
    // NTPClient already includes UTC+7; gmtime must not add another offset.
    time_t thaiEpoch = timeClient.getEpochTime();
    gmtime_r(&thaiEpoch, &local);
    local.tm_mon += 1; // Match the RTC library's 1..12 convention.
  } else if (!rtcReady || !rtc.read(&local)) return;
  if (local.tm_year < 124 || local.tm_year > 199 || local.tm_mon < 1 ||
      local.tm_mon > 12 || local.tm_mday < 1 || local.tm_mday > 31 ||
      local.tm_hour < 0 || local.tm_hour > 23 || local.tm_min < 0 || local.tm_min > 59) return;
  const uint32_t date = (local.tm_year + 1900) * 10000UL + local.tm_mon * 100UL + local.tm_mday;
  const bool crossedMidnight = observedDate != 0 && date > observedDate;
  observedDate = date;
  // Allow startup at midnight, and a delayed check after a bounded OTA transfer.
  if (!(local.tm_hour == 0 && local.tm_min < 15) && !crossedMidnight) return;
  if (date <= lastMidnightDate) return;
  if (!restartPrefsReady || restartPrefs.putUInt("last-date", date) != sizeof(uint32_t)) {
    Serial.println("[MIDNIGHT] Cannot save restart date; skipping to avoid a reboot loop");
    return;
  }
  lastMidnightDate = date;
  char message[96];
  snprintf(message, sizeof(message), "[MIDNIGHT] date=%lu Thai time; scheduled restart\r\n", (unsigned long)date);
  Serial.print(message);
  appendFile(SD, logName, message);
  ESP.restart();
}

void resetWireless()
{
  pinMode(RESET_PIN, INPUT_PULLUP);
  int resetState = digitalRead(RESET_PIN);
  static int previousResetState = -1;
  if (resetState != previousResetState)
  {
    Serial.print("Reset State: ");
    Serial.println(resetState);
    previousResetState = resetState;
  }
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
  // One bounded DHCP attempt. No reboot merely because the network is down.
  isEthernetConnected = Ethernet.begin(baseMac, 5000, 1000) != 0;
  Serial.println(isEthernetConnected ? "Ethernet ready" : "Ethernet unavailable; will retry");
  return isEthernetConnected;
}

void serviceNetwork()
{
  if (!ethernetMode)
  {
    Portal.handleClient();
    return;
  }
  if (millis() - lastConnectionCheckTime < connectionCheckInterval)
    return;
  lastConnectionCheckTime = millis();
  if (Ethernet.linkStatus() == LinkOFF)
  {
    isEthernetConnected = false;
    mqttClient.disconnect();
    return;
  }
  if (!isEthernetConnected)
  {
    // Reinitializing W5500 invalidates existing sockets.
    mqttClient.disconnect();
    ethUdp.stop();
    otaResetNetwork();
    initEthernet();
    if (isEthernetConnected)
      timeClient.begin();
    return;
  }
  const int leaseStatus = Ethernet.maintain();
  if (leaseStatus == 1 || leaseStatus == 3)
  {
    isEthernetConnected = false;
    mqttClient.disconnect();
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  if (strcmp(topic, otaNotifyTopic()) == 0) otaNotify();
  Serial.printf("Message arrived on topic: %s, length: %d\r\n", topic, length);
}

void mqttReconnect()
{
  if (mqttClient.connected() || !networkReady())
    return;
  if (mqttAttempted && millis() - lastMqttAttempt < mqttRetryInterval)
    return;
  mqttAttempted = true;
  Serial.println("Attempting MQTT connection...");
  bool connected = mqttClient.connect(clientId.c_str(), mqtt_username, mqtt_password);
  if (connected) {
    mqttClient.subscribe(otaNotifyTopic(), 1);
    otaNotify(); // Check jobs missed while disconnected, after the sensor scan.
  }
  lastMqttAttempt = millis();
  Serial.printf("MQTT %s, rc=%d\n", connected ? "connected" : "unavailable", mqttClient.state());
}

void recordPublish(bool ok)
{
  if (ok) {
    ++publishOkCount;
    lastPublishOk = millis();
    mqttPublishFailed = false;
  } else {
    ++publishFailCount;
    mqttPublishFailed = true;
  }
}

void serviceCommunicationHealth()
{
  const unsigned long now = millis();
  const bool connected = mqttClient.connected();
  const bool fault = !networkReady() || !connected || mqttPublishFailed;
  if (!fault) communicationFault = false;
  else if (!communicationFault) {
    communicationFault = true;
    communicationFaultSince = lastNetworkRecovery = now;
  }
  if (now - lastHealthLog >= 300000UL) {
    lastHealthLog = now;
    char status[300];
    snprintf(status, sizeof(status),
      "[HEALTH] uptime_s=%lu heap=%u network=%d mqtt=%d rc=%d publish_ok=%lu publish_fail=%lu last_publish_age_s=%ld modbus1_ok=%lu fail=%lu modbus2_ok=%lu fail=%lu\r\n",
      now / 1000UL, ESP.getFreeHeap(), networkReady(), connected, mqttClient.state(),
      (unsigned long)publishOkCount, (unsigned long)publishFailCount,
      publishOkCount ? (long)((now - lastPublishOk) / 1000UL) : -1L,
      (unsigned long)modbusOkCount[0], (unsigned long)modbusFailCount[0],
      (unsigned long)modbusOkCount[1], (unsigned long)modbusFailCount[1]);
    Serial.print(status);
    appendFile(SD, logName, status);
  }
  if (!communicationFault) return;
  // Called outside OTA transfers. Use elapsed unsigned times, including millis wrap.
  if (now - communicationFaultSince >= 1800000UL) {
    communicationRestart = true;
    Serial.println("[RECOVERY] Communication unavailable for 30 minutes; restarting");
    appendFile(SD, logName, "[RECOVERY] Communication unavailable for 30 minutes; restarting\r\n");
    ESP.restart();
    return;
  }
  if (now - lastNetworkRecovery < 180000UL) return;
  Serial.println("[RECOVERY] Reconnecting network after communication failure");
  appendFile(SD, logName, "[RECOVERY] Reconnecting network after communication failure\r\n");
  otaResetNetwork();
  mqttClient.disconnect();
  if (ethernetMode) {
    ethClient.stop();
    ethUdp.stop();
    initEthernet();
    if (isEthernetConnected) timeClient.begin();
  } else {
    wifiClient.stop();
    WiFi.reconnect();
  }
  mqttAttempted = false;
  lastNetworkRecovery = millis();
}

void publishData(const char *portID, int fromSlaveID, const char *sensorType)
{
  readRTC();
  if (!mqttClient.connected())
  {
    recordPublish(false);
    Serial.println(isSDCardReady()
      ? "MQTT offline; SD logging enabled, no replay queue"
      : "MQTT offline; no SD logging, no replay queue");
    return;
  }
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
      pub_topic += "/ems";
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
        recordPublish(true);
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
        recordPublish(false);
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
    pub_topic += "/ems/";
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
      recordPublish(true);
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
      recordPublish(false);
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
  const int count = strcmp(sensorType, "XYMD02") == 0 ? 2 : TOTAL_REG;
  for (int i = 0; i < count; ++i)
  {
    dataMessage += ", ";
    dataMessage += dataMeter[i];
  }
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

// Serial diagnostics only: never add these fields to sensor payloads.
void logModbusError(int port, int slaveID, uint8_t function, uint16_t reg, uint8_t code)
{
  if (port >= 1 && port <= 2) ++modbusFailCount[port - 1];
  const char *reason;
  switch (code)
  {
    case ModbusMaster::ku8MBIllegalFunction: reason = "Illegal function"; break;
    case ModbusMaster::ku8MBIllegalDataAddress: reason = "Illegal register address"; break;
    case ModbusMaster::ku8MBIllegalDataValue: reason = "Illegal data value"; break;
    case ModbusMaster::ku8MBSlaveDeviceFailure: reason = "Slave device failure"; break;
    case ModbusMaster::ku8MBInvalidSlaveID: reason = "Unexpected slave ID in response"; break;
    case ModbusMaster::ku8MBInvalidFunction: reason = "Unexpected function in response"; break;
    case ModbusMaster::ku8MBResponseTimedOut: reason = "Response timeout"; break;
    case ModbusMaster::ku8MBInvalidCRC: reason = "Response CRC mismatch"; break;
    default: reason = "Other Modbus error"; break;
  }
  Serial.printf("[Modbus ERROR] port=%d slave=%d function=0x%02X reg=%u count=2 code=0x%02X (%s)\r\n",
                port, slaveID, (unsigned int)function, (unsigned int)reg, (unsigned int)code, reason);
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
  if (result1 == node1.ku8MBResponseTimedOut || result1 == node1.ku8MBInvalidCRC) {
    logModbusError(1, slaveID, 0x04, 0x0001, result1);
    Serial.printf("[XYMD02] Reinitializing Serial1 and retrying slave=%d once\r\n", slaveID);
    Serial1.end();
    digitalWrite(RS485_DIR1, MODE_RECV);
    delay(100);
    Serial1.begin(XYMD02_buadRate, SERIAL_8N1, RS485_RX1, RS485_TX1);
    node1.begin(slaveID, Serial1);
    delay(100);
    result1 = node1.readInputRegisters(0x0001, 0x0002);
  }

  if (result1 == node1.ku8MBSuccess)
  {
    ++modbusOkCount[0];
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
    logModbusError(1, slaveID, 0x04, 0x0001, result1);
    Serial.printf("[XYMD02] No valid reading: slave=%d, 9600 8N1, RX35 TX15 DIR14; check sensor power, A/B and address. No temperature published.\r\n", slaveID);
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
    ++modbusOkCount[1];
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
    logModbusError(2, slaveID, 0x03, reg, resultCode);
    delay(500);
    return false;
    // Handle the case when both ports are unavailable
  }
}

struct SlavePresence {
  bool active = false;
  uint8_t failedRounds = 0;
};
SlavePresence slavePresence[2][5];
const unsigned long slaveDiscoveryInterval = 10UL * 60UL * 1000UL;

void recordSlaveRead(int port, int slaveID, bool ok)
{
  SlavePresence &entry = slavePresence[port - 1][slaveID - 1];
  if (ok) {
    if (!entry.active)
      Serial.printf("[DISCOVERY] port=%d slave=%d found; enabled for regular scans\r\n", port, slaveID);
    entry.active = true;
    entry.failedRounds = 0;
  } else if (entry.active) {
    if (++entry.failedRounds >= 3) {
      entry.active = false;
      entry.failedRounds = 0;
      Serial.printf("[DISCOVERY] port=%d slave=%d failed 3 rounds; waiting for rediscovery\r\n", port, slaveID);
    }
  }
}

// Read one device step per loop, keeping MQTT and reset checks alive.
void serviceSensors()
{
  static bool scanning = true;
  static bool discoveryRound = true;
  static bool discoveryStarted = false;
  static unsigned long discoveryAt = 0;
  static unsigned long completedAt = 0;
  static int slaveID = 1;
  static int registerIndex = -1; // -1: temperature, 0..9: meter registers
  if (!discoveryStarted) {
    discoveryStarted = true;
    discoveryAt = millis();
    Serial.println("[DISCOVERY] Initial scan: both ports, slave IDs 1..5");
  }
  if (!scanning)
  {
    const bool discoveryDue = millis() - discoveryAt >= slaveDiscoveryInterval;
    if (millis() - completedAt < sensorInterval && !discoveryDue)
      return;
    discoveryRound = discoveryDue;
    if (discoveryRound) {
      discoveryAt = millis();
      Serial.println("[DISCOVERY] Searching both ports for missing slave IDs 1..5");
    }
    scanning = true;
    sensorScanIdle = false;
  }
  if (registerIndex == -1)
  {
    if (discoveryRound || slavePresence[0][slaveID - 1].active)
    {
      const bool ok = readDataFromPortOne(slaveID);
      recordSlaveRead(1, slaveID, ok);
      if (ok) {
        loggingData("XYMD02");
        publishData("01", slaveID, "XYMD02");
      }
    }
    registerIndex = 0;
    return;
  }
  if (discoveryRound || slavePresence[1][slaveID - 1].active) {
    float value;
    if (ReadMeterFloat(slaveID, regAddr[registerIndex], value)) {
      dataMeter[registerIndex++] = value;
      if (registerIndex < TOTAL_REG) return;
      recordSlaveRead(2, slaveID, true);
      loggingData("PM2230");
      publishData("02", slaveID, "PM2230");
    } else {
      // Count a failed meter round once, even when a later register fails.
      recordSlaveRead(2, slaveID, false);
    }
  }
  // On a failed register, skip this meter exactly as before; never publish partial data.
  registerIndex = -1;
  if (++slaveID > 5)
  {
    slaveID = 1;
    scanning = false;
    sensorScanIdle = true;
    sensorScanHealthy = true;
    completedAt = millis();
  }
}

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.printf("Boot reset reason: %d\n", (int)esp_reset_reason());
  startWatchdog();
  restartPrefsReady = restartPrefs.begin("daily-restart", false);
  if (restartPrefsReady) lastMidnightDate = restartPrefs.getUInt("last-date", 0);

  Wire.begin(21, 22, 100E3);
  Wire.setTimeOut(50);
  SPI.begin(18, 19, 23, -1);

  esp_read_mac(baseMac, ESP_MAC_WIFI_SOFTAP);
  sprintf(macAddress, "%02X:%02X:%02X:%02X:%02X:%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  Serial.print("MAC Address: ");
  Serial.println(macAddress);
  otaConfigureBroker(mqtt_broker, mqtt_port, mqtt_username, mqtt_password);
  otaBegin(macAddress);
  sprintf(mdnsMac, "%02X%02X%02X%02X%02X%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  Serial.print("MAC Address for MDNS: ");
  Serial.println(mdnsMac);

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
  String bootLog = "Boot reset reason: " + String((int)esp_reset_reason()) + "\r\n";
  appendFile(SD, logName, bootLog.c_str());
  if (communicationRestart && esp_reset_reason() == ESP_RST_SW) {
    Serial.println("[RECOVERY] Previous restart was caused by communication timeout");
    appendFile(SD, logName, "[RECOVERY] Previous restart was caused by communication timeout\r\n");
  }
  communicationRestart = false;
  markProgress();
  delay(500);

  pinMode(JUMPER_PIN, INPUT);
  int jumperState = digitalRead(JUMPER_PIN);
  Serial.print("Jumper State: ");
  Serial.println(jumperState);
  ethernetMode = jumperState == HIGH;
  if (ethernetMode)
  {
    // Ethernet interface
    Ethernet.init(W5500_CS_PIN);
    ethClient.setConnectionTimeout(3000);
    initEthernet();
    timeClient = NTPClient(ethUdp, "0.asia.pool.ntp.org", 25200, 3600000);
    timeClient.begin();
    mqttClient.setClient(ethClient);
  }
  else
  {
    // WiFi interface
    Config.beginTimeout = 10000;
    Config.portalTimeout = 30000;
    Config.retainPortal = true;
    Config.autoReconnect = true;   // Reconnect to known access points.
    Config.reconnectInterval = 10; // Reconnection attempting interval is 5[min].
    Config.apid = "ntiot65";
    Config.psk = "66665555";
    Config.apip = IPAddress(10, 3, 2, 1);
    Config.gateway = IPAddress(10, 3, 2, 1);
    Config.netmask = IPAddress(255, 255, 255, 0);
    Portal.config(Config);
    // AutoConnect's idle timeout can be extended by an attached phone.
    // Enforce an absolute startup limit; retainPortal keeps configuration available in loop().
    const unsigned long portalStarted = millis();
    Portal.whileCaptivePortal([portalStarted]() {
      return millis() - portalStarted < 30000;
    });
    if (Portal.begin())
    {
      if (MDNS.begin(mdnsName.c_str()))
      {
        MDNS.addService("http", "tcp", 80);
      }
    }
    Portal.whileCaptivePortal(nullptr);
    wifiClient.setTimeout(3); // Arduino-ESP32 WiFiClient uses seconds here.
    timeClient = NTPClient(wifiUdp, "0.asia.pool.ntp.org", 25200, 3600);
    timeClient.begin();
    mqttClient.setClient(wifiClient);
  }

  markProgress();
  mqttClient.setSocketTimeout(3);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setServer(mqtt_broker, mqtt_port);

  rtcReady = rtc.begin();
  if (getTimeStamp())
    setupRTC();
  timeSyncAttempted = true;
  lastTimeSync = millis();
  markProgress();
}

void loop()
{
  serviceNetwork();
  markProgress();
  mqttReconnect();
  mqttClient.loop();
  markProgress();
  serviceCommunicationHealth();
  markProgress();

  if (millis() - lastResetCheckTime >= resetCheckInterval)
  {
    lastResetCheckTime = millis();
    resetWireless();
  }
  performReset();
  serviceMidnightRestart();

  if (!timeSyncAttempted || millis() - lastTimeSync >= timeSyncInterval)
  {
    timeSyncAttempted = true;
    if (!rtcReady)
      rtcReady = rtc.begin();
    if (getTimeStamp())
      setupRTC();
    lastTimeSync = millis();
  }
  markProgress();
  serviceSensors();
  otaService(ethernetMode, networkReady(), sensorScanIdle, sensorScanHealthy, markProgress);
  // Feed only after the bounded sensor step has returned, never inside a retry loop.
  markProgress();
  delay(1);
}
