/**
 * Reading an HTTP response: its status, and the two headers that tell a device to back off.
 *
 * Every transport that speaks HTTP itself has to read them, and they have to be read the same
 * way everywhere: a Zephyr board and an Arduino board reporting the same crash should hold
 * off for the same time when ingest says so. Hence one parser here, host-tested, rather than
 * a copy in each transport.
 *
 * In `core/` and pure. A transport whose HTTP stack hands it header values passes those to
 * the two value parsers; one that reads the socket itself feeds the raw bytes to
 * `sentry_http_head_feed()`, which finds the status and both headers and calls the same
 * parsers.
 */
#ifndef SENTRY_MICRO_HTTP_H_INCLUDED
#define SENTRY_MICRO_HTTP_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sentry_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse a `Retry-After` value into milliseconds.
 *
 * Sentry sends delta-seconds. The HTTP-date form is legal but would need a parsed clock the
 * device may not have, so an unparseable value yields 0 and the core falls back to its own
 * backoff — better than treating "I could not read this" as "retry immediately". A
 * fractional value keeps its whole seconds.
 *
 * Clamped to a day: a hostile or garbled header should not park the device for a month.
 * NULL and empty both yield 0.
 */
uint32_t sentry_http_retry_after_ms(const char *value);

/**
 * Parse an `X-Sentry-Rate-Limits` value into milliseconds.
 *
 * The header is `<retry_after>:<categories>:<scope>:...`, comma-separated. Only the leading
 * delay is used for now; per-category limits need the core to track which categories it is
 * holding, which is a later milestone. Same clamp as `sentry_http_retry_after_ms()`.
 */
uint32_t sentry_http_rate_limits_ms(const char *value);

/**
 * What an HTTP answer means to the core, the same for every transport that gets one:
 *
 *     2xx                  SENTRY_SEND_OK
 *     429                  SENTRY_SEND_RATE_LIMITED   (retrying cannot help yet)
 *     other 4xx            SENTRY_SEND_REJECTED       (retrying cannot fix it)
 *     anything else        SENTRY_SEND_ERROR
 *
 * `retry_after_ms` and `rate_limits_ms` are the two back-off headers as the parsers above
 * read them, 0 when absent. `Retry-After` wins when both are stated; the response carries
 * whichever applies on everything but a 2xx, where there is nothing to hold off from.
 *
 * Total: a `status` of 0 or below, which is how a transport says no status line arrived,
 * and one past 999, which no server sends, both come back as SENTRY_SEND_ERROR with
 * `http_status` 0. Whether a request reached a server at all is the transport's call, not
 * this one's: a route that never opened is SENTRY_SEND_UNAVAILABLE and never gets here.
 */
sentry_response_t sentry_http_response_for_status(
    int status, uint32_t retry_after_ms, uint32_t rate_limits_ms);

/** Longest response header line kept whole; longer ones are cut, and match nothing here. */
#define SENTRY_HTTP_LINE_BYTES 160

/**
 * What an HTTP response's head says that a transport needs: the status, and how long to back
 * off. Fed the raw bytes as they arrive, for a transport that reads the socket itself.
 *
 * TCP owes no alignment between what the server wrote and what one read returns, so a header
 * can arrive split anywhere, including between its "\r" and "\n". The parser keeps the
 * partial line between calls, in this struct, so the caller needs no buffer of its own beyond
 * whatever it reads into.
 */
typedef struct {
    /** The status code, or 0 while no status line has been read (or the first line was not
     *  one, which is how a reply that is not HTTP shows). */
    int status;
    /** From `Retry-After`, 0 if absent or unreadable. */
    uint32_t retry_after_ms;
    /** From `X-Sentry-Rate-Limits`, 0 if absent or unreadable. */
    uint32_t rate_limits_ms;
    /** Set at the blank line that ends the head, or at a first line that is not HTTP. */
    bool done;
    char line[SENTRY_HTTP_LINE_BYTES];
    size_t line_len;
} sentry_http_head_t;

/** Reset `head` for a new response. */
void sentry_http_head_init(sentry_http_head_t *head);

/**
 * Consume `len` bytes of the response. Returns true once the head is complete — the caller
 * stops reading there, since the body is of no interest — and ignores anything fed after.
 *
 * Header names are matched case-insensitively, as HTTP requires.
 */
bool sentry_http_head_feed(sentry_http_head_t *head, const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_HTTP_H_INCLUDED */
