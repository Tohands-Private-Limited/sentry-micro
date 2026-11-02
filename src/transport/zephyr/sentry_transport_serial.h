/**
 * Serial relay transport for Zephyr — deliver through a host on the other end of a UART.
 *
 * The same wire protocol as the Arduino `SerialTransport`, byte for byte, so the same
 * `scripts/serial_relay.py` serves both:
 *
 *     device                                     host (scripts/serial_relay.py)
 *     ------                                     -----------------------------
 *     @SENTRY-RELAY/1 <url> <auth> <ct> <body>   -> decode, check host, POST to Sentry
 *     @SENTRY-RELAY-RESULT/1 200 0               <- HTTP status and any retry-after
 *
 * Every field is base64, so the line framing needs no escaping. See
 * `transport/sentry_transport_serial.hpp` for the design; this is the plain-C rendering of
 * it, for a platform with no Arduino `Stream` to write to.
 *
 * Two hazards exist here that the Arduino version did not have, both because Zephyr shares
 * the console UART between several owners:
 *
 * - **Log output lands inside a request.** printk() and the logging subsystem write to the
 *   same UART. A 4 KB envelope is about 5.5 KB of base64, roughly 450 ms on the wire at
 *   115200 baud, and a log line emitted from another thread in that window is spliced into
 *   the middle of the request line: the relay then decodes garbage and the event is lost.
 *   Deferred logging makes this likely, since its thread flushes whenever it gets to run.
 *   `CONFIG_LOG_MODE_MINIMAL` (what examples/zephyr_basic uses) prints synchronously in the
 *   caller's context, so a log line can only interleave if another thread logs mid-send;
 *   the robust fix is a UART of its own for the relay.
 *
 * - **Anything else reading RX steals the answer.** The result line is read with
 *   `uart_poll_in()`, which takes bytes from whoever asks first. The shell, or any
 *   interrupt-driven console input on the same UART, will consume some or all of
 *   `@SENTRY-RELAY-RESULT/1 ...` and the request times out as "no route" even though it was
 *   delivered. Do not enable the shell on the relay's UART.
 *
 * All storage is the caller's: the struct is the whole transport, and nothing is allocated.
 */
#ifndef SENTRY_MICRO_TRANSPORT_SERIAL_ZEPHYR_H_INCLUDED
#define SENTRY_MICRO_TRANSPORT_SERIAL_ZEPHYR_H_INCLUDED

#if defined(__ZEPHYR__)

#    include <zephyr/device.h>

#    include "../../core/sentry_transport.h"

#    ifdef __cplusplus
extern "C" {
#    endif

/** Protocol markers, shared with `scripts/serial_relay.py`. Bump the version together. */
#    ifndef SENTRY_RELAY_REQUEST_PREFIX
#        define SENTRY_RELAY_REQUEST_PREFIX "@SENTRY-RELAY/1 "
#    endif
#    ifndef SENTRY_RELAY_RESULT_PREFIX
#        define SENTRY_RELAY_RESULT_PREFIX "@SENTRY-RELAY-RESULT/1 "
#    endif

/**
 * Largest envelope this transport will relay, in bytes. A sanity bound rather than a buffer
 * size — encoding is streamed in fixed chunks — so a corrupted length cannot make the device
 * spend minutes shovelling garbage down a 115200-baud link. Same bound as on Arduino.
 */
#    define SENTRY_SERIAL_MAX_ENVELOPE_BYTES 4096

/**
 * The transport and its configuration, in one caller-owned struct.
 *
 * The SDK stores a pointer to `transport`, so this must outlive it: file scope or `static`,
 * never a stack local.
 */
typedef struct {
    /** What `sentry_set_transport()` is given. Its `ctx` points back at this struct. */
    sentry_transport_t transport;
    /** The UART requests are written to and answers read from. */
    const struct device *uart;
    /** How long to wait for the host's answer, covering its own DNS, TLS and round trip. */
    uint32_t timeout_ms;
} sentry_serial_transport_t;

/**
 * Fill in `t` and return the transport to register:
 *
 *     static sentry_serial_transport_t serial;
 *     sentry_set_transport(sentry_serial_transport_init(&serial, NULL, 0));
 *
 * `uart` NULL means the console (`zephyr,console` in the devicetree), which is the point:
 * one cable, no extra wiring. `timeout_ms` 0 means `CONFIG_SENTRY_MICRO_SERIAL_TIMEOUT_MS`
 * (15000 by default).
 *
 * Returns `&t->transport`, or NULL when `t` is NULL. A UART that is not ready is not an error
 * here: the transport then reports itself unavailable, and the core buffers or drops exactly
 * as it would for a downed radio.
 */
const sentry_transport_t *sentry_serial_transport_init(
    sentry_serial_transport_t *t, const struct device *uart, uint32_t timeout_ms);

#    ifdef __cplusplus
}
#    endif

#endif /* __ZEPHYR__ */

#endif /* SENTRY_MICRO_TRANSPORT_SERIAL_ZEPHYR_H_INCLUDED */
