/*
 * The SoC hooks for Espressif parts (see device/sentry_soc.h), in two halves: one for Zephyr,
 * one for ESP-IDF, which the Arduino core also defines ESP_PLATFORM for.
 *
 * Each half is wrapped whole, includes and all, because PlatformIO's library finder compiles
 * every source under src/ for the Arduino builds too, and none of <zephyr/...> exists there.
 * Under any other build this is an empty translation unit.
 */
#if defined(__ZEPHYR__)

#    include "../sentry_soc.h"

#    include <hal/efuse_hal.h>

/* Declares esp_reset_reason_set_hint(). hal_espressif compiles the implementation for every
 * Espressif series (esp_system/port/soc/<series>/reset_reason.c) and puts this header on the
 * global include path. */
#    include <esp_private/system_internal.h>

/*
 * Chip model, resolved at compile time from the SoC series for the same reason the ESP32 port
 * uses the IDF target macros: the answer is fixed by the build, and a runtime lookup either
 * lags new silicon or reports "unknown" on it. The strings match the ESP32 port's exactly,
 * because they are the `chip` tag on every event — the same S3 must not group as two
 * different chips depending on which RTOS it happens to run. A series not listed reports
 * Zephyr's own SoC name, which is what a Zephyr user would search for.
 */
#    if defined(CONFIG_SOC_SERIES_ESP32)
#        define SENTRY_CHIP_MODEL "ESP32"
#    elif defined(CONFIG_SOC_SERIES_ESP32S2)
#        define SENTRY_CHIP_MODEL "ESP32-S2"
#    elif defined(CONFIG_SOC_SERIES_ESP32S3)
#        define SENTRY_CHIP_MODEL "ESP32-S3"
#    elif defined(CONFIG_SOC_SERIES_ESP32C2)
#        define SENTRY_CHIP_MODEL "ESP32-C2"
#    elif defined(CONFIG_SOC_SERIES_ESP32C3)
#        define SENTRY_CHIP_MODEL "ESP32-C3"
#    elif defined(CONFIG_SOC_SERIES_ESP32C5)
#        define SENTRY_CHIP_MODEL "ESP32-C5"
#    elif defined(CONFIG_SOC_SERIES_ESP32C6)
#        define SENTRY_CHIP_MODEL "ESP32-C6"
#    elif defined(CONFIG_SOC_SERIES_ESP32C61)
#        define SENTRY_CHIP_MODEL "ESP32-C61"
#    elif defined(CONFIG_SOC_SERIES_ESP32H2)
#        define SENTRY_CHIP_MODEL "ESP32-H2"
#    elif defined(CONFIG_SOC_SERIES_ESP32P4)
#        define SENTRY_CHIP_MODEL "ESP32-P4"
#    else
#        define SENTRY_CHIP_MODEL CONFIG_SOC
#    endif

const char *sentry_soc_chip_model(void) { return SENTRY_CHIP_MODEL; }

uint16_t sentry_soc_chip_revision(void)
{
    /* Already `major * 100 + minor` (hal/efuse_hal.c), the same convention the ESP32 port
     * reports, so v0.2 reads as 2 on both. */
    return (uint16_t)efuse_hal_chip_revision();
}

void sentry_soc_mark_panic(void)
{
    /* A Zephyr fatal error with CONFIG_RESET_ON_FATAL_ERROR reboots through esp_restart(),
     * which the hardware records as a software reset. ESP-IDF tells a panic apart with a
     * hint its panic handler writes before restarting; Zephyr does not build that handler,
     * so the fatal-error hook writes the hint through here instead. The HAL reads it back on
     * the next boot as ESP_RST_PANIC and hwinfo reports RESET_CPU_LOCKUP, which the platform
     * layer maps to `panic`. */
    esp_reset_reason_set_hint(ESP_RST_PANIC);
}

#elif defined(ESP_PLATFORM)

#    include "../sentry_soc.h"

#    include "esp_chip_info.h"

/*
 * Chip model is resolved at compile time from the IDF target macros rather than at
 * runtime from `esp_chip_info().model`. The enum gains a new constant with every new
 * silicon family, so a runtime switch either fails to build on older IDF or silently
 * reports "unknown" on newer chips; the CONFIG_IDF_TARGET_* macros have been stable
 * since IDF 4 and cost nothing at runtime.
 */
#    if defined(CONFIG_IDF_TARGET_ESP32)
#        define SENTRY_CHIP_MODEL "ESP32"
#    elif defined(CONFIG_IDF_TARGET_ESP32S2)
#        define SENTRY_CHIP_MODEL "ESP32-S2"
#    elif defined(CONFIG_IDF_TARGET_ESP32S3)
#        define SENTRY_CHIP_MODEL "ESP32-S3"
#    elif defined(CONFIG_IDF_TARGET_ESP32C2)
#        define SENTRY_CHIP_MODEL "ESP32-C2"
#    elif defined(CONFIG_IDF_TARGET_ESP32C3)
#        define SENTRY_CHIP_MODEL "ESP32-C3"
#    elif defined(CONFIG_IDF_TARGET_ESP32C5)
#        define SENTRY_CHIP_MODEL "ESP32-C5"
#    elif defined(CONFIG_IDF_TARGET_ESP32C6)
#        define SENTRY_CHIP_MODEL "ESP32-C6"
#    elif defined(CONFIG_IDF_TARGET_ESP32H2)
#        define SENTRY_CHIP_MODEL "ESP32-H2"
#    elif defined(CONFIG_IDF_TARGET_ESP32P4)
#        define SENTRY_CHIP_MODEL "ESP32-P4"
#    else
#        define SENTRY_CHIP_MODEL "ESP32-unknown"
#    endif

const char *sentry_soc_chip_model(void) { return SENTRY_CHIP_MODEL; }

uint16_t sentry_soc_chip_revision(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    return (uint16_t)chip.revision;
}

void sentry_soc_mark_panic(void)
{
    /* Nothing to do, and no caller: ESP-IDF's own panic handler writes the reset-reason hint
     * before it restarts, which is what the Zephyr half has to do by hand. The function
     * exists so that every platform defines the same three hooks. */
}

#endif /* __ZEPHYR__ / ESP_PLATFORM */
