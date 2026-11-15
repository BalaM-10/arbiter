/*
 * arb-untrusted: the F11 attacker. An untrusted process, run by f11_suite.sh as
 * user ARB_UID_UNTRUSTED (and, on the security-policy image, as type
 * arb_untrusted_t), that tries to reach the arm without going through the
 * guardian. It decides nothing: it records what each step returned and the arm's
 * own counters before and after, and f11_suite.sh judges.
 *
 *   arb-untrusted setpoint          connect to the arm and send it a setpoint
 *   arb-untrusted bind <hold_ms>    connect, send the binding message (a state
 *                                   query, the guardian's first message), send a
 *                                   setpoint, then keep the connection open for
 *                                   hold_ms while the real guardian starts
 *   arb-untrusted attach            try to attach /dev/arbiter before the guardian
 *   arb-untrusted squat <arm|watchdog> <hold_ms>
 *                                   register the arm's or the watchdog's name before
 *                                   the real one starts, and serve it for hold_ms:
 *                                   count every connect, message and pulse that
 *                                   arrives (a trusted client must send it nothing)
 *
 * Output:
 *   ARB-UNTRUSTED <mode> pid <n> uid <n> euid <n>
 *   COUNTERS <before|after> rejected <n> bound_pid <n>    (the arm's telemetry; or "unavailable")
 *   STEP <name> rc <n> errno <n> <text>
 *   RECEIVED connects <n> messages <n> pulses <n>      (squat)
 * Exit: 0 ran every step it could (the steps' results are in the output),
 *       2 usage, 3 started as root (the result would say nothing about F11).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/mman.h>
#include <sys/neutrino.h>

#include "arb_arm_proto.h"
#include "arb_devctl.h"
#include "arb_ids.h"
#include "arb_telemetry.h"
#include "arb_watchdog_proto.h"

#define SEND_TIMEOUT_NS 100000000ull       /* 100 ms: never hang on the arm */

static const arb_telem_t *g_tm;

static void step(const char *name, long rc, int err)
{
    printf("STEP %s rc %ld errno %d %s\n", name, rc, rc == -1 ? err : 0, rc == -1 ? strerror(err) : "ok");
}

static void counters(const char *phase)
{
    if (g_tm == NULL) {
        printf("COUNTERS %s unavailable\n", phase);
        return;
    }
    printf("COUNTERS %s rejected %llu bound_pid %lld\n", phase, (unsigned long long)atomic_load(&g_tm->rejected),
           (long long)atomic_load(&g_tm->bound_pid));
}

/* Read-only view of the arm's telemetry (mode 0644: any user may read it).
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/shm_open.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/mmap.html */
static void map_telemetry(void)
{
    const int fd = shm_open(ARB_TELEM_NAME, O_RDONLY, 0);
    void *p;

    if (fd == -1) {
        return;
    }
    p = mmap(NULL, sizeof(arb_telem_t), PROT_READ, MAP_SHARED, fd, 0);
    (void)close(fd);
    if (p != MAP_FAILED && ((const arb_telem_t *)p)->magic == ARB_TELEM_MAGIC) {
        g_tm = (const arb_telem_t *)p;
    }
}

/* One send to the arm, bounded so that a stuck arm cannot hang the test.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html */
static void send_to_arm(const char *name, int coid, const void *msg, size_t len)
{
    const uint64_t timeout = SEND_TIMEOUT_NS;
    arb_arm_reply_t reply;
    long rc;

    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout, NULL) == -1) {
        step("TimerTimeout", -1, errno);
        return;
    }
    rc = MsgSend(coid, msg, len, &reply, sizeof reply);
    step(name, rc, errno);
}

static int connect_arm(void)
{
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    const int coid = name_open(ARB_ARM_NAME, 0);
    step("name_open", coid, errno);
    return coid;
}

static void send_setpoint(int coid, uint32_t seq)
{
    arb_arm_setpoint_t sp;

    memset(&sp, 0, sizeof sp);
    sp.hdr.type = ARB_ARM_MSG_SETPOINT;
    sp.hdr.version = ARB_ARM_PROTO_VERSION;
    sp.hdr.seq = seq;
    sp.target_urad[0] = 100000;            /* 0.1 rad: small, inside every limit */
    sp.duration_ms = 1000;
    send_to_arm("setpoint", coid, &sp, sizeof sp);
}

static void attach_dev(void)
{
    static resmgr_connect_funcs_t cf;
    static resmgr_io_funcs_t iof;
    static iofunc_attr_t attr;
    dispatch_t *dpp;
    int id;

    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create.html */
    dpp = dispatch_create();
    step("dispatch_create", dpp == NULL ? -1 : 0, errno);
    if (dpp == NULL) {
        return;
    }
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_func_init.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/iofunc_attr_init.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html */
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
    id = resmgr_attach(dpp, NULL, ARB_DEV_PATH, _FTYPE_ANY, 0, &cf, &iof, &attr);
    step("resmgr_attach", id, errno);
    if (id != -1) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_detach.html */
        (void)resmgr_detach(dpp, id, 0);   /* never keep the guardian's path */
    }
}

/* Serve a stolen name for hold_ms. Every message gets EOK and no data; a
 * trusted client checks who serves the name first and so must send none.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
static void squat(const char *name, long hold_ms)
{
    union {
        struct _pulse pulse;
        uint16_t      type;
        char          raw[512];
    } msg;
    struct timespec now, end;
    name_attach_t *attach;
    long connects = 0, messages = 0, pulses = 0;

    attach = name_attach(NULL, name, 0);
    step("name_attach", attach == NULL ? -1 : 0, errno);
    if (attach == NULL) {
        return;
    }
    printf("SQUATTING %s for %ld ms\n", name, hold_ms);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/clock_gettime.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreply.html */
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += hold_ms / 1000;
    end.tv_nsec += (hold_ms % 1000) * 1000000L;
    if (end.tv_nsec >= 1000000000L) {
        end.tv_sec++;
        end.tv_nsec -= 1000000000L;
    }
    for (;;) {                                /* bound: until hold_ms has passed */
        uint64_t left_ns;
        rcvid_t rcvid;

        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec || (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec)) {
            break;
        }
        left_ns = (uint64_t)(end.tv_sec - now.tv_sec) * 1000000000ull + (uint64_t)end.tv_nsec - (uint64_t)now.tv_nsec;
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &left_ns, NULL);
        rcvid = MsgReceive(attach->chid, &msg, sizeof msg, NULL);
        if (rcvid == -1) {
            continue;                         /* the timeout, or an error: the clock decides */
        }
        if (rcvid == 0) {
            pulses++;
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            connects++;
        } else {
            messages++;
        }
        (void)MsgReply(rcvid, EOK, NULL, 0);
    }
    printf("RECEIVED connects %ld messages %ld pulses %ld\n", connects, messages, pulses);
    /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_detach.html */
    (void)name_detach(attach, 0);
}

int main(int argc, char **argv)
{
    const char *mode = argc >= 2 ? argv[1] : "";
    int coid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("ARB-UNTRUSTED %s pid %d uid %d euid %d\n", mode, (int)getpid(), (int)getuid(), (int)geteuid());
    if (geteuid() == 0) {
        printf("refusing to run as root: start me as user %d\n", ARB_UID_UNTRUSTED);
        return 3;
    }
    map_telemetry();
    if (strcmp(mode, "setpoint") == 0 && argc == 2) {
        counters("before");
        coid = connect_arm();
        if (coid != -1) {
            send_setpoint(coid, 1);
        }
        counters("after");
        return 0;
    }
    if (strcmp(mode, "bind") == 0 && argc == 3) {
        const long hold_ms = strtol(argv[2], NULL, 10);
        arb_arm_get_state_t q;
        struct timespec ts;

        counters("before");
        coid = connect_arm();
        if (coid != -1) {
            memset(&q, 0, sizeof q);
            q.hdr.type = ARB_ARM_MSG_GET_STATE;
            q.hdr.version = ARB_ARM_PROTO_VERSION;
            q.hdr.seq = 1;
            send_to_arm("get_state", coid, &q, sizeof q);
            send_setpoint(coid, 2);
        }
        counters("after");
        printf("HOLDING %ld ms\n", hold_ms);
        ts.tv_sec = hold_ms / 1000;
        ts.tv_nsec = (hold_ms % 1000) * 1000000L;
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/nanosleep.html */
        (void)nanosleep(&ts, NULL);         /* the connection stays open meanwhile */
        counters("held");
        return 0;
    }
    if (strcmp(mode, "attach") == 0 && argc == 2) {
        attach_dev();
        return 0;
    }
    if (strcmp(mode, "squat") == 0 && argc == 4 &&
        (strcmp(argv[2], "arm") == 0 || strcmp(argv[2], "watchdog") == 0)) {
        squat(strcmp(argv[2], "arm") == 0 ? ARB_ARM_NAME : ARB_WD_NAME, strtol(argv[3], NULL, 10));
        return 0;
    }
    fprintf(stderr, "usage: arb-untrusted setpoint | bind <hold_ms> | attach | squat <arm|watchdog> <hold_ms>\n");
    return 2;
}
