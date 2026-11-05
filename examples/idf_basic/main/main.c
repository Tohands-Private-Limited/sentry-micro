/*
 * sentry-micro — idf_basic
 *
 * The ESP-IDF counterpart of examples/zephyr_basic: initialise sentry-micro from a DSN, print
 * what the SDK worked out about this boot — the ingest endpoint, the device id, and why the
 * chip came up — then join WiFi, set the clock over SNTP, and report either the crash the
 * previous boot died of, with its backtrace, or else this boot as one event, posted straight
 * to ingest over HTTPS.
 *
 * Plain C, ESP-IDF only. See README.md for the build and flash commands and for what you
 * should see.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Explicit since ESP-IDF 6.0: esp_event.h no longer pulls in the FreeRTOS headers.
 * FreeRTOS.h on its own, because it has to come first. */
#include "freertos/FreeRTOS.h"

#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <sentry_micro.h>
#include <transport/esp-idf/sentry_transport_wifi.h>

#ifndef SENTRY_DSN
#    define SENTRY_DSN "https://examplePublicKey@o0.ingest.sentry.io/0"
#endif

/* Identifies this build in Sentry. Set SENTRY_MICRO_RELEASE in the environment to override. */
#ifndef SENTRY_RELEASE
#    define SENTRY_RELEASE "sentry-micro-example@" SENTRY_MICRO_VERSION
#endif

/* idf.py -DSENTRY_DEMO_CRASH=1 build: crash deliberately once the boot event is out, and
 * report it on the next boot. */
#ifndef SENTRY_DEMO_CRASH
#    define SENTRY_DEMO_CRASH 0
#endif

/* From SENTRY_MICRO_WIFI_SSID / _PASSWORD at configure time. No password: an open network. */
#ifndef WIFI_SSID
#    define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#    define WIFI_PASSWORD ""
#endif

/*
 * On the chip's native USB-Serial-JTAG the device re-enumerates on every reset, and anything
 * written before the host has reopened the port is dropped. Hold the boot report until the
 * host has had time to: a USB enumeration plus a reader's reopen poll is well under this.
 */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#    define CONSOLE_SETTLE_MS 3000
#else
#    define CONSOLE_SETTLE_MS 0
#endif

/* Association, the WPA handshake and a DHCP lease, which a slow router takes most of. */
#define WIFI_CONNECT_TIMEOUT_MS 30000
/* SNTP is one UDP exchange, and a lost answer is ordinary; each try costs at most this. */
#define TIME_SYNC_TIMEOUT_MS 5000
#define TIME_SYNC_ATTEMPTS 3
#define TIME_SYNC_SERVER "pool.ntp.org"

#define WIFI_GOT_IP_BIT BIT0

/* File scope, not a local: the SDK stores a pointer to the transport inside it. */
static sentry_wifi_transport_t wifi_transport;

static EventGroupHandle_t wifi_events;

/* The SDK's own narration, through the same console as everything else. */
static void console_logger(const char *message) { printf("[sentry] %s\n", message); }

static void print_sentry_state(void)
{
    const sentry_device_info_t *dev = sentry_get_device_info();
    const sentry_dsn_t *dsn = sentry_get_dsn();

    printf("\n");
    printf("── sentry-micro ──────────────────────────────\n");
    printf("sdk           : %s %s\n", SENTRY_MICRO_SDK_NAME, sentry_sdk_version());
    printf("enabled       : %s\n", sentry_is_enabled() ? "yes" : "no");
    printf("release       : %s\n", SENTRY_RELEASE);
    printf("ingest host   : %s (tls: %s)\n", dsn->host, dsn->is_secure ? "yes" : "no");
    printf("envelope url  : %s\n", sentry_get_envelope_url());
    printf("── device ────────────────────────────────────\n");
    printf("chip          : %s rev %u, %u core(s)\n", dev->chip_model, (unsigned)dev->chip_revision,
        (unsigned)dev->cpu_cores);
    printf("device id     : %s\n", dev->device_id);
    printf("esp-idf       : %s\n", dev->sdk_version);
    printf("reset reason  : %s%s\n", sentry_reset_reason_name(dev->reset_reason),
        sentry_reset_reason_is_crash(dev->reset_reason) ? "  <- previous boot crashed" : "");
    printf("──────────────────────────────────────────────\n");
    printf("\n");
}

/** Human-readable name for a delivery result, for the console. */
static const char *send_result_name(sentry_send_result_t result)
{
    switch (result) {
    case SENTRY_SEND_OK:
        return "delivered";
    case SENTRY_SEND_UNAVAILABLE:
        return "no route (is WiFi up?)";
    case SENTRY_SEND_REJECTED:
        return "rejected (do not retry)";
    case SENTRY_SEND_RATE_LIMITED:
        return "rate limited";
    case SENTRY_SEND_ERROR:
    default:
        return "error";
    }
}

/*
 * Runs on the default event loop's task. A dropped connection, or a join that failed, is
 * retried at once and for as long as the board runs; until it comes back the transport
 * reports "no route".
 */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT);
        printf("[wifi] disconnected (reason %u); retrying\n", event ? (unsigned)event->reason : 0u);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP_BIT);
    }
}

/* Copy a C string into one of wifi_config_t's fixed byte arrays, which need no terminator
 * when full. */
static void copy_field(uint8_t *field, size_t size, const char *value)
{
    size_t len = strlen(value);
    memcpy(field, value, len < size ? len : size);
}

/** Bring up WiFi in station mode, join WIFI_SSID and wait for an address. */
static bool wifi_connect(void)
{
    if (WIFI_SSID[0] == '\0') {
        printf("[wifi] no SSID: set SENTRY_MICRO_WIFI_SSID and run idf.py reconfigure\n");
        return false;
    }

    /* The WiFi driver keeps its calibration and settings in NVS. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        err = esp_netif_init();
    }
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK) {
        printf("[wifi] could not start the network stack: %s\n", esp_err_to_name(err));
        return false;
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        printf("[wifi] could not start the driver: %s\n", esp_err_to_name(err));
        return false;
    }

    wifi_events = xEventGroupCreate();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    copy_field(config.sta.ssid, sizeof(config.sta.ssid), WIFI_SSID);
    copy_field(config.sta.password, sizeof(config.sta.password), WIFI_PASSWORD);
    /* The weakest security accepted: WPA2 or better with a password, anything without. */
    config.sta.threshold.authmode = WIFI_PASSWORD[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        printf("[wifi] could not start station mode: %s\n", esp_err_to_name(err));
        return false;
    }

    printf("[wifi] connecting to \"%s\" ...\n", WIFI_SSID);
    TickType_t started = xTaskGetTickCount();
    EventBits_t bits = xEventGroupWaitBits(
        wifi_events, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_GOT_IP_BIT)) {
        printf("[wifi] not connected after %ums\n", (unsigned)WIFI_CONNECT_TIMEOUT_MS);
        return false;
    }

    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    esp_netif_get_ip_info(sta, &ip);
    printf("[wifi] connected: ip=" IPSTR " in %ums\n", IP2STR(&ip.ip),
        (unsigned)pdTICKS_TO_MS(xTaskGetTickCount() - started));
    return true;
}

/**
 * Set the wall clock from SNTP.
 *
 * Not needed for TLS: mbedTLS is built without date checks by default, so the certificate
 * verifies with the clock at 1970. It is what gives events a real timestamp, and what
 * sessions, logs and metrics wait for before they are sent at all. Failure is therefore not
 * fatal; the events go out with ingest's receive time instead.
 */
static bool sync_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(TIME_SYNC_SERVER);
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        printf("[time] could not start SNTP\n");
        return false;
    }

    bool synced = false;
    for (unsigned attempt = 1; attempt <= TIME_SYNC_ATTEMPTS && !synced; attempt++) {
        synced = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS)) == ESP_OK;
        if (!synced) {
            printf(
                "[time] SNTP attempt %u of %u timed out\n", attempt, (unsigned)TIME_SYNC_ATTEMPTS);
        }
    }
    if (!synced) {
        printf("[time] no time from SNTP: events go out without a timestamp\n");
        return false;
    }

    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    printf("[time] synced from %s: %04d-%02d-%02d %02d:%02d:%02d UTC\n", TIME_SYNC_SERVER,
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return true;
}

/**
 * Report the crash the previous boot left in the core dump partition, if there is one.
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
        snprintf(message, sizeof(message), "%s in task %s", type, crash.task_name);
    } else {
        snprintf(message, sizeof(message), "%s", type);
    }
    event.message = message;

    printf("\n");
    printf("── crash recovered from the previous boot ────\n");
    printf("exception     : %s\n", crash.exception_type);
    printf("task          : %s\n", crash.task_name);
    printf("pc            : 0x%08x\n", (unsigned)crash.exception_pc);
    if (crash.exception_addr_valid) {
        printf("accessing     : 0x%08x\n", (unsigned)crash.exception_addr);
    }
    printf("backtrace     : %u frames%s\n", (unsigned)crash.frame_count,
        crash.truncated ? " (truncated)" : "");
    for (uint32_t i = 0; i < crash.frame_count; i++) {
        printf("  #%u 0x%08x\n", (unsigned)i, (unsigned)crash.frames[i]);
    }
    printf("──────────────────────────────────────────────\n");

    /* Same 2 KB as the SDK's own envelope buffer, and on this stack for the same reason: no
     * heap on the crash path. CONFIG_ESP_MAIN_TASK_STACK_SIZE has the room. */
    char envelope[2048];
    size_t needed = sentry_envelope_write(envelope, sizeof(envelope), &event);
    if (needed == 0 || needed >= sizeof(envelope)) {
        printf("[sentry] crash envelope needs %u bytes\n", (unsigned)needed);
        return true;
    }

    sentry_response_t response = sentry_send_envelope((const uint8_t *)envelope, needed);
    printf("[sentry] crash report: %s (http %u)\n", send_result_name(response.result),
        (unsigned)response.http_status);

    /* Erase only once the envelope is somewhere durable — delivered, or in the buffer.
     * Erasing on a plain failure would throw away the crash we just recovered. */
    if (response.result == SENTRY_SEND_OK || sentry_buffered_count() > 0) {
        sentry_coredump_erase();
        printf("[sentry] core dump erased\n");
    } else {
        printf("[sentry] core dump kept for the next boot\n");
    }
    printf("[sentry] crash event %s\n", event_id);
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

    printf(
        "[sentry] %s (http %u", send_result_name(response.result), (unsigned)response.http_status);
    if (response.retry_after_ms > 0) {
        printf(", retry after %ums", (unsigned)response.retry_after_ms);
    }
    printf(")\n");
}

#if SENTRY_DEMO_CRASH
/*
 * A deliberate null-pointer store, three calls deep, as in the Arduino and Zephyr examples.
 *
 * `noinline` on each level so the optimiser cannot collapse them into one frame. The address
 * is read from a volatile rather than written as a constant 0: GCC proves a store to a literal
 * null pointer is undefined, treats the whole call chain as unreachable, and may delete it. A
 * volatile read is a value it cannot know. ESP-IDF's panic handler writes the core dump and
 * restarts; the next boot reports it through report_crash(), with this chain in its
 * backtrace.
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

void app_main(void)
{
    printf("\n\nsentry-micro idf_basic example\n");

    /* Before anything else that might crash: the reset reason read here belongs to the
     * *previous* boot, so the earlier this runs, the more of the firmware it covers. */
    sentry_set_logger(console_logger);

    sentry_options_t options;
    sentry_options_defaults(&options);
    options.dsn = SENTRY_DSN;
    options.release = SENTRY_RELEASE;
    options.environment = "development";
    options.board = CONFIG_IDF_TARGET;
    options.debug = true;

    if (!sentry_init(&options)) {
        /* A bad DSN disables the SDK rather than half-configuring it; the firmware carries
         * on regardless. Crash reporting must never be load-bearing. */
        printf("[sentry] init failed — check SENTRY_MICRO_DSN\n");
    }

    if (CONSOLE_SETTLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_SETTLE_MS));
    }

    print_sentry_state();

    /* Network and clock before anything is reported, so the first event already carries a
     * real timestamp. A failed connect is not fatal: the transport then reports "no route",
     * and with no offline storage the event is dropped; a recovered core dump is kept for
     * the next boot in that case. */
    bool time_synced = false;
    if (wifi_connect()) {
        time_synced = sync_time();
    }
    /* NULL: trust the DigiCert root bundled with sentry-micro, which ingest chains to. */
    sentry_set_transport(sentry_wifi_transport_init(&wifi_transport, NULL, 0));
#if defined(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY)
    printf("[sentry] transport: wifi, HTTPS straight to ingest (certificate check on)\n");
#else
    printf("[sentry] transport: wifi, HTTPS straight to ingest (certificate check OFF)\n");
#endif
    /* Session updates wait for the date, and the main loop's first flush is ten seconds
     * away; a demo crash comes sooner than that. Flushing now sends the last boot's close
     * and this boot's start the moment they can go, ahead of the crash report. 0: nothing
     * from an offline buffer, which this example does not have anyway. The SDK's own log
     * prints one "[sentry] session ..." line per update. */
    if (time_synced) {
        sentry_flush(0);
    }

    /* A recovered crash is the more interesting event, and reporting both on the same boot
     * would double up. */
    if (!report_crash()) {
        report_boot();
    }
    /* What the TLS handshake and the report left of the main task's stack: the figure to
     * size CONFIG_ESP_MAIN_TASK_STACK_SIZE by. */
    printf("[mem] main task stack never used: %u bytes\n",
        (unsigned)uxTaskGetStackHighWaterMark(NULL));

#if SENTRY_DEMO_CRASH
    /* Only when this boot did not itself follow a crash, so one power-on produces one
     * crash-and-report cycle rather than a loop. */
    if (!sentry_reset_reason_is_crash(sentry_get_device_info()->reset_reason)) {
        printf("\n[demo] crashing deliberately in 3s ...\n");
        /* Recorded and never flushed, so they are still in the log ring when the board
         * crashes. The next boot reports them as the crash event's breadcrumbs, the trail
         * that led to it, rather than as ordinary logs. */
        sentry_log(SENTRY_LEVEL_INFO, "demo: boot reported, crashing in 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        sentry_log(SENTRY_LEVEL_WARNING, "demo: calling demo_crash_outer()");
        demo_crash_outer();
    }
#endif

    while (true) {
        /* On an interval, not in a tight loop: with no route a send blocks for the
         * transport's whole timeout. With no offline storage registered this only moves
         * session updates, metrics and logs, which wait for the wall clock SNTP set. */
        vTaskDelay(pdMS_TO_TICKS(10000));
        sentry_flush(2);
    }
}
