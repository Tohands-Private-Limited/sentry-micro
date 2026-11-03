/**
 * Recording a Zephyr fatal error for the next boot to report.
 *
 * With `CONFIG_SENTRY_MICRO_FATAL_HANDLER=y` (the default) sentry-micro provides
 * `k_sys_fatal_error_handler()` itself and calls this, so there is nothing to do. An
 * application that needs its own handler sets that option to `n` and calls
 * `sentry_zephyr_record_fatal()` from it, first, before anything that might fault again:
 *
 *     void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
 *     {
 *         sentry_zephyr_record_fatal(reason, esf);
 *         ... the application's own policy, then reboot ...
 *     }
 *
 * On the next boot, `sentry_event_attach_coredump()` reads the record back exactly as it
 * reads an ESP-IDF core dump.
 */
#ifndef SENTRY_MICRO_COREDUMP_ZEPHYR_H_INCLUDED
#define SENTRY_MICRO_COREDUMP_ZEPHYR_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Declared rather than included, so this header does not drag the architecture headers into
 * every file that includes it. */
struct arch_esf;

/**
 * Store what is known about this fatal error where the next boot can find it.
 *
 * `reason` and `esf` are the two arguments Zephyr passes to `k_sys_fatal_error_handler()`;
 * `esf` may be NULL, in which case only the reason and the thread are recorded. Uses no heap
 * and takes no locks, because it runs in the fault context. On Espressif SoCs it also marks
 * the coming reset as a panic, so the reset reason reads `panic` rather than `software`.
 *
 * Does not reboot or halt: that stays the caller's decision. A handler that returns instead,
 * letting Zephyr abort just the faulting thread, should know the panic hint is already set:
 * the next software reset, whatever its cause, will read as `panic`.
 */
void sentry_zephyr_record_fatal(unsigned int reason, const struct arch_esf *esf);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_COREDUMP_ZEPHYR_H_INCLUDED */
