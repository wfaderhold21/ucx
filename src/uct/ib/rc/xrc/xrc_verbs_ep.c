/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "xrc_verbs.h"
#include "xrc_verbs_impl.h"

#include <ucs/vfs/base/vfs_cb.h>
#include <ucs/vfs/base/vfs_obj.h>
#include <ucs/arch/bitops.h>
#include <uct/ib/base/ib_log.h>


void uct_xrc_verbs_txcnt_init(uct_xrc_verbs_txcnt_t *txcnt)
{
    txcnt->pi = txcnt->ci = 0;
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_ep_fence_put(uct_xrc_verbs_iface_t *iface, uct_xrc_verbs_ep_t *ep,
                           uct_rkey_t *rkey, uint64_t *addr)
{
    uct_rc_ep_fence_put(&iface->super, &ep->nqp->fi, rkey, addr,
                        ep->super.atomic_mr_offset);
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_ep_post_send(uct_xrc_verbs_iface_t *iface, uct_xrc_verbs_ep_t *ep,
                           struct ibv_send_wr *wr, int send_flags,
                           int max_log_sge)
{
    struct ibv_send_wr *bad_wr;
    int ret;

    ucs_assertv(ep->ini_qp->state == IBV_QPS_RTS, "QP 0x%x state is %d",
                ep->nqp->qpn, ep->ini_qp->state);

    if (!(send_flags & IBV_SEND_SIGNALED)) {
        send_flags |= uct_rc_iface_tx_moderation(&iface->super, &ep->super.txqp,
                                                 IBV_SEND_SIGNALED);
    }

    if (wr->opcode == IBV_WR_SEND) {
        uct_rc_ep_fm(&iface->super, &ep->nqp->fi, 0);
    } else if (wr->opcode == IBV_WR_RDMA_READ) {
        send_flags |= uct_rc_ep_fm(&iface->super, &ep->nqp->fi,
                                  IBV_SEND_FENCE);
    }

    wr->send_flags              = send_flags;
    /* The completion is attributed to the endpoint by the position which its
     * operation has in the send queue of the QP, shared between endpoints. */
    wr->wr_id                   = ep->nqp->txcnt.pi + 1;
    /* XRC steering: every work request names the destination SRQ. */
    wr->qp_type.xrc.remote_srqn = ep->remote_srqn;

    uct_ib_log_post_send(&iface->super.super, ep->ini_qp, wr, max_log_sge,
                         (wr->opcode == IBV_WR_SEND) ? uct_rc_ep_packet_dump :
                                                       NULL);

    ret = ibv_post_send(ep->ini_qp, wr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_send() returned %d (%m)", ret);
    }

    uct_xrc_verbs_node_qp_posted(ep->nqp, ep, &iface->super,
                                 send_flags & IBV_SEND_SIGNALED);
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_ep_post_send_desc(uct_xrc_verbs_ep_t *ep, struct ibv_send_wr *wr,
                                uct_rc_iface_send_desc_t *desc, int send_flags,
                                int max_log_sge)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    UCT_XRC_VERBS_FILL_DESC_WR(wr, desc);
    uct_xrc_verbs_ep_post_send(iface, ep, wr, send_flags, max_log_sge);
    uct_rc_txqp_add_send_op_sn(&ep->super.txqp, &desc->super, ep->txcnt.pi);
}

static inline ucs_status_t
uct_xrc_verbs_ep_rdma_zcopy(uct_xrc_verbs_ep_t *ep, const uct_iov_t *iov,
                            size_t iovcnt, size_t iov_total_length,
                            uint64_t remote_addr, uint32_t rkey,
                            uct_completion_t *comp,
                            uct_rc_send_handler_t handler, uint16_t op_flags,
                            int opcode)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    struct ibv_sge sge[UCT_IB_MAX_IOV];
    struct ibv_send_wr wr;
    size_t sge_cnt;

    ucs_assertv(iovcnt <= ucs_min(UCT_IB_MAX_IOV, iface->config.max_send_sge),
                "iovcnt %zu, maxcnt (%zu, %zu)",
                iovcnt, UCT_IB_MAX_IOV, iface->config.max_send_sge);

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    sge_cnt = uct_ib_verbs_sge_fill_iov(sge, iov, iovcnt);
    /* cppcheck-suppress syntaxError */
    UCT_SKIP_ZERO_LENGTH(sge_cnt);
    UCT_XRC_VERBS_FILL_RDMA_WR_IOV(wr, wr.opcode, (enum ibv_wr_opcode)opcode,
                                   sge, sge_cnt, remote_addr, rkey);
    wr.next = NULL;

    uct_xrc_verbs_ep_post_send(iface, ep, &wr, IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_txqp_add_send_comp(&iface->super, &ep->super.txqp, handler, comp,
                              ep->txcnt.pi,
                              op_flags | UCT_RC_IFACE_SEND_OP_FLAG_ZCOPY,
                              iov, iovcnt, iov_total_length);
    return UCS_INPROGRESS;
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_ep_atomic_post(uct_xrc_verbs_ep_t *ep, int opcode,
                             uint64_t compare_add, uint64_t swap,
                             uint64_t remote_addr, uct_rkey_t rkey,
                             uct_rc_iface_send_desc_t *desc, int force_sig)
{
    uint32_t ib_rkey = uct_ib_resolve_atomic_rkey(rkey,
                                                 ep->super.atomic_mr_offset,
                                                 &remote_addr);
    struct ibv_send_wr wr;
    struct ibv_sge sge;

    UCT_XRC_VERBS_FILL_ATOMIC_WR(wr, wr.opcode, sge,
                                 (enum ibv_wr_opcode)opcode,
                                 compare_add, swap, remote_addr, ib_rkey);
    UCT_TL_EP_STAT_ATOMIC(&ep->super.super);
    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc, force_sig, INT_MAX);
    uct_rc_ep_enable_flush_remote(&ep->super);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_xrc_verbs_ep_atomic(uct_xrc_verbs_ep_t *ep, int opcode, void *result,
                        uint64_t compare_add, uint64_t swap,
                        uint64_t remote_addr, uct_rkey_t rkey,
                        uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    uct_rc_iface_send_desc_t *desc;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_ATOMIC_FETCH_DESC(&iface->super,
                                          &iface->short_desc_mp, desc,
                                          iface->super.config.atomic64_handler,
                                          result, comp);
    uct_xrc_verbs_ep_atomic_post(ep, opcode, compare_add, swap, remote_addr,
                                 rkey, desc,
                                 IBV_SEND_SIGNALED |
                                 uct_rc_ep_fm(&iface->super,
                                              &ep->nqp->fi,
                                              IBV_SEND_FENCE));
    return UCS_INPROGRESS;
}

ucs_status_t uct_xrc_verbs_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                        unsigned length, uint64_t remote_addr,
                                        uct_rkey_t rkey)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);

    UCT_CHECK_LENGTH(length, 0, iface->config.max_inline, "put_short");

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    uct_xrc_verbs_ep_fence_put(iface, ep, &rkey, &remote_addr);
    UCT_XRC_VERBS_FILL_INL_PUT_WR(iface, remote_addr, rkey, buffer, length);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, SHORT, length);
    uct_xrc_verbs_ep_post_send(iface, ep, &iface->inl_rwrite_wr,
                               IBV_SEND_INLINE | IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_ep_enable_flush_remote(&ep->super);
    return UCS_OK;
}

ssize_t uct_xrc_verbs_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                   void *arg, uint64_t remote_addr,
                                   uct_rkey_t rkey)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    size_t length;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_PUT_BCOPY_DESC(&iface->super, &iface->super.tx.mp, desc,
                                       pack_cb, arg, length);
    uct_xrc_verbs_ep_fence_put(iface, ep, &rkey, &remote_addr);
    UCT_XRC_VERBS_FILL_RDMA_WR(wr, wr.opcode, IBV_WR_RDMA_WRITE, sge,
                               length, remote_addr, rkey);
    UCT_TL_EP_STAT_OP(&ep->super.super, PUT, BCOPY, length);
    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SIGNALED, INT_MAX);
    uct_rc_ep_enable_flush_remote(&ep->super);
    return length;
}

ucs_status_t uct_xrc_verbs_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                        size_t iovcnt, uint64_t remote_addr,
                                        uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.max_send_sge,
                       "uct_xrc_verbs_ep_put_zcopy");
    uct_xrc_verbs_ep_fence_put(iface, ep, &rkey, &remote_addr);
    status = uct_xrc_verbs_ep_rdma_zcopy(ep, iov, iovcnt, 0ul, remote_addr,
                                         rkey, comp,
                                         uct_rc_ep_send_op_completion_handler,
                                         0, IBV_WR_RDMA_WRITE);
    UCT_TL_EP_STAT_OP_IF_SUCCESS(status, &ep->super.super, PUT, ZCOPY,
                                 uct_iov_total_length(iov, iovcnt));
    uct_rc_ep_enable_flush_remote(&ep->super);
    return status;
}

ucs_status_t uct_xrc_verbs_ep_get_bcopy(uct_ep_h tl_ep,
                                        uct_unpack_callback_t unpack_cb,
                                        void *arg, size_t length,
                                        uint64_t remote_addr, uct_rkey_t rkey,
                                        uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;

    UCT_CHECK_LENGTH(length, 0, iface->super.super.config.seg_size,
                     "get_bcopy");
    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_GET_BCOPY_DESC(&iface->super, &iface->super.tx.mp, desc,
                                       unpack_cb, comp, arg, length);

    UCT_XRC_VERBS_FILL_RDMA_WR(wr, wr.opcode, IBV_WR_RDMA_READ, sge, length,
                               remote_addr, uct_ib_md_direct_rkey(rkey));

    UCT_TL_EP_STAT_OP(&ep->super.super, GET, BCOPY, length);
    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SIGNALED, INT_MAX);
    UCT_RC_RDMA_READ_POSTED(&iface->super, length);
    return UCS_INPROGRESS;
}

ucs_status_t uct_xrc_verbs_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                        size_t iovcnt, uint64_t remote_addr,
                                        uct_rkey_t rkey, uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    size_t total_length          = uct_iov_total_length(iov, iovcnt);
    ucs_status_t status;

    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.max_send_sge,
                       "uct_xrc_verbs_ep_get_zcopy");
    UCT_CHECK_LENGTH(total_length,
                     iface->super.super.config.max_inl_cqe[UCT_IB_DIR_TX] + 1,
                     iface->super.config.max_get_zcopy, "get_zcopy");

    status = uct_xrc_verbs_ep_rdma_zcopy(ep, iov, iovcnt, total_length,
                                         remote_addr,
                                         uct_ib_md_direct_rkey(rkey), comp,
                                         uct_rc_ep_get_zcopy_completion_handler,
                                         UCT_RC_IFACE_SEND_OP_FLAG_IOV,
                                         IBV_WR_RDMA_READ);
    if (!UCS_STATUS_IS_ERR(status)) {
        UCT_RC_RDMA_READ_POSTED(&iface->super, total_length);
        UCT_TL_EP_STAT_OP(&ep->super.super, GET, ZCOPY, total_length);
    }

    return status;
}

ucs_status_t uct_xrc_verbs_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                       const void *buffer, unsigned length)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);

    UCT_RC_CHECK_AM_SHORT(id, length, uct_rc_am_short_hdr_t,
                          iface->config.max_inline);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    uct_xrc_verbs_iface_fill_inl_am_sge(iface, id, hdr, buffer, length);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT, sizeof(hdr) + length);
    uct_xrc_verbs_ep_post_send(iface, ep, &iface->inl_am_wr,
                               IBV_SEND_INLINE | IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_OK;
}

ucs_status_t uct_xrc_verbs_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                           const uct_iov_t *iov, size_t iovcnt)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);

    UCT_RC_CHECK_AM_SHORT(id, uct_iov_total_length(iov, iovcnt), uct_rc_hdr_t,
                          iface->config.max_inline);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    UCT_CHECK_IOV_SIZE(iovcnt, UCT_IB_MAX_IOV - 1,
                       "uct_xrc_verbs_ep_am_short_iov");
    uct_xrc_verbs_iface_fill_inl_am_sge_iov(iface, id, iov, iovcnt);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, SHORT,
                      uct_iov_total_length(iov, iovcnt));
    uct_xrc_verbs_ep_post_send(iface, ep, &iface->inl_am_wr,
                               IBV_SEND_INLINE | IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_OK;
}

ssize_t uct_xrc_verbs_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                  uct_pack_callback_t pack_cb, void *arg,
                                  unsigned flags)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    size_t length;

    UCT_CHECK_AM_ID(id);

    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);
    UCT_RC_IFACE_GET_TX_AM_BCOPY_DESC(&iface->super, &iface->super.tx.mp, desc,
                                      id, uct_rc_am_hdr_fill, uct_rc_hdr_t,
                                      pack_cb, arg, &length);
    UCT_XRC_VERBS_FILL_AM_BCOPY_WR(wr, sge, length + sizeof(uct_rc_hdr_t),
                                   wr.opcode);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, BCOPY, length);
    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SOLICITED, INT_MAX);
    UCT_RC_UPDATE_FC(&ep->super, id);

    return length;
}

ucs_status_t uct_xrc_verbs_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id,
                                       const void *header,
                                       unsigned header_length,
                                       const uct_iov_t *iov, size_t iovcnt,
                                       unsigned flags, uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_iface_send_desc_t *desc = NULL;
    struct ibv_sge sge[UCT_IB_MAX_IOV]; /* First sge is reserved for the header */
    struct ibv_send_wr wr;
    int send_flags;
    size_t sge_cnt;

    /* 1 iov consumed by am header */
    UCT_CHECK_IOV_SIZE(iovcnt, iface->config.max_send_sge - 1,
                       "uct_xrc_verbs_ep_am_zcopy");
    UCT_RC_CHECK_AM_ZCOPY(id, header_length, uct_iov_total_length(iov, iovcnt),
                          iface->config.short_desc_size,
                          iface->super.super.config.seg_size);
    UCT_RC_CHECK_RES_AND_FC(&iface->super, &ep->super, id);

    UCT_RC_IFACE_GET_TX_AM_ZCOPY_DESC(&iface->super, &iface->short_desc_mp,
                                      desc, id, header, header_length, comp,
                                      &send_flags);
    sge[0].length = sizeof(uct_rc_hdr_t) + header_length;
    sge_cnt = uct_ib_verbs_sge_fill_iov(sge + 1, iov, iovcnt);
    UCT_XRC_VERBS_FILL_AM_ZCOPY_WR_IOV(wr, sge, (sge_cnt + 1), wr.opcode);
    UCT_TL_EP_STAT_OP(&ep->super.super, AM, ZCOPY,
                      (header_length + uct_iov_total_length(iov, iovcnt)));

    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc,
                                    send_flags | IBV_SEND_SOLICITED,
                                    UCT_IB_MAX_ZCOPY_LOG_SGE(&iface->super.super));
    UCT_RC_UPDATE_FC(&ep->super, id);

    return UCS_INPROGRESS;
}

ucs_status_t uct_xrc_verbs_ep_atomic64_post(uct_ep_h tl_ep, unsigned opcode,
                                            uint64_t value, uint64_t remote_addr,
                                            uct_rkey_t rkey)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_iface_send_desc_t *desc;

    if (opcode != UCT_ATOMIC_OP_ADD) {
        return UCS_ERR_UNSUPPORTED;
    }

    UCT_RC_CHECK_RES(&iface->super, &ep->super);
    UCT_RC_IFACE_GET_TX_ATOMIC_DESC(&iface->super, &iface->short_desc_mp, desc);

    uct_xrc_verbs_ep_atomic_post(ep, IBV_WR_ATOMIC_FETCH_AND_ADD, value, 0,
                                 remote_addr, rkey, desc, IBV_SEND_SIGNALED);
    return UCS_OK;
}

ucs_status_t uct_xrc_verbs_ep_atomic64_fetch(uct_ep_h tl_ep,
                                             uct_atomic_op_t opcode,
                                             uint64_t value, uint64_t *result,
                                             uint64_t remote_addr,
                                             uct_rkey_t rkey,
                                             uct_completion_t *comp)
{
    if (opcode != UCT_ATOMIC_OP_ADD) {
        return UCS_ERR_UNSUPPORTED;
    }

    return uct_xrc_verbs_ep_atomic(ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t),
                                   IBV_WR_ATOMIC_FETCH_AND_ADD, result, value, 0,
                                   remote_addr, rkey, comp);
}

ucs_status_t uct_xrc_verbs_ep_atomic_cswap64(uct_ep_h tl_ep, uint64_t compare,
                                             uint64_t swap, uint64_t remote_addr,
                                             uct_rkey_t rkey, uint64_t *result,
                                             uct_completion_t *comp)
{
    return uct_xrc_verbs_ep_atomic(ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t),
                                   IBV_WR_ATOMIC_CMP_AND_SWP, result, compare,
                                   swap, remote_addr, rkey, comp);
}

static void uct_xrc_verbs_ep_post_flush(uct_xrc_verbs_ep_t *ep, int send_flags)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    struct ibv_send_wr wr;
    struct ibv_sge sge;
    int inl_flag;

    if (iface->config.max_inline == 0) {
        /* Flush by flow control pure grant, in case the device does not
         * support 0-size RDMA_WRITE or does not support inline. */
        sge.addr   = (uintptr_t)(iface->fc_desc + 1);
        sge.length = sizeof(uct_rc_hdr_t);
        sge.lkey   = iface->fc_desc->lkey;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode  = IBV_WR_SEND;
        inl_flag   = 0;
    } else {
        /* Flush by empty RDMA_WRITE */
        wr.sg_list             = NULL;
        wr.num_sge             = 0;
        wr.opcode              = IBV_WR_RDMA_WRITE;
        wr.wr.rdma.remote_addr = 0;
        wr.wr.rdma.rkey        = 0;
        inl_flag               = IBV_SEND_INLINE;
    }
    wr.next = NULL;

    uct_xrc_verbs_ep_post_send(iface, ep, &wr, inl_flag | send_flags, 1);
}

static ucs_status_t
uct_xrc_verbs_ep_flush_remote(uct_xrc_verbs_ep_t *ep, uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(ep->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    uct_rc_iface_send_desc_t *desc;
    struct ibv_send_wr wr;
    struct ibv_sge sge;

    UCT_RC_CHECK_RES(&iface->super, &ep->super);

    UCT_RC_IFACE_GET_TX_DESC(iface, &iface->super.tx.mp, desc);
    desc->super.handler   = uct_rc_ep_flush_remote_handler;
    desc->super.user_comp = comp;

    UCT_XRC_VERBS_FILL_RDMA_WR(wr, wr.opcode, IBV_WR_RDMA_READ, sge,
                               UCT_IB_MD_FLUSH_REMOTE_LENGTH, 0,
                               ep->super.flush_rkey);

    uct_xrc_verbs_ep_post_send_desc(ep, &wr, desc, IBV_SEND_SIGNALED, INT_MAX);
    ep->super.flags &= ~UCT_RC_EP_FLAG_FLUSH_REMOTE;

    return UCS_INPROGRESS;
}

ucs_status_t uct_xrc_verbs_ep_flush(uct_ep_h tl_ep, unsigned flags,
                                    uct_completion_t *comp)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    int already_canceled         = ep->super.flags & UCT_RC_EP_FLAG_FLUSH_CANCEL;
    ucs_status_t status;

    UCT_CHECK_PARAM(!ucs_test_all_flags(flags, UCT_FLUSH_FLAG_CANCEL |
                                               UCT_FLUSH_FLAG_REMOTE),
                    "flush flags CANCEL and REMOTE are mutually exclusive");

    if (flags & UCT_FLUSH_FLAG_REMOTE) {
        UCT_RC_IFACE_CHECK_FLUSH_REMOTE(
                uct_ib_md_is_flush_rkey_valid(ep->super.flush_rkey), ep,
                &iface->super, xrc);
        if (ep->super.flags & UCT_RC_EP_FLAG_FLUSH_REMOTE) {
            return uct_xrc_verbs_ep_flush_remote(ep, comp);
        }
    }

    status = uct_rc_ep_flush(&ep->super, iface->config.tx_max_wr, flags);
    if (status != UCS_INPROGRESS) {
        return status;
    }

    if (uct_rc_txqp_unsignaled(&ep->super.txqp) != 0) {
        UCT_RC_CHECK_RES(&iface->super, &ep->super);
        uct_xrc_verbs_ep_post_flush(ep, IBV_SEND_SIGNALED);
    }

    if (ucs_unlikely((flags & UCT_FLUSH_FLAG_CANCEL) && !already_canceled) &&
        !ep->nqp->shared) {
        /* Cancelling means moving the QP to error state, which would discard
         * the operations of the other endpoints of a shared initiator QP too,
         * so only a private one may be cancelled this way. */
        status = uct_ib_modify_qp(ep->ini_qp, IBV_QPS_ERR);
        if (status != UCS_OK) {
            return status;
        }
    }

    return uct_rc_txqp_add_flush_comp(&iface->super, &ep->super.super,
                                      &ep->super.txqp, comp, ep->txcnt.pi);
}

ucs_status_t uct_xrc_verbs_ep_fence(uct_ep_h tl_ep, unsigned flags)
{
    uct_xrc_verbs_ep_t *ep = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);

    return uct_rc_ep_fence(tl_ep, &ep->nqp->fi);
}

ucs_status_t uct_xrc_verbs_ep_post_check(uct_ep_h tl_ep, uct_completion_t *comp)
{
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);

    if (comp == NULL) {
        uct_xrc_verbs_ep_post_flush(ep, 0);
        return UCS_OK;
    }

    uct_xrc_verbs_ep_post_flush(ep, IBV_SEND_SIGNALED);
    return uct_rc_txqp_add_flush_comp(&iface->super, &ep->super.super,
                                      &ep->super.txqp, comp, ep->txcnt.pi);
}

void uct_xrc_verbs_ep_vfs_populate(uct_rc_ep_t *rc_ep)
{
    uct_rc_iface_t *rc_iface = ucs_derived_of(rc_ep->super.super.iface,
                                              uct_rc_iface_t);
    uct_xrc_verbs_ep_t *ep   = ucs_derived_of(rc_ep, uct_xrc_verbs_ep_t);

    ucs_vfs_obj_add_dir(rc_iface, ep, "ep/%p", ep);
    ucs_vfs_obj_add_ro_file(ep, ucs_vfs_show_primitive,
                            &ep->nqp->qpn, UCS_VFS_TYPE_U32_HEX,
                            "ini_qp_num");
    ucs_vfs_obj_add_ro_file(ep, ucs_vfs_show_primitive,
                            &ep->tgt_qp->qp_num, UCS_VFS_TYPE_U32_HEX,
                            "tgt_qp_num");
    uct_rc_txqp_vfs_populate(&ep->super.txqp, ep);
}

ucs_status_t uct_xrc_verbs_ep_fc_ctrl(uct_ep_t *tl_ep, unsigned op,
                                      uct_rc_pending_req_t *req)
{
    struct ibv_send_wr fc_wr;
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_rc_hdr_t *hdr;
    struct ibv_sge sge;
    int flags;

    if (!iface->fc_desc) {
        hdr                      = &iface->am_inl_hdr.rc_hdr;
        hdr->am_id               = UCT_RC_EP_FC_PURE_GRANT;
        fc_wr.sg_list            = iface->inl_sge;
        iface->inl_sge[0].addr   = (uintptr_t)hdr;
        iface->inl_sge[0].length = sizeof(*hdr);
        flags                    = IBV_SEND_INLINE;
    } else {
        hdr           = (uct_rc_hdr_t*)(iface->fc_desc + 1);
        sge.addr      = (uintptr_t)hdr;
        sge.length    = sizeof(uct_rc_hdr_t);
        sge.lkey      = iface->fc_desc->lkey;
        fc_wr.sg_list = &sge;
        flags         = 0;
    }

    /* In XRC, as in RC, only a PURE grant is sent as a separate message. */
    ucs_assert(op == UCT_RC_EP_FC_PURE_GRANT);

    UCT_RC_CHECK_TX_CQ_RES(&iface->super, &ep->super);

    fc_wr.opcode  = IBV_WR_SEND;
    fc_wr.next    = NULL;
    fc_wr.num_sge = 1;

    uct_xrc_verbs_ep_post_send(iface, ep, &fc_wr, flags, INT_MAX);
    return UCS_OK;
}

ucs_status_t uct_xrc_verbs_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_ep->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_ep_t *ep       = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    uct_ib_md_t *md              = uct_ib_iface_md(&iface->super.super);
    uct_xrc_verbs_ep_flush_addr_t *xrc_addr =
            (uct_xrc_verbs_ep_flush_addr_t*)addr;

    xrc_addr->super.flags = 0;
    uct_ib_pack_uint24(xrc_addr->super.tgt_qp_num, ep->tgt_qp->qp_num);
    uct_ib_pack_uint24(xrc_addr->super.ini_qp_num, ep->ini_qp->qp_num);
    xrc_addr->super.srqn  = htonl(iface->srq_num);

    if (uct_ib_md_is_flush_rkey_valid(md->flush_rkey)) {
        xrc_addr->super.flags |= UCT_XRC_VERBS_ADDR_HAS_ATOMIC_MR;
        xrc_addr->atomic_mr_id  = uct_ib_md_get_atomic_mr_id(md);
        xrc_addr->flush_rkey_hi = md->flush_rkey >> 16;
        if (!md->config.enable_indirect_atomic) {
            xrc_addr->super.flags |= UCT_XRC_VERBS_ADDR_NO_ATOMIC_OFFSET;
        }
    }

    return UCS_OK;
}

int uct_xrc_verbs_ep_is_connected(const uct_ep_h tl_ep,
                                  const uct_ep_is_connected_params_t *params)
{
    uct_xrc_verbs_ep_t *ep = ucs_derived_of(tl_ep, uct_xrc_verbs_ep_t);
    const uct_xrc_verbs_ep_addr_t *xrc_addr;
    uint32_t addr_qp = 0;
    ucs_status_t status;
    struct ibv_ah_attr ah_attr;
    uint32_t qp_num;

    status = uct_ib_query_qp_peer_info(ep->ini_qp, &ah_attr, &qp_num);
    if (status != UCS_OK) {
        return 0;
    }

    if (params->field_mask & UCT_EP_IS_CONNECTED_FIELD_EP_ADDR) {
        xrc_addr = (const uct_xrc_verbs_ep_addr_t*)params->ep_addr;
        addr_qp  = uct_ib_unpack_uint24(xrc_addr->tgt_qp_num);
    }

    return uct_rc_ep_is_connected(&ep->super, &ah_attr, params, qp_num,
                                  addr_qp);
}

/* Bring the XRC target QP of an endpoint up to the configured state. The
 * target QP only ever receives, so states beyond RTR are needed only to serve
 * RDMA READ and atomic requests. */
static ucs_status_t
uct_xrc_verbs_ep_tgt_connect(uct_xrc_verbs_iface_t *iface,
                             uct_xrc_verbs_ep_t *ep, uint32_t dest_qp_num,
                             struct ibv_ah_attr *ah_attr,
                             enum ibv_mtu path_mtu)
{
    uct_rc_iface_t *rc_iface = &iface->super;
    ucs_status_t status;
    struct ibv_qp_attr qp_attr;
    int ret;

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state           = IBV_QPS_RTR;
    qp_attr.path_mtu           = path_mtu;
    qp_attr.dest_qp_num        = dest_qp_num;
    qp_attr.rq_psn             = 0;
    qp_attr.max_dest_rd_atomic = rc_iface->config.max_rd_atomic;
    qp_attr.min_rnr_timer      = rc_iface->config.min_rnr_timer;
    qp_attr.ah_attr            = *ah_attr;

    ret = ibv_modify_qp(ep->tgt_qp, &qp_attr,
                        IBV_QP_STATE              | IBV_QP_AV              |
                        IBV_QP_PATH_MTU           | IBV_QP_DEST_QPN        |
                        IBV_QP_RQ_PSN             | IBV_QP_MAX_DEST_RD_ATOMIC |
                        IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        ucs_error("xrc target QP 0x%x: error modifying to RTR: %m",
                  ep->tgt_qp->qp_num);
        return UCS_ERR_IO_ERROR;
    }

    if ((iface->config.tgt_state == UCT_XRC_VERBS_TGT_STATE_RTR) ||
        !iface->config.tgt_fallback) {
        return UCS_OK;
    }

    /* XRC target QP states beyond RTR are only needed to serve RDMA READ and
     * atomic requests. They are not accepted by every device family, so a
     * failure here is not fatal: the endpoint keeps working at RTR for RDMA
     * WRITE and active messages, and the smoke test shows whether more is
     * needed. */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state           = IBV_QPS_SQD;
    qp_attr.max_dest_rd_atomic = rc_iface->config.max_rd_atomic;
    qp_attr.min_rnr_timer      = rc_iface->config.min_rnr_timer;

    ret = ibv_modify_qp(ep->tgt_qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_MAX_DEST_RD_ATOMIC |
                        IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        ucs_debug("xrc target QP 0x%x: keeping RTR state (%m)",
                  ep->tgt_qp->qp_num);
        return UCS_OK;
    }

    if (iface->config.tgt_state == UCT_XRC_VERBS_TGT_STATE_SQD) {
        return UCS_OK;
    }

    /* The verbs API has no XRC target send-ready state; RTS is the request
     * which moves an XRC target QP to the state that services reads. */
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state      = IBV_QPS_RTS;
    qp_attr.sq_psn        = 0;
    qp_attr.timeout       = rc_iface->config.timeout;
    qp_attr.rnr_retry     = rc_iface->config.rnr_retry;
    qp_attr.retry_cnt     = rc_iface->config.retry_cnt;
    qp_attr.max_rd_atomic = rc_iface->config.max_rd_atomic;

    ret = ibv_modify_qp(ep->tgt_qp, &qp_attr,
                        IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        ucs_debug("xrc target QP 0x%x: staying at SQD (%m)",
                  ep->tgt_qp->qp_num);
    }

    return UCS_OK;
}

ucs_status_t
uct_xrc_verbs_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                                  const uct_device_addr_t *dev_addr,
                                  const uct_ep_addr_t *ep_addr,
                                  const uct_ep_connect_to_ep_params_t *param)
{
    uct_xrc_verbs_ep_t *ep           = ucs_derived_of(tl_ep,
                                                      uct_xrc_verbs_ep_t);
    uct_xrc_verbs_iface_t *iface     = ucs_derived_of(tl_ep->iface,
                                                      uct_xrc_verbs_iface_t);
    const uct_ib_address_t *ib_addr  = (const uct_ib_address_t *)dev_addr;
    const uct_xrc_verbs_ep_flush_addr_t *xrc_addr =
            (const uct_xrc_verbs_ep_flush_addr_t*)ep_addr;
    ucs_status_t status;
    struct ibv_ah_attr ah_attr;
    enum ibv_mtu path_mtu;
    uint32_t tgt_qp_num, ini_qp_num;

    status = uct_ib_iface_fill_ah_attr_from_addr(&iface->super.super, ib_addr,
                                                 ep->super.path_index,
                                                 &ah_attr, &path_mtu);
    if (status != UCS_OK) {
        return status;
    }

    ucs_assert(path_mtu != UCT_IB_ADDRESS_INVALID_PATH_MTU);

    tgt_qp_num = uct_ib_unpack_uint24(xrc_addr->super.tgt_qp_num);
    ini_qp_num = uct_ib_unpack_uint24(xrc_addr->super.ini_qp_num);

    if (!(ep->conn_flags & UCT_XRC_VERBS_EP_FLAG_INI_CONNECTED)) {
        /* Outbound operations: connect the initiator to the remote target. The
         * initiator is shared between the endpoints to one node, and which of
         * their targets it is connected to does not matter, because a request
         * names the destination shared receive queue itself: the first
         * endpoint to connect brings the QP up for all of them. */
        if (ep->nqp->connected) {
            ep->remote_srqn = ntohl(xrc_addr->super.srqn);
            ep->conn_flags |= UCT_XRC_VERBS_EP_FLAG_INI_CONNECTED;
            return uct_xrc_verbs_ep_tgt_connect(iface, ep, ini_qp_num,
                                               &ah_attr, path_mtu);
        }

        status = uct_rc_iface_qp_connect(&iface->super, ep->ini_qp, tgt_qp_num,
                                        &ah_attr, path_mtu);
        if (status != UCS_OK) {
            return status;
        }

        ep->remote_srqn = ntohl(xrc_addr->super.srqn);
        ep->conn_flags |= UCT_XRC_VERBS_EP_FLAG_INI_CONNECTED;
        ep->nqp->connected = 1;
    }

    if (!(ep->conn_flags & UCT_XRC_VERBS_EP_FLAG_TGT_CONNECTED)) {
        /* Inbound operations: connect the local target to the remote
         * initiator, so that it can serve its reads and atomics. */
        status = uct_xrc_verbs_ep_tgt_connect(iface, ep, ini_qp_num, &ah_attr,
                                             path_mtu);
        if (status != UCS_OK) {
            return status;
        }

        ep->conn_flags |= UCT_XRC_VERBS_EP_FLAG_TGT_CONNECTED;
    }

    if (xrc_addr->super.flags & UCT_XRC_VERBS_ADDR_HAS_ATOMIC_MR) {
        if (xrc_addr->super.flags & UCT_XRC_VERBS_ADDR_NO_ATOMIC_OFFSET) {
            ep->super.atomic_mr_offset = 0;
        } else {
            ep->super.atomic_mr_offset = uct_ib_md_atomic_offset(
                    xrc_addr->atomic_mr_id);
        }

        ep->super.flush_rkey = ((uint32_t)xrc_addr->flush_rkey_hi << 16) +
                               ((uint32_t)xrc_addr->atomic_mr_id << 8);
    } else {
        ep->super.atomic_mr_offset = 0;
        ep->super.flush_rkey       = UCT_IB_MD_INVALID_FLUSH_RKEY;
    }

    ep->super.flags |= UCT_RC_EP_FLAG_CONNECTED;
    return UCS_OK;
}

static ucs_status_t
uct_xrc_verbs_qp_to_init(struct ibv_qp *qp, uct_xrc_verbs_iface_t *iface,
                         int access_flags)
{
    struct ibv_qp_attr qp_attr;
    int ret;

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state        = IBV_QPS_INIT;
    qp_attr.pkey_index      = iface->super.super.pkey_index;
    qp_attr.port_num        = iface->super.super.config.port_num;
    qp_attr.qp_access_flags = access_flags;

    ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        ucs_error("error modifying XRC QP 0x%x to INIT: %m", qp->qp_num);
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

UCS_CLASS_INIT_FUNC(uct_xrc_verbs_ep_t, const uct_ep_params_t *params)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(params->iface,
                                                  uct_xrc_verbs_iface_t);
    uct_xrc_verbs_node_key_t key, *key_p = NULL;
    const uct_ib_address_t *ib_addr;
    struct ibv_ah_attr ah_attr;
    enum ibv_mtu path_mtu;
    ucs_status_t status;

    /* All the endpoints of this process which talk to one remote node share
     * the initiator QP of that node: an XRC work request names the destination
     * shared receive queue itself, so the QP only fixes the node. The address
     * of the peer names it, and it is known here because UCP passes it. With
     * SHARED_INI=n no key is given, and the endpoint gets a QP of its own. */
    if (!iface->config.shared_ini) {
        ++iface->ep_priv_cfg;
    } else {
        /* Which of the two addresses the peer is named by depends on how the
         * upper layer created this endpoint: the device address when UCP
         * passes it, and otherwise the interface address, which this
         * transport packs the device and port address into. */
        if ((params->field_mask & UCT_EP_PARAM_FIELD_DEV_ADDR) &&
            (params->dev_addr != NULL)) {
            ib_addr = (const uct_ib_address_t*)params->dev_addr;
        } else if ((params->field_mask & UCT_EP_PARAM_FIELD_IFACE_ADDR) &&
                   (params->iface_addr != NULL)) {
            ib_addr = (const uct_ib_address_t*)
                    ((const uct_xrc_verbs_iface_addr_t*)
                     params->iface_addr)->raw;
        } else {
            ib_addr = NULL;
        }

        if (ib_addr == NULL) {
            /* Distinguish an upper layer which does not pass the field from
             * one which passes it empty: they need different fixes. */
            if ((params->field_mask & UCT_EP_PARAM_FIELD_DEV_ADDR) ||
                (params->field_mask & UCT_EP_PARAM_FIELD_IFACE_ADDR)) {
                ++iface->ep_priv_addr;
            } else {
                ++iface->ep_priv_mask;
            }
            status = UCS_OK;
        } else {
            status = uct_ib_iface_fill_ah_attr_from_addr(
                    &iface->super.super, ib_addr,
                    (params->field_mask & UCT_EP_PARAM_FIELD_PATH_INDEX) ?
                            params->path_index : 0, &ah_attr, &path_mtu);
            if (status == UCS_OK) {
                key.gid = ah_attr.grh.dgid;
                key.lid = ah_attr.dlid;
                key_p   = &key;
                ++iface->ep_shared;
            } else {
                ucs_trace("ep %p: cannot unpack the address of the peer (%s), "
                          "use an initiator QP of its own", self,
                          ucs_status_string(status));
                ++iface->ep_priv_unpack;
                status = UCS_OK;
            }
        }
    }

    ++iface->eps_alive;
    iface->eps_peak = ucs_max(iface->eps_peak, iface->eps_alive);

    status = uct_xrc_verbs_iface_get_node_qp(iface, key_p, &self->nqp);
    if (status != UCS_OK) {
        goto err;
    }

    self->ini_qp = self->nqp->qp;

    status = uct_xrc_verbs_iface_create_tgt_qp(iface, &self->tgt_qp);
    if (status != UCS_OK) {
        goto err_release_nqp;
    }

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_ep_t, &iface->super,
                              self->nqp->qpn, params);

    /* An XRC initiator only originates operations, it is never a target, so it
     * needs no remote access permissions. The target QP does. */
    if (!self->nqp->inited) {
        status = uct_xrc_verbs_qp_to_init(self->ini_qp, iface, 0);
        if (status != UCS_OK) {
            goto err_destroy_tgt;
        }

        self->nqp->inited = 1;
    }

    status = uct_xrc_verbs_qp_to_init(self->tgt_qp, iface,
                                      IBV_ACCESS_REMOTE_WRITE  |
                                      IBV_ACCESS_REMOTE_READ   |
                                      IBV_ACCESS_REMOTE_ATOMIC);
    if (status != UCS_OK) {
        goto err_destroy_tgt;
    }

    /* Receive completions name the target QP, so it has to resolve to this
     * endpoint (for flow control in particular). The initiator QP is not
     * registered: it is shared, and its completions are attributed to the
     * endpoint by the position which its operation has in its send queue. */
    status = uct_rc_iface_add_qp(&iface->super, &self->super,
                                 self->tgt_qp->qp_num);
    if (status != UCS_OK) {
        goto err_destroy_tgt;
    }

    uct_rc_txqp_available_set(&self->super.txqp, iface->config.tx_max_wr);
    uct_xrc_verbs_txcnt_init(&self->txcnt);
    uct_ib_fence_info_init(&self->fi);
    self->remote_srqn = 0;
    self->conn_flags  = 0;

    ucs_debug("created xrc ep %p ini_qp 0x%x tgt_qp 0x%x shared %d", self,
              self->nqp->qpn, self->tgt_qp->qp_num, key_p != NULL);

    return UCS_OK;

err_destroy_tgt:
    uct_ib_destroy_qp(self->tgt_qp);
err_release_nqp:
    uct_xrc_verbs_node_qp_release(self->nqp);
err:
    return status;
}

UCS_CLASS_CLEANUP_FUNC(uct_xrc_verbs_ep_t)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(self->super.super.super.iface,
                                                  uct_xrc_verbs_iface_t);
    unsigned i;

    uct_rc_txqp_purge_outstanding(&iface->super, &self->super.txqp,
                                  UCS_ERR_CANCELED, self->txcnt.pi, 1);
    uct_ib_modify_qp(self->tgt_qp, IBV_QPS_ERR);

    /* The target QP has no send queue and its receive queue is the interface
     * SRQ, so it owns no completion queue entries and can be released right
     * away. */
    uct_rc_iface_remove_qp(&iface->super, self->tgt_qp->qp_num);
    uct_ib_destroy_qp(self->tgt_qp);

    /* The send queue of the shared initiator QP still refers to this endpoint
     * by the operations it has in flight, so take their completions off the
     * completion queue before it is gone. If they do not arrive, the endpoint
     * is kept by the deferred cleanup below until they do. */
    ucs_assert(UCS_CIRCULAR_COMPARE16(self->txcnt.pi, >=, self->txcnt.ci));
    for (i = 0; (self->txcnt.pi != self->txcnt.ci) && (i < 1000); ++i) {
        uct_xrc_verbs_iface_poll_tx_wqes(iface);
    }

    if (self->txcnt.pi != self->txcnt.ci) {
        if (!self->nqp->shared) {
            /* The QP goes with this endpoint, so it can be errored and its
             * remaining completions discarded. */
            ucs_trace("ep %p: %u operations of initiator QP 0x%x did not "
                      "complete", self,
                      (uint16_t)(self->txcnt.pi - self->txcnt.ci),
                      self->nqp->qpn);
            uct_ib_modify_qp(self->nqp->qp, IBV_QPS_ERR);
            self->nqp->stale_eps = 1;
        } else {
            /* The QP outlives this endpoint and other endpoints post to it, so
             * it must be left alone: erroring it would stop every one of them,
             * silently.  The send queue is in order and this endpoint owns
             * exactly the slots between its own counters, so detach those and
             * only those; their completions are still popped, they simply name
             * no endpoint any more. */
            uint16_t p;

            ucs_trace("ep %p: %u operations of shared initiator QP 0x%x did "
                      "not complete", self,
                      (uint16_t)(self->txcnt.pi - self->txcnt.ci),
                      self->nqp->qpn);
            for (p = self->txcnt.ci; p != self->txcnt.pi; ++p) {
                self->nqp->eps[p & self->nqp->eps_mask] = NULL;
            }
        }
    }

    ucs_derived_of(self->super.super.super.iface,
                   uct_xrc_verbs_iface_t)->eps_alive--;

    uct_xrc_verbs_node_qp_release(self->nqp);
}

UCS_CLASS_DEFINE(uct_xrc_verbs_ep_t, uct_rc_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_xrc_verbs_ep_t, uct_ep_t,
                          const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_xrc_verbs_ep_t, uct_ep_t);
