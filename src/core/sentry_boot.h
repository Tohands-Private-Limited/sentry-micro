/**
 * Common definitions shared by every sentry-micro translation unit.
 *
 * Mirrors the role of sentry-native's `sentry_boot.h`: the one header that every
 * internal file includes first, defining the version, the export macros, and the
 * size limits. Everything here is freestanding C — no Arduino, no ESP-IDF — so the
 * portable core can also be compiled and unit-tested on a host machine.
 */
#ifndef SENTRY_MICRO_BOOT_H_INCLUDED
#define SENTRY_MICRO_BOOT_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The SDK name is per platform, not per project, because it is wire-visible: it is the
 * `sdk.name` on every event and the product half of the user agent. Sentry groups SDK
 * adoption and version health by it, so a Zephyr fleet reporting as `esp32` would be
 * indistinguishable from an Arduino one in exactly the view meant to tell them apart.
 * Chosen at compile time because the platform is; `-D` overrides it for a port that is
 * neither.
 */
#ifndef SENTRY_MICRO_SDK_NAME
#    if defined(__ZEPHYR__)
#        define SENTRY_MICRO_SDK_NAME "sentry.micro.zephyr"
#    else
#        define SENTRY_MICRO_SDK_NAME "sentry.micro.esp32"
#    endif
#endif
/* The `os.name` context beside the SDK name: the RTOS the version string next to it
 * belongs to. Per platform for the same reason, and overridable the same way. */
#ifndef SENTRY_MICRO_OS_NAME
#    if defined(__ZEPHYR__)
#        define SENTRY_MICRO_OS_NAME "zephyr"
#    else
#        define SENTRY_MICRO_OS_NAME "esp-idf"
#    endif
#endif
#define SENTRY_MICRO_SDK_VERSION "0.1.0"
#define SENTRY_MICRO_SDK_USER_AGENT SENTRY_MICRO_SDK_NAME "/" SENTRY_MICRO_SDK_VERSION

/*
 * Size limits.
 *
 * Everything in the core is fixed-size and stack- or caller-allocated: an MCU crash
 * handler cannot rely on the heap (it may be exhausted, or fragmented, or the very
 * thing that crashed), so the SDK never mallocs on the reporting path. These bounds
 * are the price of that guarantee — pick them generously enough for real DSNs but
 * small enough that a parsed DSN still fits comfortably on a FreeRTOS task stack.
 */
#define SENTRY_MICRO_MAX_DSN_LEN 256
#define SENTRY_MICRO_MAX_HOST_LEN 128
#define SENTRY_MICRO_MAX_KEY_LEN 64
#define SENTRY_MICRO_MAX_PATH_LEN 64
#define SENTRY_MICRO_MAX_PROJECT_ID_LEN 24
/* Org ids are u64 in Relay: 20 digits plus the terminator. */
#define SENTRY_MICRO_MAX_ORG_ID_LEN 21
/**
 * Scratch space `sentry_flush()` reads a buffered envelope into, on the caller's stack.
 *
 * Not a static allocation: buffering is optional, and a device that never buffers should
 * not pay for it in permanent RAM. Override with `-D` if your events are larger — an
 * envelope that does not fit is skipped, not truncated.
 */
#ifndef SENTRY_MICRO_ENVELOPE_BUFFER_BYTES
#    define SENTRY_MICRO_ENVELOPE_BUFFER_BYTES 2048
#endif

/* Ingest URL + the `X-Sentry-Auth` header value, both built once at init. */
#define SENTRY_MICRO_MAX_URL_LEN 320
#define SENTRY_MICRO_MAX_AUTH_LEN 256

#endif /* SENTRY_MICRO_BOOT_H_INCLUDED */
