/*
 * M0 message-passing sample: two QNX processes talk over a channel.
 *
 * The parent is the server: it creates a channel and receives. The child,
 * made with fork(), is the client: it connects to the parent's channel and
 * sends. One run checks, on the target:
 *
 *   M0-MSG-1  MsgSend/MsgReceive/MsgReply round trip with a fixed-size struct
 *   M0-MSG-2  the server identifies the sender's process ID from _msg_info
 *   M0-MSG-3  a too-short message is rejected with MsgError(EBADMSG)
 *   M0-MSG-4  a too-long message is rejected with MsgError(EBADMSG)
 *   M0-MSG-5  a wrong protocol version is rejected with MsgError(EPROTO)
 *   M0-MSG-6  pulses (MsgSendPulse) arrive with rcvid 0, code and value intact
 *   M0-MSG-7  orderly shutdown: client quits, server sees every event, child exits 0
 *
 * These preview NFR-Q2 (commands use synchronous messages, notifications use
 * pulses) and the length-first validation rule for trusted code. It is a
 * sample, not Arbiter code: it runs as root and uses a public channel.
 *
 * Exit status 0 means every check passed. Every run is bounded: the server
 * loop has a fixed iteration limit, and alarm() ends the run (as a failure)
 * after RUN_TIMEOUT_S. That bound is what catches a dead client here: if the
 * client exits just before the server blocks in MsgReceive(), the SIGCHLD
 * handler does not unblock it and only the alarm ends the run. The guardian
 * will detect client death through the kernel instead (M1).
 *
 * Output goes to stdout with printf(), which can block on a slow terminal.
 * Acceptable in a sample; Arbiter's trusted processes log through slog2.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/neutrino.h>

#include "arb_pulses.h"

#define DEMO_VERSION    1u
#define RUN_TIMEOUT_S   10u   /* whole run, parent and child */
#define MAX_EVENTS      64    /* bound on the server loop */
#define MAX_STATS_TRIES 5     /* bound on the client's wait for its pulses */

enum demo_op { OP_DOUBLE = 1, OP_STATS = 2, OP_QUIT = 3 };

/* Fixed-size wire structs, version field first. */
typedef struct {
    uint16_t version;
    uint16_t op;
    uint32_t seq;
    int64_t  value;
} demo_req_t;

typedef struct {
    uint16_t version;
    uint16_t op;
    uint32_t seq;
    int64_t  value;
    int32_t  sender_pid;   /* as seen by the server in _msg_info.pid */
    uint32_t pulses;       /* demo pulses received so far */
    int64_t  pulse_sum;    /* sum of their values */
} demo_reply_t;

/* Receive buffer: must hold a struct _pulse or the pulse is lost (MsgReceive page). */
typedef union {
    struct _pulse pulse;
    demo_req_t    req;
} demo_rx_t;

/* Server-side counters, checked by M0-MSG-7. */
typedef struct {
    unsigned replies;
    unsigned bad_length;
    unsigned bad_version;
    unsigned bad_sender;
    unsigned pulses;
    int64_t  pulse_sum;
    int      quit_seen;
} server_stats_t;

static volatile sig_atomic_t g_child_exited;

static void on_sigchld(int signo)
{
    (void)signo;
    g_child_exited = 1;
}

static int g_failures;

static void check(const char *id, int ok, const char *what)
{
    printf("%s %s %s\n", ok ? "PASS" : "FAIL", id, what);
    if (!ok) {
        g_failures++;
    }
}

/* ------------------------------------------------------------------ client */

/* Send one request; returns MsgSend's result, with errno set on -1. */
static long send_req(int coid, uint16_t version, uint16_t op, uint32_t seq,
                     int64_t value, demo_reply_t *rep)
{
    demo_req_t req = { .version = version, .op = op, .seq = seq, .value = value };

    memset(rep, 0, sizeof *rep);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
    return MsgSend(coid, &req, sizeof req, rep, sizeof *rep);
}

static int run_client(pid_t server_pid, int chid)
{
    demo_reply_t rep;
    long rc;
    int  coid;

    /* Side-channel connections are not inherited across fork(), so attach here.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html */
    coid = ConnectAttach(0, server_pid, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        printf("FAIL client ConnectAttach: %s\n", strerror(errno));
        return 1;
    }

    /* M0-MSG-1 and M0-MSG-2 */
    rc = send_req(coid, DEMO_VERSION, OP_DOUBLE, 1u, 21, &rep);
    check("M0-MSG-1", rc == EOK && rep.version == DEMO_VERSION && rep.seq == 1u && rep.value == 42,
          "round trip: sent 21, reply 42");
    check("M0-MSG-2", rc == EOK && rep.sender_pid == (int32_t)getpid(),
          "server saw the client's pid in _msg_info");

    /* M0-MSG-3: shorter than demo_req_t */
    {
        const uint8_t short_msg[2] = { 1u, 0u };
        rc = MsgSend(coid, short_msg, sizeof short_msg, &rep, sizeof rep);
        check("M0-MSG-3", rc == -1 && errno == EBADMSG, "short message rejected with EBADMSG");
    }

    /* M0-MSG-4: longer than demo_req_t */
    {
        uint8_t long_msg[sizeof(demo_req_t) + 8u];
        memset(long_msg, 0, sizeof long_msg);
        long_msg[0] = (uint8_t)DEMO_VERSION;
        rc = MsgSend(coid, long_msg, sizeof long_msg, &rep, sizeof rep);
        check("M0-MSG-4", rc == -1 && errno == EBADMSG, "oversized message rejected with EBADMSG");
    }

    /* M0-MSG-5 */
    rc = send_req(coid, (uint16_t)(DEMO_VERSION + 1u), OP_DOUBLE, 2u, 1, &rep);
    check("M0-MSG-5", rc == -1 && errno == EPROTO, "wrong version rejected with EPROTO");

    /* M0-MSG-6: three pulses, values 1 + 2 + 4 = 7.
     * Priority -1 means the calling thread's priority.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsendpulse.html */
    {
        static const long values[3] = { 1, 2, 4 };
        int sent_ok = 1;
        int tries;

        for (unsigned i = 0; i < 3u; i++) {
            if (MsgSendPulse(coid, -1, ARB_PULSE_DEMO, values[i]) == -1) {
                printf("info MsgSendPulse: %s\n", strerror(errno));
                sent_ok = 0;
            }
        }
        /* Pulses are asynchronous; poll the server's count a bounded number of times. */
        for (tries = 1; tries <= MAX_STATS_TRIES; tries++) {
            rc = send_req(coid, DEMO_VERSION, OP_STATS, 3u, 0, &rep);
            if (rc != EOK || rep.pulses >= 3u) {
                break;
            }
        }
        printf("info stats request answered after %d tr%s\n", tries, tries == 1 ? "y" : "ies");
        check("M0-MSG-6", sent_ok && rc == EOK && rep.pulses == 3u && rep.pulse_sum == 7,
              "3 pulses received by the server, values summed to 7");
    }

    /* Part of M0-MSG-7: orderly quit */
    rc = send_req(coid, DEMO_VERSION, OP_QUIT, 4u, 0, &rep);
    if (rc != EOK) {
        printf("FAIL client quit: %s\n", strerror(errno));
        g_failures++;
    }

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
    (void)ConnectDetach(coid);
    return g_failures == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ server */

static void serve(int chid, pid_t expected_client, server_stats_t *st)
{
    for (int n = 0; n < MAX_EVENTS && !st->quit_seen && !g_child_exited; n++) {
        demo_rx_t        rx;
        struct _msg_info info;
        demo_reply_t     rep;
        rcvid_t          rcvid;

        /* rcvid > 0: a message; rcvid == 0: a pulse (info is not updated for pulses).
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html
         * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/_msg_info.html */
        rcvid = MsgReceive(chid, &rx, sizeof rx, &info);
        if (rcvid == -1) {
            printf("info server MsgReceive: %s\n", strerror(errno));
            return; /* EINTR from SIGCHLD lands here if the client dies early */
        }

        if (rcvid == 0) {
            /* A pulse. Never reply to it. Kernel pulses have negative codes; ignore them. */
            if (rx.pulse.code == ARB_PULSE_DEMO) {
                st->pulses++;
                st->pulse_sum += rx.pulse.value.sival_long;
            }
            continue;
        }

        /* Validate length first, before reading any field. srcmsglen is what the
         * client sent, which can exceed what fitted in our buffer. */
        if (info.srcmsglen != (ssize64_t)sizeof(demo_req_t)) {
            st->bad_length++;
            /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgerror.html */
            (void)MsgError(rcvid, EBADMSG);
            continue;
        }
        if (rx.req.version != DEMO_VERSION) {
            st->bad_version++;
            (void)MsgError(rcvid, EPROTO);
            continue;
        }
        if (info.pid != expected_client) {
            st->bad_sender++;
            (void)MsgError(rcvid, EPERM);
            continue;
        }

        memset(&rep, 0, sizeof rep);
        rep.version    = DEMO_VERSION;
        rep.op         = rx.req.op;
        rep.seq        = rx.req.seq;
        rep.sender_pid = (int32_t)info.pid;

        switch (rx.req.op) {
        case OP_DOUBLE:
            if (rx.req.value > INT64_MAX / 2 || rx.req.value < INT64_MIN / 2) {
                (void)MsgError(rcvid, ERANGE);
                continue;
            }
            rep.value = rx.req.value * 2;
            break;
        case OP_STATS:
            break;
        case OP_QUIT:
            st->quit_seen = 1;
            break;
        default:
            (void)MsgError(rcvid, EINVAL);
            continue;
        }
        rep.pulses    = st->pulses;
        rep.pulse_sum = st->pulse_sum;

        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
        if (MsgReply(rcvid, EOK, &rep, sizeof rep) == -1) {
            printf("info server MsgReply: %s\n", strerror(errno));
        } else {
            st->replies++;
        }
    }
}

int main(void)
{
    server_stats_t   st;
    struct sigaction sa;
    pid_t            child;
    int              chid;
    int              status = 0;

    memset(&st, 0, sizeof st);
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Default action of SIGALRM terminates the process: a hang becomes a failure.
     * fork() does not pass timers to the child, so the child sets its own below.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/a/alarm.html */
    (void)alarm(RUN_TIMEOUT_S);

    /* SIGCHLD without SA_RESTART, so a client that dies early interrupts MsgReceive.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigaction.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigemptyset.html */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    (void)sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* Flags 0: a public channel (needs PROCMGR_AID_PUBLIC_CHANNEL; root has it),
     * default priority inheritance, no kernel pulses requested.
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html */
    chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }

    /* fork() needs PROCMGR_AID_FORK (root has it).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/f/fork.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/g/getpid.html */
    {
        pid_t server_pid = getpid();

        child = fork();
        if (child == -1) {
            perror("fork");
            return 1;
        }
        if (child == 0) {
            (void)alarm(RUN_TIMEOUT_S);
            _exit(run_client(server_pid, chid));
        }
        printf("info server pid %d chid %d, client pid %d\n", (int)server_pid, chid, (int)child);
    }

    serve(chid, child, &st);

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/w/waitpid.html */
    {
        pid_t w;
        do {
            w = waitpid(child, &status, 0);
        } while (w == -1 && errno == EINTR);
        if (w == -1) {
            perror("waitpid");
            return 1;
        }
    }

    printf("info server counters: replies %u, bad_length %u, bad_version %u, bad_sender %u, pulses %u, pulse_sum %lld\n",
           st.replies, st.bad_length, st.bad_version, st.bad_sender, st.pulses, (long long)st.pulse_sum);

    check("M0-MSG-7",
          WIFEXITED(status) && WEXITSTATUS(status) == 0 && st.quit_seen &&
          /* DOUBLE + QUIT, plus 1..MAX_STATS_TRIES STATS replies */
          st.replies >= 3u && st.replies <= 2u + MAX_STATS_TRIES &&
          st.bad_length == 2u && st.bad_version == 1u &&
          st.bad_sender == 0u && st.pulses == 3u && st.pulse_sum == 7,
          "client exited 0 and the server's counters match every event");

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channeldestroy.html */
    (void)ChannelDestroy(chid);

    if (g_failures == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("RESULT PASS msgpass\n");
        return 0;
    }
    printf("RESULT FAIL msgpass\n");
    return 1;
}
