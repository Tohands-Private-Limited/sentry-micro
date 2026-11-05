# sentry-micro as an ESP-IDF component.
#
# The repository root is the component: an application adds it by path (see
# examples/idf_basic/main/idf_component.yml) and includes <sentry_micro.h>. This file is read
# only by ESP-IDF's build. Zephyr's module.yml points at zephyr/CMakeLists.txt and
# zephyr/Kconfig, and PlatformIO goes by library.json, so neither sees it.
#
# Every source is listed by name rather than globbed, for the same reason as on Zephyr: the
# tree under src/ also holds the Arduino and Zephyr ports, which do not compile here, and a
# glob would pick up the next one of those somebody adds as well. A new portable file in
# src/core/ has to be added below (and in zephyr/CMakeLists.txt) by hand; the link error if it
# is not is immediate.
#
# ESP-IDF reads this file twice. The first pass only collects REQUIRES and PRIV_REQUIRES, with
# no configuration loaded, and stops at idf_component_register(); that is why the requirements
# are unconditional and everything that reads the environment or a CONFIG_ value comes after
# it.

set(SENTRY_MICRO_SRCS
    src/core/sentry_base64.c
    src/core/sentry_buffer.c
    src/core/sentry_coredump.c
    src/core/sentry_device_info.c
    src/core/sentry_dsn.c
    src/core/sentry_envelope.c
    src/core/sentry_http.c
    src/core/sentry_json.c
    src/core/sentry_log.c
    src/core/sentry_metrics.c
    src/core/sentry_relay.c
    src/core/sentry_session.c
    src/core/sentry_span.c
    src/core/sentry_throttle.c
    src/core/sentry_trace.c
    src/core/sentry_transport.c
    src/sentry_micro.c
    src/platform/esp-idf/sentry_device_esp32.c
    src/platform/esp-idf/sentry_coredump_esp32.c
    src/platform/esp-idf/sentry_storage_nvs.c
    # The SoC hooks (device/sentry_soc.h), shared with the Zephyr port on Espressif chips.
    src/device/espressif/sentry_soc_espressif.c
)

# HTTPS straight to ingest. The CA it trusts by default is the same file the Zephyr transport
# uses.
if(CONFIG_SENTRY_MICRO_TRANSPORT_WIFI)
  list(APPEND SENTRY_MICRO_SRCS
       src/transport/esp-idf/sentry_transport_wifi.c
       src/transport/sentry_ca_digicert_global_root_g2.c
  )
endif()

idf_component_register(
  SRCS ${SENTRY_MICRO_SRCS}
  # Applications include <sentry_micro.h>, and <transport/esp-idf/sentry_transport_wifi.h>
  # for the transport, exactly as on Arduino and Zephyr.
  INCLUDE_DIRS src
  # Private: no public header includes anything from these. mbedtls is for the optional
  # certificate bundle (CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE); esp_http_client depends on it
  # anyway, so naming it costs nothing.
  PRIV_REQUIRES
    esp_http_client
    esp_netif
    esp_timer
    espcoredump
    mbedtls
    nvs_flash
    spi_flash
)

# What the firmware reports so Sentry can symbolicate its frames: the build-id stamped into
# the uploaded ELF, the image's load address and size, and the module name. They come from the
# environment at configure time, the same variables the Zephyr module reads
# (zephyr/CMakeLists.txt), so a release script can measure a linked image and feed the result
# back into the next build. Being read at configure time, a changed value takes effect only
# after `idf.py reconfigure`. Unset, they are absent and sentry_micro.c falls back to its
# defaults.
if(DEFINED ENV{SENTRY_MICRO_BUILD_ID} AND NOT "$ENV{SENTRY_MICRO_BUILD_ID}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB}
                             PRIVATE "SENTRY_BUILD_ID_HEX=\"$ENV{SENTRY_MICRO_BUILD_ID}\"")
endif()
if(DEFINED ENV{SENTRY_MICRO_IMAGE_NAME} AND NOT "$ENV{SENTRY_MICRO_IMAGE_NAME}" STREQUAL "")
  target_compile_definitions(${COMPONENT_LIB}
                             PRIVATE "SENTRY_IMAGE_NAME=\"$ENV{SENTRY_MICRO_IMAGE_NAME}\"")
endif()
# The address and size are numbers, "0x..." as scripts/elf_info.py --export prints them.
# Checked here because a malformed value would otherwise surface as a confusing compile error
# inside sentry_micro.c.
foreach(var ADDR SIZE)
  set(value "$ENV{SENTRY_MICRO_IMAGE_${var}}")
  if(NOT value STREQUAL "")
    if(NOT value MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
      message(FATAL_ERROR "SENTRY_MICRO_IMAGE_${var} must be a number such as 0x12820, "
                          "got \"${value}\"")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SENTRY_IMAGE_${var}=${value}ULL)
  endif()
endforeach()

# Sessions are the one compile-time feature switch exposed as a Kconfig option, as on Zephyr;
# logs and metrics keep their -D form (SENTRY_MICRO_LOGS_ENABLED / SENTRY_MICRO_METRICS_ENABLED).
# Mapped both ways, because the code's own default is on: without the 0, turning the option
# off would change nothing. Component scope: nothing in the public API depends on it.
if(CONFIG_SENTRY_MICRO_SESSIONS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE SENTRY_MICRO_SESSIONS_ENABLED=1)
else()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE SENTRY_MICRO_SESSIONS_ENABLED=0)
endif()

# Crash reporting reads the core dump ESP-IDF's panic handler writes to flash. Without it the
# reader compiles to a stub that reports nothing (src/platform/esp-idf/sentry_coredump_esp32.c),
# which builds and runs without complaint; say so here instead.
if(NOT CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
  message(WARNING "sentry-micro: CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is off, so crashes are not "
                  "reported. Set it, and give the partition table a coredump partition.")
endif()
