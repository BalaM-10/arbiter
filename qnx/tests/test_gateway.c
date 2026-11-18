/*
 * On-target tests of the gateway arb-gateway (FR-N1, FR-N2, FR-N3, FR-N4,
 * FR-G10, FR-W4), run by qnx/tests/gateway_suite.sh with the watchdog, the arm,
 * the guardian and the gateway running. This program plays the planner and the
 * observers over TCP on the target's loopback, using the protocol in
 * docs/PROTOCOL.md, and checks every outcome against the guardian's own status
 * (devctl GET_STATUS on /dev/arbiter) and the arm's telemetry ring, never
 * against what the gateway says about itself.
 *
 *   test_gateway run|kill <gateway-address> <gateway-pid> <watchdog-pid> <arm-pid> <guardian-pid>
 *
 * The gateway listens on one address only (by default the target's own wired
 * interface, never every address), so the planner and observers connect to it
 * there; the tests also check that it does not answer on the loopback address.
 *
 * run: with the evidence logger (arb-logd) running, which relays the arm's
 * telemetry to the gateway (decision D1). kill: without it.
 *
 * QEMU proves function; the times printed are QEMU measurements, not claims.
 *
 * Sockets: the 8.0 C library reference lists the socket calls as "described
 * elsewhere", in the io-sock Networking APIs page and the FreeBSD 14.4 manual
 * pages it points to.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/socket_api.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/poll.html
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "arb_contract.h"
#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_net_proto.h"
#include "arb_telemetry.h"
#include "arb_test.h"

#define WAIT_MS        2000
#define STEP_URAD      20000          /* small moves near home: far inside every limit */
#define STEP_MS        300

static pid_t g_gw, g_wd, g_arm, g_guard;
static struct in_addr g_gw_addr;      /* the one address the gateway listens on */
static int g_status_fd = -1;
static const arb_telem_t *g_telem;
static uint32_t g_seq;                /* the last sequence number this planner sent */
static int g_have_seq;
static int32_t g_pos[ARB_JOINTS];     /* where this planner believes the arm is */

/* The example frames of docs/PROTOCOL.md, byte for byte. */
static const uint8_t EX_HELLO[64] = { 0x41, 0x52, 0x42, 0x50, 0x01, 0x01 };
static const uint8_t EX_COMMAND[64] = {
    0x41, 0x52, 0x42, 0x50, 0x01, 0x02, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xdc, 0x05, 0x00, 0x00,
    0x00, 0xca, 0x9a, 0x3b, 0x00, 0x00, 0x00, 0x00, 0xb1, 0x02, 0xf8, 0xff, 0x9e, 0xfa, 0x0f, 0x00 };
static const uint8_t EX_WELCOME[64] = {
    0x41, 0x52, 0x42, 0x50, 0x01, 0x81, 0x00, 0x00, 0x02, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00 };
static const uint8_t EX_VERDICT[64] = {
    0x41, 0x52, 0x42, 0x50, 0x01, 0x82, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x02, 0x05, 0x02,
    0x00, 0x00, 0x00, 0x00, 0xb1, 0x02, 0xf8, 0xff, 0x9e, 0xfa, 0x0f, 0x00, 0x2e, 0x08, 0x00, 0x00,
    0xdc, 0x05, 0x00, 0x00 };

/* ------------------------------------------------------------------ helpers */

static int64_t now_ms(void)
{
    struct timespec ts;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ms_sleep(long ms)
{
    const struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&d, NULL);
}

static int status(arb_status_t *s)
{
    memset(s, 0, sizeof *s);
    return devctl(g_status_fd, ARB_DCMD_GET_STATUS, s, sizeof *s, NULL);
}

static void frame_init(arb_net_frame_t *f, uint8_t type)
{
    static const uint8_t magic[4] = ARB_NET_MAGIC;

    memset(f, 0, sizeof *f);
    memcpy(f->h.magic, magic, sizeof magic);
    f->h.version = ARB_NET_VERSION;
    f->h.type = type;
}

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {         /* bound: n <= 64 */
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* A well-formed gateway frame: magic, version, a known type, zero reserved bytes. */
static int frame_ok(const arb_net_frame_t *f)
{
    static const uint8_t magic[4] = ARB_NET_MAGIC;

    return memcmp(f->h.magic, magic, 4) == 0 && f->h.version == ARB_NET_VERSION && f->h.reserved == 0 &&
           (f->h.type == ARB_NET_WELCOME || f->h.type == ARB_NET_VERDICT || f->h.type == ARB_NET_TELEMETRY ||
            f->h.type == ARB_NET_STATE || f->h.type == ARB_NET_ERROR);
}

static int connect_to(struct in_addr addr, int port)
{
    struct sockaddr_in a;
    const int one = 1;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        return -1;
    }
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr = addr;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == -1) {
        const int e = errno;
        (void)close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static int gw_connect(int port)
{
    return connect_to(g_gw_addr, port);
}

static int send_bytes(int fd, const void *p, size_t n)
{
    return send(fd, p, n, MSG_NOSIGNAL) == (ssize_t)n ? 0 : -1;
}

/* One frame within timeout_ms: 1 got it, 0 timed out, -1 the connection closed or failed. */
static int recv_frame(int fd, arb_net_frame_t *f, int timeout_ms)
{
    const int64_t end = now_ms() + timeout_ms;
    size_t got = 0;

    for (int i = 0; i < 1000 && got < sizeof *f; i++) {     /* bound: 1000 partial reads */
        struct pollfd p = { fd, POLLIN, 0 };
        const int64_t left = end - now_ms();
        ssize_t n;

        if (left <= 0 || poll(&p, 1, (int)left) <= 0) {
            return 0;
        }
        n = recv(fd, f->b + got, sizeof *f - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    return got == sizeof *f ? 1 : 0;
}

/* The next frame of one type, skipping TELEMETRY and STATE; 1, 0 or -1 as recv_frame. */
static int recv_type(int fd, uint8_t type, arb_net_frame_t *f, int timeout_ms)
{
    for (int i = 0; i < 2000; i++) {                         /* bound: 2000 frames */
        const int r = recv_frame(fd, f, timeout_ms);
        if (r != 1) {
            return r;
        }
        if (f->h.type == type) {
            return 1;
        }
        if (f->h.type != ARB_NET_TELEMETRY && f->h.type != ARB_NET_STATE) {
            return 0;                                         /* something else came first */
        }
    }
    return 0;
}

/* After an ERROR the gateway closes: read until the end of the stream. 1 if it closed. */
static int closed_within(int fd, int timeout_ms)
{
    arb_net_frame_t f;

    for (int i = 0; i < 2000; i++) {                         /* bound: 2000 frames */
        const int r = recv_frame(fd, &f, timeout_ms);
        if (r == -1) {
            return 1;
        }
        if (r == 0) {
            return 0;
        }
    }
    return 0;
}

/* Connect as the planner and say HELLO; returns the socket, with the WELCOME in *w. */
static int planner_open(arb_net_welcome_t *w)
{
    arb_net_frame_t f;
    const int fd = gw_connect(ARB_NET_PLANNER_PORT);

    if (fd == -1) {
        return -1;
    }
    frame_init(&f, ARB_NET_HELLO);
    if (send_bytes(fd, &f, sizeof f) != 0 || recv_type(fd, ARB_NET_WELCOME, &f, WAIT_MS) != 1) {
        (void)close(fd);
        return -1;
    }
    *w = f.welcome;
    if (w->has_seq) {
        g_seq = w->last_seq;
        g_have_seq = 1;
    }
    return fd;
}

static int send_command(int fd, uint32_t seq, int32_t q0, int32_t q1, uint32_t ms)
{
    arb_net_frame_t f;

    frame_init(&f, ARB_NET_COMMAND);
    f.command.seq = seq;
    f.command.duration_ms = ms;
    f.command.sender_ts_ns = (uint64_t)now_ms() * 1000000u;
    f.command.target_urad[0] = q0;
    f.command.target_urad[1] = q1;
    return send_bytes(fd, &f, sizeof f);
}

/* One small move, next sequence number: 1 if it came back decided ALLOW. */
static int step(int fd, arb_net_verdict_t *out)
{
    arb_net_frame_t f;
    const int32_t q0 = g_pos[0] == 0 ? -STEP_URAD : 0;

    g_seq = g_have_seq ? g_seq + 1u : 1u;
    g_have_seq = 1;
    if (send_command(fd, g_seq, q0, q0 == 0 ? 0 : STEP_URAD, STEP_MS) != 0 ||
        recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS) != 1) {
        return 0;
    }
    if (out != NULL) {
        *out = f.verdict;
    }
    if (f.verdict.result == ARB_NET_DECIDED && f.verdict.verdict == ARB_VERDICT_ALLOW && f.verdict.seq == g_seq) {
        g_pos[0] = q0;
        g_pos[1] = q0 == 0 ? 0 : STEP_URAD;
        return 1;
    }
    return 0;
}

/* Poll the guardian's state every 1 ms; returns ms until it equals want, or -1. */
static long state_within(uint8_t want, long timeout_ms)
{
    const int64_t t0 = now_ms();
    arb_status_t s;

    for (long i = 0; i < timeout_ms; i++) {                  /* bound: timeout_ms polls */
        if (status(&s) == EOK && s.state == want) {
            return (long)(now_ms() - t0);
        }
        ms_sleep(1);
    }
    return -1;
}

static int alive(pid_t p)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/k/kill.html */
    return kill(p, 0) == 0;
}

/* The arm's newest telemetry sample (the test runs as root, which may read the ring). */
static arb_telem_sample_t arm_sample(void)
{
    const uint64_t head = atomic_load_explicit(&((arb_telem_t *)g_telem)->head, memory_order_acquire);
    arb_telem_sample_t s;

    memset(&s, 0, sizeof s);
    if (head > 0) {
        s = g_telem->s[(head - 1u) % ARB_TELEM_SLOTS];
    }
    return s;
}

/* -------------------------------------------------------------------- tests */

/* The layout in qnx/common/arb_net_proto.h, built on QNX, gives exactly the
 * documented bytes, and the documented WELCOME and VERDICT read back as documented. */
static void fr_n2_frames_match_the_documented_examples(void)
{
    arb_net_frame_t f;

    frame_init(&f, ARB_NET_HELLO);
    CHECK(memcmp(f.b, EX_HELLO, 64) == 0);
    frame_init(&f, ARB_NET_COMMAND);
    f.command.seq = 7;
    f.command.duration_ms = 1500;
    f.command.sender_ts_ns = 1000000000u;
    f.command.target_urad[0] = -523599;
    f.command.target_urad[1] = 1047198;
    CHECK(memcmp(f.b, EX_COMMAND, 64) == 0);
    memcpy(f.b, EX_WELCOME, 64);
    CHECK(frame_ok(&f) && f.h.type == ARB_NET_WELCOME);
    CHECK_EQ_INT(f.welcome.state, ARB_STATE_NORMAL);
    CHECK_EQ_INT(f.welcome.has_seq, 1);
    CHECK_EQ_INT(f.welcome.joints, 2);
    CHECK_EQ_INT(f.welcome.last_seq, 6);
    CHECK_EQ_INT(f.welcome.session, 1);
    CHECK_EQ_INT(f.welcome.telemetry_period_ms, 10);
    memcpy(f.b, EX_VERDICT, 64);
    CHECK(frame_ok(&f) && f.h.type == ARB_NET_VERDICT);
    CHECK_EQ_INT(f.verdict.seq, 7);
    CHECK_EQ_INT(f.verdict.result, ARB_NET_DECIDED);
    CHECK_EQ_INT(f.verdict.verdict, ARB_VERDICT_CLAMP);
    CHECK_EQ_INT(f.verdict.rule, ARB_RULE_VELOCITY);
    CHECK_EQ_INT(f.verdict.fwd_urad[0], -523599);
    CHECK_EQ_INT(f.verdict.fwd_urad[1], 1047198);
    CHECK_EQ_INT(f.verdict.fwd_duration_ms, 2094);
    CHECK_EQ_INT(f.verdict.requested_ms, 1500);
}

/* The gateway listens on its one address only: nothing listens on the loopback
 * address, on either port, so it serves no address it was not given. */
static void fr_n1_gateway_is_not_reachable_on_other_addresses(void)
{
    const int ports[2] = { ARB_NET_PLANNER_PORT, ARB_NET_OBSERVER_PORT };
    struct in_addr lo;
    int fd;

    lo.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(g_gw_addr.s_addr != lo.s_addr);
    for (int i = 0; i < 2; i++) {                                /* bound: 2 ports */
        int e;
        fd = connect_to(lo, ports[i]);
        e = errno;
        printf("  127.0.0.1:%d: %s\n", ports[i], fd == -1 ? strerror(e) : "connected");
        CHECK(fd == -1 && e == ECONNREFUSED);
        if (fd != -1) {
            (void)close(fd);
        }
    }
    fd = gw_connect(ARB_NET_OBSERVER_PORT);                      /* while it does answer on its own */
    CHECK(fd != -1);
    if (fd != -1) {
        (void)close(fd);
    }
    ms_sleep(50);
}

/* HELLO gets WELCOME (the guardian's own state and sequence), then STATE. */
static void fr_n1_planner_gets_welcome_then_state(void)
{
    arb_net_frame_t f;
    arb_status_t s;
    const int fd = gw_connect(ARB_NET_PLANNER_PORT);

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    frame_init(&f, ARB_NET_HELLO);
    CHECK(send_bytes(fd, &f, sizeof f) == 0);
    CHECK_EQ_INT(recv_type(fd, ARB_NET_WELCOME, &f, WAIT_MS), 1);
    CHECK(frame_ok(&f));
    CHECK(all_zero(f.welcome.zero, sizeof f.welcome.zero));
    CHECK(status(&s) == EOK);
    CHECK_EQ_INT(f.welcome.state, s.state);
    CHECK_EQ_INT(f.welcome.has_seq, s.has_seq);
    CHECK_EQ_INT(f.welcome.last_seq, s.last_seq);
    CHECK_EQ_INT(f.welcome.joints, 2);
    CHECK(f.welcome.session >= 1);
    CHECK_EQ_INT(f.welcome.telemetry_period_ms, ARB_NET_TELEM_PERIOD_MS);
    printf("  WELCOME state %u has_seq %u last_seq %u session %u\n", f.welcome.state, f.welcome.has_seq,
           f.welcome.last_seq, f.welcome.session);
    CHECK_EQ_INT(recv_type(fd, ARB_NET_STATE, &f, WAIT_MS), 1);
    CHECK(frame_ok(&f));
    CHECK_EQ_INT(f.state.state, s.state);
    CHECK_EQ_INT(f.state.last_seq, s.last_seq);
    (void)close(fd);
    ms_sleep(50);
}

/* One planner at a time: a second connection gets ERROR busy and is closed; the
 * first one keeps working. */
static void fr_n1_second_planner_is_refused_busy(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    const int a = planner_open(&w);
    const int b = gw_connect(ARB_NET_PLANNER_PORT);

    CHECK(a != -1 && b != -1);
    if (a == -1 || b == -1) {
        return;
    }
    CHECK_EQ_INT(recv_type(b, ARB_NET_ERROR, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.error.code, ARB_NET_E_BUSY);
    CHECK(closed_within(b, WAIT_MS));
    CHECK(step(a, NULL));                                      /* the first planner is unaffected */
    (void)close(b);
    (void)close(a);
    ms_sleep(50);
}

/* Each well-formed COMMAND reaches the guardian once and gets its own VERDICT,
 * in order, carrying exactly what the guardian decided. */
static void fr_n1_each_command_is_forwarded_and_answered_in_order(void)
{
    arb_net_welcome_t w;
    arb_status_t s0, s1;
    const int fd = planner_open(&w);
    int allowed = 0;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    for (int i = 0; i < 10; i++) {                             /* bound: 10 */
        arb_net_verdict_t v;
        if (step(fd, &v)) {
            allowed++;
            CHECK_EQ_INT(v.rule, ARB_RULE_NONE);
            CHECK_EQ_INT(v.fwd_urad[0], g_pos[0]);
            CHECK_EQ_INT(v.fwd_urad[1], g_pos[1]);
            CHECK_EQ_INT(v.requested_ms, STEP_MS);
            CHECK_EQ_INT(v.state, ARB_STATE_NORMAL);
            CHECK(all_zero(v.zero, sizeof v.zero));
        }
        ms_sleep(STEP_MS);
    }
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(allowed, 10);
    CHECK_EQ_INT(s1.decisions - s0.decisions, 10);             /* each one decided, once */
    CHECK_EQ_INT(s1.last_seq, g_seq);
    CHECK_EQ_INT(s1.submits_refused, s0.submits_refused);      /* the gateway submits as its own user */
    printf("  10 commands: %d ALLOW, guardian decisions +%u, last_seq %u\n", allowed, s1.decisions - s0.decisions,
           s1.last_seq);
    (void)close(fd);
    ms_sleep(50);
}

/* Each kind of malformed frame: ERROR with its code, the connection closed, and
 * nothing reached the guardian. The gateway then serves the next planner. */
static void fr_n2_malformed_frames_end_the_session_unforwarded(void)
{
    struct {
        const char *what;
        int hello_first;
        int offset;                     /* byte changed in a valid COMMAND, or -1 for a HELLO-less COMMAND */
        uint8_t value;
        uint8_t code;
    } cases[] = {
        { "bad magic",           1, 0,  0x58, ARB_NET_E_MAGIC },
        { "bad version",         1, 4,  0x02, ARB_NET_E_VERSION },
        { "unknown type",        1, 5,  0x7f, ARB_NET_E_TYPE },
        { "gateway-only type",   1, 5,  ARB_NET_VERDICT, ARB_NET_E_TYPE },
        { "reserved byte set",   1, 7,  0x01, ARB_NET_E_RESERVED },
        { "unused byte set",     1, 63, 0x01, ARB_NET_E_RESERVED },
        { "COMMAND before HELLO", 0, -1, 0,   ARB_NET_E_ORDER },
        { "second HELLO",        1, 5,  ARB_NET_HELLO, ARB_NET_E_ORDER },
    };
    arb_status_t s0, s1;

    CHECK(status(&s0) == EOK);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {   /* bound: 8 cases */
        arb_net_welcome_t w;
        arb_net_frame_t f;
        int fd;

        if (cases[i].hello_first) {
            fd = planner_open(&w);
        } else {
            fd = gw_connect(ARB_NET_PLANNER_PORT);
        }
        CHECK(fd != -1);
        if (fd == -1) {
            continue;
        }
        frame_init(&f, ARB_NET_COMMAND);
        f.command.seq = g_seq + 1u;                         /* would be valid if forwarded */
        f.command.duration_ms = STEP_MS;
        if (cases[i].offset >= 0) {
            f.b[cases[i].offset] = cases[i].value;
        }
        if (cases[i].value == ARB_NET_HELLO && cases[i].offset == 5) {
            memset(f.b + 8, 0, 56);                          /* a clean second HELLO */
        }
        CHECK(send_bytes(fd, &f, sizeof f) == 0);
        if (recv_type(fd, ARB_NET_ERROR, &f, WAIT_MS) == 1) {
            if (f.error.code != cases[i].code) {
                printf("  %s: ERROR %u, expected %u\n", cases[i].what, f.error.code, cases[i].code);
                arb_test_failures++;
            }
        } else {
            printf("  %s: no ERROR frame\n", cases[i].what);
            arb_test_failures++;
        }
        if (!closed_within(fd, WAIT_MS)) {
            printf("  %s: the gateway did not close the connection\n", cases[i].what);
            arb_test_failures++;
        }
        (void)close(fd);
        ms_sleep(20);
    }
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.decisions, s0.decisions);                  /* nothing was forwarded */
    CHECK_EQ_INT(s1.bad_messages, s0.bad_messages);            /* nor anything malformed passed on */
    CHECK(alive(g_gw));
    {
        arb_net_welcome_t w;
        const int fd = planner_open(&w);
        CHECK(fd != -1 && step(fd, NULL));                     /* the next planner is served */
        if (fd != -1) {
            (void)close(fd);
        }
    }
    ms_sleep(50);
}

/* A partial frame followed by a close is dropped, never forwarded. */
static void fr_n2_a_partial_frame_is_never_forwarded(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    arb_status_t s0, s1;
    int fd = planner_open(&w);

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    frame_init(&f, ARB_NET_COMMAND);
    f.command.seq = g_seq + 1u;
    f.command.duration_ms = STEP_MS;
    CHECK(send_bytes(fd, &f, 40) == 0);
    ms_sleep(100);
    (void)close(fd);
    ms_sleep(100);
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.decisions, s0.decisions);
    fd = planner_open(&w);
    CHECK(fd != -1 && step(fd, NULL));
    if (fd != -1) {
        (void)close(fd);
    }
    ms_sleep(50);
}

/* FR-G10 through the gateway: when the planner's connection closes, the gateway
 * closes its submit handle and the guardian goes from NORMAL to HOLD at once,
 * well before the silence timeout could. */
static void fr_g10_planner_disconnect_holds_at_once(void)
{
    arb_net_welcome_t w;
    const int fd = planner_open(&w);
    long ms;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(step(fd, NULL));
    CHECK(state_within(ARB_STATE_NORMAL, 100) >= 0);
    ms_sleep(20);
    (void)close(fd);
    ms = state_within(ARB_STATE_HOLD, 1000);
    printf("  planner closed -> guardian HOLD after %ld ms (silence timeout %u ms; QEMU, function only)\n", ms,
           250u);
    CHECK(ms >= 0 && ms < 150);       /* 20 + 150 ms after the last command: before the 250 ms silence timeout */
    ms_sleep(50);
}

/* A planner that reconnects gets the guardian's last sequence number and
 * continues at last_seq + 1; the old number is refused as a replay. */
static void fr_g10_planner_resumes_at_last_seq_plus_one(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    arb_status_t s;
    const uint32_t sent = g_seq;
    const int fd = planner_open(&w);

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(status(&s) == EOK);
    CHECK_EQ_INT(w.has_seq, 1);
    CHECK_EQ_INT(w.last_seq, sent);
    CHECK_EQ_INT(w.last_seq, s.last_seq);
    CHECK(send_command(fd, w.last_seq, g_pos[0], g_pos[1], STEP_MS) == 0);
    CHECK_EQ_INT(recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.verdict.result, ARB_NET_DECIDED);
    CHECK_EQ_INT(f.verdict.verdict, ARB_VERDICT_DENY);
    CHECK_EQ_INT(f.verdict.rule, ARB_RULE_SEQUENCE);
    g_seq = w.last_seq;
    CHECK(step(fd, NULL));                                     /* last_seq + 1 */
    CHECK_EQ_INT(g_seq, w.last_seq + 1u);
    printf("  WELCOME last_seq %u: seq %u DENY (sequence), seq %u ALLOW\n", w.last_seq, w.last_seq, g_seq);
    (void)close(fd);
    ms_sleep(50);
}

/* Observers get WELCOME at once and a copy of every VERDICT, byte for byte. */
static void fr_n4_observer_gets_welcome_and_every_verdict(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    const int ob = gw_connect(ARB_NET_OBSERVER_PORT);
    int pl;

    CHECK(ob != -1);
    if (ob == -1) {
        return;
    }
    CHECK_EQ_INT(recv_type(ob, ARB_NET_WELCOME, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.welcome.session, 0);
    pl = planner_open(&w);
    CHECK(pl != -1);
    for (int i = 0; pl != -1 && i < 3; i++) {                 /* bound: 3 */
        arb_net_verdict_t v;
        CHECK(step(pl, &v));
        if (recv_type(ob, ARB_NET_VERDICT, &f, WAIT_MS) == 1) {
            CHECK(memcmp(&f.verdict, &v, sizeof v) == 0);
        } else {
            printf("  observer: no VERDICT copy for seq %u\n", v.seq);
            arb_test_failures++;
        }
        ms_sleep(STEP_MS);
    }
    if (pl != -1) {
        (void)close(pl);
    }
    (void)close(ob);
    ms_sleep(50);
}

/* Observers only receive: one that sends anything gets ERROR read-only, is closed,
 * and nothing reaches the guardian. */
static void fr_n4_observer_is_read_only(void)
{
    arb_net_frame_t f;
    arb_status_t s0, s1;
    const int ob = gw_connect(ARB_NET_OBSERVER_PORT);

    CHECK(ob != -1);
    if (ob == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    CHECK_EQ_INT(recv_type(ob, ARB_NET_WELCOME, &f, WAIT_MS), 1);
    frame_init(&f, ARB_NET_ESTOP);                             /* even a stop: observers have no say */
    CHECK(send_bytes(ob, &f, sizeof f) == 0);
    CHECK_EQ_INT(recv_type(ob, ARB_NET_ERROR, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.error.code, ARB_NET_E_READ_ONLY);
    CHECK(closed_within(ob, WAIT_MS));
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.estops, s0.estops);
    CHECK_EQ_INT(s1.decisions, s0.decisions);
    (void)close(ob);
    ms_sleep(50);
}

/* At most ARB_NET_OBSERVERS_MAX observers; one more gets ERROR busy. */
static void fr_n4_one_observer_too_many_is_refused_busy(void)
{
    arb_net_frame_t f;
    int ob[ARB_NET_OBSERVERS_MAX];
    int extra;

    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {         /* bound: 2 */
        ob[i] = gw_connect(ARB_NET_OBSERVER_PORT);
        CHECK(ob[i] != -1 && recv_type(ob[i], ARB_NET_WELCOME, &f, WAIT_MS) == 1);
    }
    extra = gw_connect(ARB_NET_OBSERVER_PORT);
    CHECK(extra != -1);
    if (extra != -1) {
        CHECK_EQ_INT(recv_type(extra, ARB_NET_ERROR, &f, WAIT_MS), 1);
        CHECK_EQ_INT(f.error.code, ARB_NET_E_BUSY);
        CHECK(closed_within(extra, WAIT_MS));
        (void)close(extra);
    }
    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {         /* bound: 2 */
        if (ob[i] != -1) {
            (void)close(ob[i]);
        }
    }
    ms_sleep(50);
}

/* FR-N3: a gateway that hangs (stopped mid-move with the planner connected) is
 * covered by the guardian's silence timeout: HOLD, and the trusted processes run
 * on untouched. When it runs again the same session continues. */
static void fr_n3_a_stopped_gateway_holds_and_trusted_processes_run_on(void)
{
    arb_net_welcome_t w;
    arb_status_t s0, s1;
    const int fd = planner_open(&w);
    long ms;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    CHECK(step(fd, NULL));
    CHECK(kill(g_gw, SIGSTOP) == 0);
    ms = state_within(ARB_STATE_HOLD, 1000);
    printf("  gateway stopped -> guardian HOLD after %ld ms (silence timeout 250 ms; QEMU, function only)\n", ms);
    CHECK(ms >= 0 && ms < 600);
    ms_sleep(200);
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.state, ARB_STATE_HOLD);                     /* HOLD, not SAFE_STOP */
    CHECK_EQ_INT(s1.watchdog_trips, s0.watchdog_trips);
    CHECK_EQ_INT(s1.watchdog_lost, s0.watchdog_lost);
    CHECK_EQ_INT(s1.arm_faults, s0.arm_faults);
    CHECK(s1.ticks > s0.ticks);                                 /* the guardian kept ticking */
    CHECK(alive(g_wd) && alive(g_arm) && alive(g_guard));
    CHECK(kill(g_gw, SIGCONT) == 0);
    ms_sleep(50);
    CHECK(step(fd, NULL));                                      /* the session goes on */
    CHECK(state_within(ARB_STATE_NORMAL, 200) >= 0);
    (void)close(fd);
    ms_sleep(50);
}

/* Collect TELEMETRY frames for ms; each one is checked against the arm's own
 * ring at the same index. Returns the number of frames that matched; *bad counts
 * those that did not, or came out of order, or declared drops. */
static int telemetry_for(int fd, long ms, int *bad, int *moving)
{
    const int64_t end = now_ms() + ms;
    uint64_t last = 0;
    int32_t first_pos = 0;
    int n = 0;

    *bad = 0;
    *moving = 0;
    for (int i = 0; i < 5000 && now_ms() < end; i++) {          /* bound: 5000 frames */
        arb_net_frame_t f;
        arb_telem_sample_t r;
        const int got = recv_frame(fd, &f, (int)(end - now_ms()));

        if (got != 1) {
            break;
        }
        if (f.h.type != ARB_NET_TELEMETRY) {
            continue;
        }
        r = g_telem->s[f.telemetry.index % ARB_TELEM_SLOTS];
        if (r.index != f.telemetry.index || r.t_ns != f.telemetry.t_ns || r.pos_urad[0] != f.telemetry.pos_urad[0] ||
            r.pos_urad[1] != f.telemetry.pos_urad[1] || r.vel_urad_s[0] != f.telemetry.vel_urad_s[0] ||
            r.vel_urad_s[1] != f.telemetry.vel_urad_s[1] || r.segment != f.telemetry.segment ||
            r.state != f.telemetry.arm_state || r.lease_ok != f.telemetry.lease_ok ||
            (n > 0 && f.telemetry.index <= last) || f.telemetry.dropped != 0 || f.telemetry.reserved != 0 ||
            !all_zero(f.telemetry.zero, sizeof f.telemetry.zero)) {
            if (*bad < 3) {
                printf("  frame index %llu does not match the arm's ring (ring index %llu)\n",
                       (unsigned long long)f.telemetry.index, (unsigned long long)r.index);
            }
            (*bad)++;
            continue;
        }
        if (n == 0) {
            first_pos = f.telemetry.pos_urad[0];
        } else if (f.telemetry.pos_urad[0] != first_pos) {
            *moving = 1;
        }
        last = f.telemetry.index;
        n++;
    }
    return n;
}

/* NFR-I1, D1: the arm's telemetry ring is 0640 and belongs to the logger's group,
 * so the gateway's user cannot open it; the gateway gets telemetry only from the
 * logger, by message passing. */
static void nfr_i1_gateway_user_cannot_open_the_arm_telemetry(void)
{
    struct stat st;
    int fd, e;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fstat.html */
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    CHECK(fd != -1 && fstat(fd, &st) == 0);
    if (fd != -1) {
        CHECK_EQ_INT(st.st_mode & 0777, 0640);
        CHECK_EQ_INT(st.st_uid, ARB_UID_ARM);
        CHECK_EQ_INT(st.st_gid, ARB_UID_LOGD);
        (void)close(fd);
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/seteuid.html */
    CHECK(seteuid((uid_t)ARB_UID_GATEWAY) == 0);
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    e = errno;
    CHECK(seteuid(0) == 0);
    CHECK(fd == -1 && e == EACCES);
    if (fd != -1) {
        (void)close(fd);
    }
    printf("  arm telemetry: mode %o, owner %u, group %u; the gateway's user: %s\n", (unsigned)(st.st_mode & 0777),
           (unsigned)st.st_uid, (unsigned)st.st_gid, fd == -1 ? strerror(e) : "opened it");
}

/* FR-N4, D1: the planner receives the arm's joint state, about every 10 ms, each
 * frame exactly the arm's own sample at that index, while the arm moves. */
static void fr_n4_planner_gets_arm_telemetry_through_the_logger(void)
{
    arb_net_welcome_t w;
    const int fd = planner_open(&w);
    int n, bad, moving;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(step(fd, NULL));                       /* a 300 ms move */
    n = telemetry_for(fd, 500, &bad, &moving);
    printf("  500 ms: %d TELEMETRY frames matching the arm's ring, %d not; arm moving: %s (QEMU, function only)\n",
           n, bad, moving ? "yes" : "no");
    CHECK(n >= 25);                              /* about 50 at 10 ms; QEMU proves function */
    CHECK_EQ_INT(bad, 0);
    CHECK(moving);
    (void)close(fd);
    ms_sleep(50);
}

/* FR-N4: observers receive the same telemetry. */
static void fr_n4_observer_gets_arm_telemetry(void)
{
    arb_net_frame_t f;
    const int ob = gw_connect(ARB_NET_OBSERVER_PORT);
    int n, bad, moving;

    CHECK(ob != -1);
    if (ob == -1) {
        return;
    }
    CHECK_EQ_INT(recv_type(ob, ARB_NET_WELCOME, &f, WAIT_MS), 1);
    n = telemetry_for(ob, 500, &bad, &moving);
    printf("  observer, 500 ms: %d TELEMETRY frames matching the arm's ring, %d not\n", n, bad);
    CHECK(n >= 25);
    CHECK_EQ_INT(bad, 0);
    (void)close(ob);
    ms_sleep(50);
}

/* D1, FR-E5's spirit for the gateway: without the logger there is no telemetry,
 * and commands are decided exactly as before. */
static void fr_n4_without_the_logger_commands_are_still_decided(void)
{
    arb_net_welcome_t w;
    const int fd = planner_open(&w);
    int n, bad, moving;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(step(fd, NULL));
    n = telemetry_for(fd, 300, &bad, &moving);
    CHECK_EQ_INT(n + bad, 0);
    CHECK(step(fd, NULL));
    (void)close(fd);
    ms_sleep(50);
}

/* FR-W4 through the gateway: ESTOP puts the guardian in SAFE_STOP (cause e-stop),
 * the planner and observers are told by STATE, and later commands are denied by
 * the state rule. No frame can reset it. Runs last: it leaves SAFE_STOP. */
static void fr_w4_estop_through_the_gateway(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    arb_status_t s0, s1;
    const int ob = gw_connect(ARB_NET_OBSERVER_PORT);
    const int fd = planner_open(&w);
    int got = 0;

    CHECK(fd != -1 && ob != -1);
    if (fd == -1 || ob == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    CHECK(step(fd, NULL));
    frame_init(&f, ARB_NET_ESTOP);
    CHECK(send_bytes(fd, &f, sizeof f) == 0);
    for (int i = 0; i < 50 && !got; i++) {                     /* bound: 50 STATE frames */
        if (recv_type(fd, ARB_NET_STATE, &f, WAIT_MS) != 1) {
            break;
        }
        got = f.state.state == ARB_STATE_SAFE_STOP && f.state.stop_cause == ARB_STOP_ESTOP;
    }
    CHECK(got);
    got = 0;
    for (int i = 0; i < 200 && !got; i++) {                    /* bound: 200 frames; the observer also */
        if (recv_frame(ob, &f, WAIT_MS) != 1) {                /* gets the VERDICT copy of the step */
            break;
        }
        got = f.h.type == ARB_NET_STATE && f.state.state == ARB_STATE_SAFE_STOP;
    }
    CHECK(got);
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.state, ARB_STATE_SAFE_STOP);
    CHECK_EQ_INT(s1.stop_cause, ARB_STOP_ESTOP);
    CHECK_EQ_INT(s1.estops, s0.estops + 1u);
    CHECK(send_command(fd, g_seq + 1u, g_pos[0], g_pos[1], STEP_MS) == 0);
    CHECK_EQ_INT(recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.verdict.verdict, ARB_VERDICT_DENY);
    CHECK_EQ_INT(f.verdict.rule, ARB_RULE_STATE);
    (void)close(fd);
    (void)close(ob);
}

/* FR-N3: kill -9 the gateway in the middle of a move. Its submit handle closes
 * with it, so the guardian holds at once; the arm stops along its segment; the
 * watchdog, the arm and the guardian run on with no trip, no fault, no brake. */
static void fr_n3_killing_the_gateway_mid_move_holds_and_trusted_processes_run_on(void)
{
    arb_net_welcome_t w;
    arb_net_frame_t f;
    arb_status_t s0, s1;
    arb_telem_sample_t a;
    uint64_t wd_brakes0, wd_lost0, disc0;
    const int fd = planner_open(&w);
    long ms;

    CHECK(fd != -1);
    if (fd == -1) {
        return;
    }
    CHECK(status(&s0) == EOK);
    wd_brakes0 = atomic_load(&((arb_telem_t *)g_telem)->wd_brakes);
    wd_lost0 = atomic_load(&((arb_telem_t *)g_telem)->wd_lost);
    disc0 = atomic_load(&((arb_telem_t *)g_telem)->disconnects);
    g_seq = g_have_seq ? g_seq + 1u : 1u;
    CHECK(send_command(fd, g_seq, -400000, 300000, 2000) == 0);      /* a 2 s move */
    CHECK_EQ_INT(recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.verdict.verdict, ARB_VERDICT_ALLOW);
    ms_sleep(100);                    /* well inside the 250 ms silence timeout */
    a = arm_sample();
    CHECK_EQ_INT(a.state, 2);                                         /* moving */
    CHECK(kill(g_gw, SIGKILL) == 0);
    ms = state_within(ARB_STATE_HOLD, 1000);
    printf("  gateway killed mid-move -> guardian HOLD after %ld ms (QEMU, function only)\n", ms);
    CHECK(ms >= 0 && ms < 100);       /* before the silence timeout could (about 150 ms after the kill) */
    ms_sleep(1500);                                                   /* past the end of the move */
    a = arm_sample();
    CHECK(a.vel_urad_s[0] == 0 && a.vel_urad_s[1] == 0);              /* the arm stopped */
    CHECK(a.pos_urad[0] > -400000);                                   /* short of the target: it held */
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.state, ARB_STATE_HOLD);
    CHECK_EQ_INT(s1.watchdog_trips, s0.watchdog_trips);
    CHECK_EQ_INT(s1.watchdog_lost, s0.watchdog_lost);
    CHECK_EQ_INT(s1.arm_faults, s0.arm_faults);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_brakes), wd_brakes0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_lost), wd_lost0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->disconnects), disc0);   /* the guardian stayed bound */
    CHECK(alive(g_wd) && alive(g_arm) && alive(g_guard));
    CHECK(!alive(g_gw));
    printf("  arm stopped at %d %d urad; watchdog, arm, guardian alive; no trip, fault or brake\n", a.pos_urad[0],
           a.pos_urad[1]);
    (void)close(fd);
}

/* ------------------------------------------------- NFR-I4: untrusted processes killed */

/* A child process that plays an untrusted party and is killed by SIGKILL. It
 * never ends by itself before the parent kills it (bounded: 30 s, then exit).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pipe.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/w/waitpid.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/e/exit.html */
static void child_wait_to_be_killed(void)
{
    for (int i = 0; i < 300; i++) {                          /* bound: 300 x 100 ms */
        ms_sleep(100);
    }
    exit(0);
}

/* Read one int the child reports through the pipe, within timeout_ms: 0 on success. */
static int read_report(int rfd, int32_t *out, int timeout_ms)
{
    struct pollfd p = { rfd, POLLIN, 0 };

    if (poll(&p, 1, timeout_ms) != 1) {
        return -1;
    }
    return read(rfd, out, sizeof *out) == (ssize_t)sizeof *out ? 0 : -1;
}

static int kill_and_reap(pid_t c)
{
    int st = 0;

    if (kill(c, SIGKILL) != 0) {
        return -1;
    }
    return waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL ? 0 : -1;
}

/* An observer (TCP, read-only) and a local client of /dev/arbiter running as the
 * untrusted user, each in its own process, killed while the planner's move runs:
 * nothing changes. The guardian stays NORMAL, the arm keeps moving under it, and
 * no trip, fault, brake or disconnect is counted (NFR-I4). */
static void nfr_i4_killing_an_observer_or_a_local_client_mid_move_changes_nothing(void)
{
    int pfd[2];
    pid_t obs, loc;
    int32_t rep_obs = -1, rep_loc = -1;
    arb_net_welcome_t w;
    arb_net_frame_t f;
    arb_status_t s0, s1;
    arb_telem_sample_t a;
    uint64_t wd_brakes0, wd_lost0, disc0;
    int fd;

    CHECK(pipe(pfd) == 0);
    fflush(stdout);                   /* nothing buffered is printed twice by a child */
    obs = fork();
    if (obs == 0) {                   /* the observer */
        const int ob = gw_connect(ARB_NET_OBSERVER_PORT);
        int32_t ok = ob != -1 && recv_type(ob, ARB_NET_WELCOME, &f, WAIT_MS) == 1;
        (void)write(pfd[1], &ok, sizeof ok);
        child_wait_to_be_killed();
    }
    loc = fork();
    if (loc == 0) {                   /* the local client, as the untrusted user */
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/setuid.html */
        int32_t uid = setuid(ARB_UID_UNTRUSTED) == 0 ? (int32_t)getuid() : -1;
        const int d = open(ARB_DEV_PATH, O_RDONLY);
        arb_status_t st;
        if (d == -1 || devctl(d, ARB_DCMD_GET_STATUS, &st, sizeof st, NULL) != EOK) {
            uid = -2;
        }
        (void)write(pfd[1], &uid, sizeof uid);
        child_wait_to_be_killed();
    }
    CHECK(obs > 0 && loc > 0);
    CHECK(read_report(pfd[0], &rep_obs, WAIT_MS) == 0);
    CHECK(read_report(pfd[0], &rep_loc, WAIT_MS) == 0);
    /* The two reports may come in either order: one is the observer's 1, the other a uid. */
    {
        const int32_t ok = rep_obs == 1 ? rep_obs : rep_loc;
        const int32_t uid = rep_obs == 1 ? rep_loc : rep_obs;
        printf("  observer connected: %s; local client: %s (uid %d), status read through /dev/arbiter\n",
               ok == 1 ? "yes" : "no", uid >= 0 ? "open" : "failed", uid);
        CHECK_EQ_INT(ok, 1);
        CHECK_EQ_INT(uid, ARB_UID_UNTRUSTED);
    }
    fd = planner_open(&w);
    CHECK(fd != -1);
    if (fd == -1 || obs <= 0 || loc <= 0) {
        if (obs > 0) { (void)kill_and_reap(obs); }
        if (loc > 0) { (void)kill_and_reap(loc); }
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return;
    }
    CHECK(status(&s0) == EOK);
    wd_brakes0 = atomic_load(&((arb_telem_t *)g_telem)->wd_brakes);
    wd_lost0 = atomic_load(&((arb_telem_t *)g_telem)->wd_lost);
    disc0 = atomic_load(&((arb_telem_t *)g_telem)->disconnects);
    g_seq = g_have_seq ? g_seq + 1u : 1u;
    g_have_seq = 1;
    CHECK(send_command(fd, g_seq, -400000, 300000, 2000) == 0);      /* a 2 s move */
    CHECK_EQ_INT(recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS), 1);
    CHECK_EQ_INT(f.verdict.verdict, ARB_VERDICT_ALLOW);
    ms_sleep(100);
    CHECK_EQ_INT(arm_sample().state, 2);                              /* moving */
    CHECK(kill_and_reap(obs) == 0);
    CHECK(kill_and_reap(loc) == 0);
    ms_sleep(50);                     /* still inside the 250 ms silence timeout */
    a = arm_sample();
    CHECK(status(&s1) == EOK);
    printf("  observer and local client killed mid-move: guardian state %u, arm state %u (2 = moving)\n",
           (unsigned)s1.state, (unsigned)a.state);
    CHECK_EQ_INT(s1.state, ARB_STATE_NORMAL);
    CHECK_EQ_INT(a.state, 2);
    CHECK_EQ_INT(s1.watchdog_trips, s0.watchdog_trips);
    CHECK_EQ_INT(s1.watchdog_lost, s0.watchdog_lost);
    CHECK_EQ_INT(s1.arm_faults, s0.arm_faults);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_brakes), wd_brakes0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_lost), wd_lost0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->disconnects), disc0);
    CHECK(alive(g_wd) && alive(g_arm) && alive(g_guard) && alive(g_gw));
    (void)close(fd);                  /* the planner leaves: HOLD, the arm stops */
    CHECK(state_within(ARB_STATE_HOLD, 1000) >= 0);
    ms_sleep(1500);
    (void)close(pfd[0]);
    (void)close(pfd[1]);
}

/* The planner's own process killed mid-move (the gateway lives on): the kernel
 * closes its socket, the gateway ends the session and closes its submit handle,
 * the guardian holds, and the arm stops short of the target with no trip, fault
 * or brake. The gateway keeps running and accepts the next planner (NFR-I4). */
static void nfr_i4_killing_the_planner_process_mid_move_holds_the_arm(void)
{
    int pfd[2];
    pid_t pl;
    int32_t rep = -1;
    arb_status_t s0, s1;
    arb_telem_sample_t a;
    uint64_t wd_brakes0, wd_lost0, disc0;
    uint32_t seq;
    long ms;

    CHECK(status(&s0) == EOK);
    wd_brakes0 = atomic_load(&((arb_telem_t *)g_telem)->wd_brakes);
    wd_lost0 = atomic_load(&((arb_telem_t *)g_telem)->wd_lost);
    disc0 = atomic_load(&((arb_telem_t *)g_telem)->disconnects);
    seq = s0.has_seq ? s0.last_seq + 1u : 1u;
    CHECK(pipe(pfd) == 0);
    fflush(stdout);
    pl = fork();
    if (pl == 0) {                    /* the planner */
        arb_net_welcome_t w;
        arb_net_frame_t f;
        const int fd = planner_open(&w);
        int32_t v = -1;
        if (fd != -1 && send_command(fd, seq, 0, 0, 2000) == 0 &&     /* a 2 s move home */
            recv_type(fd, ARB_NET_VERDICT, &f, WAIT_MS) == 1) {
            v = (int32_t)f.verdict.verdict;
        }
        (void)write(pfd[1], &v, sizeof v);
        child_wait_to_be_killed();
    }
    CHECK(pl > 0);
    if (pl <= 0) {
        (void)close(pfd[0]);
        (void)close(pfd[1]);
        return;
    }
    CHECK(read_report(pfd[0], &rep, WAIT_MS) == 0);
    CHECK_EQ_INT(rep, ARB_VERDICT_ALLOW);
    g_seq = seq;
    g_have_seq = 1;
    ms_sleep(100);
    CHECK_EQ_INT(arm_sample().state, 2);                              /* moving */
    CHECK(kill_and_reap(pl) == 0);
    ms = state_within(ARB_STATE_HOLD, 1000);
    printf("  planner process killed mid-move -> guardian HOLD after %ld ms (QEMU, function only)\n", ms);
    CHECK(ms >= 0 && ms < 100);       /* the session ended, before the silence timeout could */
    ms_sleep(1500);                                                   /* past the end of the move */
    a = arm_sample();
    CHECK(a.vel_urad_s[0] == 0 && a.vel_urad_s[1] == 0);              /* the arm stopped */
    CHECK(a.pos_urad[0] != 0 || a.pos_urad[1] != 0);                  /* short of home: it held */
    CHECK(status(&s1) == EOK);
    CHECK_EQ_INT(s1.state, ARB_STATE_HOLD);
    CHECK_EQ_INT(s1.watchdog_trips, s0.watchdog_trips);
    CHECK_EQ_INT(s1.watchdog_lost, s0.watchdog_lost);
    CHECK_EQ_INT(s1.arm_faults, s0.arm_faults);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_brakes), wd_brakes0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->wd_lost), wd_lost0);
    CHECK_EQ_INT(atomic_load(&((arb_telem_t *)g_telem)->disconnects), disc0);
    CHECK(alive(g_wd) && alive(g_arm) && alive(g_guard) && alive(g_gw));
    printf("  arm stopped at %d %d urad; gateway, watchdog, arm, guardian alive; no trip, fault or brake\n",
           a.pos_urad[0], a.pos_urad[1]);
    {
        arb_net_welcome_t w;
        const int fd = planner_open(&w);                              /* the slot is free again */
        CHECK(fd != -1);
        if (fd != -1) {
            CHECK_EQ_INT(w.last_seq, seq);
            (void)close(fd);
            ms_sleep(50);
        }
    }
    (void)close(pfd[0]);
    (void)close(pfd[1]);
}

static const arb_test_case_t run_cases[] = {
    ARB_TEST_CASE(fr_n2_frames_match_the_documented_examples),
    ARB_TEST_CASE(fr_n1_gateway_is_not_reachable_on_other_addresses),
    ARB_TEST_CASE(fr_n1_planner_gets_welcome_then_state),
    ARB_TEST_CASE(fr_n1_second_planner_is_refused_busy),
    ARB_TEST_CASE(fr_n1_each_command_is_forwarded_and_answered_in_order),
    ARB_TEST_CASE(fr_n2_malformed_frames_end_the_session_unforwarded),
    ARB_TEST_CASE(fr_n2_a_partial_frame_is_never_forwarded),
    ARB_TEST_CASE(fr_g10_planner_disconnect_holds_at_once),
    ARB_TEST_CASE(fr_g10_planner_resumes_at_last_seq_plus_one),
    ARB_TEST_CASE(fr_n4_observer_gets_welcome_and_every_verdict),
    ARB_TEST_CASE(fr_n4_observer_is_read_only),
    ARB_TEST_CASE(fr_n4_one_observer_too_many_is_refused_busy),
    ARB_TEST_CASE(fr_n3_a_stopped_gateway_holds_and_trusted_processes_run_on),
    ARB_TEST_CASE(nfr_i1_gateway_user_cannot_open_the_arm_telemetry),
    ARB_TEST_CASE(fr_n4_planner_gets_arm_telemetry_through_the_logger),
    ARB_TEST_CASE(fr_n4_observer_gets_arm_telemetry),
    ARB_TEST_CASE(fr_w4_estop_through_the_gateway),                  /* leaves SAFE_STOP: last */
};
static const arb_test_case_t kill_cases[] = {
    ARB_TEST_CASE(fr_n4_without_the_logger_commands_are_still_decided),
    ARB_TEST_CASE(nfr_i4_killing_an_observer_or_a_local_client_mid_move_changes_nothing),
    ARB_TEST_CASE(nfr_i4_killing_the_planner_process_mid_move_holds_the_arm),
    ARB_TEST_CASE(fr_n3_killing_the_gateway_mid_move_holds_and_trusted_processes_run_on),
};

int main(int argc, char **argv)
{
    const char *mode = argc >= 2 ? argv[1] : "";
    struct sigaction sa;
    int fd;

    /* inet_pton() is listed with the io-sock Networking APIs (FreeBSD 14.4 inet(3)).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/socket_api.html */
    if (argc != 7 || (strcmp(mode, "run") != 0 && strcmp(mode, "kill") != 0) ||
        inet_pton(AF_INET, argv[2], &g_gw_addr) != 1) {
        fprintf(stderr, "usage: test_gateway run|kill <gateway-address> <gateway-pid> <wd-pid> <arm-pid> "
                        "<guard-pid>\n");
        return 2;
    }
    g_gw = (pid_t)strtol(argv[3], NULL, 10);
    g_wd = (pid_t)strtol(argv[4], NULL, 10);
    g_arm = (pid_t)strtol(argv[5], NULL, 10);
    g_guard = (pid_t)strtol(argv[6], NULL, 10);
    /* A closed peer must not kill the test with SIGPIPE.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigaction.html */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &sa, NULL);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
    g_status_fd = open(ARB_DEV_PATH, O_RDONLY);
    fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    if (g_status_fd == -1 || fd == -1) {
        printf("FAIL setup: /dev/arbiter or the arm telemetry not available (%s)\n", strerror(errno));
        return 1;
    }
    g_telem = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (g_telem == MAP_FAILED) {
        printf("FAIL setup: mmap telemetry (%s)\n", strerror(errno));
        return 1;
    }
    if (strcmp(mode, "run") == 0) {
        return arb_test_run("test_gateway run", run_cases, (int)(sizeof run_cases / sizeof run_cases[0]));
    }
    return arb_test_run("test_gateway kill", kill_cases, (int)(sizeof kill_cases / sizeof kill_cases[0]));
}
