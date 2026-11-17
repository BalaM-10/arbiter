/*
 * arb-logd: the evidence logger (FR-E1, FR-E2, FR-E4, FR-E5, FR-C6).
 *
 * Reads the guardian's decision ring (qnx/common/arb_decisions.h, mapped
 * read-only) every 10 ms and writes a SHA-256 hash-chained text log, format
 * docs/EVIDENCE_LOG.md. It also relays the arm's newest telemetry sample to the
 * gateway (arb_logd_proto.h, decision D1): it maps the arm's ring read-only,
 * which the gateway's user may not, and answers each request at once, so the
 * gateway shares no memory with a trusted process (NFR-I1). The guardian never waits for it: if the logger is slow,
 * stopped or dead, the ring overwrites the oldest records and the logger, when
 * it runs again, declares exactly what it lost in a G line (FR-E5). The latest
 * hash is kept in <log>.anchor and logged to slog2 at exit (TM-C6).
 *
 *   arb-logd <contract.bin> <log>     run until SIGTERM, then write the E line;
 *                                     serves ARB_LOGD_NAME meanwhile
 *   arb-logd --selftest               print sha256("abc") (NIST FIPS 180-2 vector)
 *   arb-logd --sha256 <file>...       print "<sha256>  <file>" for each file: the build
 *                                     identifiers of the evidence packet, on the target (FR-E3)
 *
 * Not one of the trusted four (guardian, arm, watchdog, libcontract), but kept
 * plain: static buffers, bounded loops. SHA-256 comes from the QNX Cryptography
 * Library (decision D3). Runs as its own user (813) at priority 30, below every
 * trusted process.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxos.qcrypto/topic/manual/qcrypto_digests.html
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <qcrypto/qcrypto.h>

#include "arb_contract.h"
#include "arb_decisions.h"
#include "arb_hex.h"
#include "arb_ids.h"
#include "arb_log.h"
#include "arb_logd_proto.h"
#include "arb_own_pulse.h"
#include "arb_prio.h"
#include "arb_privdrop.h"
#include "arb_pulses.h"

#define POLL_MS        10
#define LINE_MAX_LEN   384u
#define OUT_BUF        (256u * 1024u)       /* flushed before it can overflow */
#define CONTRACT_MAX   (64u * 1024u)
#define RING_WAIT_MS   5000                 /* the guardian creates the ring at startup */
#define RECV_ERRORS_MAX 100

static const arb_dec_ring_t *g_ring;
static qcrypto_ctx_t *g_sha;
static int g_out = -1, g_anchor = -1;
static char g_prev[65];
static uint64_t g_n;                          /* next line number */
static uint64_t g_next;                       /* next ring index to read */
static uint64_t g_records, g_lost;
static char g_buf[OUT_BUF];
static size_t g_len;
static volatile sig_atomic_t g_stop;
static const arb_telem_t *g_telem;            /* the arm's ring, mapped on the first request */
static int g_tick_scoid = -1;                 /* our own timer's connection (arb_own_pulse.h) */
static uint64_t g_relayed, g_relay_refused, g_foreign;
static int g_chid = -1;

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* SHA-256 of data into 64 lowercase hex characters and a NUL.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxos.qcrypto/topic/qcrypto_digest_init.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxos.qcrypto/topic/qcrypto_digest_update.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxos.qcrypto/topic/qcrypto_digest_final.html */
static int sha256_hex(const void *data, size_t n, char out[65])
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[32];
    size_t dn = sizeof d;

    if (qcrypto_digest_init(g_sha) != 0 || qcrypto_digest_update(g_sha, data, n) != 0 ||
        qcrypto_digest_final(g_sha, d, &dn) != 0 || dn != sizeof d) {
        return -1;
    }
    for (size_t i = 0; i < sizeof d; i++) {   /* bound: 32 */
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15u];
    }
    out[64] = '\0';
    return 0;
}

/* SHA-256 of a whole file (up to FILE_CHUNKS x 64 KB) into hex, the same digest
 * as sha256_hex(), fed one chunk at a time. */
#define FILE_CHUNK   (64u * 1024u)
#define FILE_CHUNKS  4096u                   /* 256 MB: far above any Arbiter binary */
static int sha256_file_hex(const char *path, char out[65])
{
    static uint8_t buf[FILE_CHUNK];
    static const char hx[] = "0123456789abcdef";
    uint8_t d[32];
    size_t dn = sizeof d;
    unsigned chunks = 0;
    ssize_t n = 0;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/read.html */
    const int fd = open(path, O_RDONLY);

    if (fd == -1 || qcrypto_digest_init(g_sha) != 0) {
        if (fd != -1) {
            (void)close(fd);
        }
        return -1;
    }
    while (chunks < FILE_CHUNKS && (n = read(fd, buf, sizeof buf)) > 0) {   /* bound: FILE_CHUNKS */
        if (qcrypto_digest_update(g_sha, buf, (size_t)n) != 0) {
            n = -1;
            break;
        }
        chunks++;
    }
    (void)close(fd);
    if (n != 0 || qcrypto_digest_final(g_sha, d, &dn) != 0 || dn != sizeof d) {
        return -1;                           /* a read error, or a file too large to finish */
    }
    for (size_t i = 0; i < sizeof d; i++) {   /* bound: 32 */
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15u];
    }
    out[64] = '\0';
    return 0;
}

/* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.qnxos.qcrypto/topic/qcrypto_digest_request.html */
static int sha256_open(void)
{
    if (qcrypto_init(QCRYPTO_INIT_LAZY, NULL) != 0) {
        return -1;
    }
    return qcrypto_digest_request("sha256", NULL, 0, &g_sha) == 0 ? 0 : -1;
}

static int write_all(int fd, const char *p, size_t n)
{
    for (int tries = 0; n > 0 && tries < 1000; tries++) {   /* bound: 1000 partial writes */
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/w/write.html */
        const ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return n == 0 ? 0 : -1;
}

/* Write the buffered lines, then the anchor (the latest hash). */
static int flush(void)
{
    if (g_len > 0 && write_all(g_out, g_buf, g_len) != 0) {
        return -1;
    }
    g_len = 0;
    if (g_n > 0) {
        char a[66];
        memcpy(a, g_prev, 64);
        a[64] = '\n';
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pwrite.html */
        if (pwrite(g_anchor, a, 65, 0) != 65) {
            return -1;
        }
    }
    return 0;
}

/* One line: "<tag> <n> <body> prev <hex>", then " hash <hex>\n" (docs/EVIDENCE_LOG.md). */
static int emit(char tag, const char *body)
{
    char line[LINE_MAX_LEN];
    char h[65];
    int len;

    len = snprintf(line, sizeof line, "%c %llu %s prev %s", tag, (unsigned long long)g_n, body, g_prev);
    if (len < 0 || (size_t)len + 72u > sizeof line || sha256_hex(line, (size_t)len, h) != 0) {
        return -1;
    }
    len += snprintf(line + len, sizeof line - (size_t)len, " hash %s\n", h);
    if (g_len + (size_t)len > sizeof g_buf && flush() != 0) {
        return -1;
    }
    memcpy(g_buf + g_len, line, (size_t)len);
    g_len += (size_t)len;
    memcpy(g_prev, h, 65);
    g_n++;
    return 0;
}

static int emit_gap(uint64_t from, uint64_t to)
{
    char body[128];

    (void)snprintf(body, sizeof body, "lost %llu from_idx %llu to_idx %llu", (unsigned long long)(to - from + 1u),
                   (unsigned long long)from, (unsigned long long)to);
    g_lost += to - from + 1u;
    arb_log_2(ARB_LOG_FAULT, SLOG2_WARNING, "fell behind: %lld decision records lost (%lld so far)",
              (int64_t)(to - from + 1u), (int64_t)g_lost);
    return emit('G', body);
}

static int emit_record(const arb_dec_record_t *r)
{
    char body[320];
    const arb_verdict_t *v = &r->verdict;

    (void)snprintf(body, sizeof body,
                   "idx %llu t_ns %lld seq %u target %d %d dur %u verdict %u rule %u before %u after %u flags %u "
                   "fwd %d %d %u latency_cycles %llu",
                   (unsigned long long)r->index, (long long)r->t_ns, r->cmd.seq, r->cmd.target_urad[0],
                   r->cmd.target_urad[1], r->cmd.duration_ms, (unsigned)v->verdict, (unsigned)v->rule,
                   (unsigned)v->state_before, (unsigned)v->state_after, (unsigned)v->flags, v->target_urad[0],
                   v->target_urad[1], v->duration_ms, (unsigned long long)r->latency_cycles);
    g_records++;
    return emit('R', body);
}

/* Everything the guardian published since the last call. A record the ring has
 * already overwritten, or that may be overwritten while we copy it (index <=
 * head2 - SLOTS, arb_decisions.h), is lost and declared, never logged torn. */
static int drain(void)
{
    const uint64_t head = atomic_load_explicit(&((arb_dec_ring_t *)g_ring)->head, memory_order_acquire);
    uint64_t gap_from = 0;
    int in_gap = 0;

    for (uint64_t i = g_next; i < head; i++) {                    /* bound: head - g_next, see below */
        arb_dec_record_t copy;
        uint64_t head2;

        if (head - i > ARB_DEC_SLOTS) {                           /* overwritten already: skip in one step */
            if (!in_gap) {
                gap_from = i;
                in_gap = 1;
            }
            i = head - ARB_DEC_SLOTS;                             /* the loop's i++ moves past it */
            continue;
        }
        copy = g_ring->r[i % ARB_DEC_SLOTS];
        head2 = atomic_load_explicit(&((arb_dec_ring_t *)g_ring)->head, memory_order_acquire);
        if (copy.index != i || i + ARB_DEC_SLOTS <= head2) {
            if (!in_gap) {
                gap_from = i;
                in_gap = 1;
            }
            continue;
        }
        if (in_gap) {
            if (emit_gap(gap_from, i - 1u) != 0) {
                return -1;
            }
            in_gap = 0;
        }
        if (emit_record(&copy) != 0) {
            return -1;
        }
    }
    if (in_gap && emit_gap(gap_from, head - 1u) != 0) {
        return -1;
    }
    g_next = head;
    return flush();
}

/* FR-C6: the contract's identity is the SHA-256 of its source that the compiler
 * embeds in the table (FR-C3), the hash the guardian and the arm print and the
 * bench compares. The table is loaded and checked with libcontract, as they do;
 * a table they would refuse is refused here too. */
static int contract_hash(const char *path, char out[ARB_SHA256_HEX_LEN])
{
    static uint8_t buf[CONTRACT_MAX];
    static arb_contract_table_t table;
    size_t n = 0;
    ssize_t r;
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        return -1;
    }
    while (n < sizeof buf && (r = read(fd, buf + n, sizeof buf - n)) > 0) {   /* bound: CONTRACT_MAX bytes */
        n += (size_t)r;
    }
    (void)close(fd);
    if (n == 0 || n == sizeof buf || arb_contract_load(buf, n, &table) != ARB_LOAD_OK) {
        errno = EINVAL;
        return -1;
    }
    arb_sha256_hex(table.source_sha256, out);
    return 0;
}

static int ring_open(void)
{
    for (int i = 0; i < RING_WAIT_MS / 100; i++) {               /* bound: RING_WAIT_MS / 100 tries */
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
        const int fd = shm_open(ARB_DEC_NAME, O_RDONLY, 0);
        if (fd != -1) {
            const void *p = mmap(NULL, sizeof(arb_dec_ring_t), PROT_READ, MAP_SHARED, fd, 0);
            (void)close(fd);
            if (p == MAP_FAILED) {
                return -1;
            }
            g_ring = p;
            return g_ring->magic == ARB_DEC_MAGIC && g_ring->version == ARB_DEC_VERSION ? 0 : -1;
        }
        const struct timespec d = { 0, 100000000L };
        nanosleep(&d, NULL);
    }
    return -1;
}

/* D1: map the arm's telemetry read-only, on demand (the arm may start after the
 * logger, and the guardian's tests run with a stand-in arm that has none). The
 * ring is 0640 with the logger's group, so the logger's user may open it. */
static void telem_open(void)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
    const int fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    const void *p;

    if (fd == -1) {
        return;
    }
    p = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (p == MAP_FAILED) {
        return;
    }
    if (((const arb_telem_t *)p)->magic != ARB_TELEM_MAGIC || ((const arb_telem_t *)p)->version != ARB_TELEM_VERSION) {
        (void)munmap((void *)p, sizeof(arb_telem_t));
        return;
    }
    g_telem = p;
    arb_log_c(ARB_LOG_CLIENT, SLOG2_INFO, "mapped the arm's telemetry read-only, to relay it to the gateway");
}

/* The arm's newest sample: read head, copy the slot before it, check its index
 * (the arm may have overwritten it meanwhile: then no sample this time). */
static void relay(rcvid_t rcvid)
{
    arb_logd_telem_rep_t rep;

    memset(&rep, 0, sizeof rep);
    rep.version = (uint16_t)ARB_LOGD_PROTO_VERSION;
    if (g_telem == NULL) {
        telem_open();
    }
    if (g_telem != NULL) {
        const uint64_t head = atomic_load_explicit(&((arb_telem_t *)g_telem)->head, memory_order_acquire);
        if (head > 0) {
            rep.s = g_telem->s[(head - 1u) % ARB_TELEM_SLOTS];
            rep.head = head;
            rep.have = rep.s.index == head - 1u;
            if (!rep.have) {
                memset(&rep.s, 0, sizeof rep.s);
            }
        }
    }
    g_relayed++;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
    (void)MsgReply(rcvid, EOK, &rep, sizeof rep);
}

/* One message: a well-formed telemetry request gets the newest sample; anything
 * else an error, after its length is checked and before any field is used. */
static void serve(rcvid_t rcvid, const void *msg, const struct _msg_info *info)
{
    const arb_logd_telem_req_t *req = msg;
    uint16_t type;

    if (info->srcmsglen < (ssize64_t)sizeof type) {
        g_relay_refused++;
        (void)MsgError(rcvid, EBADMSG);
        return;
    }
    memcpy(&type, msg, sizeof type);
    if (type == _IO_CONNECT) {
        (void)MsgReply(rcvid, EOK, NULL, 0);            /* defensive, as the arm */
        return;
    }
    if (type != ARB_LOGD_MSG_TELEM || info->srcmsglen != (ssize64_t)sizeof *req ||
        req->version != ARB_LOGD_PROTO_VERSION || req->reserved != 0) {
        g_relay_refused++;
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
        (void)MsgError(rcvid, type <= _IO_MAX ? ENOSYS : EBADMSG);
        return;
    }
    relay(rcvid);
}

int main(int argc, char **argv)
{
    char contract[65], body[192];
    struct sched_param sp;
    struct sigaction sa;
    int rc;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
        char h[65];
        if (sha256_open() != 0 || sha256_hex("abc", 3, h) != 0) {
            printf("sha256 unavailable\n");
            return 1;
        }
        printf("sha256(abc) %s\n", h);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--sha256") == 0) {
        int bad = 0;
        if (sha256_open() != 0) {
            printf("sha256 unavailable\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {     /* bound: argc */
            char h[65];
            if (sha256_file_hex(argv[i], h) == 0) {
                printf("%s  %s\n", h, argv[i]);
            } else {
                printf("unreadable  %s\n", argv[i]);
                bad = 1;
            }
        }
        return bad;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: arb-logd <contract.bin> <log> | --selftest | --sha256 <file>...\n");
        return 2;
    }
    (void)arb_log_init("arb_logd", "arb-logd");
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_LOGGER;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        arb_log_fail("set FIFO priority", rc);
        return 3;
    }
    if (sha256_open() != 0) {
        arb_log_fail("qcrypto sha256", errno);
        return 4;
    }
    if (contract_hash(argv[1], contract) != 0) {
        arb_log_fail("read the contract table", errno);
        return 2;
    }
    if (ring_open() != 0) {
        arb_log_fail("open the decision ring " ARB_DEC_NAME, errno);
        return 5;
    }
    {
        char anchor_path[512];
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
        g_out = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0644);
        (void)snprintf(anchor_path, sizeof anchor_path, "%s.anchor", argv[2]);
        g_anchor = g_out == -1 ? -1 : open(anchor_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_out == -1 || g_anchor == -1) {
            arb_log_fail("create the evidence log", errno);
            return 5;
        }
    }
    /* SIGTERM ends the log cleanly.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigaction.html */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    (void)sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        arb_log_fail("sigaction", errno);
        return 3;
    }
    /* D1: our own channel (no priority inheritance: a request from the gateway at
     * priority 10 is served at 30; disconnect pulses), our name for the gateway,
     * and a 10 ms timer pulse that paces the draining. Made while still root.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_settime.html */
    {
        struct sigevent ev;
        struct itimerspec its;
        name_attach_t *attach;
        dispatch_t *dpp;
        timer_t timer;
        int coid;

        g_chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT);
        dpp = g_chid == -1 ? NULL : dispatch_create_channel(g_chid, 0);
        attach = dpp == NULL ? NULL : name_attach(dpp, ARB_LOGD_NAME, 0);
        if (attach == NULL || attach->chid != g_chid) {
            arb_log_fail("channel and name_attach " ARB_LOGD_NAME, attach == NULL ? errno : EINVAL);
            return 5;
        }
        coid = ConnectAttach(0, 0, g_chid, _NTO_SIDE_CHANNEL, 0);
        if (coid == -1) {
            arb_log_fail("ConnectAttach", errno);
            return 5;
        }
        SIGEV_PULSE_INIT(&ev, coid, ARB_PRIO_LOGGER, ARB_PULSE_LOGD_TICK, 0);
        memset(&its, 0, sizeof its);
        its.it_value.tv_nsec = POLL_MS * 1000000L;
        its.it_interval.tv_nsec = POLL_MS * 1000000L;
        if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1 || timer_settime(timer, 0, &its, NULL) == -1) {
            arb_log_fail("timer", errno);
            return 5;
        }
    }
    /* NFR-I5: root is needed no further (the files are open, the ring mapped). */
    {
        char report[ARB_PRIVDROP_REPORT_LEN];
        const char *what;
        int err;

        if (arb_privdrop(ARB_UID_LOGD, ARB_PRIO_LOGGER, 0, 0, 0, &what, &err, report) != 0) {
            arb_log_fail(what, err);
            return 8;
        }
        printf("arb-logd: %s\n", report);
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "%s", report);
    }
    memset(g_prev, '0', 64);
    g_prev[64] = '\0';
    (void)snprintf(body, sizeof body, "format 1 contract %s cycles_per_sec %llu start_ns %lld", contract,
                   (unsigned long long)g_ring->cycles_per_sec, (long long)0);
    {
        struct timespec ts;
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            (void)snprintf(body, sizeof body, "format 1 contract %s cycles_per_sec %llu start_ns %lld", contract,
                           (unsigned long long)g_ring->cycles_per_sec,
                           (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec);
        }
    }
    if (emit('H', body) != 0 || flush() != 0) {
        arb_log_fail("write the evidence log", errno);
        return 6;
    }
    printf("arb-logd: ready, %s, contract sha256 %s\n", argv[2], contract);
    arb_log_s(ARB_LOG_START, SLOG2_INFO, "ready, contract sha256 %s", contract);

    /* One message or pulse per pass, until SIGTERM (which interrupts the receive).
     * Each pass is bounded: a drain by the ring size, a request by one sample.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
    {
        int recv_errors = 0;

        while (!g_stop) {
            union {
                struct _pulse        pulse;
                arb_logd_telem_req_t req;
                uint8_t              raw[64];
            } msg;
            struct _msg_info info;
            const rcvid_t rcvid = MsgReceive(g_chid, &msg, sizeof msg, &info);

            if (rcvid == -1) {
                if (errno == EINTR) {
                    continue;                /* SIGTERM: the loop condition ends it */
                }
                if (++recv_errors >= RECV_ERRORS_MAX) {
                    arb_log_fail("MsgReceive, 100 consecutive errors; exiting", errno);
                    return 7;
                }
                continue;
            }
            recv_errors = 0;
            if (rcvid > 0) {
                serve(rcvid, &msg, &info);
                continue;
            }
            if (msg.pulse.code == ARB_PULSE_LOGD_TICK && arb_own_pulse(&g_tick_scoid, msg.pulse.scoid)) {
                if (drain() != 0) {
                    arb_log_fail("write the evidence log", errno);
                    return 6;
                }
            } else if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
                (void)ConnectDetach(msg.pulse.scoid);
            } else if (msg.pulse.code != _PULSE_CODE_UNBLOCK) {
                g_foreign++;                 /* not our tick: ignored (M1-J6) */
            }
        }
    }
    if (drain() != 0) {
        arb_log_fail("write the evidence log", errno);
        return 6;
    }
    (void)snprintf(body, sizeof body, "records %llu lost %llu", (unsigned long long)g_records,
                   (unsigned long long)g_lost);
    if (emit('E', body) != 0 || flush() != 0) {
        arb_log_fail("write the evidence log", errno);
        return 6;
    }
    arb_log_s(ARB_LOG_START, SLOG2_INFO, "stopped; last hash (anchor) %s", g_prev);
    arb_log_2(ARB_LOG_START, SLOG2_INFO, "stopped: %lld records, %lld declared lost", (int64_t)g_records,
              (int64_t)g_lost);
    arb_log_3(ARB_LOG_CLIENT, SLOG2_INFO, "relay: %lld telemetry requests answered, %lld refused, %lld foreign pulses",
              (int64_t)g_relayed, (int64_t)g_relay_refused, (int64_t)g_foreign);
    printf("arb-logd: stopped, %llu records, %llu lost, last hash %s\n", (unsigned long long)g_records,
           (unsigned long long)g_lost, g_prev);
    qcrypto_release_ctx(g_sha);
    qcrypto_uninit();
    return 0;
}
