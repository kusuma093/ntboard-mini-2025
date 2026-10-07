#pragma once
// Copy to OtaConfig.local.h for the FIRST USB provisioning build only.
// Never upload a provisioning binary to the release server.
#define OTA_PROVISIONING 1
#define OTA_HOST "10.1.220.133"
#define OTA_PORT 5000
#define OTA_BASE_PATH "/"
// MAC printed by this firmware (uses ESP_MAC_WIFI_SOFTAP).
#define OTA_DEVICE_MAC "AA:BB:CC:DD:EE:FF"
#define OTA_DEVICE_KEY "REPLACE_WITH_64_HEX_CHARACTERS_FROM_OTA_PAGE"
// For subsequent common releases, remove the local file and bump OTA_VERSION
// in OtaVersion.h. Device config/key survives in NVS.
