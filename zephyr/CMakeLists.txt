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

  # What the firmware reports so Sentry can symbolicate its frames: the build-id stamped into
  # the uploaded ELF, the image's load address and size, and the module name. They come from
  # the environment at configure time, the same mechanism scripts/env_secrets.py uses on
  # PlatformIO, because scripts/zephyr_release.py only knows them after measuring a linked
  # image and has to feed them back into the next build. Being read at configure time, a
  # changed value takes effect only after a reconfigure, which is why that script builds with
  # -p always. Unset, they are absent and sentry_micro.c falls back to its defaults; the
  # release itself is the application's, as SENTRY_RELEASE (examples/zephyr_basic).
  if(DEFINED ENV{SENTRY_MICRO_BUILD_ID} AND NOT "$ENV{SENTRY_MICRO_BUILD_ID}" STREQUAL "")
    zephyr_library_compile_definitions("SENTRY_BUILD_ID_HEX=\"$ENV{SENTRY_MICRO_BUILD_ID}\"")
  endif()
  if(DEFINED ENV{SENTRY_MICRO_IMAGE_NAME} AND NOT "$ENV{SENTRY_MICRO_IMAGE_NAME}" STREQUAL "")
    zephyr_library_compile_definitions("SENTRY_IMAGE_NAME=\"$ENV{SENTRY_MICRO_IMAGE_NAME}\"")
  endif()
  # The address and size are numbers, "0x..." as scripts/elf_info.py --export prints them.
  # Checked here because a malformed value would otherwise surface as a confusing compile
  # error inside sentry_micro.c.
  foreach(var ADDR SIZE)
    set(value "$ENV{SENTRY_MICRO_IMAGE_${var}}")
    if(NOT value STREQUAL "")
      if(NOT value MATCHES "^(0[xX][0-9a-fA-F]+|[0-9]+)$")
        message(FATAL_ERROR "SENTRY_MICRO_IMAGE_${var} must be a number such as 0x12820, "
                            "got \"${value}\"")
      endif()
      zephyr_library_compile_definitions(SENTRY_IMAGE_${var}=${value}ULL)
    endif()
  endforeach()

  # Sessions are the one compile-time feature switch exposed as a Kconfig option; logs and
  # metrics keep their -D form (SENTRY_MICRO_LOGS_ENABLED / SENTRY_MICRO_METRICS_ENABLED).
  # Library scope, like the definitions above: nothing in the public API depends on it.
  if(CONFIG_SENTRY_MICRO_SESSIONS)
    zephyr_library_compile_definitions(SENTRY_MICRO_SESSIONS_ENABLED=1)
  else()
    zephyr_library_compile_definitions(SENTRY_MICRO_SESSIONS_ENABLED=0)
  endif()

  # Applications include <sentry_micro.h>, exactly as on Arduino and ESP-IDF.
  zephyr_include_directories(${SENTRY_MICRO_SRC})

  zephyr_library_sources(
    ${SENTRY_MICRO_SRC}/core/sentry_base64.c
    ${SENTRY_MICRO_SRC}/core/sentry_buffer.c
    ${SENTRY_MICRO_SRC}/core/sentry_coredump.c
    ${SENTRY_MICRO_SRC}/core/sentry_device_info.c
    ${SENTRY_MICRO_SRC}/core/sentry_dsn.c
    ${SENTRY_MICRO_SRC}/core/sentry_envelope.c
    ${SENTRY_MICRO_SRC}/core/sentry_http.c
    ${SENTRY_MICRO_SRC}/core/sentry_json.c
    ${SENTRY_MICRO_SRC}/core/sentry_log.c
    ${SENTRY_MICRO_SRC}/core/sentry_metrics.c
    ${SENTRY_MICRO_SRC}/core/sentry_relay.c
    ${SENTRY_MICRO_SRC}/core/sentry_session.c
    ${SENTRY_MICRO_SRC}/core/sentry_span.c
    ${SENTRY_MICRO_SRC}/core/sentry_throttle.c
    ${SENTRY_MICRO_SRC}/core/sentry_trace.c
    ${SENTRY_MICRO_SRC}/core/sentry_transport.c
    ${SENTRY_MICRO_SRC}/sentry_micro.c
    ${SENTRY_MICRO_SRC}/platform/zephyr/sentry_device_zephyr.c
    ${SENTRY_MICRO_SRC}/platform/zephyr/sentry_coredump_zephyr.c
  )

  # Exactly one SoC implementation is built, chosen here at compile time. The three hooks in
  # device/sentry_soc.h have no weak fallback on purpose: a family file that is missing, or
  # a second one that also gets built, is a link error. A weak no-op sentry_soc_mark_panic()
  # that silently won would instead build fine and make every crash read back as a plain
  # software reset. A new SoC family adds its device/<family>/sentry_soc_<family>.c and a
  # line here, and nothing in platform/.
  zephyr_library_sources_ifdef(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32
    ${SENTRY_MICRO_SRC}/device/espressif/sentry_soc_espressif.c
  )
  if(NOT CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
    zephyr_library_sources(${SENTRY_MICRO_SRC}/device/generic/sentry_soc_generic.c)
  endif()

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

  # Only with the network stack the application configured; without it these do not compile,
  # and a build with no networking must still link.
  zephyr_library_sources_ifdef(CONFIG_SENTRY_MICRO_TRANSPORT_WIFI
    ${SENTRY_MICRO_SRC}/transport/zephyr/sentry_transport_wifi.c
    ${SENTRY_MICRO_SRC}/transport/zephyr/sentry_ca_digicert_global_root_g2.c
  )
endif()
