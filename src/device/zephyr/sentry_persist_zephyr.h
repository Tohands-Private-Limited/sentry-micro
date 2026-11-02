/*
 * Memory that survives a reset, for the Zephyr device files. Private to src/device/zephyr/.
 *
 * On Espressif SoCs with RTC slow memory this is the same memory the ESP32 port uses: the
 * Zephyr linker scripts collect `.rtc_noinit*` into RTC slow memory as NOLOAD, and neither
 * Zephyr's startup nor the HAL's clears it, so it is lost on power-on and kept across a
 * software reset or a panic — exactly the lifetime wanted.
 *
 * The guard is the one esp_attr.h puts around RTC_NOINIT_ATTR. hal_espressif sources each
 * series' soc_caps into Kconfig, so it is an ordinary CONFIG_ symbol here; the attribute is
 * spelled out rather than taken from esp_attr.h to keep these files off the hal's headers for
 * something this small.
 *
 * Everywhere else it is Zephyr's `__noinit`: ordinary SRAM that startup does not zero. That
 * is best effort. A warm reset usually leaves SRAM as it was, but nothing promises it, and
 * a part that powers its SRAM down in reset hands back garbage — which every user of this
 * macro rejects with a magic word and a layout id, so the worst case is the nothing-stored
 * answer rather than a wrong one.
 *
 * One macro shared by every block rather than one per file, so the trace, the log ring and
 * the crash record cannot drift into different memories on some board.
 */
#ifndef SENTRY_MICRO_PERSIST_ZEPHYR_H_INCLUDED
#define SENTRY_MICRO_PERSIST_ZEPHYR_H_INCLUDED

#if defined(__ZEPHYR__)

#    include <zephyr/linker/section_tags.h>

/* `name` is a string literal, appended to the section name so each block is its own input
 * section and shows up by name in the map file. */
#    if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32) && defined(CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED)
#        define SENTRY_PERSIST(name) __attribute__((section(".rtc_noinit." name)))
#    else
#        define SENTRY_PERSIST(name) __noinit
#    endif

#endif /* __ZEPHYR__ */

#endif /* SENTRY_MICRO_PERSIST_ZEPHYR_H_INCLUDED */
