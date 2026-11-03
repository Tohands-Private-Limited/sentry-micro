/**
 * Sessions — release health for a device that only learns how a boot ended on the next one.
 *
 * Sentry's release health counts sessions: one starts, and later it ends as `exited`,
 * `crashed` or `abnormal`. A desktop SDK sends the end when the process exits. A
 * microcontroller has no such moment: a panic resets the chip before anything can be sent,
 * and a clean reboot is indistinguishable from a crash until the next boot reads the reset
 * reason. So here one boot is one session, and it is closed by the boot after it, from that
 * boot's reset reason.
 *
 * That makes the session state something that has to outlive a reset, which is why it is a
 * plain record of fixed size rather than anything the SDK allocates: `sentry_micro.c` keeps
 * it in the same retained block as the log ring. Two slots are enough. `current` is this
 * boot's session; `previous` is the one the last boot left open, waiting for its close to be
 * sent. A third session arriving while both are still unsent overwrites the older one and is
 * counted, the same visible-truncation rule the log ring uses.
 *
 * Every function here is pure: the clock and the uptime are passed in, and nothing reads a
 * global. That is what lets the boot-to-boot behaviour be tested on a host by calling
 * `sentry_session_open()` twice on the same record, which is all a reboot is from here.
 *
 * Nothing is sent before the device knows the date. Relay requires `started` on every
 * update and silently drops a session whose `started` or `timestamp` is more than five days
 * in the past, so a session dated 1970 is not a degraded session but a lost one. A session is
 * therefore held until the wall clock is known, the same rule logs and metrics follow.
 */
#ifndef SENTRY_MICRO_SESSION_H_INCLUDED
#define SENTRY_MICRO_SESSION_H_INCLUDED

#include "sentry_boot.h"
#include "sentry_device_info.h" /* sentry_reset_reason_t */

/**
 * Compile sessions in (1, the default) or out (0).
 *
 * The record below costs 104 bytes of the retained block and the code that sends it; a
 * build that does not want release health sets this to 0 and carries neither.
 *
 *     build_flags = -D SENTRY_MICRO_SESSIONS_ENABLED=0
 */
#ifndef SENTRY_MICRO_SESSIONS_ENABLED
#    define SENTRY_MICRO_SESSIONS_ENABLED 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** A formatted session id: 36 characters, 8-4-4-4-12, plus the terminator. */
#define SENTRY_MICRO_SESSION_ID_LEN 37

/** An RFC 3339 UTC timestamp to the second, `2026-11-03T10:00:00Z`, plus the terminator. */
#define SENTRY_MICRO_RFC3339_LEN 21

/**
 * Where a session slot is in its life.
 *
 * Stored as a `uint8_t` in the slot, and zero is deliberately "free": the retained block is
 * zeroed whenever it does not come back intact, so a zeroed record has to read as "no
 * sessions at all" without anybody initialising it.
 */
typedef enum {
    SENTRY_SESSION_FREE = 0,
    /** Running, or ran and was never closed. Reported as `ok`. */
    SENTRY_SESSION_OPEN,
    /** Closed by the next boot after an ordinary reset; the close is waiting to be sent. */
    SENTRY_SESSION_EXITED,
    /** Closed by the next boot after a panic, a watchdog or a brownout. */
    SENTRY_SESSION_CRASHED,
} sentry_session_status_t;

/** One session. 48 bytes, measured on the host and on the ESP32-S3. */
typedef struct {
    /** 16 random bytes, formatted as a v4 UUID on the wire; Relay rejects anything else. */
    uint8_t sid[16];
    /**
     * When the session started, in Unix seconds. 0 until the device learns the date.
     *
     * Set once and never changed: Relay requires `started` on every update of a session and
     * requires it to be the same each time, so a value that moved between the init and the
     * close would describe two different sessions.
     */
    uint64_t started_unix_s;
    /**
     * Uptime at the last touch. The next boot reads it to report how long this one ran, and
     * to date a start this boot never learned.
     */
    uint64_t last_uptime_us;
    /** Error and fatal captures during the session. */
    uint32_t errors;
    /**
     * FNV-1a of the release this session ran. The close has to carry the same
     * `attrs.release` as the init, and the record keeps only this hash, not the string; a
     * close found after the firmware changed is skipped rather than sent under the new
     * release, which would blame it for a crash it did not have.
     */
    uint32_t release_hash;
    /** A `sentry_session_status_t`. */
    uint8_t status;
    /** Whether Relay has seen this session's first update, the one carrying `init: true`. */
    bool init_sent;
} sentry_session_t;

/** What lives in the retained block. 104 bytes, measured on the host and on the ESP32-S3. */
typedef struct {
    sentry_session_t current;
    sentry_session_t previous;
    /** Sessions overwritten before they were ever closed. Never sent; reported in the log. */
    uint16_t dropped;
} sentry_session_record_t;

/** What `sentry_session_close_previous()` did, so the caller can say so. */
typedef enum {
    /** Nothing was waiting: the first boot, or the last boot had no session. */
    SENTRY_SESSION_CLOSE_NONE = 0,
    SENTRY_SESSION_CLOSE_EXITED,
    SENTRY_SESSION_CLOSE_CRASHED,
    /** The previous session ran other firmware; it was discarded unclosed. */
    SENTRY_SESSION_CLOSE_SKIPPED_RELEASE,
} sentry_session_close_t;

/**
 * Start this boot's session.
 *
 * If `current` still holds the last boot's session, it moves to `previous` to wait for its
 * close. Should `previous` still hold an older one that was never sent, that one is
 * overwritten and counted in `dropped` — three boots in a row without a clock, typically.
 *
 * `sid` is 16 bytes from the caller's random source. NULL rotates the last session into
 * `previous` and leaves no session running this boot, which is what an entropy failure
 * should do: the last boot can still be closed even though this one cannot be counted.
 * `release` is hashed for the comparison described at `release_hash`.
 *
 * `now_unix_s` may be 0. Returns true when a session was overwritten.
 */
bool sentry_session_open(sentry_session_record_t *rec, uint64_t now_unix_s, uint64_t uptime_us,
    const uint8_t *sid, const char *release);

/**
 * Close the last boot's session from this boot's reset reason. Call once, after open.
 *
 * A panic, any watchdog and a brownout close it as `crashed` — the same set
 * `sentry_reset_reason_is_crash()` names — and everything else as `exited`: a software
 * reset, a power-on, the reset pin, a wake from deep sleep, and a reason the chip could not
 * name. A crashed session reports at least one error, the value ingest would force anyway.
 *
 * If the last boot never learned the date, its start is estimated as this boot's start
 * minus how long it ran — known only once this boot has the clock, so it is filled here if
 * the clock is already known, and by `sentry_session_touch()` otherwise. The estimate is
 * early by the time between the last touch and the reset, plus the reboot itself.
 *
 * `release` is the firmware running now; a session that ran a different one is discarded.
 */
sentry_session_close_t sentry_session_close_previous(sentry_session_record_t *rec,
    sentry_reset_reason_t reset_reason, const char *release, uint64_t now_unix_s,
    uint64_t uptime_us);

/**
 * Record that the device is still running, and date anything not yet dated.
 *
 * Sets `last_uptime_us` on the running session, so the next boot can tell how long this one
 * lasted; call it on every flush. The first call with a known clock fixes the running
 * session's start as `now_unix_s - uptime_us`, and a waiting close's unknown start as
 * described at `sentry_session_close_previous()`. Neither changes after that.
 */
void sentry_session_touch(sentry_session_record_t *rec, uint64_t now_unix_s, uint64_t uptime_us);

/** Count one error against the running session. Saturates. */
void sentry_session_note_error(sentry_session_record_t *rec);

/**
 * Whether `slot` has an update waiting: a close of any kind, or a running session whose
 * init Relay has not seen yet.
 */
bool sentry_session_pending(const sentry_session_t *slot);

/**
 * Write a complete envelope carrying one session update for `slot`:
 *
 *     {"sent_at":"2026-11-03T10:00:00Z"}
 *     {"type":"session","length":N}
 *     {"sid":"...","did":"...","init":true,"started":"...","timestamp":"...","status":"ok",...}
 *
 * `init` is true until `sentry_session_mark_sent()` records that Relay has seen the session,
 * so a close that is also the first update carries it — that is how a session that crashed
 * before the device ever learned the date is still counted. A close adds `duration`, whole
 * seconds of uptime at the last touch; Relay accepts it and ignores it. `did` is the
 * distinct id release health counts as a user; pass the device id.
 *
 * Returns the bytes needed, excluding the NUL, as the other envelope writers do, or 0 when
 * there is nothing that can be sent: no update pending, no clock (`now_unix_s` 0), no start
 * yet, or no `release`, which Relay requires and drops the session without.
 */
size_t sentry_session_envelope_write(char *buf, size_t cap, const sentry_session_t *slot,
    const char *did, const char *release, const char *environment, uint64_t now_unix_s);

/**
 * Record that the update written for `slot` was delivered (or will never be, and should be
 * forgotten). The init is then done; a close frees the slot, because a session in a terminal
 * state must never receive another update.
 */
void sentry_session_mark_sent(sentry_session_t *slot);

/** Wire string for a status: `ok`, `exited` or `crashed`. Never NULL. */
const char *sentry_session_status_name(uint8_t status);

/**
 * Format 16 random bytes as a v4 UUID string, 8-4-4-4-12 lowercase hex.
 *
 * The version and variant bits are stamped by `sentry_event_id_format()`, so a session id
 * and an event id made from the same bytes differ only in the dashes. `out` must hold
 * `SENTRY_MICRO_SESSION_ID_LEN` bytes.
 */
void sentry_session_id_format(char *out, const uint8_t sid[16]);

/**
 * Format Unix seconds as RFC 3339 UTC, `YYYY-MM-DDTHH:MM:SSZ`.
 *
 * Done here rather than with `gmtime()`, which is not on every target and on some is not
 * reentrant. Returns the length written, or 0 when `cap` is too small or the year is past
 * 9999, where the four-digit field would stop being RFC 3339.
 */
size_t sentry_rfc3339_format(char *out, size_t cap, uint64_t unix_s);

/** FNV-1a of `release`; NULL hashes as the empty string. */
uint32_t sentry_session_release_hash(const char *release);

/**
 * A stable identifier for the in-memory layout of the record, for the retained block's
 * check — the same role `sentry_log_ring_layout_id()` plays for the ring.
 */
uint32_t sentry_session_layout_id(void);

#ifdef __cplusplus
}
#endif

#endif /* SENTRY_MICRO_SESSION_H_INCLUDED */
