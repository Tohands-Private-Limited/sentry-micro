# sentry-micro — zephyr_basic
#
# The DSN and release come from the environment at configure time, the same way the
# PlatformIO examples take them (scripts/env_secrets.py), so nothing secret lives in a file.
# They are read when CMake configures: change one, rebuild with `-p always`.

cmake_minimum_required(VERSION 3.20.0)

# This repository is the module. Added before find_package(Zephyr), which is where modules
# are discovered; after it, the module is silently absent and CONFIG_SENTRY_MICRO is unknown.
get_filename_component(SENTRY_MICRO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES ${SENTRY_MICRO_ROOT})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sentry_micro_zephyr_basic)

target_sources(app PRIVATE src/main.c)

if(DEFINED ENV{SENTRY_MICRO_DSN} AND NOT "$ENV{SENTRY_MICRO_DSN}" STREQUAL "")
  target_compile_definitions(app PRIVATE "SENTRY_DSN=\"$ENV{SENTRY_MICRO_DSN}\"")
else()
  # Loud on purpose: a placeholder DSN builds and flashes fine, then reports to nowhere.
  message(WARNING "SENTRY_MICRO_DSN is not set; this build uses a placeholder DSN and "
                  "will not reach Sentry. Export it and rebuild with -p always.")
endif()

if(DEFINED ENV{SENTRY_MICRO_RELEASE} AND NOT "$ENV{SENTRY_MICRO_RELEASE}" STREQUAL "")
  target_compile_definitions(app PRIVATE "SENTRY_RELEASE=\"$ENV{SENTRY_MICRO_RELEASE}\"")
endif()

# WiFi credentials, from the environment like the DSN. Setting the SSID is what selects the
# WiFi transport over the serial relay; the network stack it needs comes from wifi.conf.
if(DEFINED ENV{SENTRY_MICRO_WIFI_SSID} AND NOT "$ENV{SENTRY_MICRO_WIFI_SSID}" STREQUAL "")
  if(NOT CONFIG_SENTRY_MICRO_TRANSPORT_WIFI)
    # Caught here rather than as an undefined sentry_wifi_transport_init() at link time.
    message(FATAL_ERROR "SENTRY_MICRO_WIFI_SSID is set, but this build has no WiFi stack. "
                        "Add -DEXTRA_CONF_FILE=wifi.conf after the -- of the west command.")
  endif()
  target_compile_definitions(app PRIVATE "WIFI_SSID=\"$ENV{SENTRY_MICRO_WIFI_SSID}\"")
  if(DEFINED ENV{SENTRY_MICRO_WIFI_PASSWORD} AND NOT "$ENV{SENTRY_MICRO_WIFI_PASSWORD}" STREQUAL "")
    target_compile_definitions(app PRIVATE
                               "WIFI_PASSWORD=\"$ENV{SENTRY_MICRO_WIFI_PASSWORD}\"")
  else()
    message(WARNING "SENTRY_MICRO_WIFI_PASSWORD is not set; this build joins the network as "
                    "an open one. Export it and rebuild with -p always if it is not.")
  endif()
elseif(CONFIG_SENTRY_MICRO_TRANSPORT_WIFI)
  # Loud for the same reason as the DSN: this builds, flashes and then never touches WiFi.
  message(WARNING "This build has the WiFi stack but SENTRY_MICRO_WIFI_SSID is not set, so it "
                  "reports over the serial relay. Export it and rebuild with -p always.")
endif()

# -DSENTRY_DEMO_CRASH=1 on the west/cmake command line: crash once after the boot event.
set(SENTRY_DEMO_CRASH 0 CACHE STRING "Crash deliberately after reporting the boot (0 or 1)")
target_compile_definitions(app PRIVATE SENTRY_DEMO_CRASH=${SENTRY_DEMO_CRASH})
