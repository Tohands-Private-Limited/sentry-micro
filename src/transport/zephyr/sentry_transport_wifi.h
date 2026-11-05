/**
 * WiFi transport for Zephyr — HTTPS POST straight to Sentry ingest, over Zephyr sockets.
 *
 * The self-contained case, as `transport/sentry_transport_wifi.hpp` is on Arduino: a board on
 * WiFi needs nothing else to report. Everything Sentry-specific has already happened by the
 * time this runs; it receives a URL, two headers and a byte buffer, makes one HTTP/1.1
 * request, and translates the answer into a `sentry_response_t` with the same mapping as the
 * Arduino transport:
 *
 *     2xx                  SENTRY_SEND_OK
 *     429                  SENTRY_SEND_RATE_LIMITED, with Retry-After / X-Sentry-Rate-Limits
 *     other 4xx            SENTRY_SEND_REJECTED      (retrying cannot fix it)
 *     5xx, garbled, slow   SENTRY_SEND_ERROR
 *     no address, no DNS,
 *     no connection        SENTRY_SEND_UNAVAILABLE   (buffer and try later)
 *
 * The HTTP is written by hand rather than through Zephyr's HTTP client: one POST with a known
 * body and two headers worth reading back needs a few hundred bytes of stack, where the
 * client library brings its own buffers, callbacks and a parser for everything else.
 *
 * **What TLS checks.** With `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY` (the default) the server's
 * chain must lead to the trusted root, and its certificate must name the ingest host. The
 * root is DigiCert Global Root G2, bundled in the module
 * (`transport/sentry_ca_digicert_global_root_g2.c`, shared with the ESP-IDF transport) and
 * valid until 2038-01-15, unless the application passes its own. The certificate's *dates*
 * are not checked: mbedTLS does that only with `CONFIG_MBEDTLS_HAVE_TIME_DATE`, which is off
 * by default and stays off here, so verification works before SNTP has set the clock. An
 * expired certificate on a host that also presents the right name and chain is the residual
 * risk that buys.
 *
 * With verification off the connection is encrypted but not authenticated: a passive
 * listener reads nothing, but a captive portal or anyone on the path can pose as ingest and
 * collect the DSN key from the auth header. The transport logs that once, on its first send.
 *
 * **Do not call this from a panic handler.** The transport itself allocates nothing — every
 * buffer is fixed and on the caller's stack — but the TLS stack takes several kilobytes from
 * mbedTLS's heap (`CONFIG_MBEDTLS_HEAP_SIZE`) for every handshake, and the network stack
 * takes packet buffers from its pools. Neither is trustworthy after a crash. The design does
 * not need it: a crash is recorded by the fatal-error hook and reported on the next boot,
 * from ordinary runtime.
 *
 * The handshake runs on the caller's thread, which needs the stack for it: RSA verification
 * and the ECDHE exchange go several KB deep. `examples/zephyr_basic/wifi.conf` gives main
 * 16 KB.
 *
 * All storage is the caller's: the struct is the whole transport.
 */
#ifndef SENTRY_MICRO_TRANSPORT_WIFI_ZEPHYR_H_INCLUDED
#define SENTRY_MICRO_TRANSPORT_WIFI_ZEPHYR_H_INCLUDED

#if defined(__ZEPHYR__)

#    include "../../core/sentry_transport.h"
#    include "../sentry_ca_digicert_global_root_g2.h"

#    ifdef __cplusplus
extern "C" {
#    endif

/**
 * The transport and its configuration, in one caller-owned struct.
 *
 * The SDK stores a pointer to `transport`, so this must outlive it: file scope or `static`,
 * never a stack local. Fill it with `sentry_wifi_transport_init()`, not by hand.
 */
typedef struct {
    /** What `sentry_set_transport()` is given. Its `ctx` points back at this struct. */
    sentry_transport_t transport;
    /** Bound on each send, and on the whole wait for the response headers. Connecting is
     *  bounded by Zephyr instead; see `sentry_wifi_transport_init()`. */
    uint32_t timeout_ms;
    /** The only host this transport will POST to. NULL means the DSN's host. */
    const char *allowed_host;
    /** The root certificate, PEM. NULL means the bundled DigiCert Global Root G2. */
    const char *ca_pem;
    /** Whether the server's certificate is checked; from Kconfig at init. */
    bool verify;
    /** The insecure warning is logged once per transport, not once per event. */
    bool warned_insecure;
    /** The CA is registered with the TLS credential store on first use. */
    bool credential_added;
} sentry_wifi_transport_t;

/**
 * Fill in `t` and return the transport to register:
 *
 *     static sentry_wifi_transport_t wifi;
 *     sentry_set_transport(sentry_wifi_transport_init(&wifi, NULL, 0));
 *
 * `ca_pem` is a PEM root certificate to trust *instead of* the bundled one, NUL-terminated
 * and with static lifetime: the TLS credential store keeps the pointer, not a copy. NULL
 * means the bundled DigiCert Global Root G2. Ignored when
 * `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY` is off.
 *
 * `timeout_ms` 0 means `CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS` (10000 by default). It bounds
 * writing the request and waiting for the answer. The steps before that have per-socket
 * limits only in Zephyr's own Kconfig, which apply to every socket in the application:
 * `CONFIG_NET_SOCKETS_DNS_TIMEOUT` (2 s by default), `CONFIG_NET_SOCKETS_CONNECT_TIMEOUT`
 * (3 s) and `CONFIG_NET_SOCKETS_TLS_CONNECT_TIMEOUT` (10 s, the handshake). A send that
 * reaches nobody therefore blocks for at most the sum.
 *
 * The CA is registered under `CONFIG_SENTRY_MICRO_WIFI_TLS_SEC_TAG` on the first send. That
 * tag holds one certificate: a second transport initialised with a different CA reuses
 * whichever was registered first.
 *
 * Returns `&t->transport`, or NULL when `t` is NULL. Nothing here touches the network, so it
 * is safe before WiFi is up; until it is, the transport reports itself unavailable.
 */
const sentry_transport_t *sentry_wifi_transport_init(
    sentry_wifi_transport_t *t, const char *ca_pem, uint32_t timeout_ms);

/**
 * Restrict POSTs to `host` instead of the DSN's host.
 *
 * Only needed when routing through a proxy you control; the default is safer. Naming a host
 * here is also what permits a plain `http://` URL to it, for a self-hosted endpoint or a
 * relay on the local network: without it, http is refused. `host` is stored by pointer.
 */
void sentry_wifi_transport_set_allowed_host(sentry_wifi_transport_t *t, const char *host);

#    ifdef __cplusplus
}
#    endif

#endif /* __ZEPHYR__ */

#endif /* SENTRY_MICRO_TRANSPORT_WIFI_ZEPHYR_H_INCLUDED */
