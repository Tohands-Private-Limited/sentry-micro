idf_component_register(
  SRCS main.c
  PRIV_REQUIRES sentry-micro esp_event esp_netif esp_wifi nvs_flash
)

# Compile definitions on this component only, so the DSN and the WiFi password reach main.c
# and nothing else. ESP-IDF stops reading this file at idf_component_register() on its first
# pass, which is why all of it comes after.

if(DEFINED ENV{SENTRY_MICRO_DSN} AND NOT "$ENV{SENTRY_MICRO_DSN}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE "SENTRY_DSN=\"$ENV{SENTRY_MICRO_DSN}\"")
else()
  # Loud on purpose: a placeholder DSN builds and flashes fine, then reports to nowhere.
  message(WARNING "SENTRY_MICRO_DSN is not set; this build uses a placeholder DSN and will "
                  "not reach Sentry. Export it and run `idf.py reconfigure`.")
endif()

if(DEFINED ENV{SENTRY_MICRO_RELEASE} AND NOT "$ENV{SENTRY_MICRO_RELEASE}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB}
                             PRIVATE "SENTRY_RELEASE=\"$ENV{SENTRY_MICRO_RELEASE}\"")
endif()

if(DEFINED ENV{SENTRY_MICRO_WIFI_SSID} AND NOT "$ENV{SENTRY_MICRO_WIFI_SSID}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB}
                             PRIVATE "WIFI_SSID=\"$ENV{SENTRY_MICRO_WIFI_SSID}\"")
  if(DEFINED ENV{SENTRY_MICRO_WIFI_PASSWORD}
     AND NOT "$ENV{SENTRY_MICRO_WIFI_PASSWORD}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB}
                               PRIVATE "WIFI_PASSWORD=\"$ENV{SENTRY_MICRO_WIFI_PASSWORD}\"")
  else()
    message(WARNING "SENTRY_MICRO_WIFI_PASSWORD is not set; this build joins the network as "
                    "an open one. Export it and run `idf.py reconfigure` if it is not.")
  endif()
else()
  # Loud for the same reason as the DSN: this builds, flashes and then has no route.
  message(WARNING "SENTRY_MICRO_WIFI_SSID is not set, so this build never joins a network and "
                  "nothing reaches Sentry. Export it and run `idf.py reconfigure`.")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE SENTRY_DEMO_CRASH=${SENTRY_DEMO_CRASH})
