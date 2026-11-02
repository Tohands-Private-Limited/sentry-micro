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
    ${SENTRY_MICRO_SRC}/core/sentry_coredump.c
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

  # The crash record reads Xtensa's exception frame and walks the stack from it. Zephyr's
  # public headers declare that frame only as a placeholder; the real layout
  # (xtensa_asm2_context.h), the stack-bounds checks (xtensa_stack.h) and the walker's entry
  # points (xtensa_backtrace.h) are in the architecture's internal include directory. They are
  # used exactly as the kernel's own console backtrace uses them, by a module that is already
  # tied to one Zephyr tree, and a change to any of them is a compile error here rather than a
  # silent misread. Xtensa builds only, and only for this library.
  if(CONFIG_XTENSA)
    zephyr_library_include_directories(${ZEPHYR_BASE}/arch/xtensa/include)
  endif()

  zephyr_library_sources_ifdef(CONFIG_SENTRY_MICRO_TRANSPORT_SERIAL
    ${SENTRY_MICRO_SRC}/transport/zephyr/sentry_transport_serial.c
  )
endif()
