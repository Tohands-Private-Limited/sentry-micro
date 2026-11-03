/*
 * The SoC hooks for Espressif parts under Zephyr (see device/sentry_soc.h).
 *
 * The three hooks are written against hal_espressif, not against Zephyr, so a later PR may
 * lift them for the ESP-IDF port too, which asks the same HAL the same questions through
 * sentry_device_esp32.c today.
 *
 * Wrapped whole, includes and all, because PlatformIO's library finder compiles every source
 * under src/ for the Arduino builds too, and none of <zephyr/...> exists there. Outside a
 * Zephyr build this is an empty translation unit.
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

#endif /* __ZEPHYR__ */
