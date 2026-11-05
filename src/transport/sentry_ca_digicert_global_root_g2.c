/*
 * DigiCert Global Root G2, the root the Zephyr and ESP-IDF WiFi transports trust by default.
 *
 * Why this one: Sentry's ingest certificate (CN=ingest.sentry.io, RSA 2048, signed
 * sha256WithRSAEncryption) is issued by "DigiCert Global G2 TLS RSA SHA256 2020 CA1", and that
 * intermediate is signed by this root. Checked against ingest on 2026-11-02. The server sends
 * the leaf and the intermediate; only the root has to be on the device.
 *
 *   Subject     C=US, O=DigiCert Inc, OU=www.digicert.com, CN=DigiCert Global Root G2
 *   Valid       2013-08-01 12:00:00 UTC to 2038-01-15 12:00:00 UTC
 *   SHA-256     CB:3C:CB:B7:60:31:E5:E0:13:8F:8D:D3:9A:23:F9:DE:
 *               47:FF:C3:5E:43:C1:14:4C:EA:27:D4:6A:5A:B1:CB:5F
 *
 * The fingerprint matches the one DigiCert publishes (knowledge.digicert.com, checked
 * 2026-11-02). Copied byte for byte from the Mozilla-derived system store
 * (/etc/ssl/certs/DigiCert_Global_Root_G2.pem). To check this copy:
 *
 *     sed -n 's/^[ =]*"\(.*\)\\n";*$/\1/p' sentry_ca_digicert_global_root_g2.c \
 *         | openssl x509 -noout -fingerprint -sha256
 *
 * Two ways this stops working, both outside the firmware's control: the root expires in 2038,
 * or Sentry moves ingest to a certificate from a different root before then. Either one makes
 * every handshake fail on every device still carrying this copy, which is why an application
 * can pass its own CA to sentry_wifi_transport_init() and ship a new one with its next update.
 *
 * PEM rather than DER so that it can be compared with the published certificate by eye; on
 * Zephyr it needs CONFIG_MBEDTLS_PEM_PARSE_C, which ESP-IDF's mbedTLS always has.
 *
 * One copy for both ports, so the bytes the two trust cannot drift apart. Wrapped whole,
 * because PlatformIO compiles every source under src/ for the Arduino builds as well, where
 * ESP_PLATFORM is defined too: there it is an empty translation unit, since the Arduino
 * WiFiTransport takes its CA from set_ca_cert() instead. Each port compiles it only with its
 * WiFi transport (zephyr/CMakeLists.txt, the root CMakeLists.txt).
 */
#if defined(__ZEPHYR__) || (defined(ESP_PLATFORM) && !defined(ARDUINO))

#    include "sentry_ca_digicert_global_root_g2.h"

/* The NUL terminator is part of the credential: mbedTLS's PEM parser requires it. */
const char sentry_ca_digicert_global_root_g2[]
    = "-----BEGIN CERTIFICATE-----\n"
      "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
      "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
      "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
      "MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
      "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
      "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
      "9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
      "2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
      "1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
      "q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
      "tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
      "vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
      "BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
      "5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
      "1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
      "NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
      "Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
      "8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
      "pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
      "MrY=\n"
      "-----END CERTIFICATE-----\n";

#endif /* __ZEPHYR__ || (ESP_PLATFORM && !ARDUINO) */
