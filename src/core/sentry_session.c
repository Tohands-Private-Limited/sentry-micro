#include "sentry_session.h"

#include <stdio.h>
#include <string.h>

#include "sentry_envelope.h" /* sentry_event_id_format() */
#include "sentry_json.h"

/*
 * Bumped whenever the meaning of the record's bytes changes without its size changing — a
 * field reordered, or a status value renumbered. The layout id folds it in, so the retained
 * block written by the old firmware is then refused rather than misread.
 */
#define SENTRY_SESSION_RECORD_VERSION 1u

uint32_t sentry_session_release_hash(const char *release)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(release ? release : ""); *p; p++) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return hash;
}

uint32_t sentry_session_layout_id(void)
{
    /* FNV-1a over what changes how these bytes are read back, as the log ring does. */
    const uint32_t parts[] = {
        SENTRY_SESSION_RECORD_VERSION,
        (uint32_t)sizeof(sentry_session_t),
        (uint32_t)sizeof(sentry_session_record_t),
    };
    uint32_t id = 2166136261u;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        for (size_t b = 0; b < 4; b++) {
            id ^= (parts[i] >> (b * 8)) & 0xffu;
            id *= 16777619u;
        }
    }
    return id;
}

static bool is_closed(uint8_t status)
{
    return status == SENTRY_SESSION_EXITED || status == SENTRY_SESSION_CRASHED;
}

/**
 * The instant this boot began, in Unix seconds, or 0 when it cannot be known.
 *
 * 0 rather than a clamped small number when the uptime is somehow larger than the clock: a
 * start that close to 1970 would be dropped by Relay anyway, and 0 keeps meaning "unknown",
 * so the session stays held instead of being sent to be thrown away.
 */
static uint64_t boot_instant_s(uint64_t now_unix_s, uint64_t uptime_us)
{
    uint64_t uptime_s = uptime_us / 1000000u;
    if (now_unix_s == 0 || uptime_s >= now_unix_s) {
        return 0;
    }
    return now_unix_s - uptime_s;
}

/**
 * Date a waiting close whose boot never learned the clock: that boot ended about when this
 * one began, and it had been running for `last_uptime_us` by its last touch.
 */
static void date_previous(sentry_session_record_t *rec, uint64_t now_unix_s, uint64_t uptime_us)
{
    sentry_session_t *prev = &rec->previous;
    if (!is_closed(prev->status) || prev->started_unix_s != 0) {
        return;
    }
    uint64_t this_boot = boot_instant_s(now_unix_s, uptime_us);
    uint64_t ran_s = prev->last_uptime_us / 1000000u;
    if (this_boot == 0 || ran_s >= this_boot) {
        return;
    }
    prev->started_unix_s = this_boot - ran_s;
}

bool sentry_session_open(sentry_session_record_t *rec, uint64_t now_unix_s, uint64_t uptime_us,
    const uint8_t *sid, const char *release)
{
    if (!rec) {
        return false;
    }

    bool dropped = false;
    if (rec->current.status == SENTRY_SESSION_OPEN) {
        if (rec->previous.status != SENTRY_SESSION_FREE) {
            /* Two boots' worth of updates never got out. The older one goes: its close was
             * already the less useful of the two, being further from anything still
             * running, and keeping it would mean losing the newer one instead. */
            if (rec->dropped < UINT16_MAX) {
                rec->dropped++;
            }
            dropped = true;
        }
        rec->previous = rec->current;
    }

    memset(&rec->current, 0, sizeof(rec->current));
    if (!sid) {
        return dropped;
    }
    memcpy(rec->current.sid, sid, sizeof(rec->current.sid));
    rec->current.status = SENTRY_SESSION_OPEN;
    rec->current.release_hash = sentry_session_release_hash(release);
    rec->current.last_uptime_us = uptime_us;
    rec->current.started_unix_s = boot_instant_s(now_unix_s, uptime_us);
    return dropped;
}

sentry_session_close_t sentry_session_close_previous(sentry_session_record_t *rec,
    sentry_reset_reason_t reset_reason, const char *release, uint64_t now_unix_s,
    uint64_t uptime_us)
{
    if (!rec || rec->previous.status != SENTRY_SESSION_OPEN) {
        /* Free, or already closed and waiting: a close is only ever decided once, by the
         * boot right after the session, because only that boot's reset reason is about it. */
        return SENTRY_SESSION_CLOSE_NONE;
    }

    sentry_session_t *prev = &rec->previous;
    if (prev->release_hash != sentry_session_release_hash(release)) {
        memset(prev, 0, sizeof(*prev));
        return SENTRY_SESSION_CLOSE_SKIPPED_RELEASE;
    }

    if (sentry_reset_reason_is_crash(reset_reason)) {
        prev->status = SENTRY_SESSION_CRASHED;
        /* The crash is an error even if nothing was captured before it. Ingest forces 0 to 1
         * on a crashed session anyway; saying so here keeps the device's own record honest. */
        if (prev->errors == 0) {
            prev->errors = 1;
        }
    } else {
        prev->status = SENTRY_SESSION_EXITED;
    }
    date_previous(rec, now_unix_s, uptime_us);
    return prev->status == SENTRY_SESSION_CRASHED ? SENTRY_SESSION_CLOSE_CRASHED
                                                  : SENTRY_SESSION_CLOSE_EXITED;
}

void sentry_session_touch(sentry_session_record_t *rec, uint64_t now_unix_s, uint64_t uptime_us)
{
    if (!rec) {
        return;
    }
    if (rec->current.status == SENTRY_SESSION_OPEN) {
        rec->current.last_uptime_us = uptime_us;
        if (rec->current.started_unix_s == 0) {
            rec->current.started_unix_s = boot_instant_s(now_unix_s, uptime_us);
        }
    }
    date_previous(rec, now_unix_s, uptime_us);
}

void sentry_session_note_error(sentry_session_record_t *rec)
{
    if (rec && rec->current.status == SENTRY_SESSION_OPEN && rec->current.errors < UINT32_MAX) {
        rec->current.errors++;
    }
}

bool sentry_session_pending(const sentry_session_t *slot)
{
    if (!slot) {
        return false;
    }
    return is_closed(slot->status) || (slot->status == SENTRY_SESSION_OPEN && !slot->init_sent);
}

void sentry_session_mark_sent(sentry_session_t *slot)
{
    if (!slot) {
        return;
    }
    if (is_closed(slot->status)) {
        /* Terminal: nothing may follow it, so there is nothing left worth keeping. */
        memset(slot, 0, sizeof(*slot));
        return;
    }
    slot->init_sent = true;
}

const char *sentry_session_status_name(uint8_t status)
{
    switch (status) {
    case SENTRY_SESSION_EXITED:
        return "exited";
    case SENTRY_SESSION_CRASHED:
        return "crashed";
    case SENTRY_SESSION_OPEN:
    default:
        return "ok";
    }
}

void sentry_session_id_format(char *out, const uint8_t sid[16])
{
    char hex[SENTRY_MICRO_EVENT_ID_LEN];
    sentry_event_id_format(hex, sid);
    /* 8-4-4-4-12: a dash before hex digits 8, 12, 16 and 20. */
    size_t o = 0;
    for (size_t i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            out[o++] = '-';
        }
        out[o++] = hex[i];
    }
    out[o] = '\0';
}

size_t sentry_rfc3339_format(char *out, size_t cap, uint64_t unix_s)
{
    if (!out || cap < SENTRY_MICRO_RFC3339_LEN) {
        if (out && cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    uint64_t days = unix_s / 86400u;
    uint32_t secs = (uint32_t)(unix_s % 86400u);

    /*
     * Days since 1970-01-01 to a civil date: Howard Hinnant's civil_from_days, from "chrono-
     * Compatible Low-Level Date Algorithms" (howardhinnant.github.io/date_algorithms.html).
     * It shifts the epoch to 0000-03-01 so the leap day falls at the end of the year, then
     * splits days into 400-year eras of exactly 146,097 days. The input here is never
     * negative, so the era arithmetic needs none of the original's handling for that.
     */
    uint64_t z = days + 719468u;
    uint64_t era = z / 146097u;
    uint64_t doe = z - era * 146097u; /* [0, 146096] */
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; /* [0, 399] */
    uint64_t year = yoe + era * 400u;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u); /* [0, 365] */
    uint64_t mp = (5u * doy + 2u) / 153u; /* [0, 11], March first */
    unsigned day = (unsigned)(doy - (153u * mp + 2u) / 5u + 1u);
    unsigned month = (unsigned)(mp < 10u ? mp + 3u : mp - 9u);
    if (month <= 2u) {
        year++;
    }
    if (year > 9999u) {
        out[0] = '\0';
        return 0;
    }

    int len = snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)year, month, day,
        (unsigned)(secs / 3600u), (unsigned)(secs / 60u % 60u), (unsigned)(secs % 60u));
    if (len < 0 || (size_t)len >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)len;
}

/** Write the session update document into `writer`, which may be in counting mode. */
static void write_session(sentry_json_t *writer, const sentry_session_t *slot, const char *did,
    const char *release, const char *environment, const char *now)
{
    char sid[SENTRY_MICRO_SESSION_ID_LEN];
    sentry_session_id_format(sid, slot->sid);
    char started[SENTRY_MICRO_RFC3339_LEN];
    sentry_rfc3339_format(started, sizeof(started), slot->started_unix_s);

    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "sid", sid);
    sentry_json_kv_string_opt(writer, "did", did);
    if (!slot->init_sent) {
        /* Omitted rather than false once sent: false is the default, and the bytes are
         * better spent elsewhere. */
        sentry_json_kv_bool(writer, "init", true);
    }
    sentry_json_kv_string(writer, "started", started);
    sentry_json_kv_string(writer, "timestamp", now);
    sentry_json_kv_string(writer, "status", sentry_session_status_name(slot->status));
    sentry_json_kv_uint(writer, "errors", slot->errors);
    if (is_closed(slot->status)) {
        sentry_json_kv_uint(writer, "duration", slot->last_uptime_us / 1000000u);
    }
    sentry_json_key(writer, "attrs");
    sentry_json_object_begin(writer);
    sentry_json_kv_string(writer, "release", release);
    sentry_json_kv_string_opt(writer, "environment", environment);
    sentry_json_object_end(writer);
    sentry_json_object_end(writer);
}

size_t sentry_session_envelope_write(char *buf, size_t cap, const sentry_session_t *slot,
    const char *did, const char *release, const char *environment, uint64_t now_unix_s)
{
    if (buf && cap > 0) {
        buf[0] = '\0';
    }
    if (!sentry_session_pending(slot) || now_unix_s == 0 || slot->started_unix_s == 0 || !release
        || !release[0]) {
        return 0;
    }

    char now[SENTRY_MICRO_RFC3339_LEN];
    char started[SENTRY_MICRO_RFC3339_LEN];
    if (sentry_rfc3339_format(now, sizeof(now), now_unix_s) == 0
        || sentry_rfc3339_format(started, sizeof(started), slot->started_unix_s) == 0) {
        return 0;
    }

    sentry_json_t counter;
    sentry_json_init(&counter, NULL, 0);
    write_session(&counter, slot, did, release, environment, now);
    size_t payload_len = counter.len;

    /* `sent_at` costs one timestamp and lets Relay correct for a device clock that is off by
     * most of an hour or more, which is the difference between a session being counted and
     * being dropped as too old. */
    char header[96];
    int header_len = snprintf(header, sizeof(header),
        "{\"sent_at\":\"%s\"}\n{\"type\":\"session\",\"length\":%u}\n", now, (unsigned)payload_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return 0;
    }

    size_t total = (size_t)header_len + payload_len + 1;
    if (!buf) {
        return total;
    }
    if (total + 1 > cap) {
        buf[0] = '\0';
        return total;
    }
    memcpy(buf, header, (size_t)header_len);
    sentry_json_t writer;
    sentry_json_init(&writer, buf + header_len, cap - (size_t)header_len);
    write_session(&writer, slot, did, release, environment, now);
    buf[header_len + payload_len] = '\n';
    buf[total] = '\0';
    return total;
}
