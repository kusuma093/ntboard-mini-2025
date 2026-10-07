#pragma once
// Public server key only. Private key stays on the API host.
#define OTA_DEFAULT_HOST "broker.ntplc.co.th"
#define OTA_DEFAULT_PORT 8080
#define OTA_DEFAULT_BASE "/ota-api/"
static const char otaEnrollmentPublicKey[] = R"KEY(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtDkZd2ovZc4a7T0kh9mA
02iLp25Tlv+pZLdctyH+tD4WHYhN762LGhloCvqsLuP6aPm3+MdQ5OF0LDRZCBFf
pF4bn+AwkpYBB5acFlESBHkyL8FMIfengGm4tpyGZSKO2Ppk5n6IkofuAJTQGUc4
kofZDZhRhOESaSSIXdudXK6O68KKk9NQ+uPsz/O65girjNvjqDHAkieptoK9MFXr
LRBbS8FYJRNkdt7s6+5KBuww3JCxbC6pFnZPYo1dLrhvydJo6Se4Eixz6cgRpW6H
BmrrmzfQMRoQnM8vt72YbmMVMm/jgusgq6SaRsGNBIpv68DJveIIqZ9Gse5Wdu5s
NwIDAQAB
-----END PUBLIC KEY-----
)KEY";
