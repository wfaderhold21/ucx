/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_XRC_VERBS_IMPL_H
#define UCT_XRC_VERBS_IMPL_H

#include <ucs/arch/bitops.h>

#include <uct/ib/rc/base/rc_iface.h>
#include <uct/ib/rc/base/rc_ep.h>


#define UCT_XRC_VERBS_IFACE_FOREACH_TXWQE(_iface, _i, _wc, _num_wcs) \
    status = uct_ib_poll_cq((_iface)->super.cq[UCT_IB_DIR_TX], &_num_wcs, \
                            _wc); \
    if (status != UCS_OK) { \
        return 0; \
    } \
    UCS_STATS_UPDATE_COUNTER((_iface)->super.stats, \
                             UCT_IB_IFACE_STAT_TX_COMPLETION, _num_wcs); \
    for (_i = 0; _i < _num_wcs; ++_i)


static inline void
uct_xrc_verbs_txqp_posted(uct_rc_txqp_t *txqp, uct_xrc_verbs_txcnt_t *txcnt,
                          uct_rc_iface_t *iface, int signaled)
{
    txcnt->pi++;
    uct_rc_txqp_posted(txqp, iface, 1, signaled);
}

/* Record the endpoint in the send queue of the initiator QP it posted to, so
 * that the completion of that operation can be attributed to it again, and
 * update the counters of both. */
static inline void
uct_xrc_verbs_node_qp_posted(uct_xrc_verbs_node_qp_t *nqp,
                             uct_xrc_verbs_ep_t *ep, uct_rc_iface_t *iface,
                             int signaled)
{
    ucs_assertv((uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci) < nqp->eps_mask,
                "qpn 0x%x pi %d ci %d", nqp->qpn, nqp->txcnt.pi,
                nqp->txcnt.ci);

    nqp->eps[nqp->txcnt.pi & nqp->eps_mask] = ep;
    ++nqp->txcnt.pi;

    uct_xrc_verbs_txqp_posted(&ep->super.txqp, &ep->txcnt, iface, signaled);
}


/* XRC sends carry the destination SRQ number in the work request. */
#if defined(IBV_QPT_XRC_SEND) && defined(IBV_QP_INIT_ATTR_XRCD)
#  define UCT_XRC_VERBS_ENABLE 1
#endif


static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_iface_handle_am(uct_rc_iface_t *iface, uct_rc_hdr_t *hdr,
                              uint64_t wr_id, uint32_t qp_num,
                              uint32_t length, uint32_t imm_data,
                              uint32_t slid)
{
    uct_ib_iface_recv_desc_t *desc;
    uct_rc_iface_ops_t *rc_ops;
    ucs_status_t status;
    void *udesc;

    desc = (uct_ib_iface_recv_desc_t *)wr_id;
    if (ucs_unlikely(hdr->am_id & UCT_RC_EP_FC_MASK)) {
        rc_ops = ucs_derived_of(iface->super.ops, uct_rc_iface_ops_t);
        status = rc_ops->fc_handler(iface, qp_num, hdr, length - sizeof(*hdr),
                                    imm_data, slid, UCT_CB_PARAM_FLAG_DESC);
    } else {
        status = uct_iface_invoke_am(&iface->super.super, hdr->am_id, hdr + 1,
                                     length - sizeof(*hdr),
                                     UCT_CB_PARAM_FLAG_DESC);
    }

    if (ucs_likely(status != UCS_INPROGRESS)) {
        ucs_mpool_put_inline(desc);
    } else {
        udesc = (char*)desc + iface->super.config.rx_headroom_offset;
        uct_recv_desc(udesc) = &iface->super.release_desc;
    }
}

static UCS_F_ALWAYS_INLINE unsigned
uct_xrc_verbs_iface_poll_rx(uct_xrc_verbs_iface_t *iface)
{
    unsigned i, num_wcs = iface->super.super.config.rx_max_poll;
    uct_ib_iface_recv_desc_t *desc;
    struct ibv_wc wc[num_wcs];
    ucs_status_t status;
    uct_rc_hdr_t *hdr;

    status = uct_ib_poll_cq(iface->super.super.cq[UCT_IB_DIR_RX], &num_wcs, wc);
    if (status != UCS_OK) {
        num_wcs = 0;
        goto out;
    }

    for (i = 0; i < num_wcs; i++) {
        desc = (uct_ib_iface_recv_desc_t *)(uintptr_t)wc[i].wr_id;
        if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
            if ((wc[i].status != IBV_WC_REM_ABORT_ERR) &&
                (wc[i].status != IBV_WC_WR_FLUSH_ERR)) {
                UCT_IB_IFACE_VERBS_COMPLETION_FATAL("receive",
                                                    &iface->super.super, i, wc);
            }

            /* A failed receive never delivers an active message. */
            ucs_mpool_put_inline(desc);
            continue;
        }

        hdr = (uct_rc_hdr_t *)uct_ib_iface_recv_desc_hdr(&iface->super.super,
                                                         desc);
        VALGRIND_MAKE_MEM_DEFINED(hdr, wc[i].byte_len);

        uct_ib_log_recv_completion(&iface->super.super, &wc[i], hdr,
                                   wc[i].byte_len, uct_rc_ep_packet_dump);
        uct_xrc_verbs_iface_handle_am(&iface->super, hdr, wc[i].wr_id,
                                      wc[i].qp_num, wc[i].byte_len,
                                      wc[i].imm_data, wc[i].slid);
    }

    UCS_STATS_UPDATE_COUNTER(iface->super.super.stats,
                             UCT_IB_IFACE_STAT_RX_COMPLETION, num_wcs);

out:
    iface->super.rx.srq.available += num_wcs;
    if (iface->super.rx.srq.available >= iface->super.super.config.rx_max_batch) {
        uct_xrc_verbs_post_recv_always(
                iface, ucs_min(iface->super.rx.srq.available,
                               iface->super.super.config.rx_max_batch));
    }

    return num_wcs;
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_iface_fill_inl_sge(uct_xrc_verbs_iface_t *iface,
                                 const void *addr0, unsigned len0,
                                 const void *addr1, unsigned len1)
{
    iface->inl_sge[0].addr      = (uintptr_t)addr0;
    iface->inl_sge[0].length    = len0;
    iface->inl_sge[1].addr      = (uintptr_t)addr1;
    iface->inl_sge[1].length    = len1;
}

static inline void
uct_xrc_verbs_iface_fill_inl_am_sge(uct_xrc_verbs_iface_t *iface,
                                    uint8_t id, uint64_t hdr,
                                    const void *buffer, unsigned length)
{
    uct_rc_am_short_hdr_t *am = &iface->am_inl_hdr;

    am->rc_hdr.am_id         = id;
    am->am_hdr               = hdr;
    iface->inl_am_wr.num_sge = 2;
    uct_xrc_verbs_iface_fill_inl_sge(iface, am, sizeof(*am), buffer, length);
}

static inline void
uct_xrc_verbs_iface_fill_inl_am_sge_iov(uct_xrc_verbs_iface_t *iface,
                                        uint8_t id, const uct_iov_t *iov,
                                        size_t iovcnt)
{
    uct_rc_hdr_t *rch        = &iface->am_inl_hdr.rc_hdr;

    rch->am_id               = id;
    iface->inl_sge[0].addr   = (uintptr_t)rch;
    iface->inl_sge[0].length = sizeof(*rch);
    iface->inl_am_wr.num_sge = uct_ib_verbs_sge_fill_iov(iface->inl_sge + 1,
                                                         iov, iovcnt) + 1;
}

#define UCT_XRC_VERBS_FILL_SGE(_wr, _sge, _length) \
    _wr.sg_list = &_sge; \
    _wr.num_sge = 1; \
    _sge.length = _length;

#define UCT_XRC_VERBS_FILL_INL_PUT_WR(_iface, _raddr, _rkey, _buf, _len) \
    _iface->inl_rwrite_wr.wr.rdma.remote_addr = _raddr; \
    _iface->inl_rwrite_wr.wr.rdma.rkey        = _rkey; \
    _iface->inl_sge[0].addr                   = (uintptr_t)_buf; \
    _iface->inl_sge[0].length                 = _len;

#define UCT_XRC_VERBS_FILL_AM_BCOPY_WR(_wr, _sge, _length, _wr_opcode) \
    UCT_XRC_VERBS_FILL_SGE(_wr, _sge, _length) \
    _wr_opcode = (typeof(_wr_opcode))IBV_WR_SEND;

#define UCT_XRC_VERBS_FILL_AM_ZCOPY_WR_IOV(_wr, _sge, _iovlen, _wr_opcode) \
    _wr.sg_list = _sge; \
    _wr.num_sge = _iovlen; \
    _wr_opcode  = (typeof(_wr_opcode))IBV_WR_SEND;

#define UCT_XRC_VERBS_FILL_RDMA_WR(_wr, _wr_opcode, _opcode, \
                                   _sge, _length, _raddr, _rkey) \
    UCT_XRC_VERBS_FILL_SGE(_wr, _sge, _length) \
    _wr.wr.rdma.remote_addr = _raddr; \
    _wr.wr.rdma.rkey        = _rkey; \
    _wr_opcode              = _opcode;

#define UCT_XRC_VERBS_FILL_RDMA_WR_IOV(_wr, _wr_opcode, _opcode, _sge, _sgelen, \
                                       _raddr, _rkey) \
    _wr.wr.rdma.remote_addr = _raddr; \
    _wr.wr.rdma.rkey        = _rkey; \
    _wr.sg_list             = _sge; \
    _wr.num_sge             = _sgelen; \
    _wr_opcode              = _opcode;

#define UCT_XRC_VERBS_FILL_DESC_WR(_wr, _desc) \
    { \
        struct ibv_sge *sge; \
        (_wr)->next    = NULL; \
        sge            = (_wr)->sg_list; \
        sge->addr      = (uintptr_t)((_desc) + 1); \
        sge->lkey      = (_desc)->lkey; \
    }

#define UCT_XRC_VERBS_FILL_ATOMIC_WR(_wr, _wr_opcode, _sge, _opcode, \
                                     _compare_add, _swap, _remote_addr, _rkey) \
    UCT_XRC_VERBS_FILL_SGE(_wr, _sge, sizeof(uint64_t)) \
    _wr_opcode                = _opcode; \
    _wr.wr.atomic.compare_add = _compare_add; \
    _wr.wr.atomic.swap        = _swap; \
    _wr.wr.atomic.remote_addr = _remote_addr; \
    _wr.wr.atomic.rkey        = _rkey;


#endif /* UCT_XRC_VERBS_IMPL_H */
