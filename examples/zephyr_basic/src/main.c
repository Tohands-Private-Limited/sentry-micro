/*
 * sentry-micro — zephyr_basic
 *
 * The Zephyr counterpart of examples/wifi_basic's boot and crash paths: initialise
 * sentry-micro from a DSN, print what the SDK worked out about this boot — the ingest
 * endpoint, the device id, and why the chip came up — then report either the crash the
 * previous boot died of, with its backtrace, or else this boot as one event, through the
 * serial relay.
 *
 * No WiFi here: the board reaches Sentry through the laptop it is plugged into, which runs
 * scripts/serial_relay.py on the same UART this console uses. See README.md for the build,
 * flash and relay commands, and for what this milestone does not do yet.
 */

#include <stdio.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <sentry_micro.h>
#include <transport/zephyr/sentry_transport_serial.h>

#ifndef SENTRY_DSN
#    define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"
#endif

/* Identifies this build in Sentry. Set SENTRY_MICRO_RELEASE in the environment to override. */
#ifndef SENTRY_RELEASE
#    define SENTRY_RELEASE "sentry-micro-example@" SENTRY_MICRO_VERSION
#endif

/* -DSENTRY_DEMO_CRASH=1 to crash deliberately once the boot event is out, and report it
 * on the next boot. */
#ifndef SENTRY_DEMO_CRASH
#    define SENTRY_DEMO_CRASH 0
#endif

/*
 * On the chip's native USB-Serial-JTAG (usb_serial.overlay) the device re-enumerates on every
 * reset, and anything written before the host has reopened the port is dropped by the driver.
 * Hold the boot report until the host has had time to: a USB enumeration plus the relay's
 * reopen poll is well under this.
 */
#if DT_HAS_CHOSEN(zephyr_console) && DT_NODE_EXISTS(DT_NODELABEL(usb_serial))                      \
    && DT_SAME_NODE(DT_CHOSEN(zephyr_console), DT_NODELABEL(usb_serial))
#    define CONSOLE_SETTLE_MS 3000
#else
#    define CONSOLE_SETTLE_MS 0
#endif

/* File scope, not a local: the SDK stores a pointer to the transport inside it. */
static sentry_serial_transport_t serial_transport;

/* The SDK's own narration, through the same console as everything else. */
static void printk_logger(const char *message) { printk("[sentry] %s\n", message); }

static void print_sentry_state(void)
{
    const sentry_device_info_t *dev = sentry_get_device_info();
    const sentry_dsn_t *dsn = sentry_get_dsn();

    printk("\n");
    printk("── sentry-micro ──────────────────────────────\n");
    printk("sdk           : %s %s\n", SENTRY_MICRO_SDK_NAME, sentry_sdk_version());
    printk("enabled       : %s\n", sentry_is_enabled() ? "yes" : "no");
    printk("release       : %s\n", SENTRY_RELEASE);
    printk("ingest host   : %s (tls: %s)\n", dsn->host, dsn->is_secure ? "yes" : "no");
    printk("envelope url  : %s\n", sentry_get_envelope_url());
    printk("── device ────────────────────────────────────\n");
    printk("chip          : %s rev %u, %u core(s)\n", dev->chip_model, (unsigned)dev->chip_revision,
        (unsigned)dev->cpu_cores);
    printk("device id     : %s\n", dev->device_id);
    printk("zephyr        : %s\n", dev->sdk_version);
    printk("reset reason  : %s%s\n", sentry_reset_reason_name(dev->reset_reason),
        sentry_reset_reason_is_crash(dev->reset_reason) ? "  <- previous boot crashed" : "");
    printk("──────────────────────────────────────────────\n");
    printk("\n");
}

/** Human-readable name for a delivery result, for the console. */
static const char *send_result_name(sentry_send_result_t result)
{
    switch (result) {
    case SENTRY_SEND_OK:
        return "delivered";
    case SENTRY_SEND_UNAVAILABLE:
        return "no route (is scripts/serial_relay.py running?)";
    case SENTRY_SEND_REJECTED:
        return "rejected (do not retry)";
    case SENTRY_SEND_RATE_LIMITED:
        return "rate limited";
    case SENTRY_SEND_ERROR:
    default:
        return "error";
    }
}

/**
 * Report the crash the previous boot recorded, if there is one.
 *
 * Returns true if a crash was reported, so the caller knows not to report the same boot a
 * second time as a plain "Device booted: panic".
 */
static bool report_crash(void)
{
    char event_id[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_t event;
    sentry_coredump_t crash;

    if (!sentry_event_prepare(&event, event_id)) {
        return false;
    }
    /* `crash` has to outlive the event, which points into it — hence both being locals of
     * this function rather than the event owning a copy. */
    if (!sentry_event_attach_coredump(&event, &crash)) {
        return false;
    }

    char message[128];
    const char *type = crash.exception_type[0] ? crash.exception_type : "Panic";
    if (crash.task_name[0]) {
        snprintf(message, sizeof(message), "%s in thread %s", type, crash.task_name);
    } else {
        snprintf(message, sizeof(message), "%s", type);
    }
    event.message = message;

    printk("\n");
    printk("── crash recovered from the previous boot ────\n");
    printk("exception     : %s\n", crash.exception_type);
    printk("thread        : %s\n", crash.task_name);
    printk("pc            : 0x%08x\n", (unsigned)crash.exception_pc);
    if (crash.exception_addr_valid) {
        printk("accessing     : 0x%08x\n", (unsigned)crash.exception_addr);
    }
    printk("backtrace     : %u frames%s\n", (unsigned)crash.frame_count,
        crash.truncated ? " (truncated)" : "");
    for (uint32_t i = 0; i < crash.frame_count; i++) {
        printk("  #%u 0x%08x\n", (unsigned)i, (unsigned)crash.frames[i]);
    }
    printk("──────────────────────────────────────────────\n");

    /* Same 2 KB as the SDK's own envelope buffer, and on this stack for the same reason: no
     * heap on the crash path. CONFIG_MAIN_STACK_SIZE has the room. */
    char envelope[2048];
    size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
    if (needed == 0 || needed >= sizeof(envelope)) {
        printk("[sentry] crash envelope needs %u bytes\n", (unsigned)needed);
        return true;
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    printk("[sentry] crash report: %s (http %u)\n", send_result_name(response.result),
        (unsigned)response.http_status);

    /* Erase only once the envelope is somewhere durable — delivered, or in the buffer.
     * Erasing on a plain failure would throw away the crash we just recovered. */
    if (response.result == SENTRY_SEND_OK || sentry_buffered_count() > 0) {
        sentry_coredump_erase();
        printk("[sentry] crash record erased\n");
    } else {
        printk("[sentry] crash record kept for the next boot\n");
    }
    printk("[sentry] crash event %s\n", event_id);
    return true;
}

static void report_boot(void)
{
    const sentry_device_info_t *dev = sentry_get_device_info();
    bool crashed = sentry_reset_reason_is_crash(dev->reset_reason);

    char message[96];
    snprintf(
        message, sizeof(message), "Device booted: %s", sentry_reset_reason_name(dev->reset_reason));

    /* A crash is fatal; an ordinary power-on is just news. Getting this wrong would either
     * page someone for a normal reboot or bury a real panic. */
    sentry_response_t response
        = sentry_capture_message(crashed ? SENTRY_LEVEL_FATAL : SENTRY_LEVEL_INFO, message);

    printk(
        "[sentry] %s (http %u", send_result_name(response.result), (unsigned)response.http_status);
    if (response.retry_after_ms > 0) {
        printk(", retry after %ums", (unsigned)response.retry_after_ms);
    }
    printk(")\n");
}

#if SENTRY_DEMO_CRASH
/*
 * A deliberate null-pointer store, three calls deep, as in the Arduino example.
 *
 * `noinline` on each level so the optimiser cannot collapse them into one frame. The address
 * is read from a volatile rather than written as a constant 0: the Zephyr SDK's GCC proves a
 * store to a literal null pointer is undefined, treats the whole call chain as unreachable,
 * and deletes the call to demo_crash_outer() — the board then carries on instead of
 * crashing. A volatile read is a value it cannot know. The next boot reports it through
 * report_crash(), with this chain in its backtrace.
 */
static volatile uintptr_t demo_crash_address = 0;

static void __attribute__((noinline)) demo_crash_innermost(void)
{
    volatile uint32_t *nowhere = (volatile uint32_t *)demo_crash_address;
    *nowhere = 0xdeadbeef;
}

static void __attribute__((noinline)) demo_crash_middle(void) { demo_crash_innermost(); }

static void __attribute__((noinline)) demo_crash_outer(void) { demo_crash_middle(); }
#endif

int main(void)
{
    printk("\n\nsentry-micro zephyr_basic example\n");

    /* Before anything else that might crash: the reset reason read here belongs to the
     * *previous* boot, so the earlier this runs, the more of the firmware it covers. */
    sentry_set_logger(printk_logger);

    sentry_options_t options;
    sentry_options_defaults(&options);
    options.dsn = SENTRY_DSN;
    options.release = SENTRY_RELEASE;
    options.environment = "development";
    options.board = CONFIG_BOARD;
    options.debug = true;

    if (!sentry_init(&options)) {
        /* A bad DSN disables the SDK rather than half-configuring it; the firmware carries
         * on regardless. Crash reporting must never be load-bearing. */
        printk("[sentry] init failed — check SENTRY_MICRO_DSN\n");
    }

    if (CONSOLE_SETTLE_MS > 0) {
        k_sleep(K_MSEC(CONSOLE_SETTLE_MS));
    }

    print_sentry_state();

    /* NULL: the console UART, which is the one the relay script is attached to. */
    sentry_set_transport(sentry_serial_transport_init(&serial_transport, NULL, 0));
    printk("[sentry] transport: serial relay on the console UART\n");

    /* A recovered crash is the more interesting event, and reporting both on the same boot
     * would double up. */
    if (!report_crash()) {
        report_boot();
    }

#if SENTRY_DEMO_CRASH
    /* Only when this boot did not itself follow a crash, so one power-on produces one
     * crash-and-report cycle rather than a loop. The fatal-error hook marks its reboot as a
     * panic, so the boot after the demo crash reads `panic` and stops here. */
    if (!sentry_reset_reason_is_crash(sentry_get_device_info()->reset_reason)) {
        printk("\n[demo] crashing deliberately in 3s ...\n");
        k_sleep(K_SECONDS(3));
        demo_crash_outer();
    }
#endif

    while (true) {
        /* On an interval, not in a tight loop: with no relay listening a send blocks for the
         * transport's whole timeout. With no offline storage registered this only moves
         * metrics and logs, and those wait for a wall clock this example never sets. */
        k_sleep(K_SECONDS(10));
        sentry_flush(2);
    }
    return 0;
}
