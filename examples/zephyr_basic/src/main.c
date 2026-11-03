/*
 * sentry-micro — zephyr_basic
 *
 * The Zephyr counterpart of examples/wifi_basic's boot and crash paths: initialise
 * sentry-micro from a DSN, print what the SDK worked out about this boot — the ingest
 * endpoint, the device id, and why the chip came up — then report either the crash the
 * previous boot died of, with its backtrace, or else this boot as one event.
 *
 * Two ways to reach Sentry, chosen when the firmware is built. With SENTRY_MICRO_WIFI_SSID in
 * the environment (and wifi.conf), the board joins that network, sets its clock over SNTP and
 * posts straight to ingest over HTTPS. Without it, it reaches Sentry through the laptop it is
 * plugged into, which runs scripts/serial_relay.py on the same UART this console uses. See
 * README.md for the build, flash and relay commands, and for what is not done yet.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <sentry_micro.h>

#if defined(WIFI_SSID)
#    include <time.h> /* struct timespec, gmtime(); <zephyr/sys/clock.h> does not include it */

#    include <zephyr/net/net_if.h>
#    include <zephyr/net/net_mgmt.h>
#    include <zephyr/net/sntp.h>
#    include <zephyr/net/wifi_mgmt.h>
#    include <zephyr/sys/clock.h>

#    include <transport/zephyr/sentry_transport_wifi.h>
#else
#    include <transport/zephyr/sentry_transport_serial.h>
#endif

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

#if defined(WIFI_SSID)
/* An open network when no password was given. */
#    ifndef WIFI_PASSWORD
#        define WIFI_PASSWORD ""
#    endif

/* Association, the WPA handshake and a DHCP lease, which a slow router takes most of. */
#    define WIFI_CONNECT_TIMEOUT_MS 30000
/* SNTP is one UDP exchange, and a lost answer is ordinary; each try costs at most this. */
#    define TIME_SYNC_TIMEOUT_MS 5000
#    define TIME_SYNC_ATTEMPTS 3
#    define TIME_SYNC_SERVER "pool.ntp.org"

/* File scope, not a local: the SDK stores a pointer to the transport inside it. */
static sentry_wifi_transport_t wifi_transport;

static struct net_mgmt_event_callback wifi_callback;
static K_SEM_DEFINE(wifi_connected, 0, 1);
static int wifi_connect_status;
#    define NO_ROUTE_HINT "no route (is WiFi up?)"
#else
/* File scope, not a local: the SDK stores a pointer to the transport inside it. */
static sentry_serial_transport_t serial_transport;
#    define NO_ROUTE_HINT "no route (is scripts/serial_relay.py running?)"
#endif

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
        return NO_ROUTE_HINT;
    case SENTRY_SEND_REJECTED:
        return "rejected (do not retry)";
    case SENTRY_SEND_RATE_LIMITED:
        return "rate limited";
    case SENTRY_SEND_ERROR:
    default:
        return "error";
    }
}

#if defined(WIFI_SSID)
/*
 * Runs on the network management thread. The connect outcome is handed to main, which prints
 * it, so its console lines come out in order with the rest of the boot.
 */
static void wifi_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(iface);
    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;
        wifi_connect_status = status ? status->status : -1;
        k_sem_give(&wifi_connected);
    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        /* After a connect: CONFIG_ESP32_WIFI_STA_RECONNECT rejoins on its own, and until it
         * does the transport reports "no route". */
        printk("[wifi] disconnected\n");
    }
}

/**
 * Join WIFI_SSID and wait for an address.
 *
 * Under CONFIG_WIFI_STA_AUTO_DHCPV4 (the default) the ESP32 driver starts DHCP itself and
 * raises the connect result only once the lease is bound, so success here means the board has
 * an address and DNS servers, not just an association.
 */
static bool wifi_connect(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    if (!iface) {
        printk("[wifi] no WiFi interface in this build\n");
        return false;
    }

    net_mgmt_init_event_callback(&wifi_callback, wifi_event,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_callback);

    struct wifi_connect_req_params params;
    memset(&params, 0, sizeof(params));
    params.ssid = (const uint8_t *)WIFI_SSID;
    params.ssid_length = (uint8_t)strlen(WIFI_SSID);
    params.psk = (const uint8_t *)WIFI_PASSWORD;
    params.psk_length = (uint8_t)strlen(WIFI_PASSWORD);
    params.security = params.psk_length ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
    params.channel = WIFI_CHANNEL_ANY;
    params.band = WIFI_FREQ_BAND_UNKNOWN;
    params.mfp = WIFI_MFP_OPTIONAL;
    /* The wait below is the bound that matters; the ESP32 driver does not read this. */
    params.timeout = SYS_FOREVER_MS;

    printk("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);
    int64_t started = k_uptime_get();
    int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (rc != 0) {
        printk("[wifi] connect request refused (%d)\n", rc);
        return false;
    }

    /*
     * One request, several results. The first join right after a flash came back as a
     * failure within a second on the bench; with CONFIG_ESP32_WIFI_STA_RECONNECT the driver
     * then keeps trying on its own and raises another result when it gets through, while a
     * second request in the meantime is refused with -EALREADY. So a failure here means "not
     * yet": keep waiting on the same request until the deadline.
     */
    for (;;) {
        int64_t remaining = WIFI_CONNECT_TIMEOUT_MS - (k_uptime_get() - started);
        if (remaining <= 0 || k_sem_take(&wifi_connected, K_MSEC(remaining)) != 0) {
            printk("[wifi] not connected after %ums\n", (unsigned)WIFI_CONNECT_TIMEOUT_MS);
            return false;
        }
        if (wifi_connect_status == WIFI_STATUS_CONN_SUCCESS) {
            break;
        }
        printk("[wifi] join failed (status %d) after %ums; the driver retries\n",
            wifi_connect_status, (unsigned)(k_uptime_get() - started));
    }

    char ip[NET_IPV4_ADDR_LEN] = "?";
    const struct net_in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (addr) {
        net_addr_ntop(NET_AF_INET, addr, ip, sizeof(ip));
    }
    printk("[wifi] connected: ip=%s in %ums\n", ip, (unsigned)(k_uptime_get() - started));
    return true;
}

/**
 * Set the wall clock from SNTP.
 *
 * Not needed for TLS here: mbedTLS is built without date checks (MBEDTLS_HAVE_TIME_DATE is
 * off), so the certificate verifies with the clock at 1970. It is what gives events a real
 * timestamp, and what logs and metrics wait for before they are sent at all. Failure is
 * therefore not fatal; the events go out with ingest's receive time instead.
 */
static bool sync_time(void)
{
    struct sntp_time now;
    int rc = -1;
    for (unsigned attempt = 1; attempt <= TIME_SYNC_ATTEMPTS && rc < 0; attempt++) {
        rc = sntp_simple(TIME_SYNC_SERVER, TIME_SYNC_TIMEOUT_MS, &now);
        if (rc < 0) {
            printk("[time] SNTP attempt %u of %u failed (%d)\n", attempt,
                (unsigned)TIME_SYNC_ATTEMPTS, rc);
        }
    }
    if (rc < 0) {
        printk("[time] no time from SNTP: events go out without a timestamp\n");
        return false;
    }

    struct timespec ts;
    ts.tv_sec = (time_t)now.seconds;
    /* The fraction is in units of 2^-32 s. */
    ts.tv_nsec = (long)(((uint64_t)now.fraction * 1000000000ull) >> 32);
    rc = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
    if (rc < 0) {
        printk("[time] could not set the clock (%d)\n", rc);
        return false;
    }

    time_t seconds = (time_t)now.seconds;
    const struct tm *utc = gmtime(&seconds);
    printk("[time] synced from %s: %04d-%02d-%02d %02d:%02d:%02d UTC\n", TIME_SYNC_SERVER,
        utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday, utc->tm_hour, utc->tm_min, utc->tm_sec);
    return true;
}
#endif

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

#if defined(WIFI_SSID)
    /* Network and clock before anything is reported, so the first event already carries a
     * real timestamp. A failed connect is not fatal: the transport then reports "no route",
     * and with no offline storage the event is dropped; a recovered crash record is kept for
     * the next boot in that case. */
    if (wifi_connect()) {
        sync_time();
    }
    /* NULL: trust the DigiCert root bundled with sentry-micro, which ingest chains to. */
    sentry_set_transport(sentry_wifi_transport_init(&wifi_transport, NULL, 0));
    printk("[sentry] transport: wifi, HTTPS straight to ingest (certificate check %s)\n",
        IS_ENABLED(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY) ? "on" : "OFF");
#else
    /* NULL: the console UART, which is the one the relay script is attached to. */
    sentry_set_transport(sentry_serial_transport_init(&serial_transport, NULL, 0));
    printk("[sentry] transport: serial relay on the console UART\n");
#endif

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
        /* On an interval, not in a tight loop: with no route a send blocks for the
         * transport's whole timeout. With no offline storage registered this only moves
         * metrics and logs, and those wait for a wall clock, which only the WiFi build sets. */
        k_sleep(K_SECONDS(10));
        sentry_flush(2);
    }
    return 0;
}
