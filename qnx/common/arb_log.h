/*
 * arb_log.h: logging through the QNX system logger, slogger2 (NFR-Q6).
 *
 * One buffer set per trusted process (arb_guardian, arb_arm, arb_watchdog),
 * registered once at startup. Read on the target with `slog2info -b arb_arm`.
 *
 * Rules for trusted code (docs/QNX_EVIDENCE.md, NFR-Q6):
 *   - Nothing is logged on the decision path. Each server loop logs what changed
 *     (state, counters) after the message was handled and replied to.
 *   - Only slog2c() (constant text) and slog2fa() are used. slog2fa() does not
 *     format: the header says "The format string is not decoded before writing,
 *     it must be decoded during the time of the read" (sys/slog2.h, slog2fa page).
 *   - Every write is bounded: the set is registered with SLOG2_LIMIT_RETRIES, so a
 *     call that cannot get the buffer fails with EBUSY instead of retrying without
 *     limit (the default), and with SLOG2_QUIET, so a failure never writes to stderr.
 *   - Logging never decides anything. If registration fails (slog2_register page:
 *     ENOENT, "slogger2 probably isn't running") the process goes on without the
 *     system log, says so once on stderr, and every log call is a no-op; failed
 *     writes are counted in arb_log_dropped. (Designed so; not yet tested with
 *     slogger2 stopped on the target.)
 *   - The pages do not say whether slog2c()/slog2fa() allocate; they write into
 *     the buffer set mapped by slog2_register() at startup.
 *   - Repeated events (a flood of forged pulses, refused senders) are logged for
 *     the first ARB_LOG_EACH occurrences, then once per ARB_LOG_EVERY, so a flood
 *     cannot wash the earlier lines out of the buffer.
 *
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2_register.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2c.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2fa.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slog2info.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/s/slogger2.html
 * Link with -lslog2.
 */
#ifndef ARB_LOG_H
#define ARB_LOG_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/slog2.h>

#define ARB_LOG_PAGES    4u        /* one buffer of 4 x 4 KB pages per process */
#define ARB_LOG_RETRIES  32u       /* bound on buffer-acquisition retries per write */
#define ARB_LOG_EACH     16u       /* repeated events: log each of the first 16 ... */
#define ARB_LOG_EVERY    1024u     /* ... then once per 1024 */

/* User codes (the slog2info code column; `slog2info -e <code>` filters on them). */
enum {
    ARB_LOG_START   = 1,           /* startup and contract hash */
    ARB_LOG_FAIL    = 2,           /* a startup step failed; the process exits */
    ARB_LOG_STATE   = 3,           /* guardian state change */
    ARB_LOG_FAULT   = 4,           /* a fault: arm send failed, pulse refused, receive errors */
    ARB_LOG_REFUSED = 5,           /* a refused sender, message or forged pulse */
    ARB_LOG_BRAKE   = 6,           /* the arm braked, and why */
    ARB_LOG_TRIP    = 7,           /* a watchdog trip, and its cause */
    ARB_LOG_CLIENT  = 8            /* a binding or registration was made or lost */
};

static slog2_buffer_t arb_log_buf;            /* NULL until registered */
static const char    *arb_log_prog = "arb";   /* for the stderr copy of startup failures */
static uint32_t       arb_log_dropped;        /* writes that failed (e.g. EBUSY) */

/* Once, first thing in main(). Returns 0, or -1 if slogger2 refused (logging off). */
static inline int arb_log_init(const char *set_name, const char *prog)
{
    slog2_buffer_set_config_t cfg;
    slog2_buffer_t handle[1];

    arb_log_prog = prog;
    memset(&cfg, 0, sizeof cfg);
    cfg.num_buffers = 1;
    cfg.buffer_set_name = set_name;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_config[0].buffer_name = "main";
    cfg.buffer_config[0].num_pages = (int)ARB_LOG_PAGES;
    cfg.max_retries = ARB_LOG_RETRIES;
    if (slog2_register(&cfg, handle, SLOG2_LIMIT_RETRIES | SLOG2_QUIET) == -1) {
        arb_log_buf = NULL;
        fprintf(stderr, "%s: slog2_register: %s; running without the system log\n", prog, strerror(errno));
        return -1;
    }
    arb_log_buf = handle[0];
    return 0;
}

static inline void arb_log_ret(int rc)
{
    if (rc == -1) {
        arb_log_dropped++;
    }
}

/* Constant text. */
static inline void arb_log_c(uint16_t code, uint8_t sev, const char *text)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2c(arb_log_buf, code, sev, text));
    }
}

/* Text with one to three integers (formatted by the reader, not here). Every
 * integer is passed as 64 bits, so the format uses %lld for each. */
static inline void arb_log_1(uint16_t code, uint8_t sev, const char *fmt, int64_t a)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_SIGNED(a), SLOG2_FA_END));
    }
}

static inline void arb_log_2(uint16_t code, uint8_t sev, const char *fmt, int64_t a, int64_t b)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_SIGNED(a), SLOG2_FA_SIGNED(b), SLOG2_FA_END));
    }
}

static inline void arb_log_3(uint16_t code, uint8_t sev, const char *fmt, int64_t a, int64_t b, int64_t c)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_SIGNED(a), SLOG2_FA_SIGNED(b),
                            SLOG2_FA_SIGNED(c), SLOG2_FA_END));
    }
}

/* Text with one string (startup only: the contract hash, a path). */
static inline void arb_log_s(uint16_t code, uint8_t sev, const char *fmt, const char *s)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_STRING(s), SLOG2_FA_END));
    }
}

/* One constant string and one integer (e.g. a registration: role, pid). */
static inline void arb_log_si(uint16_t code, uint8_t sev, const char *fmt, const char *s1, int64_t a)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_STRING(s1), SLOG2_FA_SIGNED(a), SLOG2_FA_END));
    }
}

/* Two constant strings and one integer (e.g. a watchdog trip: role, cause, pid). */
static inline void arb_log_ssi(uint16_t code, uint8_t sev, const char *fmt, const char *s1, const char *s2, int64_t a)
{
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, code, sev, fmt, SLOG2_FA_STRING(s1), SLOG2_FA_STRING(s2),
                            SLOG2_FA_SIGNED(a), SLOG2_FA_END));
    }
}

/* A startup step failed: to the system log and to the operator's terminal. Only
 * before the server loop starts, or on the way out. */
static inline void arb_log_fail(const char *what, int err)
{
    fprintf(stderr, "%s: %s: %s\n", arb_log_prog, what, strerror(err));
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, ARB_LOG_FAIL, SLOG2_ERROR, "%s failed, errno %d", SLOG2_FA_STRING(what),
                            SLOG2_FA_SIGNED(err), SLOG2_FA_END));
    }
}

/* F11 case 4: the server behind a name is not the expected user (uid -1: a user
 * this process may not even query). Only before the server loop starts. */
static inline void arb_log_impostor(const char *name, int64_t pid, int64_t uid, int64_t want)
{
    fprintf(stderr, "%s: the server of %s is pid %lld, user %lld, not user %lld: refused, not starting\n",
            arb_log_prog, name, (long long)pid, (long long)uid, (long long)want);
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, ARB_LOG_REFUSED, SLOG2_ERROR,
                            "the server of %s is pid %lld, user %lld, not user %lld: refused", SLOG2_FA_STRING(name),
                            SLOG2_FA_SIGNED(pid), SLOG2_FA_SIGNED(uid), SLOG2_FA_SIGNED(want), SLOG2_FA_END));
    }
}

/* A startup refusal with a code rather than an errno (e.g. the contract load result). */
static inline void arb_log_fail_code(const char *what, int64_t code)
{
    fprintf(stderr, "%s: %s (code %lld); not starting\n", arb_log_prog, what, (long long)code);
    if (arb_log_buf != NULL) {
        arb_log_ret(slog2fa(arb_log_buf, ARB_LOG_FAIL, SLOG2_ERROR, "%s (code %lld); not starting",
                            SLOG2_FA_STRING(what), SLOG2_FA_SIGNED((long long)code), SLOG2_FA_END));
    }
}

/* For a counter that moved from `before` to `after`: log this change? True for
 * the first ARB_LOG_EACH counts, then each time it crosses a multiple of ARB_LOG_EVERY. */
static inline int arb_log_worth(uint64_t before, uint64_t after)
{
    if (after == before) {
        return 0;
    }
    return before < ARB_LOG_EACH || before / ARB_LOG_EVERY != after / ARB_LOG_EVERY;
}

#endif /* ARB_LOG_H */
