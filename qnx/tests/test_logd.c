/*
 * On-target tests of the evidence logger arb-logd (FR-E1, FR-E2, FR-E4, FR-E5,
 * FR-C6), with the guardian and the stub arm (qnx/tests/guardian_suite.sh).
 * Format: docs/EVIDENCE_LOG.md. The hashes themselves are recomputed on the host
 * with Python's hashlib (`arbiter evidence verify`, in every bench scenario);
 * here the chain's structure, the contents and the failure behaviour are checked.
 *
 *   test_logd run  <logd-pid> <log> <contract sha256>
 *   test_logd kill <logd-pid> <log>
 *
 * QEMU proves function; the times are QEMU measurements, not claims.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "arb_as_gateway.h"
#include "arb_contract.h"
#include "arb_decisions.h"
#include "arb_devctl.h"
#include "arb_test.h"

#define LOG_MAX   (4u * 1024u * 1024u)
#define LINES_MAX 20000u

static int g_fd = -1;
static uint32_t g_seq = 5000;
static pid_t g_logd;
static const char *g_log;
static const char *g_contract;

static char g_buf[LOG_MAX + 1];
static char *g_line[LINES_MAX];
static unsigned g_nlines;

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

/* Read the whole log and split it into lines (in place). Returns the line count. */
static unsigned load_log(void)
{
    ssize_t n;
    size_t got = 0;
    int fd;

    g_nlines = 0;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    fd = open(g_log, O_RDONLY);
    if (fd == -1) {
        return 0;
    }
    while (got < LOG_MAX && (n = read(fd, g_buf + got, LOG_MAX - got)) > 0) {   /* bound: LOG_MAX bytes */
        got += (size_t)n;
    }
    (void)close(fd);
    g_buf[got] = '\0';
    for (char *p = g_buf; *p != '\0' && g_nlines < LINES_MAX;) {                  /* bound: LINES_MAX */
        char *e = strchr(p, '\n');
        if (e == NULL) {
            break;                       /* an unfinished last line is not a record yet */
        }
        *e = '\0';
        g_line[g_nlines++] = p;
        p = e + 1;
    }
    return g_nlines;
}

/* The value after " <key> " in a line, or NULL. */
static const char *field(const char *line, const char *key)
{
    char pat[48];
    const char *p;

    (void)snprintf(pat, sizeof pat, " %s ", key);
    p = strstr(line, pat);
    return p == NULL ? NULL : p + strlen(pat);
}

static long long num(const char *line, const char *key)
{
    const char *v = field(line, key);
    return v == NULL ? -1 : strtoll(v, NULL, 10);
}

static int hex64(const char *v)
{
    if (v == NULL) {
        return 0;
    }
    for (int i = 0; i < 64; i++) {      /* bound: 64 */
        if (!((v[i] >= '0' && v[i] <= '9') || (v[i] >= 'a' && v[i] <= 'f'))) {
            return 0;
        }
    }
    return v[64] == '\0' || v[64] == ' ';
}

static int submit(int32_t q0, uint32_t ms, uint32_t seq, arb_verdict_wire_t *w)
{
    arb_submit_t u;
    int rc;

    memset(&u, 0, sizeof u);
    u.cmd.version = ARB_PROTO_VERSION;
    u.cmd.seq = seq;
    u.cmd.target_urad[0] = q0;
    u.cmd.duration_ms = ms;
    rc = arb_devctl_gw(g_fd, ARB_DCMD_SUBMIT, &u, sizeof u);
    *w = u.verdict;
    return rc;
}

/* The R line for command sequence number seq, or NULL. */
static const char *record_for(uint32_t seq)
{
    for (unsigned i = 0; i < g_nlines; i++) {           /* bound: g_nlines */
        if (g_line[i][0] == 'R' && num(g_line[i], "seq") == (long long)seq) {
            return g_line[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- run */

/* FR-E1: each decided command appears, with what the guardian replied. */
static void fr_e1_every_decision_is_logged_with_its_fields(void)
{
    arb_verdict_wire_t w[3];
    uint32_t s[3];

    s[0] = ++g_seq;
    s[1] = ++g_seq;
    s[2] = s[0];                                         /* a replay: denied */
    CHECK_EQ_INT(submit(-150000, 1000, s[0], &w[0]), EOK);
    CHECK_EQ_INT(submit(-700000, 100, s[1], &w[1]), EOK);
    CHECK_EQ_INT(submit(-150000, 1000, s[2], &w[2]), EOK);
    ms_sleep(200);                                       /* the logger reads every 10 ms */
    load_log();
    for (int i = 0; i < 3; i++) {                        /* bound: 3 */
        const char *r = i < 2 ? record_for(s[i]) : NULL;
        if (i == 2) {                                    /* the replay is the second R line with s[0] */
            for (unsigned k = 0, seen = 0; k < g_nlines; k++) {   /* bound: g_nlines */
                if (g_line[k][0] == 'R' && num(g_line[k], "seq") == (long long)s[0] && ++seen == 2) {
                    r = g_line[k];
                }
            }
        }
        CHECK(r != NULL);
        if (r == NULL) {
            continue;
        }
        printf("info %s\n", r);
        CHECK_EQ_INT(num(r, "verdict"), w[i].verdict);
        CHECK_EQ_INT(num(r, "rule"), w[i].rule);
        CHECK_EQ_INT(num(r, "after"), w[i].state);
        CHECK(num(r, "t_ns") > 0 && num(r, "latency_cycles") > 0 && num(r, "idx") >= 0);
        CHECK(field(r, "target") != NULL && field(r, "dur") != NULL && field(r, "fwd") != NULL &&
              field(r, "before") != NULL && field(r, "flags") != NULL);
    }
}

/* FR-E2 (structure), FR-C6: line 0 is the header with the contract's hash; every
 * line is numbered in order and carries the previous line's hash. */
static void fr_e2_each_line_carries_the_previous_lines_hash(void)
{
    unsigned bad = 0;

    CHECK(load_log() >= 2);
    if (g_nlines < 2) {
        return;
    }
    CHECK(g_line[0][0] == 'H' && num(g_line[0], "format") == 1);
    CHECK(field(g_line[0], "contract") != NULL && strncmp(field(g_line[0], "contract"), g_contract, 64) == 0);
    CHECK(strncmp(field(g_line[0], "prev"), "0000000000000000000000000000000000000000000000000000000000000000", 64) == 0);
    for (unsigned i = 0; i < g_nlines; i++) {                 /* bound: g_nlines */
        const char *h = field(g_line[i], "hash");
        const char *p = field(g_line[i], "prev");
        if (strtol(g_line[i] + 2, NULL, 10) != (long)i || !hex64(h) || !hex64(p) ||
            (i > 0 && strncmp(p, field(g_line[i - 1], "hash"), 64) != 0)) {
            if (bad++ < 3) {
                printf("  bad line %u: %.120s\n", i, g_line[i]);
            }
        }
    }
    printf("info %u lines, %u not linked to the previous one\n", g_nlines, bad);
    CHECK_EQ_INT(bad, 0);
}

/* FR-E5: a logger that cannot keep up loses the oldest records, and says so in
 * the chain (a G line), and the guardian never waits for it. */
static void fr_e5_a_logger_that_falls_behind_declares_what_it_lost(void)
{
    const uint32_t n = ARB_DEC_SLOTS + 300u;
    arb_verdict_wire_t w;
    uint32_t ok = 0;
    long long lost = 0;
    int r_after = 0;

    CHECK_EQ_INT(kill(g_logd, SIGSTOP), 0);
    for (uint32_t i = 0; i < n; i++) {                   /* bound: n */
        ok += submit(-150000, 1000, 1u, &w) == EOK;      /* replays of an old sequence number */
    }
    CHECK_EQ_INT(kill(g_logd, SIGCONT), 0);
    ms_sleep(500);
    load_log();
    for (unsigned i = 0; i < g_nlines; i++) {            /* bound: g_nlines */
        if (g_line[i][0] == 'G') {
            lost += num(g_line[i], "lost");
            printf("info %s\n", g_line[i]);
        } else if (g_line[i][0] == 'R' && lost > 0) {
            r_after++;
        }
    }
    printf("info %u of %u answered while the logger was stopped; %lld declared lost; %d records after\n", ok, n,
           lost, r_after);
    CHECK_EQ_INT(ok, n);
    CHECK(lost >= 300);
    CHECK(r_after > 0);
}

/* FR-E4: stopped cleanly, the logger ends the chain with an E line and publishes
 * the last hash in <log>.anchor, so lines removed from the end can be detected. */
static void fr_e4_the_logger_ends_with_a_record_and_publishes_its_anchor(void)
{
    char path[256], anchor[80];
    const char *last;
    ssize_t n = -1;
    int fd, status = 0;

    CHECK_EQ_INT(kill(g_logd, SIGTERM), 0);
    for (int i = 0; i < 50 && waitpid(g_logd, &status, WNOHANG) == 0; i++) {   /* bound: 50 x 20 ms */
        ms_sleep(20);
    }
    ms_sleep(50);
    load_log();
    CHECK(g_nlines > 0);
    if (g_nlines == 0) {
        return;
    }
    last = g_line[g_nlines - 1];
    printf("info last line: %.160s\n", last);
    CHECK(last[0] == 'E' && num(last, "records") > 0);
    (void)snprintf(path, sizeof path, "%s.anchor", g_log);
    fd = open(path, O_RDONLY);
    if (fd != -1) {
        n = read(fd, anchor, sizeof anchor - 1);
        (void)close(fd);
    }
    CHECK(n >= 64);
    if (n >= 64) {
        anchor[n] = '\0';
        CHECK(strncmp(anchor, field(last, "hash"), 64) == 0);
    }
}

/* ------------------------------------------------------------------ kill */

/* FR-E5: killing the logger changes no decision: the guardian keeps answering,
 * with the same verdicts, and records no fault. */
static void fr_e5_killing_the_logger_changes_no_decision(void)
{
    arb_verdict_wire_t w;
    arb_status_t s0, s1;
    int ok_before = 0, ok_after = 0, status = 0;

    memset(&s0, 0, sizeof s0);
    memset(&s1, 0, sizeof s1);
    ms_sleep(500);                                       /* the rate limiter refills (50/s, burst 10) */
    CHECK_EQ_INT(devctl(g_fd, ARB_DCMD_GET_STATUS, &s0, sizeof s0, NULL), EOK);
    for (int i = 0; i < 10; i++) {                       /* bound: 10 */
        const int rc = submit(i % 2 ? -150000 : -100000, 1000, ++g_seq, &w);
        ok_before += rc == EOK && w.verdict == ARB_VERDICT_ALLOW;
        if (rc != EOK || w.verdict != ARB_VERDICT_ALLOW) {
            printf("info before the kill, command %d: rc %d verdict %u rule %u state %u\n", i, rc,
                   (unsigned)w.verdict, (unsigned)w.rule, (unsigned)w.state);
        }
        ms_sleep(40);                                    /* below the contract's 50 commands/s */
    }
    CHECK_EQ_INT(kill(g_logd, SIGKILL), 0);
    (void)waitpid(g_logd, &status, 0);
    for (int i = 0; i < 10; i++) {                       /* bound: 10 */
        ok_after += submit(i % 2 ? -150000 : -100000, 1000, ++g_seq, &w) == EOK && w.verdict == ARB_VERDICT_ALLOW;
        ms_sleep(40);
    }
    CHECK_EQ_INT(devctl(g_fd, ARB_DCMD_GET_STATUS, &s1, sizeof s1, NULL), EOK);
    printf("info allowed before the kill %d/10, after %d/10; guardian arm faults %u -> %u, state %u\n", ok_before,
           ok_after, s0.arm_faults, s1.arm_faults, (unsigned)s1.state);
    CHECK_EQ_INT(ok_before, 10);
    CHECK_EQ_INT(ok_after, 10);
    CHECK_EQ_INT(s1.arm_faults, s0.arm_faults);
    CHECK_EQ_INT(s1.bad_messages, s0.bad_messages);
    CHECK(s1.state != ARB_STATE_SAFE_STOP);
}

static const arb_test_case_t run_cases[] = {
    ARB_TEST_CASE(fr_e1_every_decision_is_logged_with_its_fields),
    ARB_TEST_CASE(fr_e2_each_line_carries_the_previous_lines_hash),
    ARB_TEST_CASE(fr_e5_a_logger_that_falls_behind_declares_what_it_lost),
    ARB_TEST_CASE(fr_e4_the_logger_ends_with_a_record_and_publishes_its_anchor),   /* stops the logger: last */
};
static const arb_test_case_t kill_cases[] = { ARB_TEST_CASE(fr_e5_killing_the_logger_changes_no_decision) };

int main(int argc, char **argv)
{
    const char *mode = argc >= 2 ? argv[1] : "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        printf("usage: test_logd run <logd-pid> <log> <contract sha256> | kill <logd-pid> <log>\n");
        return 2;
    }
    g_logd = (pid_t)strtol(argv[2], NULL, 10);
    g_log = argv[3];
    g_fd = open(ARB_DEV_PATH, O_RDWR);
    if (g_fd == -1) {
        printf("FAIL cannot open %s: %s\n", ARB_DEV_PATH, strerror(errno));
        return 1;
    }
    {
        /* Continue after the guardian's published sequence number (FR-G10): an
         * earlier test_logd process used the same guardian. */
        arb_status_t st;
        memset(&st, 0, sizeof st);
        if (devctl(g_fd, ARB_DCMD_GET_STATUS, &st, sizeof st, NULL) == EOK && st.has_seq && st.last_seq > g_seq) {
            g_seq = st.last_seq;
        }
    }
    if (strcmp(mode, "run") == 0 && argc == 5 && strlen(argv[4]) == 64) {
        g_contract = argv[4];
        return arb_test_run("test_logd run", run_cases, 4);
    }
    if (strcmp(mode, "kill") == 0) {
        return arb_test_run("test_logd kill", kill_cases, 1);
    }
    printf("usage: test_logd run <logd-pid> <log> <contract sha256> | kill <logd-pid> <log>\n");
    return 2;
}
