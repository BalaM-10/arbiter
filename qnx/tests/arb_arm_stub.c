/*
 * Test stand-in for the arm controller (not trusted code, not the real arm).
 * Lets the guardian be tested on QNX before the arm exists, and misbehaves on
 * request so the FR-G9 timeout path can be tested without test hooks in real code.
 *
 *   arb_arm_stub normal   reply to every request; the "arm" jumps to each target
 *   arb_arm_stub hang     reply to state queries, never reply to a setpoint
 *   arb_arm_stub stale    reply to every setpoint with STALE_START
 *
 * It also checks two things the 8.0 docs leave open (docs/DESIGN_M1_ADDENDUM.md,
 * decision 7): that name_attach() with our own dispatch handle uses our channel
 * (so our channel flags hold), and how the connect message from name_open() must
 * be answered.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "arb_arm_proto.h"

typedef enum { MODE_NORMAL, MODE_HANG, MODE_STALE } mode_t_;

/* Decision 7 of docs/DESIGN_M1_ADDENDUM.md: name_attach() given our own dispatch
 * handle must register our channel, so the flags we chose (no _NTO_CHF_UNBLOCK) hold. */
static int fr_g6_name_attach_uses_own_channel(int chid, const name_attach_t *attach)
{
    const int ok = attach->chid == chid;

    printf("%s fr_g6_name_attach_uses_own_channel (own chid %d, name_attach chid %d)\n",
           ok ? "PASS" : "FAIL", chid, attach->chid);
    return ok;
}

int main(int argc, char **argv)
{
    union {
        struct _pulse       pulse;
        uint16_t            type;
        arb_arm_hdr_t       hdr;
        arb_arm_setpoint_t  sp;
        char                raw[256];
    } msg;
    struct _msg_info info;
    name_attach_t *attach;
    dispatch_t *dpp;
    int32_t pos[ARB_JOINTS] = { 0, 0 };
    mode_t_ mode;
    int chid;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        printf("usage: arb_arm_stub normal|hang|stale\n");
        return 2;
    }
    if (strcmp(argv[1], "normal") == 0) {
        mode = MODE_NORMAL;
    } else if (strcmp(argv[1], "hang") == 0) {
        mode = MODE_HANG;
    } else if (strcmp(argv[1], "stale") == 0) {
        mode = MODE_STALE;
    } else {
        printf("unknown mode %s\n", argv[1]);
        return 2;
    }

    /* The arm's channel: no _NTO_CHF_UNBLOCK (addendum section 3).
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/channelcreate.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/d/dispatch_create_channel.html
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_attach.html */
    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
    if (chid == -1) {
        printf("FAIL stub ChannelCreate: %s\n", strerror(errno));
        return 1;
    }
    dpp = dispatch_create_channel(chid, 0);
    if (dpp == NULL) {
        printf("FAIL stub dispatch_create_channel: %s\n", strerror(errno));
        return 1;
    }
    attach = name_attach(dpp, ARB_ARM_NAME, 0);
    if (attach == NULL) {
        printf("FAIL stub name_attach: %s\n", strerror(errno));
        return 1;
    }
    (void)fr_g6_name_attach_uses_own_channel(chid, attach);

    for (;;) {
        /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgreceive.html */
        const rcvid_t rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        arb_arm_reply_t reply;

        if (rcvid == -1) {
            continue;
        }
        if (rcvid == 0) {
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                /* QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectdetach.html */
                (void)ConnectDetach(msg.pulse.scoid);
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            /* name_open() connects with a connect message; the name_attach page does
             * not say how to answer it. This stub answers EOK and the guardian's
             * name_open() result shows whether that is right. */
            (void)MsgReply(rcvid, EOK, NULL, 0);
            printf("info stub answered the connect message from name_open() with EOK\n");
            continue;
        }
        if (msg.type <= _IO_MAX) {
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }

        memset(&reply, 0, sizeof reply);
        reply.version = (uint16_t)ARB_ARM_PROTO_VERSION;
        reply.seq = msg.hdr.seq;
        if (msg.type == ARB_ARM_MSG_GET_STATE && info.srcmsglen == (ssize64_t)sizeof(arb_arm_get_state_t)) {
            reply.status = ARB_ARM_OK;
            reply.pos_urad[0] = pos[0];
            reply.pos_urad[1] = pos[1];
            (void)MsgReply(rcvid, EOK, &reply, sizeof reply);
        } else if (msg.type == ARB_ARM_MSG_SETPOINT && info.srcmsglen == (ssize64_t)sizeof(arb_arm_setpoint_t)) {
            if (mode == MODE_HANG) {
                continue;                  /* hold the message: the guardian must time out */
            }
            if (mode == MODE_STALE) {
                reply.status = ARB_ARM_STALE_START;
            } else {
                reply.status = ARB_ARM_OK;
                pos[0] = msg.sp.target_urad[0];
                pos[1] = msg.sp.target_urad[1];
            }
            reply.pos_urad[0] = pos[0];
            reply.pos_urad[1] = pos[1];
            (void)MsgReply(rcvid, EOK, &reply, sizeof reply);
        } else {
            (void)MsgError(rcvid, EBADMSG);
        }
    }
}
