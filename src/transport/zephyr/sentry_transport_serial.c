/*
 * Wrapped whole, includes and all, because PlatformIO compiles every source under src/ for
 * the Arduino builds as well; there it is an empty translation unit.
 */
#if defined(__ZEPHYR__)

#    include "sentry_transport_serial.h"

#    include <stdlib.h>
#    include <string.h>

#    include <zephyr/devicetree.h>
#    include <zephyr/drivers/uart.h>
#    include <zephyr/kernel.h>

#    include "../../core/sentry_base64.h"
#    include "../../sentry_micro.h"

#    ifndef CONFIG_SENTRY_MICRO_SERIAL_TIMEOUT_MS
#        define CONFIG_SENTRY_MICRO_SERIAL_TIMEOUT_MS 15000
#    endif

#    if DT_HAS_CHOSEN(zephyr_console)
#        define SENTRY_SERIAL_CONSOLE DEVICE_DT_GET(DT_CHOSEN(zephyr_console))
#    else
#        define SENTRY_SERIAL_CONSOLE NULL
#    endif

static void write_text(const struct device *uart, const char *text)
{
    while (*text) {
        uart_poll_out(uart, (unsigned char)*text++);
    }
}

/**
 * Write one base64-encoded field, followed by a space.
 *
 * Encoded in fixed chunks straight to the UART rather than into a buffer: a 4 KB envelope
 * would need a 5.4 KB base64 buffer on the caller's stack, on top of the 2 KB envelope
 * already there. This way the cost is 65 bytes regardless of envelope size.
 *
 * The chunk is a multiple of 3 so that base64 padding can only ever occur on the final,
 * short chunk — padding mid-stream would corrupt the encoding.
 */
static void write_field(const struct device *uart, const uint8_t *data, size_t len)
{
    const size_t chunk_bytes = 48;
    char encoded[SENTRY_BASE64_ENCODED_LEN(48) + 1];

    for (size_t offset = 0; offset < len; offset += chunk_bytes) {
        size_t take = len - offset;
        if (take > chunk_bytes) {
            take = chunk_bytes;
        }
        sentry_base64_encode(encoded, sizeof(encoded), data + offset, take);
        write_text(uart, encoded);
    }
    uart_poll_out(uart, ' ');
}

static void write_text_field(const struct device *uart, const char *text)
{
    write_field(uart, (const uint8_t *)text, strlen(text));
}

/**
 * Read one newline-terminated line into `buf`, or return false at the deadline.
 *
 * Keeps reading past lines that are not ours: the relay script passes device output through
 * to a terminal and a user may be typing, so the result is not guaranteed to be the very
 * next thing received.
 */
static bool read_line(const struct device *uart, char *buf, size_t cap, int64_t deadline_ms)
{
    size_t len = 0;
    while (k_uptime_get() < deadline_ms) {
        unsigned char c;
        if (uart_poll_in(uart, &c) != 0) {
            /* Nothing yet, or a driver that cannot poll at all — the deadline handles both.
             * Yield rather than spin: a cooperative or high-priority caller starving the
             * idle thread for 15 s is how a watchdog reset happens. */
            k_sleep(K_MSEC(2));
            continue;
        }
        if (c == '\n') {
            buf[len] = '\0';
            return true;
        }
        if (c == '\r') {
            continue;
        }
        if (len + 1 < cap) {
            buf[len++] = (char)c;
        }
        /* An over-long line is truncated rather than abandoned; the prefix check rejects it
         * if it was not ours. */
    }
    return false;
}

static sentry_response_t response_with(
    sentry_send_result_t result, long status, long retry_after_ms)
{
    sentry_response_t response = sentry_response_make(result);
    response.http_status = (uint16_t)status;
    response.retry_after_ms = (uint32_t)retry_after_ms;
    return response;
}

static sentry_response_t serial_send(
    void *ctx, const char *url, const sentry_headers_t *headers, const uint8_t *body, size_t len)
{
    const sentry_serial_transport_t *t = (const sentry_serial_transport_t *)ctx;
    if (!t || !t->uart || !url || !body || len == 0) {
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }
    if (!device_is_ready(t->uart)) {
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }
    if (len > SENTRY_SERIAL_MAX_ENVELOPE_BYTES) {
        /* Refuse rather than truncate: half an envelope is not a smaller event, it is a
         * malformed one that ingest will reject after the host has done the work. */
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    /* The host performs the request, so it — not the device — is what a rogue URL would
     * abuse. The script enforces its own whitelist, and this is the matching check on the
     * near side so neither end has to trust the other. */
    const sentry_dsn_t *dsn = sentry_get_dsn();
    if (!dsn || !dsn->valid || !sentry_url_host_matches(url, dsn->host)) {
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    const char *auth = headers && headers->auth ? headers->auth : "";
    const char *content_type = headers && headers->content_type ? headers->content_type
                                                                : "application/x-sentry-envelope";

    /* uart_poll_out() returns once each byte is in the FIFO, so there is no separate flush:
     * by the time the newline is written the request is on its way. */
    write_text(t->uart, SENTRY_RELAY_REQUEST_PREFIX);
    write_text_field(t->uart, url);
    write_text_field(t->uart, auth);
    write_text_field(t->uart, content_type);
    write_field(t->uart, body, len);
    uart_poll_out(t->uart, '\n');

    /* Wait for the host's verdict. A missing relay never answers, which times out into "no
     * route" — exactly what the caller should buffer and retry against. */
    int64_t deadline = k_uptime_get() + (int64_t)t->timeout_ms;
    char line[96];
    const size_t prefix_len = strlen(SENTRY_RELAY_RESULT_PREFIX);
    while (read_line(t->uart, line, sizeof(line), deadline)) {
        if (strncmp(line, SENTRY_RELAY_RESULT_PREFIX, prefix_len) != 0) {
            continue;
        }
        /* "<status> <retry_after_ms>" */
        char *cursor = line + prefix_len;
        long status = strtol(cursor, &cursor, 10);
        long retry_after_ms = strtol(cursor, NULL, 10);
        if (retry_after_ms < 0) {
            retry_after_ms = 0;
        }

        if (status <= 0) {
            /* The host reached nothing at all — its own network is down. */
            return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
        }
        if (status == 429) {
            return response_with(SENTRY_SEND_RATE_LIMITED, status, retry_after_ms);
        }
        if (status >= 200 && status < 300) {
            return response_with(SENTRY_SEND_OK, status, 0);
        }
        if (status >= 400 && status < 500) {
            return response_with(SENTRY_SEND_REJECTED, status, retry_after_ms);
        }
        return response_with(SENTRY_SEND_ERROR, status, retry_after_ms);
    }

    return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
}

/*
 * True when the UART is usable, which is all that can be known: nothing on a bare UART
 * distinguishes "no script running" from "script is thinking". An unanswered request times
 * out into SENTRY_SEND_UNAVAILABLE, the same signal the caller would act on anyway.
 */
static bool serial_is_available(void *ctx)
{
    const sentry_serial_transport_t *t = (const sentry_serial_transport_t *)ctx;
    return t && t->uart && device_is_ready(t->uart);
}

static const char *serial_name(void *ctx)
{
    (void)ctx;
    return "serial";
}

const sentry_transport_t *sentry_serial_transport_init(
    sentry_serial_transport_t *t, const struct device *uart, uint32_t timeout_ms)
{
    if (!t) {
        return NULL;
    }
    t->uart = uart ? uart : SENTRY_SERIAL_CONSOLE;
    t->timeout_ms = timeout_ms ? timeout_ms : CONFIG_SENTRY_MICRO_SERIAL_TIMEOUT_MS;
    t->transport.send = serial_send;
    t->transport.is_available = serial_is_available;
    t->transport.name = serial_name;
    t->transport.ctx = t;
    return &t->transport;
}

#endif /* __ZEPHYR__ */
