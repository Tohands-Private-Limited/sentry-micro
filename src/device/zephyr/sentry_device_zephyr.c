/*
 * The device layer on Zephyr.
 *
 * Wrapped whole, includes and all, because PlatformIO's library finder compiles every source
 * under src/ for the Arduino builds too, and none of <zephyr/...> exists there. Outside a
 * Zephyr build this is an empty translation unit, exactly as the Arduino transports are
 * under ESP-IDF.
 */
#if defined(__ZEPHYR__)

#    include "../sentry_device.h"

#    include <errno.h>
#    include <stdio.h>
#    include <string.h>
#    include <time.h> /* struct timespec */

#    include <zephyr/devicetree.h>
#    include <zephyr/drivers/hwinfo.h>
#    include <zephyr/kernel.h>
#    include <zephyr/linker/section_tags.h>
#    include <zephyr/random/random.h>
#    include <zephyr/sys/clock.h>
#    include <zephyr/version.h>

#    if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
#        include <zephyr/sys/sys_heap.h>
#    endif

#    if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
#        include <hal/efuse_hal.h>
#    endif

/*
 * Every timestamp and uptime the core writes is 64-bit and formatted with %llu/%lld. The libc
 * printf levels that drop long long do not fail: they print a wrong number, or the format
 * spec itself, into a field Sentry then parses. zephyr/Kconfig steers picolibc to a level
 * that keeps it, but an application can still override that choice in its own prj.conf, so
 * the build is refused here rather than shipping a fleet that reports every event at 1970.
 */
#    if defined(CONFIG_PICOLIBC) && !defined(CONFIG_PICOLIBC_IO_LONG_LONG)                         \
        && !defined(CONFIG_PICOLIBC_IO_FLOAT) && !defined(CONFIG_PICOLIBC_IO_MINIMAL_LONG_LONG)
#        error "sentry-micro needs long long printf: set CONFIG_PICOLIBC_IO_LONG_LONG=y"
#    endif
#    if defined(CONFIG_NEWLIB_LIBC_NANO)
#        error "sentry-micro needs long long printf, which newlib-nano lacks: use picolibc"
#    endif
#    if defined(CONFIG_MINIMAL_LIBC) && !defined(CONFIG_CBPRINTF_FULL_INTEGRAL)
#        error "sentry-micro needs long long printf: set CONFIG_CBPRINTF_FULL_INTEGRAL=y"
#    endif

/*
 * Chip model, resolved at compile time from the SoC series for the same reason the ESP32 port
 * uses the IDF target macros: the answer is fixed by the build, and a runtime lookup either
 * lags new silicon or reports "unknown" on it. The strings match the ESP32 port's exactly,
 * because they are the `chip` tag on every event — the same S3 must not group as two
 * different chips depending on which RTOS it happens to run. Anything else reports
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

/* Load-bearing for symbolication, not decoration: the families unwind differently. The three
 * named here are spelled the way the ESP32 port spells them; anything else gets Zephyr's
 * own architecture name. */
#    if defined(CONFIG_XTENSA)
#        define SENTRY_CHIP_ARCH "xtensa"
#    elif defined(CONFIG_RISCV)
#        define SENTRY_CHIP_ARCH "riscv"
#    elif defined(CONFIG_ARM)
#        define SENTRY_CHIP_ARCH "arm"
#    else
#        define SENTRY_CHIP_ARCH CONFIG_ARCH
#    endif

/*
 * The heap the free-memory numbers describe.
 *
 * This is the k_malloc() pool — the kernel's system heap — not the libc malloc() arena,
 * which on Zephyr is a separate region with its own size. It is the one a Zephyr driver or
 * subsystem allocates from, so it is the one whose exhaustion predicts a failure here.
 * Zephyr has no public accessor for it: `_system_heap` is a non-static `struct k_heap`
 * defined by `K_HEAP_DEFINE` in kernel/mempool.c, and it only exists when the pool has a
 * size at all. That is a CMake definition rather than a Kconfig symbol (kernel/CMakeLists.txt
 * sums CONFIG_HEAP_MEM_POOL_SIZE and every HEAP_MEM_POOL_ADD_SIZE_*), and kernel.h guards on
 * it exactly like this. An undefined macro reads as 0, which is the honest fallback: no pool,
 * no numbers.
 */
#    if defined(CONFIG_SYS_HEAP_RUNTIME_STATS) && (K_HEAP_MEM_POOL_SIZE > 0)
#        define SENTRY_HEAP_STATS 1
extern struct k_heap _system_heap;
#    else
#        define SENTRY_HEAP_STATS 0
#    endif

static sentry_reset_reason_t map_reset_reason(uint32_t cause)
{
    /*
     * hwinfo reports a bitmask, not one value, because some SoCs latch several causes at
     * once — a watchdog that fired during a brownout, say. Sentry wants one, so the bits are
     * tested most-specific first: anything that names a crash outranks the generic causes
     * it may have been latched alongside.
     *
     * Two things are lost in the translation, both because hwinfo has no bit for them:
     *
     * - The three ESP watchdogs (interrupt, task, RTC) all arrive as RESET_WATCHDOG, so they
     *   collapse into the generic `wdt` rather than `int_wdt` / `task_wdt`.
     * - USB, JTAG and SDIO resets map to no bit at all on ESP32 (hwinfo_esp32.c returns 0
     *   for them), so they report as `unknown`.
     *
     * And one milestone-1 gap: a Zephyr fatal error with CONFIG_RESET_ON_FATAL_ERROR reboots
     * through esp_restart(), which the hardware records as a software reset. ESP-IDF tells
     * a panic apart with a hint its panic handler writes before restarting, and Zephyr does
     * not build that handler, so a panic currently reads as `software` here. Setting the
     * hint is the job of the fatal-error hook in sentry_coredump_zephyr.c, next milestone.
     */
    if (cause & RESET_CPU_LOCKUP) {
        return SENTRY_RESET_PANIC;
    }
    if (cause & RESET_WATCHDOG) {
        return SENTRY_RESET_WDT;
    }
    if (cause & RESET_BROWNOUT) {
        return SENTRY_RESET_BROWNOUT;
    }
    if (cause & RESET_SOFTWARE) {
        return SENTRY_RESET_SOFTWARE;
    }
    if (cause & RESET_PIN) {
        return SENTRY_RESET_EXTERNAL;
    }
    if (cause & RESET_LOW_POWER_WAKE) {
        return SENTRY_RESET_DEEPSLEEP;
    }
    if (cause & RESET_POR) {
        return SENTRY_RESET_POWERON;
    }
    if (cause & RESET_DEBUG) {
        return SENTRY_RESET_JTAG;
    }
    return SENTRY_RESET_UNKNOWN;
}

static sentry_reset_reason_t read_reset_reason(void)
{
    uint32_t cause = 0;
    if (hwinfo_get_reset_cause(&cause) != 0) {
        return SENTRY_RESET_UNKNOWN;
    }
    /* Cleared after reading because on most SoCs the cause register is sticky: left alone,
     * the next boot reports this boot's cause OR'd with its own. ESP32 derives the cause
     * afresh on every boot and returns -ENOSYS here, which is not an error. */
    (void)hwinfo_clear_reset_cause();
    return map_reset_reason(cause);
}

#    if SENTRY_HEAP_STATS
static bool heap_stats(struct sys_memory_stats *stats)
{
    return sys_heap_runtime_stats_get(&_system_heap.heap, stats) == 0;
}
#    endif

void sentry_device_info_get(sentry_device_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    snprintf(out->chip_model, sizeof(out->chip_model), "%s", SENTRY_CHIP_MODEL);
    snprintf(out->arch, sizeof(out->arch), "%s", SENTRY_CHIP_ARCH);
#    if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32)
    /* Already `major * 100 + minor` (hal/efuse_hal.c), the same convention the ESP32 port
     * reports, so v0.2 reads as 2 on both. */
    out->chip_revision = (uint16_t)efuse_hal_chip_revision();
#    endif
    /* The cores this image schedules on, not the cores on the die: the non-SMP
     * esp32s3_devkitc/esp32s3/procpu build reports 1 on a dual-core S3, because the second
     * core runs a separate image (or nothing) and this one never sees it. */
    out->cpu_cores = (uint8_t)CONFIG_MP_MAX_NUM_CPUS;

    snprintf(out->sdk_version, sizeof(out->sdk_version), "%s", KERNEL_VERSION_STRING);

    /* The factory id is the only identifier that survives reflashing and a storage erase, so
     * it is what the fleet is keyed on. On ESP32 hwinfo returns the 6-byte eFuse MAC — the
     * same bytes, and so the same id, the ESP32 port reports. Other SoCs return longer
     * serials; the first six bytes keep the field the 12 hex digits the wire contract and
     * every existing saved search expect. */
    uint8_t id[6] = { 0 };
    ssize_t id_len = hwinfo_get_device_id(id, sizeof(id));
    if (id_len > 0) {
        char *cursor = out->device_id;
        for (ssize_t i = 0; i < id_len && i < (ssize_t)sizeof(id); i++) {
            snprintf(cursor, 3, "%02x", id[i]);
            cursor += 2;
        }
    }

#    if DT_HAS_CHOSEN(zephyr_flash)
    out->flash_size_bytes = (uint32_t)DT_REG_SIZE(DT_CHOSEN(zephyr_flash));
#    endif

#    if SENTRY_HEAP_STATS
    struct sys_memory_stats stats;
    if (heap_stats(&stats)) {
        out->total_heap_bytes = (uint32_t)(stats.free_bytes + stats.allocated_bytes);
    }
#    endif

    out->reset_reason = read_reset_reason();
}

uint32_t sentry_device_free_heap(void)
{
#    if SENTRY_HEAP_STATS
    struct sys_memory_stats stats;
    if (heap_stats(&stats)) {
        return (uint32_t)stats.free_bytes;
    }
#    endif
    return 0;
}

uint32_t sentry_device_min_free_heap(void)
{
#    if SENTRY_HEAP_STATS
    /* Zephyr tracks the high-water mark of allocation rather than the low-water mark of free
     * space, but over a fixed-size pool they are the same fact: the pool size minus the most
     * that was ever handed out at once. */
    struct sys_memory_stats stats;
    if (heap_stats(&stats)) {
        size_t total = stats.free_bytes + stats.allocated_bytes;
        return (uint32_t)(total - stats.max_allocated_bytes);
    }
#    endif
    return 0;
}

uint64_t sentry_device_uptime_ms(void) { return (uint64_t)k_uptime_get(); }

uint64_t sentry_device_uptime_us(void)
{
    /* From ticks rather than scaled up from milliseconds, so spans keep whatever resolution
     * the system timer has rather than rounding every one of them to a whole millisecond. */
    return (uint64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

bool sentry_device_random(uint8_t *out, size_t len)
{
    if (!out) {
        return false;
    }
#    if defined(CONFIG_TEST_RANDOM_GENERATOR)
    /* The test generator is a deterministic xoroshiro seeded from the clock: every device
     * that boots the same image mints the same sequence of event ids, and ingest
     * deduplicates a whole fleet's boot events into one. No ids is better than ids that
     * lie about how often something happened. */
    (void)len;
    return false;
#    elif defined(CONFIG_CSPRNG_ENABLED)
    /* The entropy driver, through the CSPRNG. On ESP32 that is the hardware RNG, with the
     * same caveat as under ESP-IDF: it is only truly entropic once RF (WiFi or BT) has been
     * enabled, and seeded but not entropic before. Fine for event ids, which need
     * uniqueness across a fleet rather than unpredictability, and the MAC-derived device id
     * keeps two boards apart even in the degenerate case. */
    return sys_csrand_get(out, len) == 0;
#    else
    sys_rand_get(out, len);
    return true;
#    endif
}

/*
 * The wall clock. Zephyr has no battery-backed clock on these parts either, so until the
 * application sets SYS_CLOCK_REALTIME it reads seconds since boot from an epoch of 0. Same
 * 2020-09-13 threshold as the ESP32 port, so "never set" reports 0 and ingest substitutes
 * its receive time rather than dating every event 1970.
 */
#    define SENTRY_PLAUSIBLE_EPOCH_S 1600000000LL

uint64_t sentry_device_unix_time(void)
{
    struct timespec now;
    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &now) != 0 || now.tv_sec < SENTRY_PLAUSIBLE_EPOCH_S) {
        return 0;
    }
    return (uint64_t)now.tv_sec;
}

uint64_t sentry_device_unix_time_us(void)
{
    /* The same clock read the same way, so the two cannot disagree about whether it has
     * been set. */
    struct timespec now;
    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &now) != 0 || now.tv_sec < SENTRY_PLAUSIBLE_EPOCH_S) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

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
 * spelled out rather than taken from esp_attr.h to keep this file off the hal's headers for
 * something this small.
 *
 * Everywhere else it is Zephyr's `__noinit`: ordinary SRAM that startup does not zero. That
 * is best effort. A warm reset usually leaves SRAM as it was, but nothing promises it, and
 * a part that powers its SRAM down in reset hands back garbage — which the magic, layout id
 * and length checks below reject, so the worst case is the no-previous-boot answer rather
 * than a wrong one.
 */
#    if defined(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32) && defined(CONFIG_SOC_RTC_SLOW_MEM_SUPPORTED)
#        define SENTRY_PERSIST_TRACE __attribute__((section(".rtc_noinit.sentry_trace")))
#        define SENTRY_PERSIST_BLOCK __attribute__((section(".rtc_noinit.sentry_block")))
#    else
#        define SENTRY_PERSIST_TRACE __noinit
#        define SENTRY_PERSIST_BLOCK __noinit
#    endif

/*
 * The magic word is what distinguishes "we stored this" from whatever was in that memory at
 * power-on. Without it, uninitialised memory would be read as a trace id and every event
 * would join a fictional trace. Same values and layout as the ESP32 port.
 */
#    define SENTRY_RTC_TRACE_MAGIC 0x53545243u /* "STRC" */
/* Headroom over sizeof(sentry_trace_context_t), so the next field added to it does not also
 * need this file touched; see the ESP32 port. */
#    define SENTRY_RTC_TRACE_CAP 160

static SENTRY_PERSIST_TRACE struct {
    uint32_t magic;
    uint32_t len;
    uint8_t bytes[SENTRY_RTC_TRACE_CAP];
} g_rtc_trace;

void sentry_device_trace_persist(const void *bytes, size_t len)
{
    if (!bytes || len == 0 || len > sizeof(g_rtc_trace.bytes)) {
        /* Too large to keep: the crash arrives without a trace. Truncating would be worse —
         * half a trace id looks exactly like one that points somewhere. */
        g_rtc_trace.magic = 0;
        return;
    }
    memcpy(g_rtc_trace.bytes, bytes, len);
    g_rtc_trace.len = (uint32_t)len;
    g_rtc_trace.magic = SENTRY_RTC_TRACE_MAGIC;
}

void sentry_device_trace_recover(void *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    memset(out, 0, len);
    if (g_rtc_trace.magic == SENTRY_RTC_TRACE_MAGIC && g_rtc_trace.len == len) {
        memcpy(out, g_rtc_trace.bytes, len);
    }
}

/*
 * The block the log ring lives in, so the lines leading up to a panic are still there to
 * explain it. Sized like the ESP32 port's; on the S3 that is about 1 KB of its 8 KB of RTC
 * slow memory.
 */
#    ifndef SENTRY_RTC_BLOCK_CAP
#        define SENTRY_RTC_BLOCK_CAP 1024
#    endif
#    define SENTRY_RTC_BLOCK_MAGIC 0x53424c4bu /* "SBLK" */

static SENTRY_PERSIST_BLOCK struct {
    uint32_t magic;
    uint32_t layout_id;
    uint32_t len;
    uint8_t bytes[SENTRY_RTC_BLOCK_CAP];
} g_rtc_block;

void *sentry_device_persistent_block(size_t bytes, uint32_t layout_id, bool *recovered)
{
    if (recovered) {
        *recovered = false;
    }
    if (bytes == 0 || bytes > sizeof(g_rtc_block.bytes)) {
        return NULL;
    }

    /* All three have to agree: the magic alone answers "did somebody store something here",
     * which after an OTA is exactly the question that misleads — the bytes are real, they
     * are simply a different shape now. */
    if (g_rtc_block.magic == SENTRY_RTC_BLOCK_MAGIC && g_rtc_block.layout_id == layout_id
        && g_rtc_block.len == (uint32_t)bytes) {
        if (recovered) {
            *recovered = true;
        }
        return g_rtc_block.bytes;
    }

    memset(g_rtc_block.bytes, 0, bytes);
    g_rtc_block.magic = SENTRY_RTC_BLOCK_MAGIC;
    g_rtc_block.layout_id = layout_id;
    g_rtc_block.len = (uint32_t)bytes;
    return g_rtc_block.bytes;
}

#endif /* __ZEPHYR__ */
