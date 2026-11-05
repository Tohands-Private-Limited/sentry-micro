/**
 * Host tests for the back-off headers in `sentry_http.c`.
 *
 * What these numbers drive is how long a device stays quiet after ingest asks it to. Read
 * too short and a fleet in a boot loop hammers ingest until the project is rate-limited for
 * everyone; read too long and a device sits on a crash report for a day. Both the Arduino
 * and the Zephyr WiFi transports call these, so pinning them here pins both.
 *
 * `sentry_http_response_for_status()` is the ESP-IDF transport's status-to-result mapping, the
 * same table the Zephyr transport applies inline.
 *
 * The response-head parser is the Zephyr transport's alone (Arduino's HTTPClient has its
 * own), and is tested here because it cannot be on a board: what a socket read happens to
 * split, and where, is not something a test against a live server can choose.
 */

#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "sentry_http.h"

void setUp(void) { }
void tearDown(void) { }

static void test_retry_after_reads_delta_seconds(void)
{
    TEST_ASSERT_EQUAL_UINT32(60000u, sentry_http_retry_after_ms("60"));
    TEST_ASSERT_EQUAL_UINT32(1000u, sentry_http_retry_after_ms("1"));
    /* Leading whitespace is what an HTTP stack that does not trim the value hands over. */
    TEST_ASSERT_EQUAL_UINT32(5000u, sentry_http_retry_after_ms(" 5"));
}

static void test_retry_after_keeps_the_whole_seconds_of_a_fraction(void)
{
    TEST_ASSERT_EQUAL_UINT32(1000u, sentry_http_retry_after_ms("1.5"));
    /* Under a second rounds down to nothing stated, and the core's own backoff applies. */
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms("0.5"));
}

static void test_retry_after_ignores_garbage_and_non_positive_values(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms("soon"));
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms("0"));
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms("-30"));
}

static void test_retry_after_does_not_parse_an_http_date(void)
{
    /* Legal HTTP, but it needs a wall clock to mean anything; 0 hands the decision back to
     * the core rather than misreading the day of the month as a delay. */
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms("Wed, 21 Oct 2015 07:28:00 GMT"));
}

static void test_retry_after_is_clamped_to_a_day(void)
{
    TEST_ASSERT_EQUAL_UINT32(86400000u, sentry_http_retry_after_ms("86400"));
    TEST_ASSERT_EQUAL_UINT32(86400000u, sentry_http_retry_after_ms("86401"));
    TEST_ASSERT_EQUAL_UINT32(86400000u, sentry_http_retry_after_ms("2592000"));
    /* Past the range of a 32-bit long, where strtol() saturates on the chip. */
    TEST_ASSERT_EQUAL_UINT32(86400000u, sentry_http_retry_after_ms("99999999999"));
}

static void test_retry_after_treats_null_and_empty_as_not_stated(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_retry_after_ms(""));
}

static void test_rate_limits_reads_the_leading_delay(void)
{
    TEST_ASSERT_EQUAL_UINT32(60000u, sentry_http_rate_limits_ms("60:error:organization"));
    TEST_ASSERT_EQUAL_UINT32(2000u, sentry_http_rate_limits_ms("2::key"));
}

static void test_rate_limits_uses_the_first_entry_of_a_list(void)
{
    TEST_ASSERT_EQUAL_UINT32(
        120000u, sentry_http_rate_limits_ms("120:transaction:key, 2700:default;error:org"));
}

static void test_rate_limits_shares_the_clamp(void)
{
    TEST_ASSERT_EQUAL_UINT32(86400000u, sentry_http_rate_limits_ms("604800:error:project"));
}

static void test_rate_limits_treats_null_and_empty_as_not_stated(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_rate_limits_ms(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_rate_limits_ms(""));
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_rate_limits_ms(":error:organization"));
}

static const char INGEST_429[] = "HTTP/1.1 429 Too Many Requests\r\n"
                                 "Server: nginx\r\n"
                                 "x-sentry-rate-limits: 60:error:organization\r\n"
                                 "Content-Length: 0\r\n"
                                 "\r\n"
                                 "{\"id\":\"ignored\"}";

static void test_head_reads_status_and_stops_at_the_blank_line(void)
{
    static const char reply[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{}";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_INT(200, head.status);
    TEST_ASSERT_EQUAL_UINT32(0u, head.retry_after_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, head.rate_limits_ms);
}

static void test_head_matches_header_names_in_any_case(void)
{
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, INGEST_429, strlen(INGEST_429)));
    TEST_ASSERT_EQUAL_INT(429, head.status);
    TEST_ASSERT_EQUAL_UINT32(60000u, head.rate_limits_ms);

    static const char upper[] = "HTTP/1.1 429 x\r\nRETRY-AFTER:30\r\n\r\n";
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, upper, strlen(upper)));
    TEST_ASSERT_EQUAL_UINT32(30000u, head.retry_after_ms);
}

static void test_head_survives_any_split_between_reads(void)
{
    /* Every split point, the "\r\n" pairs included: what a recv() returns is the network's
     * choice, not the server's. */
    size_t len = strlen(INGEST_429);
    for (size_t split = 0; split <= len; split++) {
        sentry_http_head_t head;
        sentry_http_head_init(&head);
        bool done = sentry_http_head_feed(&head, INGEST_429, split);
        if (!done) {
            done = sentry_http_head_feed(&head, INGEST_429 + split, len - split);
        }
        TEST_ASSERT_TRUE_MESSAGE(done, "head never completed");
        TEST_ASSERT_EQUAL_INT(429, head.status);
        TEST_ASSERT_EQUAL_UINT32(60000u, head.rate_limits_ms);
    }
}

static void test_head_survives_one_byte_at_a_time(void)
{
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    bool done = false;
    for (size_t i = 0; INGEST_429[i] && !done; i++) {
        done = sentry_http_head_feed(&head, INGEST_429 + i, 1);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_INT(429, head.status);
    TEST_ASSERT_EQUAL_UINT32(60000u, head.rate_limits_ms);
}

static void test_head_reads_both_back_off_headers(void)
{
    static const char reply[] = "HTTP/1.0 503 Service Unavailable\r\n"
                                "Retry-After: 120\r\n"
                                "X-Sentry-Rate-Limits: 30::key\r\n"
                                "\r\n";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_INT(503, head.status);
    TEST_ASSERT_EQUAL_UINT32(120000u, head.retry_after_ms);
    TEST_ASSERT_EQUAL_UINT32(30000u, head.rate_limits_ms);
}

static void test_head_accepts_bare_newlines(void)
{
    static const char reply[] = "HTTP/1.1 413 Payload Too Large\nRetry-After: 5\n\n";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_INT(413, head.status);
    TEST_ASSERT_EQUAL_UINT32(5000u, head.retry_after_ms);
}

static void test_head_gives_up_on_a_reply_that_is_not_http(void)
{
    /* A captive portal on plain http, or a TLS alert read as text: done, with no status. */
    static const char reply[] = "<html><body>Sign in to continue</body></html>\r\n";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_INT(0, head.status);
}

static void test_head_is_not_done_without_the_blank_line(void)
{
    /* What a timeout mid-head leaves: the status is known, the rest is not. */
    static const char partial[] = "HTTP/1.1 200 OK\r\nServer: ngi";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_FALSE(sentry_http_head_feed(&head, partial, strlen(partial)));
    TEST_ASSERT_EQUAL_INT(200, head.status);
}

static void test_head_cuts_an_over_long_line_without_losing_the_next(void)
{
    char reply[600];
    char filler[400];
    memset(filler, 'a', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';
    snprintf(
        reply, sizeof(reply), "HTTP/1.1 429 x\r\nSet-Cookie: %s\r\nRetry-After: 7\r\n\r\n", filler);
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_INT(429, head.status);
    TEST_ASSERT_EQUAL_UINT32(7000u, head.retry_after_ms);
}

static void test_head_does_not_take_a_prefix_for_a_header(void)
{
    /* "Retry-After-Ms" is not "Retry-After"; only the exact name followed by ':' counts. */
    static const char reply[] = "HTTP/1.1 429 x\r\nRetry-After-Ms: 9\r\n\r\n";
    sentry_http_head_t head;
    sentry_http_head_init(&head);
    TEST_ASSERT_TRUE(sentry_http_head_feed(&head, reply, strlen(reply)));
    TEST_ASSERT_EQUAL_UINT32(0u, head.retry_after_ms);
}

static void test_status_2xx_is_ok_and_drops_any_back_off(void)
{
    sentry_response_t r = sentry_http_response_for_status(200, 5000u, 7000u);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_OK, r.result);
    TEST_ASSERT_EQUAL_UINT16(200, r.http_status);
    TEST_ASSERT_EQUAL_UINT32(0u, r.retry_after_ms);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_OK, sentry_http_response_for_status(204, 0, 0).result);
}

static void test_status_429_prefers_retry_after_over_rate_limits(void)
{
    sentry_response_t r = sentry_http_response_for_status(429, 60000u, 30000u);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_RATE_LIMITED, r.result);
    TEST_ASSERT_EQUAL_UINT16(429, r.http_status);
    TEST_ASSERT_EQUAL_UINT32(60000u, r.retry_after_ms);
}

static void test_status_429_falls_back_to_rate_limits(void)
{
    sentry_response_t r = sentry_http_response_for_status(429, 0, 30000u);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_RATE_LIMITED, r.result);
    TEST_ASSERT_EQUAL_UINT32(30000u, r.retry_after_ms);
    /* Neither stated: the core falls back to its own backoff. */
    TEST_ASSERT_EQUAL_UINT32(0u, sentry_http_response_for_status(429, 0, 0).retry_after_ms);
}

static void test_status_other_4xx_is_rejected_and_keeps_its_back_off(void)
{
    sentry_response_t r = sentry_http_response_for_status(403, 0, 10000u);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_REJECTED, r.result);
    TEST_ASSERT_EQUAL_UINT16(403, r.http_status);
    TEST_ASSERT_EQUAL_UINT32(10000u, r.retry_after_ms);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_REJECTED, sentry_http_response_for_status(400, 0, 0).result);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_REJECTED, sentry_http_response_for_status(413, 0, 0).result);
}

static void test_status_5xx_and_others_are_errors(void)
{
    sentry_response_t r = sentry_http_response_for_status(503, 2000u, 0);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, r.result);
    TEST_ASSERT_EQUAL_UINT16(503, r.http_status);
    TEST_ASSERT_EQUAL_UINT32(2000u, r.retry_after_ms);
    /* Redirects are not followed, so one is an answer nobody can use. */
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, sentry_http_response_for_status(302, 0, 0).result);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, sentry_http_response_for_status(100, 0, 0).result);
}

static void test_status_without_a_status_line_is_an_error_with_no_status(void)
{
    sentry_response_t r = sentry_http_response_for_status(0, 5000u, 5000u);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, r.result);
    TEST_ASSERT_EQUAL_UINT16(0, r.http_status);
    TEST_ASSERT_EQUAL_UINT32(0u, r.retry_after_ms);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, sentry_http_response_for_status(-1, 0, 0).result);
    r = sentry_http_response_for_status(70000, 0, 0);
    TEST_ASSERT_EQUAL_INT(SENTRY_SEND_ERROR, r.result);
    TEST_ASSERT_EQUAL_UINT16(0, r.http_status);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_retry_after_reads_delta_seconds);
    RUN_TEST(test_retry_after_keeps_the_whole_seconds_of_a_fraction);
    RUN_TEST(test_retry_after_ignores_garbage_and_non_positive_values);
    RUN_TEST(test_retry_after_does_not_parse_an_http_date);
    RUN_TEST(test_retry_after_is_clamped_to_a_day);
    RUN_TEST(test_retry_after_treats_null_and_empty_as_not_stated);
    RUN_TEST(test_rate_limits_reads_the_leading_delay);
    RUN_TEST(test_rate_limits_uses_the_first_entry_of_a_list);
    RUN_TEST(test_rate_limits_shares_the_clamp);
    RUN_TEST(test_rate_limits_treats_null_and_empty_as_not_stated);
    RUN_TEST(test_head_reads_status_and_stops_at_the_blank_line);
    RUN_TEST(test_head_matches_header_names_in_any_case);
    RUN_TEST(test_head_survives_any_split_between_reads);
    RUN_TEST(test_head_survives_one_byte_at_a_time);
    RUN_TEST(test_head_reads_both_back_off_headers);
    RUN_TEST(test_head_accepts_bare_newlines);
    RUN_TEST(test_head_gives_up_on_a_reply_that_is_not_http);
    RUN_TEST(test_head_is_not_done_without_the_blank_line);
    RUN_TEST(test_head_cuts_an_over_long_line_without_losing_the_next);
    RUN_TEST(test_head_does_not_take_a_prefix_for_a_header);
    RUN_TEST(test_status_2xx_is_ok_and_drops_any_back_off);
    RUN_TEST(test_status_429_prefers_retry_after_over_rate_limits);
    RUN_TEST(test_status_429_falls_back_to_rate_limits);
    RUN_TEST(test_status_other_4xx_is_rejected_and_keeps_its_back_off);
    RUN_TEST(test_status_5xx_and_others_are_errors);
    RUN_TEST(test_status_without_a_status_line_is_an_error_with_no_status);
    return UNITY_END();
}
