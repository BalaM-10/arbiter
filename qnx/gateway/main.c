/*
 * arb-gateway: the untrusted door between a planner and the guardian
 * (FR-N1, FR-N2, FR-N3, FR-N4; protocol docs/PROTOCOL.md).
 *
 *   arb-gateway [-a <IPv4 address> | -i <interface>] [-p <planner-port>] [-o <observer-port>]
 *
 * It listens on one address, never on every address (THREAT_MODEL A2): by
 * default the target's own wired interface, the first interface that is up and
 * running, is not loopback and has an IPv4 address; -i names the interface, -a
 * the address. The wildcard 0.0.0.0 is refused. Ports default to 7700 and 7701.
 *
 * One planner connection at a time (port 7700) and up to two read-only
 * observers (port 7701). Each well-formed COMMAND becomes one ARB_DCMD_SUBMIT
 * devctl() on /dev/arbiter, made as the gateway's own user (FR-G10), and its
 * verdict goes back to the planner and to every observer. The gateway checks
 * frames, never commands: it does not know the contract (FR-P3).
 *
 * Untrusted, and built so that nothing it does can matter to safety:
 *   - priority 10, below every trusted process (arb_prio.h);
 *   - it listens as root, then drops to its own user with nothing kept but its
 *     priority (NFR-I5). After the drop it cannot open any new socket, which it
 *     checks at startup (evidence/runs/2026-11-17-qemu/gateway-net-privdrop-probe.log);
 *   - it maps no trusted memory (NFR-I1): the arm's joint state comes from the
 *     evidence logger by message passing, each send bounded (decision D1);
 *   - it never blocks on a client: non-blocking sockets and a bounded send queue
 *     per connection (PROTOCOL.md, back-pressure);
 *   - when the planner leaves, its submit handle closes and the guardian moves
 *     to HOLD at once (FR-G10); if the gateway hangs, the guardian's silence
 *     timeout does the same; if it dies, the kernel closes the handle for it.
 *
 * Sockets: the 8.0 C library reference lists the socket calls as "described
 * elsewhere", in the io-sock Networking APIs page and the FreeBSD 14.4 manual
 * pages it points to (socket, bind, listen, accept, recv, send, setsockopt).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/socket_api.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/poll.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fcntl.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/devctl.html
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/socket.h>

#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_log.h"
#include "arb_logd_proto.h"
#include "arb_net_proto.h"
#include "arb_prio.h"
#include "arb_privdrop.h"

#define FRAMES_PER_WAKE  8                   /* frames read from one client per poll wake-up */
#define POLL_FDS         (2 + 1 + ARB_NET_OBSERVERS_MAX)
#define LOGD_TIMEOUT_NS  20000000ull          /* one relay request: 20 ms at most */
#define LOGD_RETRY_MS    1000                 /* a missing logger is looked for once a second */

typedef struct {
    int             fd;                      /* -1: slot free */
    int             hello;                   /* planner: HELLO received */
    uint8_t         in[ARB_NET_FRAME_LEN];
    size_t          in_len;
    arb_net_frame_t q[ARB_NET_QUEUE_FRAMES]; /* frames waiting to be sent */
    unsigned        q_head, q_count;
    size_t          q_off;                   /* bytes of q[q_head] already sent */
    uint32_t        dropped;                 /* TELEMETRY frames dropped since the last one queued */
} conn_t;

static conn_t g_planner;
static conn_t g_obs[ARB_NET_OBSERVERS_MAX];
static int g_listen_planner = -1, g_listen_obs = -1;
static int g_status_fd = -1;                 /* /dev/arbiter, read only: status and nothing else */
static int g_submit_fd = -1;                 /* /dev/arbiter, read-write: open only during a planner session */
static uint32_t g_sessions;
static int g_logd = -1;                      /* connection to the logger's relay, or -1 */
static int64_t g_logd_retry_ms;              /* when to look for the logger again */
static uint64_t g_telem_last = UINT64_MAX;   /* index of the last sample sent */
static arb_status_t g_last;                  /* the status last sent in a STATE frame */
static int g_have_last;
static struct {                              /* counted, and logged after each wake-up */
    uint64_t sessions, malformed, too_slow, refused_busy, commands, unavailable;
} g_count, g_logged;

/* ------------------------------------------------------------------ helpers */

static int64_t now_ms(void)
{
    struct timespec ts;
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html */
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void frame_init(arb_net_frame_t *f, uint8_t type)
{
    static const uint8_t magic[4] = ARB_NET_MAGIC;

    memset(f, 0, sizeof *f);
    memcpy(f->h.magic, magic, sizeof magic);
    f->h.version = (uint8_t)ARB_NET_VERSION;
    f->h.type = type;
}

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {         /* bound: n <= 56 */
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void conn_reset(conn_t *c)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static int set_nonblocking(int fd)
{
    const int fl = fcntl(fd, F_GETFL);
    return fl == -1 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Send what the socket takes now, never waiting. 0, or -1 if the connection failed. */
static int conn_flush(conn_t *c)
{
    for (unsigned i = 0; c->q_count > 0 && i < ARB_NET_QUEUE_FRAMES; i++) {   /* bound: the queue */
        const uint8_t *p = c->q[c->q_head].b + c->q_off;
        const ssize_t n = send(c->fd, p, ARB_NET_FRAME_LEN - c->q_off, MSG_NOSIGNAL);

        if (n < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
        }
        c->q_off += (size_t)n;
        if (c->q_off < ARB_NET_FRAME_LEN) {
            return 0;                        /* the socket is full: the rest later */
        }
        c->q_off = 0;
        c->q_head = (c->q_head + 1u) % ARB_NET_QUEUE_FRAMES;
        c->q_count--;
    }
    return 0;
}

/* Queue one frame. A TELEMETRY frame that does not fit is dropped and counted;
 * any other frame that does not fit means the client does not read: -1. */
static int conn_queue(conn_t *c, const arb_net_frame_t *f)
{
    if (c->fd == -1) {
        return 0;
    }
    if (c->q_count == ARB_NET_QUEUE_FRAMES) {
        if (f->h.type == ARB_NET_TELEMETRY) {
            c->dropped++;
            return 0;
        }
        return -1;
    }
    c->q[(c->q_head + c->q_count) % ARB_NET_QUEUE_FRAMES] = *f;
    if (f->h.type == ARB_NET_TELEMETRY) {
        c->q[(c->q_head + c->q_count) % ARB_NET_QUEUE_FRAMES].telemetry.dropped = c->dropped;
        c->dropped = 0;
    }
    c->q_count++;
    return 0;
}

static void planner_end(void);

/* Close a connection. For the planner this also ends its session. */
static void conn_close(conn_t *c)
{
    if (c->fd == -1) {
        return;
    }
    (void)close(c->fd);
    if (c == &g_planner) {
        planner_end();
    }
    conn_reset(c);
}

/* ERROR <code>, one try to send it, then close (PROTOCOL.md: no resynchronising). */
static void conn_fail(conn_t *c, uint8_t code)
{
    arb_net_frame_t f;

    frame_init(&f, (uint8_t)ARB_NET_ERROR);
    f.error.code = code;
    if (code == ARB_NET_E_SLOW) {
        g_count.too_slow++;
    } else if (code != ARB_NET_E_BUSY && code != ARB_NET_E_UNAVAILABLE) {
        g_count.malformed++;
    }
    if (c->q_count == ARB_NET_QUEUE_FRAMES) {      /* no room even for the ERROR */
        c->q_count--;
    }
    (void)conn_queue(c, &f);
    (void)conn_flush(c);
    conn_close(c);
}

/* Queue a frame for a client; a client that cannot take it is too slow. */
static void send_to(conn_t *c, const arb_net_frame_t *f)
{
    if (c->fd != -1 && conn_queue(c, f) != 0) {
        conn_fail(c, (uint8_t)ARB_NET_E_SLOW);
    }
}

static void send_to_observers(const arb_net_frame_t *f)
{
    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {   /* bound: 2 */
        send_to(&g_obs[i], f);
    }
}

/* ----------------------------------------------------------- the guardian */

/* The guardian's status, through a read-only handle opened on demand. A guardian
 * that went away is noticed here and the handle opened again later. */
static int guardian_status(arb_status_t *s)
{
    int rc;

    if (g_status_fd == -1) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/o/open.html */
        g_status_fd = open(ARB_DEV_PATH, O_RDONLY);
        if (g_status_fd == -1) {
            return -1;
        }
    }
    memset(s, 0, sizeof *s);
    rc = devctl(g_status_fd, ARB_DCMD_GET_STATUS, s, sizeof *s, NULL);
    if (rc != EOK) {
        (void)close(g_status_fd);
        g_status_fd = -1;
        return -1;
    }
    return 0;
}

static void state_frame(const arb_status_t *s, arb_net_frame_t *f)
{
    frame_init(f, (uint8_t)ARB_NET_STATE);
    f->state.state = s->state;
    f->state.stop_cause = s->stop_cause;
    f->state.has_seq = s->has_seq;
    f->state.last_seq = s->last_seq;
    f->state.decisions = s->decisions;
    f->state.estops = s->estops;
    f->state.watchdog_lost = s->watchdog_lost;
    f->state.submits_refused = s->submits_refused;
    f->state.resets = s->resets;
}

static void welcome_frame(const arb_status_t *s, uint32_t session, arb_net_frame_t *f)
{
    frame_init(f, (uint8_t)ARB_NET_WELCOME);
    f->welcome.state = s->state;
    f->welcome.stop_cause = s->stop_cause;
    f->welcome.has_seq = s->has_seq;
    f->welcome.joints = (uint8_t)ARB_JOINTS;
    f->welcome.last_seq = s->last_seq;
    f->welcome.session = session;
    f->welcome.telemetry_period_ms = ARB_NET_TELEM_PERIOD_MS;
}

/* STATE to every client when the state, the stop cause or a stop/reset count
 * changed since the last STATE (or always, if force). */
static void state_changed(int force)
{
    arb_status_t s;
    arb_net_frame_t f;

    if (guardian_status(&s) != 0) {
        return;
    }
    if (!force && g_have_last && s.state == g_last.state && s.stop_cause == g_last.stop_cause &&
        s.estops == g_last.estops && s.resets == g_last.resets && s.watchdog_lost == g_last.watchdog_lost) {
        return;
    }
    g_last = s;
    g_have_last = 1;
    state_frame(&s, &f);
    send_to(&g_planner, &f);
    send_to_observers(&f);
}

/* The planner's session ended (it left, or was closed): give up the submit
 * handle, so the guardian moves to HOLD at once (FR-G10). */
static void planner_end(void)
{
    if (g_submit_fd != -1) {
        (void)close(g_submit_fd);
        g_submit_fd = -1;
    }
}

/* ------------------------------------------------------------ telemetry */

/* D1: the arm's newest sample, from the evidence logger, which maps the arm's
 * ring read-only (the gateway's user may not open it, NFR-I1). Each request is
 * bounded with TimerTimeout(), so a stopped logger delays the gateway by 20 ms
 * at most; a logger that is gone is looked for again once a second.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_close.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
static void telemetry_tick(void)
{
    const arb_logd_telem_req_t req = { (uint16_t)ARB_LOGD_MSG_TELEM, (uint16_t)ARB_LOGD_PROTO_VERSION, 0 };
    const uint64_t timeout = LOGD_TIMEOUT_NS;
    arb_logd_telem_rep_t rep;
    arb_net_frame_t f;
    long rc;

    if (g_logd == -1) {
        if (now_ms() < g_logd_retry_ms) {
            return;
        }
        g_logd = name_open(ARB_LOGD_NAME, 0);
        if (g_logd == -1) {
            g_logd_retry_ms = now_ms() + LOGD_RETRY_MS;
            return;
        }
        arb_log_c(ARB_LOG_CLIENT, SLOG2_INFO, "connected to the logger's telemetry relay");
    }
    memset(&rep, 0, sizeof rep);
    rc = TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL);
    if (rc != -1) {
        rc = MsgSend(g_logd, &req, sizeof req, &rep, sizeof rep);
    }
    if (rc == -1 || rep.version != ARB_LOGD_PROTO_VERSION) {
        arb_log_1(ARB_LOG_FAULT, SLOG2_NOTICE, "telemetry relay request failed (errno %lld): reconnecting later",
                  (int64_t)errno);
        (void)name_close(g_logd);
        g_logd = -1;
        g_logd_retry_ms = now_ms() + LOGD_RETRY_MS;
        return;
    }
    if (!rep.have || rep.s.index == g_telem_last) {
        return;
    }
    g_telem_last = rep.s.index;
    frame_init(&f, (uint8_t)ARB_NET_TELEMETRY);
    f.telemetry.index = rep.s.index;
    f.telemetry.t_ns = rep.s.t_ns;
    f.telemetry.pos_urad[0] = rep.s.pos_urad[0];
    f.telemetry.pos_urad[1] = rep.s.pos_urad[1];
    f.telemetry.vel_urad_s[0] = rep.s.vel_urad_s[0];
    f.telemetry.vel_urad_s[1] = rep.s.vel_urad_s[1];
    f.telemetry.segment = rep.s.segment;
    f.telemetry.arm_state = rep.s.state;
    f.telemetry.lease_ok = rep.s.lease_ok;
    send_to(&g_planner, &f);
    send_to_observers(&f);
}

/* ---------------------------------------------------------------- frames */

/* The header and the zero bytes of a planner frame; 0, or the ERROR code. */
static uint8_t planner_frame_check(const arb_net_frame_t *f)
{
    static const uint8_t magic[4] = ARB_NET_MAGIC;

    if (memcmp(f->h.magic, magic, sizeof magic) != 0) {
        return (uint8_t)ARB_NET_E_MAGIC;
    }
    if (f->h.version != ARB_NET_VERSION) {
        return (uint8_t)ARB_NET_E_VERSION;
    }
    switch (f->h.type) {
    case ARB_NET_HELLO:
    case ARB_NET_ESTOP:
        return f->h.reserved == 0 && all_zero(f->b + 8, ARB_NET_FRAME_LEN - 8) ? 0 : (uint8_t)ARB_NET_E_RESERVED;
    case ARB_NET_COMMAND:
        return f->h.reserved == 0 && all_zero(f->command.zero, sizeof f->command.zero) ? 0
                                                                                        : (uint8_t)ARB_NET_E_RESERVED;
    default:
        return (uint8_t)ARB_NET_E_TYPE;
    }
}

static void on_hello(void)
{
    arb_status_t s;
    arb_net_frame_t f;

    if (g_planner.hello) {
        conn_fail(&g_planner, (uint8_t)ARB_NET_E_ORDER);
        return;
    }
    /* The submit handle: open for writing, as the guardian requires for SUBMIT. */
    g_submit_fd = open(ARB_DEV_PATH, O_RDWR);
    if (g_submit_fd == -1 || guardian_status(&s) != 0) {
        arb_log_1(ARB_LOG_FAULT, SLOG2_WARNING, "planner HELLO: the guardian is unavailable (errno %lld)",
                  (int64_t)errno);
        g_count.unavailable++;
        conn_fail(&g_planner, (uint8_t)ARB_NET_E_UNAVAILABLE);
        return;
    }
    g_planner.hello = 1;
    g_sessions++;
    g_count.sessions++;
    welcome_frame(&s, g_sessions, &f);
    send_to(&g_planner, &f);
    state_frame(&s, &f);
    send_to(&g_planner, &f);
}

static void on_command(const arb_net_command_t *c)
{
    arb_submit_t sub;
    arb_net_frame_t f;
    int rc;

    memset(&sub, 0, sizeof sub);
    sub.cmd.version = (uint16_t)ARB_PROTO_VERSION;
    sub.cmd.seq = c->seq;
    sub.cmd.sender_ts_ns = c->sender_ts_ns;
    sub.cmd.target_urad[0] = c->target_urad[0];
    sub.cmd.target_urad[1] = c->target_urad[1];
    sub.cmd.duration_ms = c->duration_ms;
    g_count.commands++;
    rc = devctl(g_submit_fd, ARB_DCMD_SUBMIT, &sub, sizeof sub, NULL);

    frame_init(&f, (uint8_t)ARB_NET_VERDICT);
    f.verdict.seq = c->seq;
    if (rc == EOK) {
        f.verdict.result = (uint8_t)ARB_NET_DECIDED;
        f.verdict.verdict = sub.verdict.verdict;
        f.verdict.rule = sub.verdict.rule;
        f.verdict.state = sub.verdict.state;
        f.verdict.flags = sub.verdict.flags;
        f.verdict.fwd_urad[0] = sub.verdict.target_urad[0];
        f.verdict.fwd_urad[1] = sub.verdict.target_urad[1];
        f.verdict.fwd_duration_ms = sub.verdict.duration_ms;
        f.verdict.requested_ms = sub.verdict.requested_ms;
    } else {
        f.verdict.result = (uint8_t)(rc == EPERM || rc == EBUSY ? ARB_NET_REFUSED
                                     : rc == EBADMSG             ? ARB_NET_REJECTED
                                                                 : ARB_NET_UNAVAILABLE);
        f.verdict.requested_ms = c->duration_ms;
    }
    send_to(&g_planner, &f);
    send_to_observers(&f);
    if (rc != EOK && f.verdict.result == ARB_NET_UNAVAILABLE) {
        arb_log_1(ARB_LOG_FAULT, SLOG2_WARNING, "submit failed: the guardian is unavailable (devctl %lld)",
                  (int64_t)rc);
        g_count.unavailable++;
        conn_fail(&g_planner, (uint8_t)ARB_NET_E_UNAVAILABLE);
    }
}

static void on_estop(void)
{
    /* FR-W4: any client may stop; the guardian accepts it on any open. */
    const int rc = devctl(g_submit_fd, ARB_DCMD_ESTOP, NULL, 0, NULL);

    if (rc != EOK) {
        g_count.unavailable++;
        conn_fail(&g_planner, (uint8_t)ARB_NET_E_UNAVAILABLE);
        return;
    }
    arb_log_c(ARB_LOG_STATE, SLOG2_WARNING, "emergency stop from the planner, passed to the guardian");
    state_changed(1);
}

static void planner_frame(const arb_net_frame_t *f)
{
    const uint8_t bad = planner_frame_check(f);

    if (bad != 0) {
        conn_fail(&g_planner, bad);
        return;
    }
    if (f->h.type == ARB_NET_HELLO) {
        on_hello();
        return;
    }
    if (!g_planner.hello) {
        conn_fail(&g_planner, (uint8_t)ARB_NET_E_ORDER);
        return;
    }
    if (f->h.type == ARB_NET_COMMAND) {
        on_command(&f->command);
    } else {
        on_estop();
    }
}

/* Read what the planner sent; handle each complete frame, at most FRAMES_PER_WAKE. */
static void planner_readable(void)
{
    for (int i = 0; i < FRAMES_PER_WAKE && g_planner.fd != -1; i++) {   /* bound: FRAMES_PER_WAKE */
        const ssize_t n = recv(g_planner.fd, g_planner.in + g_planner.in_len,
                               ARB_NET_FRAME_LEN - g_planner.in_len, 0);
        if (n == 0) {
            conn_close(&g_planner);          /* the planner left: HOLD (FR-G10) */
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                conn_close(&g_planner);
            }
            return;
        }
        g_planner.in_len += (size_t)n;
        if (g_planner.in_len == ARB_NET_FRAME_LEN) {
            arb_net_frame_t f;
            memcpy(f.b, g_planner.in, sizeof f.b);
            g_planner.in_len = 0;
            planner_frame(&f);
        }
    }
}

/* Observers only receive: any byte from one ends it (FR-D6). */
static void observer_readable(conn_t *c)
{
    uint8_t b[ARB_NET_FRAME_LEN];
    const ssize_t n = recv(c->fd, b, sizeof b, 0);

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_close(c);
    } else if (n > 0) {
        conn_fail(c, (uint8_t)ARB_NET_E_READ_ONLY);
    }
}

/* ------------------------------------------------------------ connections */

static int accept_one(int lfd)
{
    const int one = 1;
    const int fd = accept(lfd, NULL, NULL);

    if (fd == -1) {
        return -1;
    }
    if (set_nonblocking(fd) == -1) {
        (void)close(fd);
        return -1;
    }
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* A client for whom there is no slot: ERROR busy, then close. */
static void refuse_busy(int fd)
{
    conn_t c;

    conn_reset(&c);
    c.fd = fd;
    g_count.refused_busy++;
    conn_fail(&c, (uint8_t)ARB_NET_E_BUSY);
}

static void planner_accept(void)
{
    const int fd = accept_one(g_listen_planner);

    if (fd == -1) {
        return;
    }
    if (g_planner.fd != -1) {
        refuse_busy(fd);
        return;
    }
    conn_reset(&g_planner);
    g_planner.fd = fd;
}

static void observer_accept(void)
{
    arb_status_t s;
    arb_net_frame_t f;
    const int fd = accept_one(g_listen_obs);

    if (fd == -1) {
        return;
    }
    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {          /* bound: 2 */
        if (g_obs[i].fd == -1) {
            conn_reset(&g_obs[i]);
            g_obs[i].fd = fd;
            if (guardian_status(&s) != 0) {
                memset(&s, 0, sizeof s);                       /* state 0: unknown */
            }
            welcome_frame(&s, 0, &f);
            send_to(&g_obs[i], &f);
            state_frame(&s, &f);
            send_to(&g_obs[i], &f);
            return;
        }
    }
    refuse_busy(fd);
}

/* A listening socket, made while still root (after the drop no socket can be made). */
/* The IPv4 address of an interface that is up and running and not loopback: the
 * one named, or the first such. 0, or -1 if there is none. getifaddrs() and
 * freeifaddrs() are listed with the io-sock Networking APIs (FreeBSD 14.4
 * getifaddrs(3)); called once, at startup, while still root.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/socket_api.html */
#define IFADDRS_MAX 64
static int wired_address(const char *want, struct in_addr *addr, char name[IFNAMSIZ])
{
    struct ifaddrs *all, *ifa;
    int n = 0, found = 0;

    if (getifaddrs(&all) == -1) {
        return -1;
    }
    for (ifa = all; ifa != NULL && n < IFADDRS_MAX && !found; ifa = ifa->ifa_next, n++) {   /* bound: 64 */
        const unsigned fl = ifa->ifa_flags;
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET || (fl & IFF_UP) == 0 ||
            (fl & IFF_RUNNING) == 0 || (fl & IFF_LOOPBACK) != 0 ||
            (want != NULL && strcmp(ifa->ifa_name, want) != 0)) {
            continue;
        }
        *addr = ((const struct sockaddr_in *)(const void *)ifa->ifa_addr)->sin_addr;
        (void)snprintf(name, IFNAMSIZ, "%s", ifa->ifa_name);
        found = 1;
    }
    freeifaddrs(all);
    if (!found) {
        errno = ENXIO;
        return -1;
    }
    return 0;
}

static int listen_on(struct in_addr addr, int port)
{
    struct sockaddr_in a;
    const int one = 1;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        return -1;
    }
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr = addr;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == -1 ||
        bind(fd, (struct sockaddr *)&a, sizeof a) == -1 || listen(fd, 2) == -1 || set_nonblocking(fd) == -1) {
        const int e = errno;
        (void)close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static void log_counts(void)
{
    if (g_count.sessions != g_logged.sessions) {
        arb_log_1(ARB_LOG_CLIENT, SLOG2_INFO, "planner session %lld started", (int64_t)g_count.sessions);
    }
    if (arb_log_worth(g_logged.malformed, g_count.malformed)) {
        arb_log_1(ARB_LOG_REFUSED, SLOG2_WARNING, "malformed frame: connection closed (%lld so far)",
                  (int64_t)g_count.malformed);
    }
    if (arb_log_worth(g_logged.too_slow, g_count.too_slow)) {
        arb_log_1(ARB_LOG_REFUSED, SLOG2_WARNING, "client too slow to read: connection closed (%lld so far)",
                  (int64_t)g_count.too_slow);
    }
    if (arb_log_worth(g_logged.refused_busy, g_count.refused_busy)) {
        arb_log_1(ARB_LOG_REFUSED, SLOG2_NOTICE, "connection refused: no free slot (%lld so far)",
                  (int64_t)g_count.refused_busy);
    }
    g_logged = g_count;
}

static int any_client(void)
{
    int n = g_planner.fd != -1;

    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {          /* bound: 2 */
        n += g_obs[i].fd != -1;
    }
    return n > 0;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct sched_param sp;
    struct sigaction sa;
    int planner_port = ARB_NET_PLANNER_PORT, obs_port = ARB_NET_OBSERVER_PORT;
    const char *opt_addr = NULL, *opt_if = NULL;
    struct in_addr addr;
    char ifname[IFNAMSIZ] = "-";
    char addr_text[INET_ADDRSTRLEN];
    int64_t next_tick;
    int rc;

    (void)arb_log_init("arb_gateway", "arb-gateway");
    for (int i = 1; i < argc; i += 2) {                          /* bound: argc */
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (v == NULL) {
            argc = -1;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_addr = v;
        } else if (strcmp(argv[i], "-i") == 0) {
            opt_if = v;
        } else if (strcmp(argv[i], "-p") == 0) {
            planner_port = atoi(v);
        } else if (strcmp(argv[i], "-o") == 0) {
            obs_port = atoi(v);
        } else {
            argc = -1;
        }
        if (argc == -1) {
            break;
        }
    }
    if (argc == -1 || (opt_addr != NULL && opt_if != NULL) || planner_port < 1 || planner_port > 65535 ||
        obs_port < 1 || obs_port > 65535 || planner_port == obs_port) {
        fprintf(stderr, "usage: arb-gateway [-a <IPv4 address> | -i <interface>] [-p <planner-port>] "
                        "[-o <observer-port>]\n");
        return 2;
    }
    /* One address, never all of them (THREAT_MODEL A2). inet_pton() and
     * inet_ntop() are listed with the io-sock Networking APIs (FreeBSD inet(3)).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/socket_api.html */
    if (opt_addr != NULL) {
        if (inet_pton(AF_INET, opt_addr, &addr) != 1) {
            fprintf(stderr, "arb-gateway: -a %s is not an IPv4 address\n", opt_addr);
            return 2;
        }
    } else if (wired_address(opt_if, &addr, ifname) != 0) {
        arb_log_fail(opt_if != NULL ? "no IPv4 address on that interface (or it is down, or loopback)"
                                    : "no wired interface with an IPv4 address", errno);
        return 5;
    }
    if (addr.s_addr == htonl(INADDR_ANY)) {
        fprintf(stderr, "arb-gateway: refused: the wildcard address 0.0.0.0 would listen on every interface\n");
        arb_log_c(ARB_LOG_FAIL, SLOG2_ERROR, "refused to listen on the wildcard address 0.0.0.0");
        return 2;
    }
    (void)inet_ntop(AF_INET, &addr, addr_text, sizeof addr_text);
    /* Below every trusted process.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/p/pthread_setschedparam.html */
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = ARB_PRIO_GATEWAY;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        arb_log_fail("set FIFO priority", rc);
        return 3;
    }
    /* A client that closes must not kill the gateway with SIGPIPE (sends also pass MSG_NOSIGNAL).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigaction.html */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    (void)sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        arb_log_fail("sigaction SIGPIPE", errno);
        return 3;
    }
    conn_reset(&g_planner);
    for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {          /* bound: 2 */
        conn_reset(&g_obs[i]);
    }
    g_listen_planner = listen_on(addr, planner_port);
    g_listen_obs = g_listen_planner == -1 ? -1 : listen_on(addr, obs_port);
    if (g_listen_planner == -1 || g_listen_obs == -1) {
        arb_log_fail("listen on the planner and observer ports", errno);
        return 5;
    }
    /* NFR-I5: drop to the gateway's user, keeping only its priority. */
    {
        char report[ARB_PRIVDROP_REPORT_LEN];
        const char *what;
        int err, probe;

        if (arb_privdrop(ARB_UID_GATEWAY, ARB_PRIO_GATEWAY, 0, 0, 0, &what, &err, report) != 0) {
            arb_log_fail(what, err);
            return 8;
        }
        printf("arb-gateway: %s\n", report);
        arb_log_s(ARB_LOG_START, SLOG2_INFO, "%s", report);
        /* Checked like the drop itself: a gateway that could still open sockets
         * (for example, connect out) does not run. */
        probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe != -1 || errno != EPERM) {
            if (probe != -1) {
                (void)close(probe);
            }
            arb_log_fail("check: socket() was not refused after the drop", probe != -1 ? 0 : errno);
            return 8;
        }
        printf("arb-gateway: refused as expected after the drop: socket() (no new socket of any kind)\n");
        arb_log_c(ARB_LOG_START, SLOG2_INFO, "refused as expected after the drop: socket() (no new socket of any kind)");
    }
    printf("arb-gateway: ready, listening on %s ports %d %d (interface %s), priority %d\n", addr_text, planner_port,
           obs_port, ifname, ARB_PRIO_GATEWAY);
    arb_log_s(ARB_LOG_START, SLOG2_INFO, "ready, listening on %s (ports 7700 and 7701 unless -p, -o)", addr_text);
    fflush(stdout);

    next_tick = now_ms() + ARB_NET_TELEM_PERIOD_MS;
    for (;;) {                                                   /* the gateway's lifetime */
        struct pollfd p[POLL_FDS];
        int n = 0, ip = -1, io[ARB_NET_OBSERVERS_MAX];
        int64_t wait = next_tick - now_ms();

        p[n++] = (struct pollfd){ g_listen_planner, POLLIN, 0 };
        p[n++] = (struct pollfd){ g_listen_obs, POLLIN, 0 };
        if (g_planner.fd != -1) {
            ip = n;
            p[n++] = (struct pollfd){ g_planner.fd, (short)(POLLIN | (g_planner.q_count ? POLLOUT : 0)), 0 };
        }
        for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {      /* bound: 2 */
            io[i] = -1;
            if (g_obs[i].fd != -1) {
                io[i] = n;
                p[n++] = (struct pollfd){ g_obs[i].fd, (short)(POLLIN | (g_obs[i].q_count ? POLLOUT : 0)), 0 };
            }
        }
        if (wait < 0) {
            wait = 0;
        }
        if (poll(p, (nfds_t)n, (int)wait) > 0) {
            if (ip != -1 && (p[ip].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                planner_readable();
            }
            for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {  /* bound: 2 */
                if (io[i] != -1 && g_obs[i].fd != -1 && (p[io[i]].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    observer_readable(&g_obs[i]);
                }
            }
            if ((p[0].revents & POLLIN) != 0) {
                planner_accept();
            }
            if ((p[1].revents & POLLIN) != 0) {
                observer_accept();
            }
        }
        if (now_ms() >= next_tick) {                           /* every 10 ms */
            next_tick += ARB_NET_TELEM_PERIOD_MS;
            if (next_tick < now_ms()) {
                next_tick = now_ms() + ARB_NET_TELEM_PERIOD_MS;   /* fell behind (stopped): no catch-up burst */
            }
            if (any_client()) {
                state_changed(0);
                telemetry_tick();
            }
        }
        if (g_planner.fd != -1 && conn_flush(&g_planner) != 0) {
            conn_close(&g_planner);
        }
        for (int i = 0; i < ARB_NET_OBSERVERS_MAX; i++) {      /* bound: 2 */
            if (g_obs[i].fd != -1 && conn_flush(&g_obs[i]) != 0) {
                conn_close(&g_obs[i]);
            }
        }
        log_counts();
    }
}
