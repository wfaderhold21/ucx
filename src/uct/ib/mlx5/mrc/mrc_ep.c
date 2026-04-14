/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2025. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "mrc_iface.h"

#include <uct/ib/base/ib_iface.h>
#include <uct/ib/base/ib_md.h>
#include <uct/base/uct_iface.h>
#include <ucs/debug/log.h>
#include <ucs/type/status.h>


/* Forward declaration: defined later in this file, used by the EP destructor. */
static ucs_arbiter_cb_result_t
uct_mrc_ep_purge_pending_cb(ucs_arbiter_t *arbiter, ucs_arbiter_group_t *group,
                             ucs_arbiter_elem_t *elem, void *arg);


/* ============================================================
 * Helper: fill ibv_ah_attr from the remote packed IB address
 * ============================================================ */

static ucs_status_t
uct_mrc_ep_fill_ah_attr(uct_mrc_iface_t *iface,
                         const uct_ib_address_t *ib_addr,
                         struct ibv_ah_attr *ah_attr_p,
                         enum ibv_mtu *path_mtu_p)
{
    return uct_ib_iface_fill_ah_attr_from_addr(&iface->super, ib_addr,
                                               0, ah_attr_p, path_mtu_p);
}


/* ============================================================
 * EP class constructor
 *
 * Phase 3 in the spec.  Called during UCX wireup (wireup.c:1102).
 * This is when MRC Service IPC occurs via mrc_create_qp_hint().
 * ============================================================ */

UCS_CLASS_INIT_FUNC(uct_mrc_ep_t, const uct_ep_params_t *params)
{
    uct_mrc_iface_t           *iface = ucs_derived_of(params->iface,
                                                       uct_mrc_iface_t);
    struct mrc_qp_hint_init_attr hint_init = {};
    struct mrc_qp_init_attr      qp_init   = {};
    struct ibv_qp_attr           ibv_attr  = {};
    struct mrc_qp_attr           mrc_attr  = {};
    ucs_status_t                 status;

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super.super);

    self->mrc_qp         = NULL;
    self->mrc_hint       = NULL;
    self->qp_num         = 0;
    self->remote_qp_num  = 0;
    self->tx_outstanding = 0;
    ucs_arbiter_group_init(&self->arb_group);
    ucs_list_head_init(&self->flush_comp_list);

    /* -------------------------------------------------------
     * Create QP hint — MRC Service IPC (mrc.h:282-302).
     * v1: per-EP hint with conservative num_send_peers=1.
     * ------------------------------------------------------- */
    hint_init.attr.num_qps_per_peer      = 1;
    hint_init.attr.num_send_peers        = iface->config.num_send_peers;
    hint_init.attr.num_remote_recv_peers = iface->config.num_remote_recv_peers;

    self->mrc_hint = mrc_create_qp_hint(iface->mrc_ctx, &hint_init);
    if (self->mrc_hint == NULL) {
        ucs_error("mrc_create_qp_hint() failed on " UCT_IB_IFACE_FMT,
                  UCT_IB_IFACE_ARG(&iface->super));
        status = UCS_ERR_IO_ERROR;
        goto err;
    }

    /* -------------------------------------------------------
     * Create QP via libmrc (mrc.h:352-379).
     * No RX CQ in v1: recv_cq = send_cq, max_recv_wr = 0.
     * IBV_QP_PORT is NOT supported by mrc_modify_qp (mrc.h:508);
     * the port is implicit from the mrc_context / PD.
     * ------------------------------------------------------- */
    qp_init.send_cq            = iface->mrc_cq_tx;
    qp_init.recv_cq            = iface->mrc_cq_tx;
    qp_init.pd                 = uct_ib_iface_md(&iface->super)->pd;
    qp_init.cap.max_send_wr    = iface->config.tx_queue_len;
    qp_init.cap.max_recv_wr    = 0;
    qp_init.cap.max_send_sge   = UCT_IB_MAX_IOV;
    qp_init.cap.max_inline_data = iface->config.max_inline;
    qp_init.protocol_version   = MRC_PROTOCOL_VERSION_1;

    self->mrc_qp = mrc_create_qp(iface->mrc_ctx, &qp_init);
    if (self->mrc_qp == NULL) {
        ucs_error("mrc_create_qp() failed on " UCT_IB_IFACE_FMT,
                  UCT_IB_IFACE_ARG(&iface->super));
        status = UCS_ERR_IO_ERROR;
        goto err_destroy_hint;
    }

    /* -------------------------------------------------------
     * RESET → INIT: attach QP hint and EV profile.
     * Supported IBV attrs at INIT (mrc.h:412-435):
     *   IBV_QP_STATE, IBV_QP_ACCESS_FLAGS, IBV_QP_PKEY_INDEX.
     * IBV_QP_PORT is explicitly NOT supported; do not pass it.
     * ------------------------------------------------------- */
    ibv_attr.qp_state        = IBV_QPS_INIT;
    ibv_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
    ibv_attr.pkey_index      = iface->super.pkey_index;
    mrc_attr.qp_hint         = self->mrc_hint;
    mrc_attr.ev_profile_id   = iface->config.ev_profile_id;

    if (mrc_modify_qp(self->mrc_qp,
                      &ibv_attr,
                      IBV_QP_STATE | IBV_QP_ACCESS_FLAGS | IBV_QP_PKEY_INDEX,
                      &mrc_attr,
                      MRC_QP_HINT | MRC_QP_EV_PROFILE_ID) != 0) {
        ucs_error("mrc_modify_qp(RESET->INIT) failed on " UCT_IB_IFACE_FMT,
                  UCT_IB_IFACE_ARG(&iface->super));
        status = UCS_ERR_IO_ERROR;
        goto err_destroy_qp;
    }

    /* Get QPN for address advertisement (mrc.h:542). */
    mrc_get_qpn(self->mrc_qp, &self->qp_num);

    return UCS_OK;

err_destroy_qp:
    mrc_destroy_qp(self->mrc_qp);
    self->mrc_qp = NULL;
err_destroy_hint:
    mrc_destroy_qp_hint(self->mrc_hint);
    self->mrc_hint = NULL;
err:
    return status;
}


/* ============================================================
 * EP class destructor
 *
 * Teardown ordering per spec §9:
 *   QP first (decrements hint qp_refcnt), then hint.
 *   mrc_destroy_qp_hint returns EBUSY if refcnt > 0.
 * ============================================================ */

static UCS_CLASS_CLEANUP_FUNC(uct_mrc_ep_t)
{
    uct_mrc_iface_t      *iface = ucs_derived_of(self->super.super.iface,
                                                  uct_mrc_iface_t);
    uct_mrc_flush_comp_t *fc, *tmp;

    ucs_arbiter_group_purge(&iface->tx_arbiter, &self->arb_group,
                            uct_mrc_ep_purge_pending_cb, NULL);

    /* Cancel any pending EP flush completions. */
    ucs_list_for_each_safe(fc, tmp, &self->flush_comp_list, list) {
        ucs_list_del(&fc->list);
        uct_invoke_completion(fc->comp, UCS_ERR_CANCELED);
        ucs_free(fc);
    }

    if (self->mrc_qp != NULL) {
        mrc_destroy_qp(self->mrc_qp);      /* mrc.h:391 */
        self->mrc_qp = NULL;
    }
    if (self->mrc_hint != NULL) {
        mrc_destroy_qp_hint(self->mrc_hint); /* mrc.h:315 */
        self->mrc_hint = NULL;
    }
}

UCS_CLASS_DEFINE(uct_mrc_ep_t, uct_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_mrc_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_mrc_ep_t, uct_ep_t);


/* ============================================================
 * ep_get_address / ep_is_connected
 * ============================================================ */

ucs_status_t uct_mrc_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr)
{
    uct_mrc_ep_t      *ep       = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_ep_addr_t *mrc_addr = (uct_mrc_ep_addr_t *)addr;

    mrc_addr->qp_num = ep->qp_num;
    return UCS_OK;
}

int uct_mrc_ep_is_connected(uct_ep_h tl_ep,
                              const uct_ep_is_connected_params_t *params)
{
    uct_mrc_ep_t *ep   = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    union ibv_gid *rgid;

    /* Require device_addr in params — matches the connect_to_ep_v2 path. */
    UCT_EP_IS_CONNECTED_CHECK_DEV_ADDR(params);

    /* remote_qp_num is set only after connect_to_ep_v2 completes (RTS). */
    if (ep->remote_qp_num == 0) {
        return 0;
    }

    /* If the caller supplies an EP address, verify it matches. */
    if ((params->field_mask & UCT_EP_IS_CONNECTED_FIELD_EP_ADDR) &&
        (((const uct_mrc_ep_addr_t *)params->ep_addr)->qp_num !=
         ep->remote_qp_num)) {
        return 0;
    }

    rgid = ep->remote_is_global ? &ep->remote_gid : NULL;
    return uct_ib_iface_is_same_device(
                (const uct_ib_address_t *)params->device_addr,
                ep->remote_lid, rgid);
}


/* ============================================================
 * ep_connect_to_ep_v2
 *
 * Phase 5: INIT → RTR → RTS.
 * Called after OOB address exchange; both remote LID/GID (from
 * device_addr) and remote QPN (from ep_addr) are available.
 * ============================================================ */

ucs_status_t
uct_mrc_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                              const uct_device_addr_t *device_addr,
                              const uct_ep_addr_t *ep_addr,
                              const uct_ep_connect_to_ep_params_t *params)
{
    uct_mrc_ep_t              *ep      = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t           *iface   = ucs_derived_of(ep->super.super.iface,
                                                         uct_mrc_iface_t);
    const uct_ib_address_t    *ib_addr = (const uct_ib_address_t *)device_addr;
    const uct_mrc_ep_addr_t   *mrc_addr = (const uct_mrc_ep_addr_t *)ep_addr;
    struct ibv_ah_attr          ah_attr  = {};
    struct ibv_qp_attr          ibv_attr = {};
    struct mrc_qp_attr          mrc_attr = {};
    enum ibv_mtu                path_mtu;
    ucs_status_t                status;

    /* Build AH attr from the remote IB device address (LID/GID). */
    status = uct_mrc_ep_fill_ah_attr(iface, ib_addr, &ah_attr, &path_mtu);
    if (status != UCS_OK) {
        return status;
    }

    /* -----------------------------------------------------------
     * INIT → RTR (mrc.h:404-407)
     *
     * Required MRC attrs: MRC_QP_MAX_WIMM_DEST, MRC_QP_MPR_DEST,
     *                     MRC_QP_DYNAMIC_MPR_DEST.
     * NOT supported by mrc_modify_qp: IBV_QP_TIMEOUT,
     *   IBV_QP_RETRY_CNT, IBV_QP_MAX_DEST_RD_ATOMIC, IBV_QP_PORT.
     * ----------------------------------------------------------- */
    ibv_attr.qp_state        = IBV_QPS_RTR;
    ibv_attr.path_mtu        = (path_mtu != UCT_IB_ADDRESS_INVALID_PATH_MTU) ?
                                path_mtu : iface->super.config.path_mtu;
    ibv_attr.dest_qp_num     = mrc_addr->qp_num;
    ibv_attr.rq_psn          = 0;
    ibv_attr.ah_attr         = ah_attr;

    mrc_attr.wimm.max_wimm_dest   = iface->dev_attr.wimm_attr.max_wimm_dest;
    mrc_attr.mpr.mpr_dest         = iface->config.mpr_dest;
    mrc_attr.mpr.dynamic_mpr_dest = 1;

    if (mrc_modify_qp(ep->mrc_qp,
                      &ibv_attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN,
                      &mrc_attr,
                      MRC_QP_MAX_WIMM_DEST | MRC_QP_MPR_DEST |
                      MRC_QP_DYNAMIC_MPR_DEST) != 0) {
        ucs_error("mrc_modify_qp(INIT->RTR) failed on " UCT_IB_IFACE_FMT
                  " dest_qpn=%u",
                  UCT_IB_IFACE_ARG(&iface->super), mrc_addr->qp_num);
        return UCS_ERR_IO_ERROR;
    }

    /* -----------------------------------------------------------
     * RTR → RTS (mrc.h:408-411)
     *
     * Required MRC attrs: MRC_QP_MAX_WIMM, MRC_QP_MPR, MRC_QP_TIMEOUT.
     * ----------------------------------------------------------- */
    memset(&ibv_attr, 0, sizeof(ibv_attr));
    memset(&mrc_attr, 0, sizeof(mrc_attr));

    ibv_attr.qp_state = IBV_QPS_RTS;
    ibv_attr.sq_psn   = 0;

    mrc_attr.wimm.max_wimm = iface->dev_attr.wimm_attr.max_wimm;
    mrc_attr.mpr.mpr       = iface->config.mpr;
    mrc_attr.timeout       = iface->config.timeout;

    if (mrc_modify_qp(ep->mrc_qp,
                      &ibv_attr, IBV_QP_STATE | IBV_QP_SQ_PSN,
                      &mrc_attr,
                      MRC_QP_MAX_WIMM | MRC_QP_MPR | MRC_QP_TIMEOUT) != 0) {
        ucs_error("mrc_modify_qp(RTR->RTS) failed on " UCT_IB_IFACE_FMT,
                  UCT_IB_IFACE_ARG(&iface->super));
        return UCS_ERR_IO_ERROR;
    }

    /* Record remote routing info for ep_is_connected validation. */
    ep->remote_qp_num    = mrc_addr->qp_num;
    ep->remote_lid       = ah_attr.dlid;
    ep->remote_is_global = ah_attr.is_global;
    if (ah_attr.is_global) {
        ep->remote_gid = ah_attr.grh.dgid;
    }

    ucs_debug("mrc_mlx5 EP %p: QP %u connected to remote QPN %u",
              ep, ep->qp_num, mrc_addr->qp_num);
    return UCS_OK;
}


/* ============================================================
 * PUT_SHORT  (inline RDMA Write)
 * ============================================================ */

ucs_status_t uct_mrc_ep_put_short(uct_ep_h tl_ep,
                                   const void *buffer, unsigned length,
                                   uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_mrc_ep_t    *ep    = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                              uct_mrc_iface_t);
    struct ibv_send_wr wr  = {};
    struct ibv_sge     sge = { (uintptr_t)buffer, length, 0 };
    struct ibv_send_wr *bad_wr;

    if (ucs_unlikely(ep->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        return UCS_ERR_NO_RESOURCE;
    }
    if (ucs_unlikely(iface->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        return UCS_ERR_NO_RESOURCE;
    }

    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.send_flags          = IBV_SEND_INLINE | IBV_SEND_SIGNALED;
    wr.wr_id               = 0; /* no descriptor or completion */
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = (uint32_t)rkey;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;

    if (ucs_unlikely(mrc_post_send(ep->mrc_qp, &wr, &bad_wr) != 0)) {
        return UCS_ERR_IO_ERROR;
    }

    UCT_TL_EP_STAT_OP(&ep->super, PUT, SHORT, length);
    ep->tx_outstanding++;
    iface->tx_outstanding++;
    return UCS_OK;
}


/* ============================================================
 * PUT_BCOPY  (bounce-buffer RDMA Write)
 * ============================================================ */

ssize_t uct_mrc_ep_put_bcopy(uct_ep_h tl_ep,
                               uct_pack_callback_t pack_cb, void *arg,
                               uint64_t remote_addr, uct_rkey_t rkey)
{
    uct_mrc_ep_t         *ep    = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t      *iface = ucs_derived_of(ep->super.super.iface,
                                                   uct_mrc_iface_t);
    uct_mrc_send_desc_t  *desc;
    struct ibv_send_wr    wr    = {};
    struct ibv_sge        sge;
    struct ibv_send_wr   *bad_wr;
    size_t                packed;

    if (ucs_unlikely(ep->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        UCT_TL_IFACE_STAT_TX_NO_DESC(&iface->super.super);
        return (ssize_t)UCS_ERR_NO_RESOURCE;
    }
    if (ucs_unlikely(iface->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        UCT_TL_IFACE_STAT_TX_NO_DESC(&iface->super.super);
        return (ssize_t)UCS_ERR_NO_RESOURCE;
    }

    desc = ucs_mpool_get(&iface->tx_desc_mp);
    if (ucs_unlikely(desc == NULL)) {
        UCT_TL_IFACE_STAT_TX_NO_DESC(&iface->super.super);
        return (ssize_t)UCS_ERR_NO_RESOURCE;
    }

    packed = pack_cb(desc + 1, arg);

    sge.addr   = (uintptr_t)(desc + 1);
    sge.length = packed;
    sge.lkey   = desc->lkey;

    /* Encode the desc pointer with the BCOPY tag bit so progress can
     * distinguish it from a PUT_ZCOPY completion pointer. */
    wr.wr_id               = (uintptr_t)desc | UCT_MRC_WR_ID_BCOPY_FLAG;
    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = (uint32_t)rkey;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;

    if (ucs_unlikely(mrc_post_send(ep->mrc_qp, &wr, &bad_wr) != 0)) {
        ucs_mpool_put(desc);
        return (ssize_t)UCS_ERR_IO_ERROR;
    }

    UCT_TL_EP_STAT_OP(&ep->super, PUT, BCOPY, packed);
    ep->tx_outstanding++;
    iface->tx_outstanding++;
    return (ssize_t)packed;
}


/* ============================================================
 * PUT_ZCOPY  (zero-copy RDMA Write)
 * ============================================================ */

ucs_status_t uct_mrc_ep_put_zcopy(uct_ep_h tl_ep,
                                   const uct_iov_t *iov, size_t iovcnt,
                                   uint64_t remote_addr, uct_rkey_t rkey,
                                   uct_completion_t *comp)
{
    uct_mrc_ep_t    *ep    = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                              uct_mrc_iface_t);
    struct ibv_send_wr  wr     = {};
    struct ibv_sge      sge[UCT_IB_MAX_IOV];
    struct ibv_send_wr *bad_wr;
    size_t              i;

    ucs_assert(iovcnt <= UCT_IB_MAX_IOV);

    if (ucs_unlikely(ep->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        return UCS_ERR_NO_RESOURCE;
    }
    if (ucs_unlikely(iface->tx_outstanding >= (unsigned)iface->config.tx_queue_len)) {
        return UCS_ERR_NO_RESOURCE;
    }

    for (i = 0; i < iovcnt; i++) {
        sge[i].addr   = (uintptr_t)iov[i].buffer;
        sge[i].length = iov[i].length;
        sge[i].lkey   = uct_ib_memh_get_lkey(iov[i].memh);
    }

    /* comp may be NULL (fire-and-forget PUT_ZCOPY).
     * If non-NULL it must be aligned, so bit 0 is always clear — no
     * collision with UCT_MRC_WR_ID_BCOPY_FLAG. */
    wr.wr_id               = (uintptr_t)comp; /* 0 if no completion needed */
    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = (uint32_t)rkey;
    wr.sg_list             = sge;
    wr.num_sge             = iovcnt;

    if (ucs_unlikely(mrc_post_send(ep->mrc_qp, &wr, &bad_wr) != 0)) {
        return UCS_ERR_IO_ERROR;
    }

    UCT_TL_EP_STAT_OP(&ep->super, PUT, ZCOPY,
                      uct_iov_total_length(iov, iovcnt));
    ep->tx_outstanding++;
    iface->tx_outstanding++;
    return UCS_INPROGRESS;
}


/* ============================================================
 * ep_flush
 * ============================================================ */

ucs_status_t uct_mrc_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp)
{
    uct_mrc_ep_t         *ep = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_flush_comp_t *fc;

    if (ep->tx_outstanding == 0) {
        UCT_TL_EP_STAT_FLUSH(&ep->super);
        return UCS_OK;
    }

    if (comp != NULL) {
        fc = ucs_malloc(sizeof(*fc), "mrc_flush_comp");
        if (ucs_unlikely(fc == NULL)) {
            return UCS_ERR_NO_MEMORY;
        }
        fc->comp = comp;
        ++comp->count;
        ucs_list_add_tail(&ep->flush_comp_list, &fc->list);
    }

    UCT_TL_EP_STAT_FLUSH_WAIT(&ep->super);
    return UCS_INPROGRESS;
}


/* ============================================================
 * Pending-send support
 *
 * uct_mrc_ep_pending_add: enqueue a pending request on the EP's
 *   arbiter group and schedule the group with the iface arbiter.
 *
 * uct_mrc_ep_pending_purge: drain the EP's arbiter group, calling
 *   the user purge callback for each element.
 *
 * uct_mrc_ep_process_pending: arbiter dispatch callback;
 *   calls the pending request's send function and reschedules or
 *   removes the group from the arbiter accordingly.
 * ============================================================ */

ucs_status_t uct_mrc_ep_pending_add(uct_ep_h tl_ep,
                                     uct_pending_req_t *req,
                                     unsigned flags)
{
    uct_mrc_ep_t    *ep    = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t *iface = ucs_derived_of(ep->super.super.iface,
                                              uct_mrc_iface_t);

    UCT_TL_EP_STAT_PEND(&ep->super);
    ucs_arbiter_group_push_elem(&ep->arb_group, &req->arb_elem);
    ucs_arbiter_group_schedule(&iface->tx_arbiter, &ep->arb_group);
    return UCS_OK;
}


/* Purge callback adapter for ucs_arbiter_group_purge.
 * When arg is NULL (EP destructor path) elements are silently removed;
 * when arg is a uct_purge_cb_args_t * (ep_pending_purge path) the user
 * callback is invoked for each element. */
static ucs_arbiter_cb_result_t
uct_mrc_ep_purge_pending_cb(ucs_arbiter_t *arbiter,
                             ucs_arbiter_group_t *group,
                             ucs_arbiter_elem_t *elem,
                             void *arg)
{
    uct_purge_cb_args_t *args = arg;
    uct_pending_req_t   *req  = ucs_container_of(elem, uct_pending_req_t,
                                                  arb_elem);
    if (args != NULL) {
        args->cb(req, args->arg);
    }
    return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
}

void uct_mrc_ep_pending_purge(uct_ep_h tl_ep,
                               uct_pending_purge_callback_t cb,
                               void *arg)
{
    uct_mrc_ep_t       *ep    = ucs_derived_of(tl_ep, uct_mrc_ep_t);
    uct_mrc_iface_t    *iface = ucs_derived_of(ep->super.super.iface,
                                                uct_mrc_iface_t);
    uct_purge_cb_args_t args  = { cb, arg };

    ucs_arbiter_group_purge(&iface->tx_arbiter, &ep->arb_group,
                             uct_mrc_ep_purge_pending_cb, &args);
}


ucs_arbiter_cb_result_t
uct_mrc_ep_process_pending(ucs_arbiter_t *arbiter,
                            ucs_arbiter_group_t *group,
                            ucs_arbiter_elem_t *elem,
                            void *arg)
{
    uct_mrc_ep_t      *ep  = ucs_container_of(group, uct_mrc_ep_t, arb_group);
    uct_pending_req_t *req = ucs_container_of(elem, uct_pending_req_t,
                                               arb_elem);
    ucs_status_t       status;

    /* Inhibit recursive pending dispatch. */
    status = req->func(req);

    if (status == UCS_OK) {
        return UCS_ARBITER_CB_RESULT_REMOVE_ELEM;
    } else if (status == UCS_INPROGRESS) {
        return UCS_ARBITER_CB_RESULT_NEXT_GROUP;
    } else {
        /* No send resource; re-queue and stop dispatching this group. */
        return UCS_ARBITER_CB_RESULT_RESCHED_GROUP;
    }
}
