/*
 * Crash capture on Zephyr: not yet. Every call here answers "nothing stored", honestly.
 *
 * `is_supported` returning false is what keeps that honest rather than merely quiet: the SDK
 * and the boot event still work, crashes simply arrive as a reset reason without a
 * backtrace, and the caller can tell that apart from "no crashes have happened".
 *
 * What the next milestone puts here, on Espressif first:
 *
 * - Override `k_sys_fatal_error_handler()`, Zephyr's weak hook that runs on every fatal
 *   error with the faulting thread's saved context.
 * - Collect the program counters with `arch_stack_walk()` from that context, plus the
 *   exception cause and faulting address, into a small fixed record.
 * - Store the record in `.rtc_noinit`, beside the trace and log-ring blocks in
 *   sentry_device_zephyr.c, so it survives the reboot with no flash write from inside a
 *   fault handler.
 * - Call `esp_reset_reason_set_hint(ESP_RST_PANIC)` before rebooting. It is compiled into
 *   every Zephyr ESP32 image already (hal_espressif builds
 *   `esp_system/port/soc/<series>/reset_reason.c`); nothing calls it, which is why a panic
 *   currently reads back as a software reset.
 * - Reboot, and on the next boot have `sentry_coredump_read()` hand the record to the same
 *   `sentry_event_attach_coredump()` path the ESP-IDF port feeds.
 *
 * A full `CONFIG_DEBUG_COREDUMP` dump to a flash partition is a different, much larger
 * artefact: kept for a later milestone as an attachment for offline analysis, not as the
 * source of the backtrace, which the record above already provides in a few hundred bytes.
 *
 * Wrapped whole, includes and all, for the same reason as sentry_device_zephyr.c.
 */
#if defined(__ZEPHYR__)

#    include "../sentry_coredump_device.h"

#    include <string.h>

bool sentry_coredump_is_supported(void) { return false; }

bool sentry_coredump_available(void) { return false; }

bool sentry_coredump_read(sentry_coredump_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return false;
}

bool sentry_coredump_erase(void) { return false; }

#endif /* __ZEPHYR__ */
