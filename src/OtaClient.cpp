#include "OtaClient.h"
#include "OtaVersion.h"
#include "OtaEnrollment.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Ethernet.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Update.h>
#include <PubSubClient.h>
#include <esp_ota_ops.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <bootloader_random.h>
#if __has_include("OtaConfig.local.h")
#include "OtaConfig.local.h"
#endif
#ifndef OTA_PROVISIONING
#define OTA_PROVISIONING 0
#endif

namespace {
const char metadata[] = "NTOTA1|ntboard-mini-2025|" OTA_VERSION "|";
const char model[] = "ntboard-mini-2025";
Preferences prefs;
bool ready = false;
bool enrolling = false;
mbedtls_ctr_drbg_context enrollmentRng;
String deviceId, host, key, base;
uint16_t port;
unsigned long lastPoll = 0;
bool polled = false;
bool checkRequested = true;
String notifyTopic;
String pendingJob, targetVersion, pendingResult, pendingMessage;
bool awaitingHealth = false;
EthernetClient lan;
WiFiClient wifi;
PubSubClient otaMqtt;
String brokerHost, brokerUser, brokerPassword;
uint16_t brokerPort;
String rpcReply;
bool rpcReceived = false;
const unsigned long idleTimeout = 15000;
const unsigned long transferTimeout = 600000;

String hex(const uint8_t *bytes, size_t count) {
  String value; value.reserve(count * 2);
  const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < count; ++i) { value += digits[bytes[i] >> 4]; value += digits[bytes[i] & 15]; }
  return value;
}
String sign(const String &message) {
  uint8_t digest[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
      (const uint8_t *)key.c_str(), key.length(), (const uint8_t *)message.c_str(), message.length(), digest) != 0) return "";
  return hex(digest, sizeof(digest));
}
bool validHex(const String &s) {
  if (s.length() != 64) return false;
  for (unsigned int i = 0; i < s.length(); ++i) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}
bool equalSignature(const String &a, const String &b) {
  if (a.length() != 64 || b.length() != 64) return false;
  uint8_t difference = 0;
  for (int i = 0; i < 64; ++i) difference |= a[i] ^ b[i];
  return difference == 0;
}
bool savePending() {
  DynamicJsonDocument doc(512);
  doc["job"] = pendingJob; doc["version"] = targetVersion;
  doc["result"] = pendingResult; doc["message"] = pendingMessage;
  String data; serializeJson(doc, data);
  return prefs.putString("pending", data) == data.length();
}
struct Response { int length = -1; String signature; };
bool mqttRequest(Client &client, const String &action, const String &body, Response &response) {
  if (brokerHost.isEmpty() || body.length() > 2048) return false;
  otaMqtt.setClient(client);
  otaMqtt.setServer(brokerHost.c_str(), brokerPort);
  otaMqtt.setSocketTimeout(3);
  otaMqtt.setKeepAlive(30);
  if (!otaMqtt.setBufferSize(4096)) return false;
  otaMqtt.setCallback([](char *topic, byte *payload, unsigned int length) {
    if (length < 66 || length > 3072 || String(topic) != "ems/" + deviceId + "/ota/rpc/response") return;
    rpcReply = ""; rpcReply.reserve(length);
    for (unsigned int i = 0; i < length; ++i) rpcReply += (char)payload[i];
    rpcReceived = true;
  });
  if (!otaMqtt.connected()) {
    String id = "ota-" + deviceId; id.replace(":", "");
    if (!otaMqtt.connect(id.c_str(), brokerUser.c_str(), brokerPassword.c_str())) return false;
    if (!otaMqtt.subscribe(("ems/" + deviceId + "/ota/rpc/response").c_str(), 1)) { client.stop(); return false; }
  }
  DynamicJsonDocument envelope(3072);
  envelope["action"] = action; envelope["body"] = body;
  String domain = action == "enroll" ? "enroll" : action == "poll" ? "poll" : "chunk";
  envelope["signature"] = sign(domain + "\n" + body);
  String packet; serializeJson(envelope, packet);
  // QoS0 requests have explicit retries. Requests are never retained.
  for (int attempt = 0; attempt < 3; ++attempt) {
    rpcReceived = false; rpcReply = "";
    if (!otaMqtt.publish(("ems/" + deviceId + "/ota/rpc/request").c_str(), packet.c_str(), false)) return false;
    unsigned long started = millis();
    while (otaMqtt.connected() && millis() - started < idleTimeout) {
      otaMqtt.loop();
      if (rpcReceived) {
        if (rpcReply[64] != '\n') { rpcReceived = false; continue; }
        String signature = rpcReply.substring(0, 64);
        String data = rpcReply.substring(65);
        String replyDomain = action == "enroll" ? "enrollment" : action == "poll" ? "response" : "chunk-response";
        if (!equalSignature(sign(replyDomain + "\n" + data), signature)) { rpcReceived = false; continue; }
        // Ignore delayed replies from older requests rather than accepting stale state.
        DynamicJsonDocument sent(2048), answer(2048);
        if (deserializeJson(sent, body) || deserializeJson(answer, data)) return false;
        bool matches = action == "poll" ? answer["counter"].as<uint64_t>() == sent["counter"].as<uint64_t>() :
          action == "enroll" ? answer["nonce"].as<String>() == sent["nonce"].as<String>() :
          answer["jobId"].as<String>() == sent["jobId"].as<String>() && answer["offset"].as<int>() == sent["offset"].as<int>() && answer["sha256"].as<String>() == sent["sha256"].as<String>();
        if (!matches) { rpcReceived = false; continue; }
        response.signature = signature; rpcReply = data; response.length = data.length(); return true;
      }
      delay(1);
    }
    if (!otaMqtt.connected()) return false;
  }
  return false;
}
bool request(Client &client, const char *, const String &, const String &body, Response &response, bool enrollment = false) {
  return mqttRequest(client, enrollment ? "enroll" : "poll", body, response);
}
bool readBody(Client &, int length, String &body) {
  if (length < 0 || length > 2048 || rpcReply.length() != (unsigned int)length) return false;
  body = rpcReply; return true;
}
int enrollmentEntropy(void *, unsigned char *out, size_t length) { esp_fill_random(out, length); return 0; }
int enrollmentRandom(void *, unsigned char *out, size_t length) { return mbedtls_ctr_drbg_random(&enrollmentRng, out, length); }
void enroll(Client &client) {
  uint8_t random[16]; if (enrollmentRandom(nullptr, random, sizeof(random)) != 0) return;
  String nonce = hex(random, sizeof(random));
  String secret = deviceId + "|" + key;
  mbedtls_pk_context pk; mbedtls_pk_init(&pk);
  int result = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)otaEnrollmentPublicKey, sizeof(otaEnrollmentPublicKey));
  unsigned char encrypted[256]; size_t encryptedLength = 0;
  if (result == 0) {
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
    result = mbedtls_pk_encrypt(&pk, (const unsigned char *)secret.c_str(), secret.length(), encrypted,
      &encryptedLength, sizeof(encrypted), enrollmentRandom, nullptr);
  }
  mbedtls_pk_free(&pk);
  if (result != 0) { Serial.println("OTA enrollment encryption failed"); return; }
  unsigned char encoded[345]; size_t encodedLength;
  if (mbedtls_base64_encode(encoded, sizeof(encoded), &encodedLength, encrypted, encryptedLength) != 0) return;
  encoded[encodedLength] = 0;
  DynamicJsonDocument doc(768);
  doc["deviceId"] = deviceId; doc["version"] = OTA_VERSION; doc["nonce"] = nonce;
  doc["encryptedKey"] = (const char *)encoded;
  String body; serializeJson(doc, body);
  Response response; String reply;
  bool ok = request(client, "POST", base + "api/ota/device/enroll", body, response, true) &&
    readBody(client, response.length, reply) && equalSignature(sign("enrollment\n" + reply), response.signature);
  client.stop();
  DynamicJsonDocument answer(512);
  if (!ok || deserializeJson(answer, reply) || answer["nonce"].as<String>() != nonce) {
    Serial.println("OTA enrollment unavailable; retry in one minute"); return;
  }
  if (!(answer["approved"] | false)) { Serial.println("OTA waiting for approval on web page"); return; }
  DynamicJsonDocument config(512);
  config["host"] = host; config["port"] = port; config["base"] = base; config["key"] = key;
  String stored; serializeJson(config, stored);
  if (prefs.putString("config", stored) != stored.length()) return;
  enrolling = false; ready = true; polled = false;
  Serial.println("OTA enrollment approved; device configuration saved");
}

bool install(Client &client, const String &, int size, const String &sha, void (*progress)()) {
  if (!Update.begin(size, U_FLASH)) return false;
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash); mbedtls_sha256_starts_ret(&hash, 0);
  unsigned long started = millis();
  int received = 0;
  bool good = true;
  while (received < size) {
    if (millis() - started >= transferTimeout) { good = false; break; }
    DynamicJsonDocument query(384);
    query["deviceId"] = deviceId; query["jobId"] = pendingJob;
    query["sha256"] = sha; query["offset"] = received;
    String body; serializeJson(query, body);
    Response response;
    if (!mqttRequest(client, "chunk", body, response)) { good = false; break; }
    DynamicJsonDocument chunk(2048);
    if (deserializeJson(chunk, rpcReply)) { good = false; break; }
    String encoded = chunk["data"] | "";
    uint8_t buffer[1024]; size_t count = 0;
    if (mbedtls_base64_decode(buffer, sizeof(buffer), &count, (const unsigned char *)encoded.c_str(), encoded.length()) != 0 ||
        count != (size_t)min(1024, size - received) || Update.write(buffer, count) != count) { good = false; break; }
    mbedtls_sha256_update_ret(&hash, buffer, count);
    received += count;
    progress(); // Only verified in-order bytes advance flash and watchdog progress.
    delay(25); // Bound broker load to at most about 40 KB/s per updating board.
  }
  uint8_t digest[32]; mbedtls_sha256_finish_ret(&hash, digest); mbedtls_sha256_free(&hash);
  if (!good || received != size || !equalSignature(hex(digest, 32), sha)) { Update.abort(); return false; }
  pendingResult = "Installing"; pendingMessage = "";
  if (!savePending()) { Update.abort(); return false; }
  if (!Update.end()) return false;
  otaMqtt.disconnect(); client.stop();
  Serial.println("OTA MQTT image verified; restarting into new image");
  delay(100); ESP.restart(); return true;
}
}

void otaBegin(const char *boardMac) {
  Serial.println(metadata);
  Serial.println("NTOTA_TRANSPORT|mqtt-v1|");
  deviceId = boardMac;
  notifyTopic = "ems/" + deviceId + "/ota/notify";
  if (!prefs.begin("nt-ota", false)) return;
#if OTA_PROVISIONING
  Serial.println("NTOTA_PROVISIONING_IMAGE_DO_NOT_PUBLISH");
  if (deviceId == OTA_DEVICE_MAC && validHex(OTA_DEVICE_KEY) && !prefs.isKey("config")) {
    DynamicJsonDocument provision(512);
    provision["host"] = OTA_HOST; provision["port"] = OTA_PORT;
    provision["base"] = OTA_BASE_PATH; provision["key"] = OTA_DEVICE_KEY;
    String data; serializeJson(provision, data);
    if (prefs.putString("config", data) != data.length()) return;
  }
#endif
  DynamicJsonDocument config(512);
  if (!prefs.isKey("config")) {
    // otaBegin runs before RF/ADC initialization, including on Ethernet-only boards.
    mbedtls_ctr_drbg_init(&enrollmentRng);
    bootloader_random_enable();
    int seeded = mbedtls_ctr_drbg_seed(&enrollmentRng, enrollmentEntropy, nullptr,
      (const unsigned char *)deviceId.c_str(), deviceId.length());
    bootloader_random_disable();
    if (seeded != 0) return;
    mbedtls_ctr_drbg_set_reseed_interval(&enrollmentRng, INT_MAX);
    host = OTA_DEFAULT_HOST; port = OTA_DEFAULT_PORT; base = OTA_DEFAULT_BASE;
    key = prefs.getString("enroll-key");
    if (!validHex(key)) {
      uint8_t random[32]; if (enrollmentRandom(nullptr, random, sizeof(random)) != 0) return;
      key = hex(random, sizeof(random));
      if (prefs.putString("enroll-key", key) != key.length()) return;
    }
    enrolling = true;
    lan.setConnectionTimeout(3000); wifi.setTimeout(3);
    Serial.println("OTA automatic MQTT enrollment ready"); return;
  }
  if (deserializeJson(config, prefs.getString("config"))) { Serial.println("OTA stored configuration invalid"); return; }
  host = config["host"].as<String>(); port = config["port"] | 0;
  base = config["base"].as<String>(); key = config["key"].as<String>();
  if (!validHex(key)) return; // MQTT uses the existing board key, not stored HTTP settings.
  ready = true;
  lan.setConnectionTimeout(3000); wifi.setTimeout(3);
  DynamicJsonDocument pending(512);
  if (!deserializeJson(pending, prefs.getString("pending"))) {
    pendingJob = pending["job"].as<String>(); targetVersion = pending["version"].as<String>();
    pendingResult = pending["result"].as<String>(); pendingMessage = pending["message"].as<String>();
    if (pendingResult == "Installing" && targetVersion == OTA_VERSION) awaitingHealth = true;
    else if (pendingResult == "Installing" || pendingResult == "Downloading") {
      pendingResult = "Failed"; pendingMessage = "Transfer or boot interrupted; running previous firmware"; savePending();
    }
  }
}

void otaConfigureBroker(const char *hostname, uint16_t mqttPort, const char *username, const char *password) {
  brokerHost = hostname; brokerPort = mqttPort; brokerUser = username; brokerPassword = password;
}
const char *otaNotifyTopic() { return notifyTopic.c_str(); }
void otaNotify() { checkRequested = true; }

void otaResetNetwork() {
  // Release socket references BEFORE reinitializing the shared W5500.
  otaMqtt.disconnect();
  lan.stop();
  wifi.stop();
  checkRequested = true;
}

void otaService(bool ethernet, bool connected, bool betweenScans, bool healthy, void (*progress)()) {
  if (enrolling && connected && betweenScans) {
    if (polled && millis() - lastPoll < 60000UL) return;
    polled = true; lastPoll = millis();
    Client &client = ethernet ? static_cast<Client &>(lan) : static_cast<Client &>(wifi);
    progress(); enroll(client); return;
  }
  if (!ready || !connected || !betweenScans) return;
  if (awaitingHealth) {
    if (!healthy) return;
    pendingResult = "Succeeded"; pendingMessage = "Boot and first sensor scan completed";
    if (!savePending()) return;
    awaitingHealth = false;
  }
  // Coalesce MQTT notifications/reconnects; signed MQTT RPC remains authoritative.
  const unsigned long interval = (checkRequested || !pendingJob.isEmpty()) ? 60000UL : 3600000UL;
  if (polled && millis() - lastPoll < interval) return;
  checkRequested = false;
  polled = true; lastPoll = millis();
  const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
  if (!slot) { Serial.println("OTA partition missing"); return; }
  uint64_t counter = prefs.getULong64("counter", 0) + 1;
  if (counter > INT64_MAX || prefs.putULong64("counter", counter) != sizeof(counter)) return;
  DynamicJsonDocument payload(768);
  payload["deviceId"] = deviceId; payload["model"] = model; payload["version"] = OTA_VERSION;
  payload["transport"] = "mqtt-v1";
  payload["counter"] = counter; payload["slotBytes"] = slot->size;
  if (!pendingJob.isEmpty()) { payload["jobId"] = pendingJob; payload["result"] = pendingResult; payload["message"] = pendingMessage; }
  String body; serializeJson(payload, body);
  Client &client = ethernet ? static_cast<Client &>(lan) : static_cast<Client &>(wifi);
  Response response;
  String reply;
  if (!request(client, "POST", base + "api/ota/device/poll", body, response) ||
      !readBody(client, response.length, reply) || !equalSignature(sign("response\n" + reply), response.signature)) {
    Serial.println("OTA MQTT poll failed; retry in one minute"); checkRequested = true; client.stop(); return;
  }
  client.stop();
  DynamicJsonDocument manifest(2048);
  if (deserializeJson(manifest, reply) || manifest["counter"].as<uint64_t>() != counter) return;
  String acknowledged = manifest["acknowledged"] | "";
  if (!pendingJob.isEmpty() && acknowledged == pendingJob) {
    if (!prefs.remove("pending")) return;
    pendingJob = ""; targetVersion = ""; pendingResult = ""; pendingMessage = "";
  }
  if (!pendingJob.isEmpty() || manifest["update"].isNull()) return;
  JsonObject update = manifest["update"];
  String version = update["version"] | "";
  String sha = update["sha256"] | "";
  String file = update["file"] | "";
  String job = update["jobId"] | "";
  int size = update["size"] | 0;
  if (String(update["model"] | "") != model || version == OTA_VERSION || version.isEmpty() || version.length() > 32 ||
      job.isEmpty() || job.length() > 20 || !validHex(sha) || file != "api/ota/device/files/" + sha || size < 256 || (size_t)size > slot->size) return;
  pendingJob = job; targetVersion = version; pendingResult = "Downloading"; pendingMessage = "";
  if (!savePending()) { pendingJob = ""; return; }
  Serial.println("OTA downloading; sensor collection pauses until finished or aborted");
  progress();
  if (!install(client, file, size, sha, progress)) {
    if (Update.isRunning()) Update.abort();
    pendingResult = "Failed"; pendingMessage = "Download, hash, flash or NVS verification failed"; savePending();
    Serial.println("OTA failed; continuing current firmware");
  }
  client.stop(); lastPoll = millis();
}
