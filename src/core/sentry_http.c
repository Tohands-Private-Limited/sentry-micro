#include "sentry_http.h"

#include <stdlib.h>
#include <string.h>

uint32_t sentry_http_retry_after_ms(const char *value)
{
    if (!value || value[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    long seconds = strtol(value, &end, 10);
    if (end == value || seconds <= 0) {
        return 0;
    }
    /* A day, and the same on every target: `long` is 32 bits on the chip and 64 on a laptop,
     * but an out-of-range value saturates to at least this either way. */
    const long max_seconds = 24L * 60 * 60;
    if (seconds > max_seconds) {
        seconds = max_seconds;
    }
    return (uint32_t)seconds * 1000u;
}

uint32_t sentry_http_rate_limits_ms(const char *value)
{
    /* strtol() stops at the first ':', so the leading delay of the first entry is exactly
     * what the shared parser reads. */
    return sentry_http_retry_after_ms(value);
}

void sentry_http_head_init(sentry_http_head_t *head)
{
    if (head) {
        memset(head, 0, sizeof(*head));
    }
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/** The value of `line` if it is the header `name`, with leading whitespace skipped; else NULL. */
static const char *header_value(const char *line, const char *name)
{
    size_t i = 0;
    for (; name[i]; i++) {
        if (lower(line[i]) != lower(name[i])) {
            return NULL;
        }
    }
    if (line[i] != ':') {
        return NULL;
    }
    const char *value = line + i + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    return value;
}

/* One complete line of the head. */
static void take_line(sentry_http_head_t *head, const char *line)
{
    if (head->status == 0) {
        /* "HTTP/1.1 200 OK". Anything else in first place is not an HTTP answer: stop, with
         * the status still 0 for the caller to report as an error. */
        if (strncmp(line, "HTTP/1.", 7) == 0 && line[7] != '\0' && line[8] == ' ') {
            head->status = (int)strtol(line + 9, NULL, 10);
        }
        if (head->status <= 0) {
            head->status = 0;
            head->done = true;
        }
        return;
    }
    if (line[0] == '\0') {
        head->done = true;
        return;
    }
    const char *value = header_value(line, "Retry-After");
    if (value) {
        head->retry_after_ms = sentry_http_retry_after_ms(value);
    }
    value = header_value(line, "X-Sentry-Rate-Limits");
    if (value) {
        head->rate_limits_ms = sentry_http_rate_limits_ms(value);
    }
}

bool sentry_http_head_feed(sentry_http_head_t *head, const char *data, size_t len)
{
    if (!head) {
        return false;
    }
    for (size_t i = 0; i < len && data && !head->done; i++) {
        char c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (head->line_len + 1 < sizeof(head->line)) {
                head->line[head->line_len++] = c;
            }
            continue;
        }
        head->line[head->line_len] = '\0';
        head->line_len = 0;
        take_line(head, head->line);
    }
    return head->done;
}
