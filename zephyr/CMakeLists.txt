# sentry-micro on Zephyr.
#
# Every source is listed by name rather than globbed. The tree under src/ also holds the
# ESP-IDF and Arduino ports, which do not compile here, and a glob would pick up the next
# one of those somebody adds as well. A new portable file in src/core/ has to be added
# below by hand; the link error if it is not is immediate.

if(CONFIG_SENTRY_MICRO)
  get_filename_component(SENTRY_MICRO_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
  set(SENTRY_MICRO_SRC ${SENTRY_MICRO_ROOT}/src)

  zephyr_library_named(sentry_micro)

  # Applications include <sentry_micro.h>, exactly as on Arduino and ESP-IDF.
  zephyr_include_directories(${SENTRY_MICRO_SRC})

  zephyr_library_sources(
    ${SENTRY_MICRO_SRC}/core/sentry_base64.c
    ${SENTRY_MICRO_SRC}/core/sentry_buffer.c
    ${SENTRY_MICRO_SRC}/core/sentry_device_info.c
    ${SENTRY_MICRO_SRC}/core/sentry_dsn.c
    ${SENTRY_MICRO_SRC}/core/sentry_envelope.c
    ${SENTRY_MICRO_SRC}/core/sentry_json.c
    ${SENTRY_MICRO_SRC}/core/sentry_log.c
    ${SENTRY_MICRO_SRC}/core/sentry_metrics.c
    ${SENTRY_MICRO_SRC}/core/sentry_relay.c
    ${SENTRY_MICRO_SRC}/core/sentry_span.c
    ${SENTRY_MICRO_SRC}/core/sentry_throttle.c
    ${SENTRY_MICRO_SRC}/core/sentry_trace.c
    ${SENTRY_MICRO_SRC}/core/sentry_transport.c
    ${SENTRY_MICRO_SRC}/sentry_micro.c
    ${SENTRY_MICRO_SRC}/device/zephyr/sentry_device_zephyr.c
    ${SENTRY_MICRO_SRC}/device/zephyr/sentry_coredump_zephyr.c
  )

  zephyr_library_sources_ifdef(CONFIG_SENTRY_MICRO_TRANSPORT_SERIAL
    ${SENTRY_MICRO_SRC}/transport/zephyr/sentry_transport_serial.c
  )
endif()
