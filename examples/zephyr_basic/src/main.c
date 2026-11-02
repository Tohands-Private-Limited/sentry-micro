/*
 * sentry-micro — zephyr_basic
 *
 * The Zephyr counterpart of examples/wifi_basic's boot path: initialise sentry-micro from a
 * DSN, print what the SDK worked out about this boot — the ingest endpoint, the device id,
 * and why the chip came up — then report that boot as one event through the serial relay.
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

/* -DSENTRY_DEMO_CRASH=1 to crash deliberately once the boot event is out. */
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
 * crashing. A volatile read is a value it cannot know. In this milestone the next boot
 * reports only the reset reason, with no backtrace — this exists to show exactly what a
 * Zephyr fatal error reads back as, which today is `software` (see README.md, known gaps).
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

    report_boot();

#if SENTRY_DEMO_CRASH
    /* Only when this boot did not itself follow a crash, so one power-on produces one
     * crash-and-report cycle rather than a loop. With this milestone a Zephyr panic reads
     * back as a software reset, so this guard does not hold yet and the demo loops — power
     * it off once you have seen what it reports. */
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
