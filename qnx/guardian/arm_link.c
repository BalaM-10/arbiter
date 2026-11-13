/*
 * Guardian -> arm controller link. Trusted code: no allocation, bounded calls,
 * every reply validated before use.
 */
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "arm_link.h"

int arm_link_open(arm_link_t *l)
{
    memset(l, 0, sizeof *l);
    /* name_open() returns a side-channel coid usable with MsgSend().
     * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/n/name_open.html */
    l->coid = name_open(ARB_ARM_NAME, 0);
    return l->coid == -1 ? -1 : 0;
}

/*
 * One bounded request/reply. TimerTimeout() applies to "the next kernel call made
 * by the same thread", so it is armed immediately before MsgSend(), with nothing
 * in between. SEND and REPLY together cover an arm that never receives and an arm
 * that never replies; the arm's channel has no _NTO_CHF_UNBLOCK, so the kernel
 * releases this thread at the timeout either way.
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html
 * QNX 8.0: https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html
 */
static arm_link_result_t bounded_send(arm_link_t *l, const void *req, size_t req_len,
                                      uint32_t seq, arb_arm_reply_t *reply)
{
    const uint64_t timeout_ns = ARM_LINK_TIMEOUT_NS;
    long rc;

    memset(reply, 0, sizeof *reply);
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &timeout_ns, NULL) == -1) {
        l->last_errno = errno;
        return ARM_LINK_ERROR;
    }
    rc = MsgSend(l->coid, req, req_len, reply, sizeof *reply);
    if (rc == -1) {
        l->last_errno = errno;
        return errno == ETIMEDOUT ? ARM_LINK_TIMEOUT : ARM_LINK_ERROR;
    }
    if (reply->version != ARB_ARM_PROTO_VERSION || reply->seq != seq) {
        l->last_errno = EBADMSG;
        return ARM_LINK_ERROR;
    }
    return ARM_LINK_OK;
}

arm_link_result_t arm_link_get_state(arm_link_t *l, int32_t pos_urad[ARB_JOINTS])
{
    arb_arm_get_state_t req;
    arb_arm_reply_t reply;
    arm_link_result_t r;

    memset(&req, 0, sizeof req);
    req.hdr.type = (uint16_t)ARB_ARM_MSG_GET_STATE;
    req.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    req.hdr.seq = ++l->seq;
    r = bounded_send(l, &req, sizeof req, req.hdr.seq, &reply);
    if (r != ARM_LINK_OK) {
        return r;
    }
    if (reply.status != ARB_ARM_OK) {
        l->last_errno = EBADMSG;
        return ARM_LINK_ERROR;
    }
    pos_urad[0] = reply.pos_urad[0];
    pos_urad[1] = reply.pos_urad[1];
    return ARM_LINK_OK;
}

arm_link_result_t arm_link_send_setpoint(arm_link_t *l, const int32_t start_urad[ARB_JOINTS],
                                         const int32_t target_urad[ARB_JOINTS], uint32_t duration_ms)
{
    arb_arm_setpoint_t req;
    arb_arm_reply_t reply;
    arm_link_result_t r;

    memset(&req, 0, sizeof req);
    req.hdr.type = (uint16_t)ARB_ARM_MSG_SETPOINT;
    req.hdr.version = (uint16_t)ARB_ARM_PROTO_VERSION;
    req.hdr.seq = ++l->seq;
    req.start_urad[0] = start_urad[0];
    req.start_urad[1] = start_urad[1];
    req.target_urad[0] = target_urad[0];
    req.target_urad[1] = target_urad[1];
    req.duration_ms = duration_ms;
    r = bounded_send(l, &req, sizeof req, req.hdr.seq, &reply);
    if (r != ARM_LINK_OK) {
        return r;
    }
    switch (reply.status) {
    case ARB_ARM_OK:
        return ARM_LINK_OK;
    case ARB_ARM_STALE_START:
        return ARM_LINK_STALE;
    default:
        l->last_errno = EBADMSG;
        return ARM_LINK_ERROR;
    }
}
