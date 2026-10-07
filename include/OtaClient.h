#pragma once
#include <stdint.h>

// Uses a separate MQTT connection; call only between complete sensor scans.
void otaBegin(const char *boardMac);
const char *otaNotifyTopic();
void otaNotify();
void otaResetNetwork();
void otaService(bool ethernet, bool connected, bool betweenScans, bool healthy, void (*progress)());

void otaConfigureBroker(const char *host, uint16_t port, const char *username, const char *password);
