/*
 * Wrapped whole, includes and all, because PlatformIO compiles every source under src/ for the
 * Arduino builds as well, and those define ESP_PLATFORM too; there it is an empty translation
 * unit. On ESP-IDF it is compiled only with CONFIG_SENTRY_MICRO_TRANSPORT_WIFI (the root
 * CMakeLists.txt).
 */
#if defined(ESP_PLATFORM) && !defined(ARDUINO) && !defined(__ZEPHYR__)

#    include "sentry_transport_wifi.h"

#    include <limits.h>
#    include <stdio.h>
#    include <string.h>
#    include <strings.h> /* strcasecmp */

#    include "sdkconfig.h"

#    include "esp_err.h"
#    include "esp_http_client.h"
#    include "esp_netif.h"
#    if defined(CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE)
#        include "esp_crt_bundle.h"
#    endif

#    include "../../core/sentry_http.h"
#    include "../../sentry_micro.h"

#    ifndef CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS
#        define CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS 10000
#    endif

/*
 * With verification off the transport hands ESP-TLS no CA at all, and ESP-TLS then refuses
 * every handshake ("No server verification option set") unless it was told to skip the check.
 * Refused here so that shows up in the build rather than as a device that never delivers.
 */
#    if !defined(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY)                                              \
        && !defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
#        error                                                                                     \
            "CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY is off: also set CONFIG_ESP_TLS_INSECURE=y and CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y, or ESP-TLS refuses every connection"
#    endif

static void log_line(const char *message)
{
    sentry_logger_fn logger = sentry_get_logger();
    if (logger) {
        logger(message);
    }
}

/** The two back-off headers, collected by the event handler while the head is read. */
typedef struct {
    uint32_t retry_after_ms;
    uint32_t rate_limits_ms;
} wifi_reply_t;

/*
 * Called by esp_http_client on the caller's task, once per response header while
 * esp_http_client_fetch_headers() reads the head. Only the two that say how long to back off
 * are kept, read by the same parsers as every other transport's.
 */
static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data || !event->header_key
        || !event->header_value) {
        return ESP_OK;
    }
    wifi_reply_t *reply = (wifi_reply_t *)event->user_data;
    if (strcasecmp(event->header_key, "Retry-After") == 0) {
        reply->retry_after_ms = sentry_http_retry_after_ms(event->header_value);
    } else if (strcasecmp(event->header_key, "X-Sentry-Rate-Limits") == 0) {
        reply->rate_limits_ms = sentry_http_rate_limits_ms(event->header_value);
    }
    return ESP_OK;
}

/*
 * A connect that failed, with what ESP-TLS knows about why. The HTTP layer reports DNS, TCP
 * and handshake failures all as ESP_ERR_HTTP_CONNECT; the TLS error code is what tells a
 * refused certificate from a host that never answered.
 */
static void log_connect_failure(esp_http_client_handle_t client, esp_err_t err)
{
    int tls_code = 0;
    int tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
    char line[128];
    snprintf(line, sizeof(line), "wifi transport: connect failed (%s, tls error 0x%x, flags 0x%x)",
        esp_err_to_name(err), (unsigned)tls_code, (unsigned)tls_flags);
    log_line(line);
}

static bool wifi_is_available(void *ctx)
{
    (void)ctx;
    /*
     * An address, not just an association: between the association and the DHCP lease the
     * interface is up with nothing to send from, and a send in that window fails on connect
     * rather than reporting "no route" — which is what the auto-select chain assumes this
     * answer means. Same reasoning as the Arduino transport's WiFi.localIP() check. The
     * default interface is the one lwIP routes through, and NULL until one exists.
     */
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif || !esp_netif_is_netif_up(netif)) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* What ESP-TLS is to trust for this request. Set only on an https URL. */
static void set_trust(const sentry_wifi_transport_t *t, esp_http_client_config_t *config)
{
    if (!t->verify) {
        /* Nothing to verify against: ESP-TLS skips the check under
         * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY, which the build insists on above. The host
         * name still goes out as SNI, which ingest needs to answer at all. */
        return;
    }
    if (t->ca_pem) {
        /* 0 means "PEM, NUL-terminated": esp_http_client takes a non-zero length as DER. */
        config->cert_pem = t->ca_pem;
        config->cert_len = 0;
        return;
    }
#    if defined(CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE)
    config->crt_bundle_attach = esp_crt_bundle_attach;
#    else
    config->cert_pem = sentry_ca_digicert_global_root_g2;
    config->cert_len = 0;
#    endif
}

static sentry_response_t wifi_send(
    void *ctx, const char *url, const sentry_headers_t *headers, const uint8_t *body, size_t len)
{
    sentry_wifi_transport_t *t = (sentry_wifi_transport_t *)ctx;
    if (!t || !url || !body || len == 0 || len > (size_t)INT_MAX) {
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }
    if (!wifi_is_available(t)) {
        /* Not an error: the caller should buffer this and try again later. */
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    /* Whitelist before anything touches the network. Defaults to the DSN's host, so this
     * holds without the application configuring anything. */
    const sentry_dsn_t *dsn = sentry_get_dsn();
    const char *allowed = t->allowed_host;
    if (!allowed && dsn && dsn->valid) {
        allowed = dsn->host;
    }
    if (!allowed || !sentry_url_host_matches(url, allowed)) {
        log_line("wifi transport refused a URL outside the configured ingest host");
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    bool https = strncmp(url, "https://", 8) == 0;
    if (!https && strncmp(url, "http://", 7) != 0) {
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }
    if (!https && !t->allowed_host) {
        /* Refused, not sent. Over plain HTTP the auth header — which carries the key that can
         * write to the project — crosses the network in clear, so it is allowed only to a
         * host the application named itself, as on Zephyr and Arduino. REJECTED rather than
         * UNAVAILABLE so the core does not buffer and retry what no wait can fix. */
        log_line("wifi transport refused plain http to the DSN host; "
                 "call sentry_wifi_transport_set_allowed_host() to permit it");
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }
    if (https && !t->verify && !t->warned_insecure) {
        t->warned_insecure = true;
        log_line("TLS certificate verification is OFF (CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY=n)");
    }

    wifi_reply_t reply;
    memset(&reply, 0, sizeof(reply));

    esp_http_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = (int)t->timeout_ms;
    config.user_agent = SENTRY_MICRO_SDK_USER_AGENT;
    /* None are followed, as on Zephyr: ingest never sends one, and following it would carry
     * the auth header wherever it pointed. The open/write/fetch sequence below does not
     * follow redirects anyway; this keeps it so if that ever changes. */
    config.disable_auto_redirect = true;
    config.event_handler = on_http_event;
    config.user_data = &reply;
    if (https) {
        set_trust(t, &config);
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        /* Out of memory, or a URL esp_http_client cannot parse: a problem on this device,
         * not a missing route. */
        log_line("wifi transport: could not create the HTTP client");
        return sentry_response_make(SENTRY_SEND_ERROR);
    }

    const char *content_type = headers && headers->content_type ? headers->content_type
                                                                : "application/x-sentry-envelope";
    if (esp_http_client_set_header(client, "Content-Type", content_type) != ESP_OK
        || esp_http_client_set_header(
               client, "X-Sentry-Auth", headers && headers->auth ? headers->auth : "")
            != ESP_OK) {
        log_line("wifi transport: could not set the request headers");
        esp_http_client_cleanup(client);
        return sentry_response_make(SENTRY_SEND_ERROR);
    }

    /* DNS, the TCP connect and, for https, the handshake with its certificate check. */
    esp_err_t err = esp_http_client_open(client, (int)len);
    if (err != ESP_OK) {
        /*
         * No route, as on Zephyr and Arduino. A refused certificate lands here too, and that
         * is still the right call for the usual cause: a captive portal answering in ingest's
         * place, which clears once the network does.
         */
        log_connect_failure(client, err);
        esp_http_client_cleanup(client);
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    size_t written = 0;
    while (written < len) {
        int sent
            = esp_http_client_write(client, (const char *)body + written, (int)(len - written));
        if (sent <= 0) {
            /* The connection went away mid-request, which the other transports report as
             * no route as well. */
            log_line("wifi transport: send failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
        }
        written += (size_t)sent;
    }

    /* Reads the status line and every header, firing on_http_event() for each. The body is
     * never read: ingest's is a small JSON event id the SDK has no use for. */
    int64_t fetched = esp_http_client_fetch_headers(client);
    int status = fetched < 0 ? 0 : esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status <= 0) {
        /* Sent, but no answer that parses: a timeout, a dropped connection or a garbled
         * reply. A genuine error, not a missing route. */
        log_line("wifi transport: no HTTP status line in the reply");
    }
    return sentry_http_response_for_status(status, reply.retry_after_ms, reply.rate_limits_ms);
}

static const char *wifi_name(void *ctx)
{
    (void)ctx;
    return "wifi";
}

const sentry_transport_t *sentry_wifi_transport_init(
    sentry_wifi_transport_t *t, const char *ca_pem, uint32_t timeout_ms)
{
    if (!t) {
        return NULL;
    }
    t->timeout_ms = timeout_ms ? timeout_ms : CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS;
    t->allowed_host = NULL;
    t->ca_pem = ca_pem;
#    if defined(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY)
    t->verify = true;
#    else
    t->verify = false;
#    endif
    t->warned_insecure = false;
    t->transport.send = wifi_send;
    t->transport.is_available = wifi_is_available;
    t->transport.name = wifi_name;
    t->transport.ctx = t;
    return &t->transport;
}

void sentry_wifi_transport_set_allowed_host(sentry_wifi_transport_t *t, const char *host)
{
    if (t) {
        t->allowed_host = host;
    }
}

#endif /* ESP_PLATFORM && !ARDUINO && !__ZEPHYR__ */
