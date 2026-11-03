/**
 * Host tests for sessions.
 *
 * A reboot, from the record's point of view, is nothing more than `sentry_session_open()`
 * being called again on the same bytes, followed by `sentry_session_close_previous()` with
 * the new boot's reset reason. So every boot-to-boot case below runs on one record, the way
 * the retained block carries it across a real reset. The wire checks matter for the usual
 * reason: Relay drops a malformed session silently, with no outcome and no error.
 */

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "sentry_session.h"

#define RELEASE "sentry-micro-example@0.1.0"
#define OTHER_RELEASE "sentry-micro-example@0.2.0"
#define DID "dcb4d92a7d94"
/* 2026-11-03T10:00:00Z. */
#define NOW_S 1793700000ULL
#define SECOND_US 1000000ULL

static const uint8_t SID_A[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
static const uint8_t SID_B[16] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    0xfb, 0xfc, 0xfd, 0xfe, 0xff };
static const uint8_t SID_C[16] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11 };

void setUp(void) { }
void tearDown(void) { }

/** Write `slot`'s update and fail the test if it does not produce one. */
static size_t write_or_fail(char *buf, size_t cap, const sentry_session_t *slot, uint64_t now_s)
{
    size_t len = sentry_session_envelope_write(buf, cap, slot, DID, RELEASE, "production", now_s);
    TEST_ASSERT_TRUE_MESSAGE(len > 0 && len < cap, "no session envelope was written");
    return len;
}

/** Copy the value of `"started":"..."` out of an envelope. */
static void extract_started(const char *envelope, char *out, size_t cap)
{
    const char *at = strstr(envelope, "\"started\":\"");
    TEST_ASSERT_NOT_NULL(at);
    at += strlen("\"started\":\"");
    const char *end = strchr(at, '"');
    TEST_ASSERT_NOT_NULL(end);
    size_t len = (size_t)(end - at);
    TEST_ASSERT_TRUE(len < cap);
    memcpy(out, at, len);
    out[len] = '\0';
}

/* ── The record ───────────────────────────────────────────────────────────── */

static void test_the_record_fits_its_budget(void)
{
    /* The retained block holds this beside an 856-byte log ring in 1,024 bytes. */
    printf("sizeof(sentry_session_t) = %u, sizeof(sentry_session_record_t) = %u\n",
        (unsigned)sizeof(sentry_session_t), (unsigned)sizeof(sentry_session_record_t));
    TEST_ASSERT_EQUAL_UINT(48, sizeof(sentry_session_t));
    TEST_ASSERT_EQUAL_UINT(104, sizeof(sentry_session_record_t));
    TEST_ASSERT_NOT_EQUAL(0, sentry_session_layout_id());
}

static void test_a_zeroed_record_is_an_empty_one(void)
{
    /* What the retained block holds after a power-on or a layout change. Nothing may need
     * initialising beyond the zeroing the block already gets. */
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));

    TEST_ASSERT_FALSE(sentry_session_pending(&rec.current));
    TEST_ASSERT_FALSE(sentry_session_pending(&rec.previous));
    sentry_session_touch(&rec, NOW_S, SECOND_US);
    sentry_session_note_error(&rec);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_NONE,
        sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, NOW_S, SECOND_US));

    TEST_ASSERT_FALSE(sentry_session_open(&rec, 0, SECOND_US, SID_A, RELEASE));
    TEST_ASSERT_EQUAL(SENTRY_SESSION_OPEN, rec.current.status);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_FREE, rec.previous.status);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_NONE,
        sentry_session_close_previous(&rec, SENTRY_RESET_POWERON, RELEASE, 0, SECOND_US));
    TEST_ASSERT_EQUAL_UINT(0, rec.dropped);
}

/* ── Open, then close on the next boot ────────────────────────────────────── */

static void test_a_panic_closes_the_previous_session_as_crashed(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, 2 * SECOND_US, SID_A, RELEASE);
    sentry_session_touch(&rec, NOW_S + 40, 42 * SECOND_US);

    /* The reboot. */
    sentry_session_open(&rec, NOW_S + 60, SECOND_US, SID_B, RELEASE);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_CRASHED,
        sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, NOW_S + 60, SECOND_US));

    TEST_ASSERT_EQUAL_MEMORY(SID_A, rec.previous.sid, 16);
    TEST_ASSERT_EQUAL_MEMORY(SID_B, rec.current.sid, 16);
    TEST_ASSERT_TRUE(sentry_session_pending(&rec.previous));

    char buf[512];
    write_or_fail(buf, sizeof(buf), &rec.previous, NOW_S + 61);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"crashed\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sid\":\"00010203-0405-4607-8809-0a0b0c0d0e0f\""));
    /* Uptime at the last touch, which is all the next boot knows of how long this one ran. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"duration\":42"));
    /* The init was never sent, so the close is the first update Relay sees of this sid. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"init\":true"));
}

static void test_watchdogs_and_brownout_also_close_as_crashed(void)
{
    const sentry_reset_reason_t crashes[]
        = { SENTRY_RESET_INT_WDT, SENTRY_RESET_TASK_WDT, SENTRY_RESET_WDT, SENTRY_RESET_BROWNOUT };
    for (size_t i = 0; i < sizeof(crashes) / sizeof(crashes[0]); i++) {
        sentry_session_record_t rec;
        memset(&rec, 0, sizeof(rec));
        sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
        sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
        TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_CRASHED,
            sentry_session_close_previous(&rec, crashes[i], RELEASE, NOW_S, SECOND_US));
    }
}

static void test_every_other_reset_closes_as_exited(void)
{
    const sentry_reset_reason_t exits[] = { SENTRY_RESET_SOFTWARE, SENTRY_RESET_POWERON,
        SENTRY_RESET_EXTERNAL, SENTRY_RESET_DEEPSLEEP, SENTRY_RESET_UNKNOWN, SENTRY_RESET_USB,
        SENTRY_RESET_JTAG, SENTRY_RESET_SDIO };
    for (size_t i = 0; i < sizeof(exits) / sizeof(exits[0]); i++) {
        sentry_session_record_t rec;
        memset(&rec, 0, sizeof(rec));
        sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
        sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
        TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_EXITED,
            sentry_session_close_previous(&rec, exits[i], RELEASE, NOW_S, SECOND_US));
        TEST_ASSERT_EQUAL_UINT32(0, rec.previous.errors);
    }

    char buf[512];
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_open(&rec, NOW_S + 10, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, NOW_S + 10, SECOND_US);
    write_or_fail(buf, sizeof(buf), &rec.previous, NOW_S + 10);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"exited\""));
}

static void test_a_close_is_decided_only_once(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, NOW_S, SECOND_US);
    /* A second call on the same boot must not turn a crash into an exit. */
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_NONE,
        sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, NOW_S, SECOND_US));
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CRASHED, rec.previous.status);
}

/* ── Init and close agree ─────────────────────────────────────────────────── */

static void test_the_init_and_the_close_carry_the_same_started(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    char buf[512];
    char started_init[32];
    char started_close[32];

    /* Boot one: the clock arrives 7.5 s in, so the start is 7 s before it. */
    sentry_session_open(&rec, 0, 300000, SID_A, RELEASE);
    sentry_session_touch(&rec, NOW_S, 7500000);
    write_or_fail(buf, sizeof(buf), &rec.current, NOW_S);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"init\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"ok\""));
    TEST_ASSERT_NULL(strstr(buf, "\"duration\""));
    extract_started(buf, started_init, sizeof(started_init));
    TEST_ASSERT_EQUAL_STRING("2026-11-03T09:59:53Z", started_init);
    sentry_session_mark_sent(&rec.current);
    TEST_ASSERT_TRUE(rec.current.init_sent);
    TEST_ASSERT_FALSE(sentry_session_pending(&rec.current));

    /* Later touches, with the clock and the uptime both moved on, leave the start alone. */
    sentry_session_touch(&rec, NOW_S + 3600, 3608 * SECOND_US);
    sentry_session_touch(&rec, NOW_S + 7201, 7206 * SECOND_US);

    /* Boot two. */
    sentry_session_open(&rec, 0, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, 0, SECOND_US);
    sentry_session_touch(&rec, NOW_S + 8000, 5 * SECOND_US);
    write_or_fail(buf, sizeof(buf), &rec.previous, NOW_S + 8000);
    extract_started(buf, started_close, sizeof(started_close));
    TEST_ASSERT_EQUAL_STRING(started_init, started_close);
    /* Relay has already counted this session from its init. */
    TEST_ASSERT_NULL(strstr(buf, "\"init\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"duration\":7206"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":\"2026-11-03T12:13:20Z\""));

    sentry_session_mark_sent(&rec.previous);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_FREE, rec.previous.status);
    TEST_ASSERT_FALSE(sentry_session_pending(&rec.previous));
}

static void test_nothing_is_written_before_the_clock_is_known(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    char buf[512];

    sentry_session_open(&rec, 0, SECOND_US, SID_A, RELEASE);
    sentry_session_touch(&rec, 0, 30 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(0, rec.current.started_unix_s);
    TEST_ASSERT_TRUE(sentry_session_pending(&rec.current));
    /* Pending, but a session dated 1970 is one Relay drops, so it is held instead. */
    TEST_ASSERT_EQUAL_size_t(
        0, sentry_session_envelope_write(buf, sizeof(buf), &rec.current, DID, RELEASE, NULL, 0));
    TEST_ASSERT_EQUAL_size_t(0,
        sentry_session_envelope_write(buf, sizeof(buf), &rec.current, DID, RELEASE, NULL, NOW_S));
}

static void test_an_unknown_start_is_filled_once_the_clock_arrives(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, 0, 500000, SID_A, RELEASE);
    sentry_session_touch(&rec, NOW_S, 20 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 20, rec.current.started_unix_s);
    sentry_session_touch(&rec, NOW_S + 1000, 1021 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 20, rec.current.started_unix_s);
    TEST_ASSERT_EQUAL_UINT64(1021 * SECOND_US, rec.current.last_uptime_us);
}

static void test_a_close_with_no_start_is_dated_from_how_long_it_ran(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));

    /* Boot one never learns the date and crashes 30 s in. */
    sentry_session_open(&rec, 0, SECOND_US, SID_A, RELEASE);
    sentry_session_touch(&rec, 0, 30 * SECOND_US);

    /* Boot two learns it 11 s in, so it began at NOW_S - 11, and boot one about 30 s before
     * that. */
    sentry_session_open(&rec, 0, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, 0, SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(0, rec.previous.started_unix_s);
    sentry_session_touch(&rec, NOW_S, 11 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 11 - 30, rec.previous.started_unix_s);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 11, rec.current.started_unix_s);

    /* And it stays put. */
    sentry_session_touch(&rec, NOW_S + 50, 61 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 11 - 30, rec.previous.started_unix_s);

    char buf[512];
    write_or_fail(buf, sizeof(buf), &rec.previous, NOW_S + 50);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"init\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"started\":\"2026-11-03T09:59:19Z\""));
}

static void test_a_close_found_with_the_clock_already_known_is_dated_at_once(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, 0, SECOND_US, SID_A, RELEASE);
    sentry_session_touch(&rec, 0, 30 * SECOND_US);
    /* A port whose clock survives the reset (an RTC chip) knows the date at init. */
    sentry_session_open(&rec, NOW_S, 2 * SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, NOW_S, 2 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 2 - 30, rec.previous.started_unix_s);
    TEST_ASSERT_EQUAL_UINT64(NOW_S - 2, rec.current.started_unix_s);
}

/* ── Errors ───────────────────────────────────────────────────────────────── */

static void test_a_crash_reports_at_least_one_error(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, NOW_S, SECOND_US);
    TEST_ASSERT_EQUAL_UINT32(1, rec.previous.errors);

    /* Errors already counted are kept, not replaced. */
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_note_error(&rec);
    sentry_session_note_error(&rec);
    sentry_session_note_error(&rec);
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_TASK_WDT, RELEASE, NOW_S, SECOND_US);
    TEST_ASSERT_EQUAL_UINT32(3, rec.previous.errors);

    char buf[512];
    write_or_fail(buf, sizeof(buf), &rec.previous, NOW_S);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"errors\":3"));
}

static void test_errors_count_against_the_running_session_only(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, NOW_S, SECOND_US);
    sentry_session_note_error(&rec);
    TEST_ASSERT_EQUAL_UINT32(1, rec.current.errors);
    TEST_ASSERT_EQUAL_UINT32(0, rec.previous.errors);
}

/* ── Firmware changes, lost sessions and no entropy ───────────────────────── */

static void test_a_session_from_other_firmware_is_not_closed(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);

    /* An OTA: the record survives, the release does not. Closing it under the new release
     * would put the old firmware's crash on the new one's health. */
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, OTHER_RELEASE);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_SKIPPED_RELEASE,
        sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, OTHER_RELEASE, NOW_S, SECOND_US));
    TEST_ASSERT_EQUAL(SENTRY_SESSION_FREE, rec.previous.status);
    TEST_ASSERT_FALSE(sentry_session_pending(&rec.previous));
    TEST_ASSERT_EQUAL(SENTRY_SESSION_OPEN, rec.current.status);
}

static void test_a_third_unsent_session_overwrites_the_oldest(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));

    /* Three boots, none of which ever learns the date. */
    TEST_ASSERT_FALSE(sentry_session_open(&rec, 0, SECOND_US, SID_A, RELEASE));
    TEST_ASSERT_FALSE(sentry_session_open(&rec, 0, SECOND_US, SID_B, RELEASE));
    sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, 0, SECOND_US);
    TEST_ASSERT_TRUE(sentry_session_open(&rec, 0, SECOND_US, SID_C, RELEASE));
    sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, 0, SECOND_US);

    TEST_ASSERT_EQUAL_UINT(1, rec.dropped);
    TEST_ASSERT_EQUAL_MEMORY(SID_B, rec.previous.sid, 16);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_EXITED, rec.previous.status);
    TEST_ASSERT_EQUAL_MEMORY(SID_C, rec.current.sid, 16);
}

static void test_a_sent_close_frees_its_slot_for_the_next_boot(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_B, RELEASE);
    sentry_session_close_previous(&rec, SENTRY_RESET_SOFTWARE, RELEASE, NOW_S, SECOND_US);
    sentry_session_mark_sent(&rec.previous);
    TEST_ASSERT_FALSE(sentry_session_open(&rec, NOW_S, SECOND_US, SID_C, RELEASE));
    TEST_ASSERT_EQUAL_UINT(0, rec.dropped);
}

static void test_no_entropy_still_closes_the_last_boot(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, SECOND_US, SID_A, RELEASE);

    sentry_session_open(&rec, NOW_S, SECOND_US, NULL, RELEASE);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_FREE, rec.current.status);
    TEST_ASSERT_EQUAL(SENTRY_SESSION_CLOSE_CRASHED,
        sentry_session_close_previous(&rec, SENTRY_RESET_PANIC, RELEASE, NOW_S, SECOND_US));
    /* And nothing is counted against a session that does not exist. */
    sentry_session_note_error(&rec);
    sentry_session_touch(&rec, NOW_S, 5 * SECOND_US);
    TEST_ASSERT_EQUAL_UINT32(0, rec.current.errors);
    TEST_ASSERT_FALSE(sentry_session_pending(&rec.current));
}

/* ── Wire format ──────────────────────────────────────────────────────────── */

static void test_the_envelope_has_the_shape_relay_expects(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, 0, SID_A, RELEASE);

    char buf[512];
    size_t len = write_or_fail(buf, sizeof(buf), &rec.current, NOW_S + 5);
    printf("%s", buf);
    TEST_ASSERT_EQUAL_size_t(strlen(buf), len);

    const char *expected_header = "{\"sent_at\":\"2026-11-03T10:00:05Z\"}\n"
                                  "{\"type\":\"session\",\"length\":";
    TEST_ASSERT_EQUAL_INT(0, strncmp(buf, expected_header, strlen(expected_header)));

    /* The stated length is exactly the payload, which ends at the closing newline. */
    unsigned stated = 0;
    const char *length_at = strstr(buf, "\"length\":");
    TEST_ASSERT_EQUAL_INT(1, sscanf(length_at, "\"length\":%u", &stated));
    const char *payload = strchr(length_at, '\n') + 1;
    TEST_ASSERT_EQUAL_size_t(stated, strlen(payload) - 1);
    TEST_ASSERT_EQUAL_CHAR('\n', payload[stated]);

    TEST_ASSERT_EQUAL_STRING(
        "{\"sid\":\"00010203-0405-4607-8809-0a0b0c0d0e0f\",\"did\":\"" DID "\",\"init\":true,"
        "\"started\":\"2026-11-03T10:00:00Z\",\"timestamp\":\"2026-11-03T10:00:05Z\","
        "\"status\":\"ok\",\"errors\":0,"
        "\"attrs\":{\"release\":\"" RELEASE "\",\"environment\":\"production\"}}\n",
        payload);
}

static void test_a_session_without_a_release_is_never_written(void)
{
    /* Relay drops a session with no attrs.release, and this SDK lets the release be unset. */
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, 0, SID_A, NULL);
    char buf[512];
    TEST_ASSERT_EQUAL_size_t(
        0, sentry_session_envelope_write(buf, sizeof(buf), &rec.current, DID, NULL, NULL, NOW_S));
    TEST_ASSERT_EQUAL_size_t(
        0, sentry_session_envelope_write(buf, sizeof(buf), &rec.current, DID, "", NULL, NOW_S));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}

static void test_optional_fields_are_omitted_when_unknown(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, 0, SID_A, RELEASE);
    char buf[512];
    size_t len
        = sentry_session_envelope_write(buf, sizeof(buf), &rec.current, NULL, RELEASE, NULL, NOW_S);
    TEST_ASSERT_TRUE(len > 0 && len < sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "\"did\""));
    TEST_ASSERT_NULL(strstr(buf, "\"environment\""));
}

static void test_a_short_buffer_gets_the_size_and_no_partial_envelope(void)
{
    sentry_session_record_t rec;
    memset(&rec, 0, sizeof(rec));
    sentry_session_open(&rec, NOW_S, 0, SID_A, RELEASE);

    size_t needed
        = sentry_session_envelope_write(NULL, 0, &rec.current, DID, RELEASE, "production", NOW_S);
    char buf[64];
    size_t got = sentry_session_envelope_write(
        buf, sizeof(buf), &rec.current, DID, RELEASE, "production", NOW_S);
    TEST_ASSERT_EQUAL_size_t(needed, got);
    TEST_ASSERT_TRUE(got >= sizeof(buf));
    TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
}

static void test_session_ids_are_v4_uuids(void)
{
    char out[SENTRY_MICRO_SESSION_ID_LEN];
    sentry_session_id_format(out, SID_A);
    /* Version nibble 4 and variant 10xx stamped in, so Relay's UUID parser accepts it. */
    TEST_ASSERT_EQUAL_STRING("00010203-0405-4607-8809-0a0b0c0d0e0f", out);
    sentry_session_id_format(out, SID_B);
    TEST_ASSERT_EQUAL_STRING("f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff", out);
    TEST_ASSERT_EQUAL_size_t(36, strlen(out));
}

/* ── RFC 3339 ─────────────────────────────────────────────────────────────── */

static void assert_rfc3339(const char *expected, uint64_t unix_s)
{
    char out[SENTRY_MICRO_RFC3339_LEN];
    TEST_ASSERT_EQUAL_size_t(20, sentry_rfc3339_format(out, sizeof(out), unix_s));
    TEST_ASSERT_EQUAL_STRING(expected, out);
}

static void test_rfc3339_dates(void)
{
    assert_rfc3339("1970-01-01T00:00:00Z", 0);
    assert_rfc3339("2026-11-03T10:00:00Z", NOW_S);
    /* A leap day, and the second after it. */
    assert_rfc3339("2024-02-29T00:00:00Z", 1709164800ULL);
    assert_rfc3339("2024-03-01T00:00:00Z", 1709164800ULL + 86400);
    /* One second past the signed 32-bit limit: the reason the input is 64-bit. */
    assert_rfc3339("2038-01-19T03:14:08Z", 2147483648ULL);
    /* 2100 is divisible by 4 and is not a leap year, which a four-year rule gets wrong. */
    assert_rfc3339("2100-02-28T23:59:59Z", 4107542399ULL);
    assert_rfc3339("2100-03-01T00:00:00Z", 4107542400ULL);
    assert_rfc3339("9999-12-31T23:59:59Z", 253402300799ULL);
}

static void test_rfc3339_refuses_what_it_cannot_write(void)
{
    char out[SENTRY_MICRO_RFC3339_LEN];
    /* Year 10000 would need a fifth digit. */
    TEST_ASSERT_EQUAL_size_t(0, sentry_rfc3339_format(out, sizeof(out), 253402300800ULL));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
    TEST_ASSERT_EQUAL_size_t(0, sentry_rfc3339_format(out, sizeof(out) - 1, NOW_S));
    TEST_ASSERT_EQUAL_CHAR('\0', out[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_record_fits_its_budget);
    RUN_TEST(test_a_zeroed_record_is_an_empty_one);
    RUN_TEST(test_a_panic_closes_the_previous_session_as_crashed);
    RUN_TEST(test_watchdogs_and_brownout_also_close_as_crashed);
    RUN_TEST(test_every_other_reset_closes_as_exited);
    RUN_TEST(test_a_close_is_decided_only_once);
    RUN_TEST(test_the_init_and_the_close_carry_the_same_started);
    RUN_TEST(test_nothing_is_written_before_the_clock_is_known);
    RUN_TEST(test_an_unknown_start_is_filled_once_the_clock_arrives);
    RUN_TEST(test_a_close_with_no_start_is_dated_from_how_long_it_ran);
    RUN_TEST(test_a_close_found_with_the_clock_already_known_is_dated_at_once);
    RUN_TEST(test_a_crash_reports_at_least_one_error);
    RUN_TEST(test_errors_count_against_the_running_session_only);
    RUN_TEST(test_a_session_from_other_firmware_is_not_closed);
    RUN_TEST(test_a_third_unsent_session_overwrites_the_oldest);
    RUN_TEST(test_a_sent_close_frees_its_slot_for_the_next_boot);
    RUN_TEST(test_no_entropy_still_closes_the_last_boot);
    RUN_TEST(test_the_envelope_has_the_shape_relay_expects);
    RUN_TEST(test_a_session_without_a_release_is_never_written);
    RUN_TEST(test_optional_fields_are_omitted_when_unknown);
    RUN_TEST(test_a_short_buffer_gets_the_size_and_no_partial_envelope);
    RUN_TEST(test_session_ids_are_v4_uuids);
    RUN_TEST(test_rfc3339_dates);
    RUN_TEST(test_rfc3339_refuses_what_it_cannot_write);
    return UNITY_END();
}
