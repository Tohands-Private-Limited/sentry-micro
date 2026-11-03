/*
 * Wrapped whole, includes and all, because PlatformIO compiles every source under src/ for
 * the Arduino builds as well; there it is an empty translation unit. On Zephyr it is compiled
 * only with CONFIG_SENTRY_MICRO_TRANSPORT_WIFI (zephyr/CMakeLists.txt), so an application
 * with no network stack still links.
 */
#if defined(__ZEPHYR__)

#    include "sentry_transport_wifi.h"

#    include <errno.h>
#    include <stdio.h>
#    include <string.h>

#    include <zephyr/kernel.h>
#    include <zephyr/net/net_if.h>
#    include <zephyr/net/socket.h>
#    include <zephyr/net/tls_credentials.h>

#    include "../../core/sentry_http.h"
#    include "../../sentry_micro.h"

#    ifndef CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS
#        define CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS 10000
#    endif
#    ifndef CONFIG_SENTRY_MICRO_WIFI_TLS_SEC_TAG
#        define CONFIG_SENTRY_MICRO_WIFI_TLS_SEC_TAG 1397050964
#    endif

/*
 * The CA is PEM, and mbedTLS reads only DER without its PEM parser: the credential would be
 * registered without complaint and every handshake would then fail on an empty trust store.
 * Refused here so that shows up in the build rather than on a deployed device. Offloaded TLS
 * (a modem's own stack) does not go through mbedTLS, hence the first condition.
 */
#    if defined(CONFIG_MBEDTLS) && defined(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY)                    \
        && !defined(CONFIG_MBEDTLS_PEM_PARSE_C)
#        error "the WiFi transport's CA is PEM: set CONFIG_MBEDTLS_PEM_PARSE_C=y"
#    endif

/*
 * Largest piece of the body handed to one send. The TLS layer cannot put more than its
 * outgoing record size into one record; a larger write comes back short and the loop in
 * send_all() goes round again, so this is about not asking for what cannot be done in one go
 * rather than about correctness.
 */
#    if defined(CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN)
#        define WIFI_SEND_CHUNK_BYTES CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN
#    else
#        define WIFI_SEND_CHUNK_BYTES 1024
#    endif

static void log_line(const char *message)
{
    sentry_logger_fn logger = sentry_get_logger();
    if (logger) {
        logger(message);
    }
}

static void log_failure(const char *what, int err)
{
    char line[96];
    snprintf(line, sizeof(line), "wifi transport: %s (%d)", what, err);
    log_line(line);
}

static sentry_response_t response_with(
    sentry_send_result_t result, uint16_t status, uint32_t retry_after_ms)
{
    sentry_response_t response = sentry_response_make(result);
    response.http_status = status;
    response.retry_after_ms = retry_after_ms;
    return response;
}

/** The parts of an envelope URL the request needs. `host` and `port` are copies, NUL-ended. */
typedef struct {
    bool https;
    char host[SENTRY_MICRO_MAX_HOST_LEN];
    char port[6];
    /** Points into the URL: everything from the first '/' after the host. */
    const char *path;
    /** True when the URL named its port, so the Host header has to as well. */
    bool explicit_port;
} wifi_url_t;

/*
 * Split `url` into scheme, host, port and path. The host was already checked against the
 * whitelist by sentry_url_host_matches(), which also refuses userinfo, so this only has to
 * find the pieces, not distrust them.
 */
static bool parse_url(const char *url, wifi_url_t *out)
{
    const char *cursor;
    if (strncmp(url, "https://", 8) == 0) {
        out->https = true;
        cursor = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->https = false;
        cursor = url + 7;
    } else {
        return false;
    }

    size_t host_len = strcspn(cursor, ":/");
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return false;
    }
    memcpy(out->host, cursor, host_len);
    out->host[host_len] = '\0';
    cursor += host_len;

    out->explicit_port = *cursor == ':';
    if (out->explicit_port) {
        cursor++;
        size_t port_len = strspn(cursor, "0123456789");
        if (port_len == 0 || port_len >= sizeof(out->port)) {
            return false;
        }
        memcpy(out->port, cursor, port_len);
        out->port[port_len] = '\0';
        cursor += port_len;
    } else {
        strcpy(out->port, out->https ? "443" : "80");
    }

    if (*cursor != '/' && *cursor != '\0') {
        return false;
    }
    out->path = *cursor ? cursor : "/";
    return true;
}

/** Write all of `data`, however the socket chooses to split it. False on error or timeout. */
static bool send_all(int sock, const void *data, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)data;
    while (len > 0) {
        size_t take = len < WIFI_SEND_CHUNK_BYTES ? len : WIFI_SEND_CHUNK_BYTES;
        ssize_t sent = zsock_send(sock, cursor, take, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        len -= (size_t)sent;
    }
    return true;
}

/*
 * Read the status line and headers into `head`, until the blank line that ends them.
 *
 * The body is never read — ingest's is a small JSON event id the SDK has no use for — and the
 * socket is closed with it still unread. Stops early on a timeout or a closed connection;
 * `head->status` is then whatever was read before that, 0 if no status line was.
 */
static void read_head(int sock, sentry_http_head_t *head, uint32_t timeout_ms)
{
    char chunk[128];
    /* SO_RCVTIMEO bounds each recv(); this bounds the whole head, so a server trickling a
     * byte at a time cannot hold the caller indefinitely. */
    int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;

    while (k_uptime_get() < deadline) {
        ssize_t got = zsock_recv(sock, chunk, sizeof(chunk), 0);
        if (got <= 0) {
            return;
        }
        if (sentry_http_head_feed(head, chunk, (size_t)got)) {
            return;
        }
    }
}

/* Every option the request needs, before connect(): TLS reads its options at the handshake. */
static bool configure_socket(const sentry_wifi_transport_t *t, const wifi_url_t *url, int sock)
{
    if (url->https) {
        if (t->verify) {
            sec_tag_t tags[] = { CONFIG_SENTRY_MICRO_WIFI_TLS_SEC_TAG };
            if (zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags, sizeof(tags))
                != 0) {
                return false;
            }
        }
        /* Set with verification off too, and not only so the name is checked: ingest closes
         * any handshake that does not say which host it wants (SNI), and this is what puts
         * the name in it. */
        if (zsock_setsockopt(
                sock, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, url->host, strlen(url->host) + 1)
            != 0) {
            return false;
        }
        int verify = t->verify ? ZSOCK_TLS_PEER_VERIFY_REQUIRED : ZSOCK_TLS_PEER_VERIFY_NONE;
        if (zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY, &verify, sizeof(verify))
            != 0) {
            return false;
        }
    }
    /* On a TLS socket the TLS layer implements both, around its own record reads and writes. */
    struct zsock_timeval timeout;
    timeout.tv_sec = (time_t)(t->timeout_ms / 1000);
    timeout.tv_usec = (suseconds_t)((t->timeout_ms % 1000) * 1000);
    if (zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &timeout, sizeof(timeout))
        != 0) {
        return false;
    }
    return zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &timeout, sizeof(timeout))
        == 0;
}

/*
 * Open a socket to `url`, TLS when it is https, and connect it. Returns the socket, or -1
 * with `*result` set to what the caller should report.
 */
static int open_connection(
    sentry_wifi_transport_t *t, const wifi_url_t *url, sentry_send_result_t *result)
{
    struct zsock_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = NET_AF_INET;
    hints.ai_socktype = NET_SOCK_STREAM;

    struct zsock_addrinfo *addr = NULL;
    int err = zsock_getaddrinfo(url->host, url->port, &hints, &addr);
    if (err != 0 || !addr) {
        /* No answer from DNS is no route, the same as the Arduino transport reports a
         * connection it could not open: worth retrying once the network is back. */
        log_failure("DNS lookup failed", err);
        *result = SENTRY_SEND_UNAVAILABLE;
        return -1;
    }

    int sock = zsock_socket(
        addr->ai_family, addr->ai_socktype, url->https ? NET_IPPROTO_TLS_1_2 : NET_IPPROTO_TCP);
    if (sock < 0) {
        /* Out of sockets or TLS contexts: a resource problem on this device, not a route. */
        log_failure("no socket", errno);
        zsock_freeaddrinfo(addr);
        *result = SENTRY_SEND_ERROR;
        return -1;
    }

    if (!configure_socket(t, url, sock)) {
        log_failure("could not configure the socket", errno);
        zsock_close(sock);
        zsock_freeaddrinfo(addr);
        *result = SENTRY_SEND_ERROR;
        return -1;
    }

    /* For TLS this is the handshake too, certificate check included. */
    err = zsock_connect(sock, addr->ai_addr, addr->ai_addrlen);
    int connect_errno = errno;
    zsock_freeaddrinfo(addr);
    if (err < 0) {
        /*
         * No route, as on Arduino, where a failed TLS connect reports "connection refused"
         * as well. A refused certificate lands here too, and that is still the right call
         * for the usual cause: a captive portal answering in ingest's place, which clears
         * once the network does. ECONNABORTED is the TLS layer's code for a failed
         * handshake, which is worth naming: it is also what a missing SNI or ciphersuite
         * in the build looks like.
         */
        log_failure(connect_errno == ECONNABORTED ? "TLS handshake failed" : "connect failed",
            connect_errno);
        zsock_close(sock);
        *result = SENTRY_SEND_UNAVAILABLE;
        return -1;
    }
    return sock;
}

/*
 * Register the CA with the TLS credential store, once. Not done at init so that init stays
 * free of side effects and safe before the network exists.
 */
static bool ensure_credential(sentry_wifi_transport_t *t)
{
    if (!t->verify || t->credential_added) {
        return true;
    }
    const char *pem = t->ca_pem ? t->ca_pem : sentry_ca_digicert_global_root_g2;
    /* The length includes the NUL: mbedTLS's PEM parser needs to see it. */
    int err = tls_credential_add(
        CONFIG_SENTRY_MICRO_WIFI_TLS_SEC_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, pem, strlen(pem) + 1);
    if (err != 0 && err != -EEXIST) {
        /* -EEXIST is a second transport, or a second init, finding the tag already filled,
         * which is success. Anything else is the store being full
         * (CONFIG_TLS_MAX_CREDENTIALS_NUMBER). */
        log_failure("could not register the CA certificate", -err);
        return false;
    }
    t->credential_added = true;
    return true;
}

static bool wifi_is_available(void *ctx)
{
    (void)ctx;
    /*
     * An address, not just an association. Under CONFIG_WIFI_STA_AUTO_DHCPV4 the interface
     * runs for a moment before DHCP has bound, and a send in that window fails on connect
     * rather than reporting "no route" — which is what the auto-select chain assumes this
     * answer means. Same reasoning as the Arduino transport's WiFi.localIP() check.
     */
    struct net_if *iface = net_if_get_first_wifi();
    return iface && net_if_is_up(iface)
        && net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

static sentry_response_t wifi_send(
    void *ctx, const char *url, const sentry_headers_t *headers, const uint8_t *body, size_t len)
{
    sentry_wifi_transport_t *t = (sentry_wifi_transport_t *)ctx;
    if (!t || !url || !body || len == 0) {
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

    wifi_url_t target;
    if (!parse_url(url, &target)) {
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }
    if (!target.https && !t->allowed_host) {
        /* Refused, not sent. Over plain HTTP the auth header — which carries the key that can
         * write to the project — crosses the network in clear, so it is allowed only to a
         * host the application named itself, such as a relay on its own LAN; the Arduino
         * transport makes the same trade with SENTRY_MICRO_WIFI_TLS. REJECTED rather than
         * UNAVAILABLE so the core does not buffer and retry something no amount of waiting
         * can fix. */
        log_line("wifi transport refused plain http to the DSN host; "
                 "call sentry_wifi_transport_set_allowed_host() to permit it");
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    if (target.https && !t->verify && !t->warned_insecure) {
        t->warned_insecure = true;
        log_line("TLS certificate verification is OFF (CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY=n)");
    }
    if (target.https && !ensure_credential(t)) {
        return sentry_response_make(SENTRY_SEND_ERROR);
    }

    /* The whole head in one write: on TLS each send is a record of its own, and the head
     * split across several would cost a record header and a TCP segment apiece. */
    char head[512];
    int head_len = snprintf(head, sizeof(head),
        "POST %s HTTP/1.1\r\n"
        "Host: %s%s%s\r\n"
        "User-Agent: %s\r\n"
        "Content-Type: %s\r\n"
        "X-Sentry-Auth: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        target.path, target.host, target.explicit_port ? ":" : "",
        target.explicit_port ? target.port : "", SENTRY_MICRO_SDK_USER_AGENT,
        headers && headers->content_type ? headers->content_type : "application/x-sentry-envelope",
        headers && headers->auth ? headers->auth : "", (unsigned)len);
    if (head_len < 0 || (size_t)head_len >= sizeof(head)) {
        /* Only an absurd path or auth header gets here, and it would be the same next time. */
        log_line("wifi transport: request head does not fit its buffer");
        return sentry_response_make(SENTRY_SEND_REJECTED);
    }

    sentry_send_result_t failure = SENTRY_SEND_ERROR;
    int sock = open_connection(t, &target, &failure);
    if (sock < 0) {
        return sentry_response_make(failure);
    }

    /* No redirects are followed, because none are read: ingest never sends one, and
     * following it would carry the auth header wherever it pointed. */
    if (!send_all(sock, head, (size_t)head_len) || !send_all(sock, body, len)) {
        /* The connection went away mid-request, which Arduino's HTTPClient reports as
         * "connection lost" and the transport there maps to no route. */
        log_failure("send failed", errno);
        zsock_close(sock);
        return sentry_response_make(SENTRY_SEND_UNAVAILABLE);
    }

    sentry_http_head_t reply;
    sentry_http_head_init(&reply);
    read_head(sock, &reply, t->timeout_ms);
    zsock_close(sock);

    if (reply.status <= 0) {
        /* Sent, but no answer that parses: a timeout, a dropped connection or a garbled
         * reply. A genuine error, not a missing route. */
        log_line("wifi transport: no HTTP status line in the reply");
        return sentry_response_make(SENTRY_SEND_ERROR);
    }
    /* A status line with its headers cut short still counts: the status is what decides,
     * and a missing Retry-After falls back to the core's own backoff. */

    uint16_t status = (uint16_t)reply.status;
    uint32_t retry_after_ms = reply.retry_after_ms ? reply.retry_after_ms : reply.rate_limits_ms;
    if (status == 429) {
        return response_with(SENTRY_SEND_RATE_LIMITED, status, retry_after_ms);
    }
    if (status >= 200 && status < 300) {
        return response_with(SENTRY_SEND_OK, status, 0);
    }
    if (status >= 400 && status < 500) {
        /* Ingest understood the request and will not accept it — a bad key, or a malformed
         * envelope. Retrying cannot change that, and a device in a boot loop retrying forever
         * is how a project gets rate-limited for everyone. */
        return response_with(SENTRY_SEND_REJECTED, status, retry_after_ms);
    }
    return response_with(SENTRY_SEND_ERROR, status, retry_after_ms);
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
    t->verify = IS_ENABLED(CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY);
    t->warned_insecure = false;
    t->credential_added = false;
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

#endif /* __ZEPHYR__ */
