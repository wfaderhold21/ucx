/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2025. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "mrc_iface.h"

#include <uct/ib/base/ib_md.h>
#include <uct/ib/base/ib_device.h>
#include <uct/base/uct_iface.h>
#include <uct/base/uct_md.h>
#include <ucs/debug/log.h>
#include <ucs/sys/string.h>


/* ============================================================
 * Configuration table
 * ============================================================ */

ucs_config_field_t uct_mrc_iface_config_table[] = {
    {"", UCT_IB_SEND_OVERHEAD_DEFAULT(0), NULL,
     ucs_offsetof(uct_mrc_iface_config_t, super),
     UCS_CONFIG_TYPE_TABLE(uct_ib_iface_config_table)},

    {"TX_QUEUE_LEN", "256",
     "TX QP send queue depth.",
     ucs_offsetof(uct_mrc_iface_config_t, tx_queue_len),
     UCS_CONFIG_TYPE_INT},

    {"MAX_INLINE", "64",
     "Maximum inline data size for PUT_SHORT (bytes).",
     ucs_offsetof(uct_mrc_iface_config_t, max_inline),
     UCS_CONFIG_TYPE_INT},

    {"MPR", "1",
     "Requestor MPR value (units = 128 PSNs).",
     ucs_offsetof(uct_mrc_iface_config_t, mpr),
     UCS_CONFIG_TYPE_INT},

    {"MPR_DEST", "1",
     "Responder MPR value.",
     ucs_offsetof(uct_mrc_iface_config_t, mpr_dest),
     UCS_CONFIG_TYPE_INT},

    {"TIMEOUT", "14",
     "QP ACK timeout (1.024 * 2^N microseconds).",
     ucs_offsetof(uct_mrc_iface_config_t, timeout),
     UCS_CONFIG_TYPE_INT},

    {"NUM_SEND_PEERS", "1",
     "QP hint: number of simultaneous send peers (v1: 1 for p2p).",
     ucs_offsetof(uct_mrc_iface_config_t, num_send_peers),
     UCS_CONFIG_TYPE_INT},

    {"NUM_REMOTE_RECV_PEERS", "1",
     "QP hint: incast factor at target (v1: 1 for p2p).",
     ucs_offsetof(uct_mrc_iface_config_t, num_remote_recv_peers),
     UCS_CONFIG_TYPE_INT},

    {"EV_PROFILE_ID", "0",
     "EV profile ID (0 = default profile).",
     ucs_offsetof(uct_mrc_iface_config_t, ev_profile_id),
     UCS_CONFIG_TYPE_ULONG},

    {NULL}
};


/* ============================================================
 * Helper: TX bounce-buffer pool object initialiser
 *
 * Called once per object when the pool page is first allocated.
 * Stores the ibv_mr lkey so POST_SEND can reference it directly.
 * ============================================================ */

static void uct_mrc_send_desc_init(uct_iface_h tl_iface, void *obj,
                                   uct_mem_h memh)
{
    uct_mrc_send_desc_t *desc = obj;

    desc->lkey = uct_ib_memh_get_lkey(memh);
}


/* ============================================================
 * create_cq / destroy_cq callbacks
 *
 * Called from inside uct_ib_iface_t's constructor.
 * self->mrc_ctx must already be set (pre-super pattern).
 * ============================================================ */

static ucs_status_t
uct_mrc_iface_create_cq(uct_ib_iface_t *ib_iface, uct_ib_dir_t dir,
                         const uct_ib_iface_init_attr_t *init_attr,
                         int preferred_cpu, size_t inl)
{
    uct_mrc_iface_t *iface = ucs_derived_of(ib_iface, uct_mrc_iface_t);

    if (dir == UCT_IB_DIR_RX) {
        /* No RX CQ in v1: no WRITE_WITH_IMM, no mrc_post_recv.
         * The QP reuses the TX CQ as recv_cq with max_recv_wr=0. */
        ib_iface->cq[UCT_IB_DIR_RX] = NULL;
        return UCS_OK;
    }

    /* TX CQ: owned exclusively by libmrc; no ibv_cq* is exposed. */
    iface->mrc_cq_tx = mrc_create_cq(iface->mrc_ctx,
                                      init_attr->cq_len[UCT_IB_DIR_TX],
                                      NULL, NULL, preferred_cpu);
    if (iface->mrc_cq_tx == NULL) {
        ucs_error("mrc_create_cq() failed on " UCT_IB_IFACE_FMT,
                  UCT_IB_IFACE_ARG(ib_iface));
        return UCS_ERR_IO_ERROR;
    }

    /* Bypass the ibv_cq* path: uct_ib_iface_pre_arm / uct_ib_iface_arm_cq
     * both dereference cq[dir] unconditionally; since MRC has no ibv_cq*
     * we must prevent those functions from being called.
     * Setting cq[TX]=NULL is safe because:
     *   (a) mrc_create_qp uses mrc_cq* directly (not ibv_cq*), and
     *   (b) iface_event_arm returns UCS_ERR_UNSUPPORTED so the arm path
     *       is never reached. */
    ib_iface->cq[UCT_IB_DIR_TX] = NULL;
    return UCS_OK;
}

static void
uct_mrc_iface_destroy_cq(uct_ib_iface_t *ib_iface, uct_ib_dir_t dir)
{
    uct_mrc_iface_t *iface = ucs_derived_of(ib_iface, uct_mrc_iface_t);

    if (dir == UCT_IB_DIR_TX && iface->mrc_cq_tx != NULL) {
        mrc_destroy_cq(iface->mrc_cq_tx);
        iface->mrc_cq_tx = NULL;
    }
    /* RX: nothing to destroy */
}


/* Forward declaration: defined below, called by async_event_handler above. */
static uct_mrc_ep_t *uct_mrc_iface_lookup_qp(uct_mrc_iface_t *iface,
                                              struct mrc_qp *qp);


/* ============================================================
 * Async event handler (registered on the device event channel)
 * ============================================================ */

static void
uct_mrc_iface_async_event_handler(int fd, ucs_event_set_types_t events,
                                   void *arg)
{
    uct_mrc_iface_t *iface = arg;
    struct mrc_async_event event;
    uct_mrc_ep_t *ep;

    mrc_get_async_event(iface->mrc_ctx, &event); /* mrc.h:594 */

    switch (event.event_type) {
    case IBV_EVENT_QP_FATAL:
    case IBV_EVENT_QP_REQ_ERR:
    case IBV_EVENT_QP_ACCESS_ERR:
        ep = uct_mrc_iface_lookup_qp(iface, event.element.qp);
        if (ep != NULL) {
            uct_ib_handle_failure(&iface->super, ep,
                                  UCS_ERR_ENDPOINT_TIMEOUT);
        }
        break;
    case IBV_EVENT_CQ_ERR:
        ucs_error("MRC CQ error on iface %p", iface);
        break;
    default:
        ucs_debug("Unhandled MRC async event %d on iface %p",
                  event.event_type, iface);
        break;
    }

    mrc_ack_async_event(&event); /* mrc.h:606 */
}

/* Lookup an EP by QP pointer (for async error dispatch). */
static uct_mrc_ep_t *uct_mrc_iface_lookup_qp(uct_mrc_iface_t *iface,
                                              struct mrc_qp *qp)
{
    /* v1: linear scan; production would use a hash table keyed by QPN. */
    uct_base_iface_t *base = &iface->super.super;
    uct_ep_h tl_ep;

    ucs_list_for_each(tl_ep, &base->super.tl_ep_list, list) {
        uct_mrc_ep_t *ep = ucs_derived_of(tl_ep, uct_mrc_ep_t);
        if (ep->mrc_qp == qp) {
            return ep;
        }
    }
    return NULL;
}

/* Lookup an EP by QP number (for TX completion processing). */
static uct_mrc_ep_t *uct_mrc_iface_lookup_qpn(uct_mrc_iface_t *iface,
                                               uint32_t qp_num)
{
    uct_base_iface_t *base = &iface->super.super;
    uct_ep_h tl_ep;

    ucs_list_for_each(tl_ep, &base->super.tl_ep_list, list) {
        uct_mrc_ep_t *ep = ucs_derived_of(tl_ep, uct_mrc_ep_t);
        if (ep->qp_num == qp_num) {
            return ep;
        }
    }
    return NULL;
}

/* Invoke and drain pending EP-level flush completions. */
static void uct_mrc_ep_invoke_pending_flush(uct_mrc_ep_t *ep)
{
    uct_mrc_flush_comp_t *fc, *tmp;

    ucs_list_for_each_safe(fc, tmp, &ep->flush_comp_list, list) {
        ucs_list_del(&fc->list);
        uct_invoke_completion(fc->comp, UCS_OK);
        ucs_free(fc);
    }
}

/* Invoke and drain pending iface-level flush completions. */
static void uct_mrc_iface_invoke_pending_flush(uct_mrc_iface_t *iface)
{
    uct_mrc_flush_comp_t *fc, *tmp;

    ucs_list_for_each_safe(fc, tmp, &iface->flush_comp_list, list) {
        ucs_list_del(&fc->list);
        uct_invoke_completion(fc->comp, UCS_OK);
        ucs_free(fc);
    }
}


/* ============================================================
 * handle_failure callback
 * ============================================================ */

static void
uct_mrc_iface_handle_failure(uct_ib_iface_t *ib_iface, void *arg,
                              ucs_status_t status)
{
    uct_mrc_ep_t *ep = arg;

    ucs_debug("MRC EP %p failure: %s", ep, ucs_status_string(status));
    uct_iface_handle_ep_err(&ib_iface->super.super, &ep->super.super, status);
}


/* ============================================================
 * event_cq callback (no-op for poll-only mode)
 * ============================================================ */

static void
uct_mrc_iface_event_cq(uct_ib_iface_t *ib_iface, uct_ib_dir_t dir)
{
    /* Poll-only: no CQ event arming. */
}


/* ============================================================
 * iface_event_arm: unsupported (cq[dir] == NULL)
 * ============================================================ */

static ucs_status_t
uct_mrc_iface_event_arm(uct_iface_h tl_iface, unsigned events)
{
    /* MRC uses mrc_cq* not ibv_cq*; event arming is not available in v1.
     * Advertising UCT_IFACE_FLAG_EVENT_* is intentionally omitted. */
    return UCS_ERR_UNSUPPORTED;
}


/* ============================================================
 * Progress: drain TX CQ and dispatch pending sends
 * ============================================================ */

unsigned uct_mrc_iface_progress(uct_iface_h tl_iface)
{
    uct_mrc_iface_t *iface = ucs_derived_of(tl_iface, uct_mrc_iface_t);
    struct ibv_wc wc[UCT_MRC_MAX_TX_POLL];
    unsigned count = 0;
    int nc, i;

    /* Poll TX completions */
    nc = mrc_poll_cq(iface->mrc_cq_tx, UCT_MRC_MAX_TX_POLL, wc);
    for (i = 0; i < nc; i++) {
        uct_mrc_ep_t *ep;

        ep = uct_mrc_iface_lookup_qpn(iface, wc[i].qp_num);

        /* Check CQE status before touching flush completions: an error CQE
         * must not cause pending flush callbacks to fire with UCS_OK. */
        if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
            ucs_error("MRC TX error on " UCT_IB_IFACE_FMT
                      ": %s (wr_id=0x%lx qpn=%u)",
                      UCT_IB_IFACE_ARG(&iface->super),
                      ibv_wc_status_str(wc[i].status),
                      wc[i].wr_id, wc[i].qp_num);
            ucs_assert(iface->tx_outstanding > 0);
            iface->tx_outstanding--;
            if (ep != NULL) {
                ucs_assert(ep->tx_outstanding > 0);
                ep->tx_outstanding--;
                uct_ib_handle_failure(&iface->super, ep,
                                      UCS_ERR_ENDPOINT_TIMEOUT);
            }
            continue;
        }

        ucs_assert(iface->tx_outstanding > 0);
        iface->tx_outstanding--;

        /* Decrement per-EP outstanding; fire EP flush completions if drained. */
        if (ucs_likely(ep != NULL)) {
            ucs_assert(ep->tx_outstanding > 0);
            ep->tx_outstanding--;
            if (ep->tx_outstanding == 0) {
                uct_mrc_ep_invoke_pending_flush(ep);
            }
        }

        if (wc[i].wr_id == 0) {
            /* PUT_SHORT: inline, no descriptor or completion to invoke */
        } else if (wc[i].wr_id & UCT_MRC_WR_ID_BCOPY_FLAG) {
            /* PUT_BCOPY: return bounce buffer to pool */
            uct_mrc_send_desc_t *desc =
                    (uct_mrc_send_desc_t *)(wc[i].wr_id &
                                           ~UCT_MRC_WR_ID_BCOPY_FLAG);
            ucs_assert(desc != NULL);
            ucs_mpool_put(desc);
        } else {
            /* PUT_ZCOPY: invoke user completion callback */
            uct_completion_t *comp = (uct_completion_t *)wc[i].wr_id;
            uct_invoke_completion(comp, UCS_OK);
        }
        count++;
    }

    /* Fire iface-level flush completions if all TX ops are drained. */
    if (iface->tx_outstanding == 0) {
        uct_mrc_iface_invoke_pending_flush(iface);
    }

    /* Drain pending sends */
    ucs_arbiter_dispatch(&iface->tx_arbiter, 1,
                         uct_mrc_ep_process_pending, NULL);
    return count;
}


/* ============================================================
 * iface_query
 * ============================================================ */

static ucs_status_t
uct_mrc_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_mrc_iface_t *iface = ucs_derived_of(tl_iface, uct_mrc_iface_t);
    ucs_status_t status;

    status = uct_ib_iface_query(&iface->super, iface_attr);
    if (status != UCS_OK) {
        return status;
    }

    iface_attr->cap.flags = UCT_IFACE_FLAG_PUT_SHORT |
                            UCT_IFACE_FLAG_PUT_BCOPY |
                            UCT_IFACE_FLAG_PUT_ZCOPY |
                            UCT_IFACE_FLAG_PENDING   |
                            UCT_IFACE_FLAG_CONNECT_TO_EP;

    iface_attr->cap.put.max_short      = iface->config.max_inline;
    iface_attr->cap.put.max_bcopy      = iface->super.config.seg_size;
    iface_attr->cap.put.min_zcopy      = 0;
    iface_attr->cap.put.max_zcopy      = uct_ib_iface_port_attr(&iface->super)->max_msg_sz;
    iface_attr->cap.put.opt_zcopy_align = UCS_SYS_PCI_MAX_PAYLOAD;
    iface_attr->cap.put.align_mtu      = uct_ib_mtu_value(iface->super.config.path_mtu);
    iface_attr->cap.put.max_iov        = UCT_IB_MAX_IOV;

    iface_attr->iface_addr_len         = sizeof(uct_mrc_iface_addr_t);
    iface_attr->ep_addr_len            = sizeof(uct_mrc_ep_addr_t);
    iface_attr->max_conn_priv          = 0;

    iface_attr->overhead               = 10e-9; /* ~10 ns typical WQE post */
    iface_attr->latency.m              = 0;

    return UCS_OK;
}


/* ============================================================
 * iface_get_address
 * Returns port LID and primary GID (both populated by super init).
 * ============================================================ */

static ucs_status_t
uct_mrc_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *addr)
{
    uct_mrc_iface_t       *iface    = ucs_derived_of(tl_iface, uct_mrc_iface_t);
    uct_mrc_iface_addr_t  *mrc_addr = (uct_mrc_iface_addr_t *)addr;

    mrc_addr->lid = uct_ib_iface_port_attr(&iface->super)->lid;
    mrc_addr->gid = iface->super.gid_info.gid;
    return UCS_OK;
}


/* ============================================================
 * iface_flush / iface_fence
 * ============================================================ */

static ucs_status_t
uct_mrc_iface_flush(uct_iface_h tl_iface, unsigned flags,
                    uct_completion_t *comp)
{
    uct_mrc_iface_t      *iface = ucs_derived_of(tl_iface, uct_mrc_iface_t);
    uct_mrc_flush_comp_t *fc;

    if (iface->tx_outstanding == 0) {
        UCT_TL_IFACE_STAT_FLUSH(&iface->super.super);
        return UCS_OK;
    }

    if (comp != NULL) {
        fc = ucs_malloc(sizeof(*fc), "mrc_flush_comp");
        if (ucs_unlikely(fc == NULL)) {
            return UCS_ERR_NO_MEMORY;
        }
        fc->comp = comp;
        ++comp->count;
        ucs_list_add_tail(&iface->flush_comp_list, &fc->list);
    }

    UCT_TL_IFACE_STAT_FLUSH_WAIT(&iface->super.super);
    return UCS_INPROGRESS;
}

static ucs_status_t
uct_mrc_iface_fence(uct_iface_h tl_iface, unsigned flags)
{
    UCT_TL_IFACE_STAT_FENCE(&ucs_derived_of(tl_iface, uct_mrc_iface_t)->super.super);
    return UCS_OK;
}


/* ============================================================
 * iface_progress_enable: wire up the worker progress callback
 * ============================================================ */

static void
uct_mrc_iface_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
    uct_base_iface_progress_enable_cb(
            ucs_derived_of(tl_iface, uct_base_iface_t),
            (ucs_callback_t)uct_mrc_iface_progress, flags);
}


/* ============================================================
 * UCS_CLASS_INIT: iface constructor
 *
 * Pre-super pattern: mrc_create_context() MUST run before
 * UCS_CLASS_CALL_SUPER_INIT because the super constructor fires
 * self->ops->create_cq() (at ib_iface.c:1798/1804) which needs
 * self->mrc_ctx.  Writing self->mrc_ctx before super init is safe
 * because UCS_CLASS_CALL_SUPER_INIT does not memset (class.h:188).
 * ============================================================ */

static uct_ib_iface_ops_t uct_mrc_iface_ib_ops;
static uct_iface_ops_t    uct_mrc_iface_tl_ops;

UCS_CLASS_INIT_FUNC(uct_mrc_iface_t, uct_md_h md, uct_worker_h worker,
                    const uct_iface_params_t *params,
                    const uct_iface_config_t *tl_config)
{
    uct_mrc_iface_config_t *config  = ucs_derived_of(tl_config,
                                                      uct_mrc_iface_config_t);
    uct_ib_md_t            *ib_md   = ucs_derived_of(md, uct_ib_md_t);
    uct_ib_iface_init_attr_t init_attr = {};
    struct mrc_context_attr  ctx_attr  = {};
    ucs_status_t             status;
    int                      supported;

    /* Step 1: verify MRC hardware support */
    if (mrc_query_device(ib_md->dev.ibv_context,
                         &self->dev_attr, &supported) != 0) {
        ucs_error("mrc_query_device() failed");
        return UCS_ERR_IO_ERROR;
    }
    if (!supported) {
        ucs_debug("MRC not supported on device %s",
                  uct_ib_device_name(&ib_md->dev));
        return UCS_ERR_UNSUPPORTED;
    }

    /* Step 2: create MRC context (local only; no MRC Service contact).
     * MUST be pre-super: create_cq callback needs self->mrc_ctx. */
    ctx_attr.mrc_api_version_used = MRC_API_CURRENT_VERSION;
    self->mrc_ctx = mrc_create_context(ib_md->dev.ibv_context, &ctx_attr);
    if (self->mrc_ctx == NULL) {
        ucs_error("mrc_create_context() failed on device %s",
                  uct_ib_device_name(&ib_md->dev));
        return UCS_ERR_IO_ERROR;
    }

    /* Step 3: build init_attr for the IB base class.
     * rx_hdr_len=0 and rx_priv_len=0 because there is no RX path in v1. */
    init_attr.qp_type                 = IBV_QPT_RC;
    init_attr.seg_size                = config->super.seg_size;
    init_attr.rx_hdr_len              = 0;
    init_attr.rx_priv_len             = 0;
    init_attr.cq_len[UCT_IB_DIR_TX]   = config->tx_queue_len;
    init_attr.cq_len[UCT_IB_DIR_RX]   = 0;
    init_attr.dev_name                = params->mode.device.dev_name;

    /* Step 4: super init – fires create_cq callbacks which need mrc_ctx. */
    UCS_CLASS_CALL_SUPER_INIT(uct_ib_iface_t,
                              &uct_mrc_iface_tl_ops,
                              &uct_mrc_iface_ib_ops,
                              md, worker, params,
                              &config->super,
                              &init_attr);

    /* Step 5: cache configuration */
    self->config.tx_queue_len          = config->tx_queue_len;
    self->config.num_send_peers        = config->num_send_peers;
    self->config.num_remote_recv_peers = config->num_remote_recv_peers;
    self->config.mpr                   = config->mpr;
    self->config.mpr_dest              = config->mpr_dest;
    self->config.timeout               = (uint8_t)config->timeout;
    self->config.max_inline            = config->max_inline;
    self->config.ev_profile_id         = config->ev_profile_id;
    self->tx_outstanding               = 0;

    /* Step 6: TX bounce-buffer pool (PUT_BCOPY) */
    status = uct_iface_mpool_init(
            &self->super.super,
            &self->tx_desc_mp,
            sizeof(uct_mrc_send_desc_t) + config->super.seg_size,
            sizeof(uct_mrc_send_desc_t),
            UCS_SYS_CACHE_LINE_SIZE,
            &config->super.tx.mp,
            config->tx_queue_len,
            uct_mrc_send_desc_init,
            "mrc_tx_desc");
    if (status != UCS_OK) {
        goto err_destroy_mrc_ctx;
    }

    /* Step 7: pending send arbiter */
    ucs_arbiter_init(&self->tx_arbiter);

    /* Step 8: pending iface-flush list */
    ucs_list_head_init(&self->flush_comp_list);

    return UCS_OK;

err_destroy_mrc_ctx:
    mrc_destroy_context(self->mrc_ctx);
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_mrc_iface_t)
{
    uct_mrc_flush_comp_t *fc, *tmp;

    uct_base_iface_progress_disable(&self->super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    /* Cancel any pending iface flush completions. */
    ucs_list_for_each_safe(fc, tmp, &self->flush_comp_list, list) {
        ucs_list_del(&fc->list);
        uct_invoke_completion(fc->comp, UCS_ERR_CANCELED);
        ucs_free(fc);
    }

    ucs_arbiter_cleanup(&self->tx_arbiter);
    ucs_mpool_cleanup(&self->tx_desc_mp, 1);

    /* Teardown ordering per spec §9:
     *   CQ before context (all EPs are already gone by this point).
     * NOTE: Do NOT call UCS_CLASS_CLEANUP(uct_ib_iface_t) here — the UCS
     * class hierarchy calls the parent cleanup automatically after this
     * function returns.  An explicit call would cause double cleanup. */
    if (self->mrc_cq_tx != NULL) {
        mrc_destroy_cq(self->mrc_cq_tx);
        self->mrc_cq_tx = NULL;
    }
    mrc_destroy_context(self->mrc_ctx);
}

UCS_CLASS_DEFINE(uct_mrc_iface_t, uct_ib_iface_t);

static UCS_CLASS_DEFINE_NEW_FUNC(uct_mrc_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t *,
                                 const uct_iface_config_t *);

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_mrc_iface_t, uct_iface_t);


/* ============================================================
 * Ops tables
 *
 * uct_mrc_iface_ib_ops  – IB-layer callbacks (create_cq etc.)
 * uct_mrc_iface_tl_ops  – external UCT API operations
 * ============================================================ */

static uct_ib_iface_ops_t uct_mrc_iface_ib_ops = {
    .super = {
        .iface_query_v2        = uct_iface_base_query_v2,
        .iface_estimate_perf   = uct_ib_iface_estimate_perf,
        .iface_vfs_refresh     = (uct_iface_vfs_refresh_func_t)
                                         ucs_empty_function,
        .ep_query              = (uct_ep_query_func_t)
                                         ucs_empty_function_return_unsupported,
        .ep_invalidate         = (uct_ep_invalidate_func_t)
                                         ucs_empty_function_return_unsupported,
        .ep_connect_to_ep_v2   = uct_mrc_ep_connect_to_ep_v2,
        .iface_is_reachable_v2 = uct_ib_iface_is_reachable_v2,
        .ep_is_connected       = uct_mrc_ep_is_connected,
        .ep_get_device_ep      = (uct_ep_get_device_ep_func_t)
                                         ucs_empty_function_return_unsupported,
        .ep_put_sgl_zcopy      = (uct_ep_put_sgl_zcopy_func_t)
                                         ucs_empty_function_return_unsupported,
    },
    .create_cq      = uct_mrc_iface_create_cq,
    .destroy_cq     = uct_mrc_iface_destroy_cq,
    .event_cq       = uct_mrc_iface_event_cq,
    .handle_failure = uct_mrc_iface_handle_failure,
};

static uct_iface_ops_t uct_mrc_iface_tl_ops = {
    /* PUT operations */
    .ep_put_short             = uct_mrc_ep_put_short,
    .ep_put_bcopy             = uct_mrc_ep_put_bcopy,
    .ep_put_zcopy             = uct_mrc_ep_put_zcopy,

    /* Unsupported in v1 */
    .ep_get_bcopy             = (uct_ep_get_bcopy_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_get_zcopy             = (uct_ep_get_zcopy_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_am_short              = (uct_ep_am_short_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_am_short_iov          = (uct_ep_am_short_iov_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_am_bcopy              = (uct_ep_am_bcopy_func_t)
                                        ucs_empty_function_return_ep_timeout,
    .ep_am_zcopy              = (uct_ep_am_zcopy_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic_cswap64        = (uct_ep_atomic_cswap64_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic64_post         = (uct_ep_atomic64_post_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic64_fetch        = (uct_ep_atomic64_fetch_func_t)
                                        ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)
                                        ucs_empty_function_return_unsupported,

    /* Pending */
    .ep_pending_add           = uct_mrc_ep_pending_add,
    .ep_pending_purge         = uct_mrc_ep_pending_purge,

    /* Flow control */
    .ep_flush                 = uct_mrc_ep_flush,
    .ep_fence                 = uct_base_ep_fence,
    .ep_check                 = (uct_ep_check_func_t)
                                        ucs_empty_function_return_unsupported,

    /* EP lifecycle */
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_mrc_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_mrc_ep_t),
    .ep_get_address           = uct_mrc_ep_get_address,
    .ep_connect_to_ep         = uct_base_ep_connect_to_ep,

    /* Iface operations */
    .iface_flush              = uct_mrc_iface_flush,
    .iface_fence              = uct_mrc_iface_fence,
    .iface_progress_enable    = uct_mrc_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_mrc_iface_progress,
    .iface_event_fd_get       = uct_ib_iface_event_fd_get,
    .iface_event_arm          = uct_mrc_iface_event_arm,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_mrc_iface_t),
    .iface_query              = uct_mrc_iface_query,
    .iface_get_address        = uct_mrc_iface_get_address,
    .iface_get_device_address = uct_ib_iface_get_device_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
};


/* ============================================================
 * Transport registration
 * ============================================================ */

static ucs_status_t
uct_mrc_query_tl_devices(uct_md_h md,
                          uct_tl_device_resource_t **tl_devices_p,
                          unsigned *num_tl_devices_p)
{
    uct_ib_md_t *ib_md = ucs_derived_of(md, uct_ib_md_t);
    struct mrc_device_attr dev_attr;
    int supported;

    /* MRC requires the mlx5 memory domain (ConnectX-8/9 hardware). */
    if (strcmp(ib_md->name, UCT_IB_MD_NAME(mlx5))) {
        return UCS_ERR_NO_DEVICE;
    }

    /* Capability gate: only advertise ports on which MRC is supported. */
    if (mrc_query_device(ib_md->dev.ibv_context, &dev_attr, &supported) != 0
        || !supported) {
        return UCS_ERR_NO_DEVICE;
    }

    return uct_ib_device_query_ports(&ib_md->dev,
                                     UCT_IB_DEVICE_FLAG_MLX5_PRM,
                                     tl_devices_p, num_tl_devices_p);
}

UCT_TL_DEFINE_ENTRY(&uct_ib_component, mrc_mlx5,
                    uct_mrc_query_tl_devices,
                    uct_mrc_iface_t, "MRC_",
                    uct_mrc_iface_config_table,
                    uct_mrc_iface_config_t);
