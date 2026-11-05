/**
 * WiFi transport for ESP-IDF — HTTPS POST straight to Sentry ingest, through `esp_http_client`.
 *
 * The ESP-IDF counterpart of `transport/zephyr/sentry_transport_wifi.h`, with the same names
 * and the same contract, so an application moving between the two changes an include path and
 * nothing else. It receives a URL, two headers and a byte buffer, makes one HTTP/1.1 request,
 * and translates the answer with `sentry_http_response_for_status()`:
 *
 *     2xx                  SENTRY_SEND_OK
 *     429                  SENTRY_SEND_RATE_LIMITED, with Retry-After / X-Sentry-Rate-Limits
 *     other 4xx            SENTRY_SEND_REJECTED      (retrying cannot fix it)
 *     5xx, garbled, slow   SENTRY_SEND_ERROR
 *     no address, no DNS,
 *     no connection        SENTRY_SEND_UNAVAILABLE   (buffer and try later)
 *
 * "WiFi" is the name it shares with the other ports; nothing in it is specific to the radio.
 * It sends over whichever interface lwIP routes through, and reports itself available when
 * the default interface (`esp_netif_get_default_netif()`) is up with an IPv4 address, so it
 * serves an Ethernet board unchanged.
 *
 * **What TLS checks.** With `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY` (the default) the server's
 * chain must lead to the trusted root, and its certificate must name the ingest host. The
 * root is DigiCert Global Root G2, bundled with sentry-micro
 * (`transport/sentry_ca_digicert_global_root_g2.c`, the same bytes the Zephyr transport
 * trusts) and valid until 2038-01-15, unless the application passes its own. With
 * `CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE` it trusts ESP-IDF's certificate bundle instead,
 * which outlives any one root but costs tens of KB of flash. The certificate's *dates* are
 * not checked unless `CONFIG_MBEDTLS_HAVE_TIME_DATE` is set, which is off by default, so
 * verification works before SNTP has set the clock.
 *
 * With verification off the connection is encrypted but not authenticated: anything on the
 * path can pose as ingest and collect the DSN key from the auth header. ESP-TLS refuses a
 * connection with nothing to verify against unless `CONFIG_ESP_TLS_INSECURE` and
 * `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` are set, and the build refuses the combination
 * that would fail every send. The transport logs the insecure state once, on its first send.
 *
 * **Do not call this from a panic handler.** `esp_http_client` and the TLS handshake allocate
 * from the heap on every request, and neither the heap nor the network stack is trustworthy
 * after a crash. The design does not need it: ESP-IDF writes the core dump, and the crash is
 * reported on the next boot from ordinary runtime.
 *
 * The handshake runs on the caller's task, which needs the stack for it: certificate
 * verification and the key exchange go several KB deep. `examples/idf_basic` gives the main
 * task 12 KB.
 *
 * All storage the transport keeps is the caller's: the struct is the whole transport.
 */
#ifndef SENTRY_MICRO_TRANSPORT_WIFI_IDF_H_INCLUDED
#define SENTRY_MICRO_TRANSPORT_WIFI_IDF_H_INCLUDED

/* ESP-IDF only. Arduino defines ESP_PLATFORM as well and has transport/sentry_transport_wifi.hpp
 * instead; Zephyr has transport/zephyr/sentry_transport_wifi.h. */
#if defined(ESP_PLATFORM) && !defined(ARDUINO) && !defined(__ZEPHYR__)

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
    /** `esp_http_client`'s network timeout: each connect, send and receive step. */
    uint32_t timeout_ms;
    /** The only host this transport will POST to. NULL means the DSN's host. */
    const char *allowed_host;
    /** The root certificate, PEM. NULL means the bundled root, or ESP-IDF's bundle with
     *  `CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE`. */
    const char *ca_pem;
    /** Whether the server's certificate is checked; from Kconfig at init. */
    bool verify;
    /** The insecure warning is logged once per transport, not once per event. */
    bool warned_insecure;
} sentry_wifi_transport_t;

/**
 * Fill in `t` and return the transport to register:
 *
 *     static sentry_wifi_transport_t wifi;
 *     sentry_set_transport(sentry_wifi_transport_init(&wifi, NULL, 0));
 *
 * `ca_pem` is a PEM root certificate to trust *instead of* the default, NUL-terminated and
 * with static lifetime: it is read on every handshake, not copied. NULL means DigiCert Global
 * Root G2 as bundled, or ESP-IDF's certificate bundle when
 * `CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE` is set; a CA passed here wins over both. Ignored
 * when `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY` is off.
 *
 * `timeout_ms` 0 means `CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS` (10000 by default). It is
 * `esp_http_client`'s network timeout, which bounds the connect, the handshake, each write of
 * the request and the wait for the response headers, one step at a time. DNS resolution is
 * lwIP's own and is not covered.
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

#endif /* ESP_PLATFORM && !ARDUINO && !__ZEPHYR__ */

#endif /* SENTRY_MICRO_TRANSPORT_WIFI_IDF_H_INCLUDED */
