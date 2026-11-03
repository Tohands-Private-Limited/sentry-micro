/*
 * The few things the platform files need from the silicon rather than from the OS.
 * Private to the SDK: the platform files in src/platform/ include it, applications do not.
 *
 * Under Zephyr the hooks are all there is to the silicon. Zephyr already hides the OS differences
 * between SoCs (hwinfo, the clock, the heap). What is left is what no Zephyr API covers on every
 * part, and it is exactly this: where a block of memory survives a reset, how to mark a reboot as a
 * panic, which chip this is, and which revision of it. Each SoC family implements the three
 * functions in src/device/<family>/sentry_soc_<family>.c, and zephyr/CMakeLists.txt compiles
 * exactly one. There are no weak fallbacks on purpose: a family that forgot sentry_soc_mark_panic()
 * must fail to link, not quietly build a port whose crashes read back as plain software resets.
 *
 * ESP-IDF, which the Arduino core is built on, answers the same questions through the same
 * hooks, from the same Espressif file, so the chip model and revision have one definition for
 * every platform and cannot drift between them. The Zephyr-only parts are marked below.
 */
#ifndef SENTRY_MICRO_SOC_H_INCLUDED
#define SENTRY_MICRO_SOC_H_INCLUDED

#include <stdint.h>

#if defined(__ZEPHYR__)

#    include <zephyr/linker/section_tags.h>

/*
 * Memory that survives a reset.
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
 * the crash record cannot drift into different memories on some board. It is a macro, not a
 * function, because it places a variable; that is why it is chosen here by the header and
 * not by the per-family source file.
 *
 * `name` is a string literal, appended to the section name so each block is its own input
 * section and shows up by name in the map file.
 */
#    if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32) && defined(CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED)
#        define SENTRY_PERSIST(name) __attribute__((section(".rtc_noinit." name)))
#    else
#        define SENTRY_PERSIST(name) __noinit
#    endif

/*
 * SENTRY_PERSIST is Zephyr-only on purpose. sentry_device_esp32.c keeps esp_attr.h's
 * RTC_NOINIT_ATTR, which names the section `.rtc_noinit` and is empty on a part without RTC
 * slow memory; SENTRY_PERSIST would rename it to `.rtc_noinit.<name>`. That moves where the
 * classic ESP32 keeps its trace and block across a reset, which a build cannot confirm and
 * this repository has no way to run, so the ESP-IDF attributes are left alone.
 */
#endif /* __ZEPHYR__ */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Marks the coming reboot as a panic, so the next boot can tell it from a software reset.
 *
 * Called first from the fatal-error hook, before anything that could fault again, and before
 * the reboot. It may be a no-op on a SoC whose reset reason already distinguishes a panic
 * without help (or one that has nothing to offer, in which case a crash reads as a software
 * reset, as it would without this SDK).
 */
void sentry_soc_mark_panic(void);

/**
 * The chip model, as it appears in the `chip` tag on every event. Never NULL.
 *
 * Families that share a part with another port use the strings that port uses, so one chip
 * does not group as two depending on the RTOS it runs.
 */
const char *sentry_soc_chip_model(void);

/**
 * The silicon revision, 0 when it cannot be read. The Espressif implementation reports
 * `major * 100 + minor`, the convention of the ESP-IDF port, so v0.2 reads as 2 on both.
 */
uint16_t sentry_soc_chip_revision(void);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_SOC_H_INCLUDED */
