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

#include <uct/api/uct.h>
#include <uct/ib/rc/base/rc_iface.h>
#include <uct/ib/base/ib_device.h>
#include <uct/ib/base/ib_log.h>
#include <uct/base/uct_md.h>
#include <ucs/arch/bitops.h>
#include <ucs/arch/cpu.h>
#include <arpa/inet.h>
#include <ucs/debug/log.h>
#include <ucs/sys/string.h>
#include <ucs/sys/sys.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


#define UCT_XRC_VERBS_XRCD_DIR_DEFAULT "/dev/shm"


static uct_rc_iface_ops_t uct_xrc_verbs_iface_ops;
static uct_iface_ops_t    uct_xrc_verbs_iface_tl_ops;


static ucs_config_field_t uct_xrc_verbs_iface_config_table[] = {
  {"RC_", UCT_IB_SEND_OVERHEAD_DEFAULT(UCT_XRC_VERBS_IFACE_OVERHEAD), NULL,
   ucs_offsetof(uct_xrc_verbs_iface_config_t, super),
   UCS_CONFIG_TYPE_TABLE(uct_rc_iface_config_table)},

  {"MAX_AM_HDR", "128",
   "Buffer size to reserve for active message headers. If set to 0, the\n"
   "transport will not support zero-copy active messages.",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, max_am_hdr),
   UCS_CONFIG_TYPE_MEMUNITS},

  {"TX_MAX_WR", "-1",
   "Limits the number of outstanding posted work requests. The actual limit is\n"
   "a minimum between this value and the TX queue length. -1 means no limit.",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, tx_max_wr), UCS_CONFIG_TYPE_UINT},

  {"TGT_STATE", "rtr",
   "Highest QP state requested for the XRC target (receive side) QP:\n"
   " - rtr : INIT -> RTR. Enough for RDMA WRITE and active messages.\n"
   " - sqd : additionally request the drain state.\n"
   " - sqt : additionally request the send-ready state, which is needed to\n"
   "         serve RDMA READ and atomic requests on some device families.\n"
   "An unsupported transition is not fatal, the QP stays in the last accepted\n"
   "state.",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, tgt_state),
   UCS_CONFIG_TYPE_ENUM(uct_xrc_verbs_tgt_state_names)},

  {"TGT_FALLBACK", "y",
   "Whether to request the optional XRC target QP states selected by\n"
   "TGT_STATE. \"n\" requests only the RTR state.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, tgt_fallback),
   UCS_CONFIG_TYPE_BOOL},

  {"DOMAIN_DIR", UCT_XRC_VERBS_XRCD_DIR_DEFAULT,
   "Directory for the files which back the XRC domains. With DOMAIN_SHARED=n\n"
   "each interface opens its own domain, and the file is removed as soon as the\n"
   "domain is opened. With DOMAIN_SHARED=y the file name is derived from the\n"
   "boot id, device and port, so all processes on a node share one domain.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, domain_dir),
   UCS_CONFIG_TYPE_STRING},

  {"DOMAIN_SHARED", "y",
   "Share one XRC domain between all processes of a node, by backing it with a\n"
   "file whose name depends only on the boot id, the device and the port. The\n"
   "kernel deduplicates XRC domains by file inode and reference-counts them,\n"
   "so every process on the node gets the same XRC domain number. The file is\n"
   "left in place, since a peer may open it later; it disappears with the\n"
   "tmpfs when the node is rebooted.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, domain_shared),
   UCS_CONFIG_TYPE_BOOL},

  {"SHARED_INI", "y",
   "Share one XRC initiator QP between all the endpoints which send to the same\n"
   "remote process. An XRC work request names the destination shared receive\n"
   "queue itself, so the lanes to one process can serve from a single initiator\n"
   "QP, which is the QP-count saving XRC exists for. They cannot be shared\n"
   "between two processes of one node, because an initiator is bound to the\n"
   "target QP it was connected to and it is that target QP whose protection\n"
   "domain validates an rkey. With \"n\" every endpoint gets an initiator QP of\n"
   "its own, as in the first version of this transport.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, shared_ini),
   UCS_CONFIG_TYPE_BOOL},

  {"SHARED_TGT", "y",
   "Share one XRC target QP between all the endpoints which pair with the same\n"
   "remote process. With a shared initiator QP the peer sends every lane through\n"
   "one QP, and that QP is connected to one target QP only, so a target QP per\n"
   "endpoint leaves all but one of them idle. Requires SHARED_INI=y; the\n"
   "receiving endpoint is named by a token in the immediate data.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, shared_tgt),
   UCS_CONFIG_TYPE_BOOL},

  {"INI_SCOPE", "process",
   "What one shared initiator QP serves:\n"
   " - process : the lanes to one remote process (validated default).\n"
   " - node    : the lanes to every process of a remote node. Experimental:\n"
   "             correct only if the responder validates the rkey of an RDMA or\n"
   "             atomic request against the destination SRQ of the request, not\n"
   "             against the target QP it arrived on. Size TX_MAX_WR so one\n"
   "             queue can hold all the lanes to a node, or the queue splits\n"
   "             and shared target QPs report connect conflicts.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, ini_scope),
   UCS_CONFIG_TYPE_ENUM(uct_xrc_verbs_ini_scope_names)},

  {"TGT_PD", "y",
   "Create the XRC target QP with the protection domain of this process. With\n"
   "\"n\" the target QP is created with the XRC domain alone, as the reference\n"
   "raw-verbs program does, so that its access checks can only come from the\n"
   "destination SRQ.\n",
   ucs_offsetof(uct_xrc_verbs_iface_config_t, tgt_pd),
   UCS_CONFIG_TYPE_BOOL},

  {NULL}
};

/* A send completion names the shared initiator QP and how many of its
 * operations have completed, so the endpoints are taken from the QP in the
 * order they posted in, which the send queue of a reliable connection
 * preserves. */
static uct_xrc_verbs_node_qp_t *
uct_xrc_verbs_node_qp_lookup(uct_xrc_verbs_iface_t *iface, uint32_t qpn);

static void uct_xrc_verbs_node_qp_destroy(uct_xrc_verbs_node_qp_t *nqp);

static unsigned
uct_xrc_verbs_get_tx_res_count(uct_xrc_verbs_node_qp_t *nqp,
                              struct ibv_wc *wc)
{
    return wc->wr_id - nqp->txcnt.ci;
}

static UCS_F_ALWAYS_INLINE void
uct_xrc_verbs_update_tx_res(uct_xrc_verbs_iface_t *iface,
                            uct_xrc_verbs_node_qp_t *nqp, unsigned count)
{
    uct_rc_iface_t *rc_iface = &iface->super;
    uct_xrc_verbs_ep_t *ep;
    unsigned i;

    if (ucs_unlikely(nqp->retired || nqp->stale_eps)) {
        /* The endpoints which posted these operations are gone with it, only
         * the counters and the credits of the interface have to be restored. */
        nqp->txcnt.ci += count;
        uct_rc_iface_update_reads(rc_iface);
        uct_rc_iface_add_cq_credits(rc_iface, count);
        return;
    }

    for (i = 0; i < count; ++i) {
        ep = nqp->eps[nqp->eps_mask & nqp->txcnt.ci];
        ++nqp->txcnt.ci;
        if (ucs_unlikely(ep == NULL)) {
            /* Posted by an endpoint which was destroyed before this completed:
             * the queue has to advance, there is simply nobody to tell. */
            continue;
        }

        ++ep->txcnt.ci;
        uct_rc_txqp_completion_desc(&ep->super.txqp, ep->txcnt.ci);
        uct_rc_txqp_available_add(&ep->super.txqp, 1);
        uct_rc_ep_schedule_pending(&ep->super);
    }

    uct_rc_iface_update_reads(rc_iface);
    uct_rc_iface_add_cq_credits(rc_iface, count);
}

static void uct_xrc_verbs_handle_failure(uct_ib_iface_t *ib_iface, void *arg,
                                         ucs_status_t ep_status)
{
    struct ibv_wc *wc       = arg;
    uct_rc_iface_t *iface   = ucs_derived_of(ib_iface, uct_rc_iface_t);
    uct_xrc_verbs_iface_t *iface_xrc = ucs_derived_of(ib_iface,
                                                      uct_xrc_verbs_iface_t);
    ucs_log_level_t log_lvl = UCS_LOG_LEVEL_FATAL;
    char peer_info[128]     = {};
    unsigned dest_qpn;
    uct_xrc_verbs_node_qp_t *nqp;
    uct_xrc_verbs_ep_t *ep;
    ucs_status_t status;
    unsigned count;
    struct ibv_ah_attr ah_attr;

    nqp = uct_xrc_verbs_node_qp_lookup(iface_xrc, wc->qp_num);
    if (nqp != NULL) {
        /* The operations of several endpoints are in flight on the QP. Purge
         * the ones this completion covers, from the first one affected, which
         * is the one the error is reported for. */
        count = uct_xrc_verbs_get_tx_res_count(nqp, wc);
        if (count == 0) {
            return;
        }

        ep = nqp->eps[nqp->txcnt.ci & nqp->eps_mask];
        uct_xrc_verbs_update_tx_res(iface_xrc, nqp, count);
    } else {
        /* Every send queue of this transport is an initiator QP in the list:
         * the target QPs have none, and no QP is in the QP number table. */
        ucs_debug("%s: send completion of unknown QP 0x%x: %s",
                 uct_ib_device_name(uct_ib_iface_device(ib_iface)), wc->qp_num,
                 ibv_wc_status_str(wc->status));
        return;
    }

    ucs_arbiter_group_purge(&iface->tx.arbiter, &ep->super.arb_group,
                            uct_rc_ep_arbiter_purge_internal_cb, NULL);
    uct_rc_ep_schedule_pending(&ep->super);

    if (ep->super.flags & (UCT_RC_EP_FLAG_ERR_HANDLER_INVOKED |
                           UCT_RC_EP_FLAG_FLUSH_CANCEL)) {
        goto out;
    }

    ep->super.flags |= UCT_RC_EP_FLAG_ERR_HANDLER_INVOKED;
    uct_rc_fc_restore_wnd(iface, &ep->super.fc);

    status  = uct_iface_handle_ep_err(&iface->super.super.super,
                                      &ep->super.super.super, ep_status);
    log_lvl = uct_base_iface_failure_log_level(&ib_iface->super, status,
                                               ep_status);
    status  = uct_ib_query_qp_peer_info((nqp != NULL) ? nqp->qp : ep->ini_qp,
                                       &ah_attr, &dest_qpn);
    if (status == UCS_OK) {
        uct_ib_log_dump_qp_peer_info(ib_iface, &ah_attr, dest_qpn, peer_info,
                                     sizeof(peer_info));
    }

    ucs_log(log_lvl,
            "send completion with error: %s [qpn 0x%x wrid 0x%lx "
            "vendor_err 0x%x]\n%s",
            ibv_wc_status_str(wc->status), wc->qp_num, wc->wr_id,
            wc->vendor_err, peer_info);

out:
    uct_rc_iface_arbiter_dispatch(iface);
}

static UCS_F_ALWAYS_INLINE unsigned
uct_xrc_verbs_iface_poll_tx(uct_xrc_verbs_iface_t *iface)
{
    uct_rc_iface_t *rc_iface = &iface->super;
    uct_xrc_verbs_node_qp_t *nqp;
    uint16_t count;
    int i;
    unsigned num_wcs = iface->super.super.config.tx_max_poll;
    struct ibv_wc wc[num_wcs];
    ucs_status_t status;

    if (ucs_unlikely((rc_iface->tx.cq_available <= 0) &&
                     !iface->credits_reported)) {
        iface->credits_reported = 1;
        ucs_info("%s: iface %p out of send completion credits: %u completions "
                 "of unknown queues, %u failed",
                 uct_ib_device_name(uct_ib_iface_device(&iface->super.super)),
                 iface, iface->poll_unknown, iface->poll_fail);
    }

    UCT_XRC_VERBS_IFACE_FOREACH_TXWQE(&iface->super, i, wc, num_wcs) {
        nqp = uct_xrc_verbs_node_qp_lookup(iface, wc[i].qp_num);
        if (ucs_unlikely(nqp == NULL)) {
            ++iface->poll_unknown;
        } else if (ucs_unlikely(wc[i].status != IBV_WC_SUCCESS)) {
            ++iface->poll_fail;
        }

        if (ucs_unlikely((wc[i].status != IBV_WC_SUCCESS) || (nqp == NULL))) {
            status = uct_ib_wc_to_ucs_status(wc[i].status);
            iface->super.super.ops->handle_failure(&iface->super.super, &wc[i],
                                                   status);
            continue;
        }

        count = uct_xrc_verbs_get_tx_res_count(nqp, &wc[i]);
        ucs_trace_poll("xrc_verbs iface %p tx_wc wrid 0x%lx nqp 0x%x count %d",
                       iface, wc[i].wr_id, nqp->qpn, count);

        uct_xrc_verbs_update_tx_res(iface, nqp, count);
        ucs_arbiter_dispatch(&iface->super.tx.arbiter, 1,
                             uct_rc_ep_process_pending, NULL);
        if (ucs_unlikely(nqp->retired &&
                         (nqp->txcnt.pi == nqp->txcnt.ci))) {
            uct_xrc_verbs_node_qp_destroy(nqp);
        }
    }

    return num_wcs;
}

unsigned uct_xrc_verbs_iface_poll_tx_wqes(uct_xrc_verbs_iface_t *iface)
{
    return uct_xrc_verbs_iface_poll_tx(iface);
}

static unsigned uct_xrc_verbs_iface_progress(void *arg)
{
    uct_xrc_verbs_iface_t *iface = arg;
    unsigned count;

    count = uct_xrc_verbs_iface_poll_rx(iface);
    if (!uct_rc_iface_poll_tx(&iface->super, count)) {
        return count;
    }

    return count + uct_xrc_verbs_iface_poll_tx(iface);
}

static void
uct_xrc_verbs_iface_init_inl_wrs(uct_xrc_verbs_iface_t *iface)
{
    memset(&iface->inl_am_wr, 0, sizeof(iface->inl_am_wr));
    iface->inl_am_wr.sg_list    = iface->inl_sge;
    iface->inl_am_wr.opcode     = IBV_WR_SEND;
    iface->inl_am_wr.send_flags = IBV_SEND_INLINE;

    memset(&iface->inl_rwrite_wr, 0, sizeof(iface->inl_rwrite_wr));
    iface->inl_rwrite_wr.sg_list    = iface->inl_sge;
    iface->inl_rwrite_wr.num_sge    = 1;
    iface->inl_rwrite_wr.opcode     = IBV_WR_RDMA_WRITE;
    iface->inl_rwrite_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
}

static ucs_status_t
uct_xrc_verbs_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_iface,
                                                  uct_xrc_verbs_iface_t);
    ucs_status_t status;

    status = uct_rc_iface_query(&iface->super, iface_attr,
                                iface->config.max_inline,
                                iface->config.max_inline,
                                iface->config.short_desc_size,
                                iface->config.max_send_sge - 1,
                                sizeof(uct_rc_hdr_t),
                                iface->config.max_send_sge);
    if (status != UCS_OK) {
        return status;
    }

    iface_attr->cap.flags |= UCT_IFACE_FLAG_EP_CHECK    |
                             UCT_IFACE_FLAG_EP_KEEPALIVE;
    iface_attr->latency.m += 1e-9;  /* 1 ns per each extra QP */

    /* Software overhead */
    iface_attr->overhead = UCT_XRC_VERBS_IFACE_OVERHEAD;

    /* Both the initiator and the target QP number are published. */
    iface_attr->ep_addr_len = sizeof(uct_xrc_verbs_ep_flush_addr_t);

    /* The address of the interface names the node it is on and the process it
     * belongs to, which is how an endpoint finds the shared initiator QP of its
     * peer. Where the device address leaves no room for the process, the
     * address names only the node and no endpoint will share. */
    iface_attr->iface_addr_len = iface->srqn_in_addr ?
            (uint8_t)(UCT_XRC_VERBS_IFACE_ADDR_SRQN_OFF + sizeof(uint32_t)) :
            ucs_min(iface->super.super.addr_size,
                    (uint8_t)UCT_XRC_VERBS_IFACE_ADDR_MAX);

    return UCS_OK;
}

static ucs_status_t
uct_xrc_verbs_iface_open_xrcd(uct_xrc_verbs_iface_t *iface,
                              const char *domain_dir, int shared)
{
    uct_ib_device_t *dev = uct_ib_iface_device(&iface->super.super);
    struct ibv_xrcd_init_attr xrcd_attr;
    uint64_t boot_id_high, boot_id_low;
    const char *job_id;
    ucs_status_t status;
    struct stat st;
    int fd;

    /* The XRC domain is backed by a file: the driver identifies a domain by the
     * inode behind the descriptor and reference-counts it, so every process
     * which opens the same file is given the same domain. One shared domain per
     * node is what lets a single XRC initiator QP and a single XRC receive
     * queue serve all processes of the node, so in that mode the file has to
     * stay - a peer may open it later - and it goes away with the tmpfs at the
     * next boot. A private domain needs no persistence, because an XRC
     * connection only pairs a target QP with the shared receive queue of its
     * own owner, so its file is removed as soon as the domain is opened. */
    if (domain_dir == NULL) {
        domain_dir = UCT_XRC_VERBS_XRCD_DIR_DEFAULT;
    }

    status = ucs_sys_get_boot_id(&boot_id_high, &boot_id_low);
    if (shared && (status == UCS_OK)) {
        snprintf(iface->xrcd_path, sizeof(iface->xrcd_path),
                 "%s/ucx-xrcd-%016lx%016lx-%s-%d", domain_dir,
                 (unsigned long)boot_id_high, (unsigned long)boot_id_low,
                 uct_ib_device_name(dev), iface->super.super.config.port_num);
    } else {
        job_id = getenv("SLURM_JOB_ID");
        snprintf(iface->xrcd_path, sizeof(iface->xrcd_path),
                 "%s/ucx-xrcd-%s-%s-%d-%p", domain_dir,
                 (job_id != NULL) ? job_id : "0", uct_ib_device_name(dev),
                 (int)getpid(), iface);
    }

    fd = open(iface->xrcd_path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        ucs_error("%s: failed to create XRC domain file %s: %m",
                  uct_ib_device_name(dev), iface->xrcd_path);
        return UCS_ERR_IO_ERROR;
    }

    iface->xrcd_fd = fd;
    if (fstat(fd, &st) != 0) {
        st.st_ino = 0;
    }

    memset(&xrcd_attr, 0, sizeof(xrcd_attr));
    xrcd_attr.comp_mask = IBV_XRCD_INIT_ATTR_FD | IBV_XRCD_INIT_ATTR_OFLAGS;
    xrcd_attr.fd        = iface->xrcd_fd;
    xrcd_attr.oflags    = O_CREAT;

    iface->xrcd = ibv_open_xrcd(dev->ibv_context, &xrcd_attr);
    if (iface->xrcd == NULL) {
        ucs_debug("%s: ibv_open_xrcd(%s) failed: %m",
                  uct_ib_device_name(dev), iface->xrcd_path);
        close(iface->xrcd_fd);
        iface->xrcd_fd = -1;
        if (!shared) {
            unlink(iface->xrcd_path);
        }
        return UCS_ERR_UNSUPPORTED;
    }

    if (!shared && (unlink(iface->xrcd_path) != 0)) {
        ucs_debug("failed to remove XRC domain file %s: %m", iface->xrcd_path);
    }

    /* The driver identifies a domain by the inode of its backing file, so the
     * inode is what decides whether two processes got the same domain. */
    ucs_info("%s: iface %p opened %s XRC domain %s inode %lu",
             uct_ib_device_name(dev), iface, shared ? "shared" : "private",
             iface->xrcd_path, (unsigned long)st.st_ino);
    return UCS_OK;
}

static void uct_xrc_verbs_iface_close_xrcd(uct_xrc_verbs_iface_t *iface)
{
    if (iface->xrcd != NULL) {
        ibv_close_xrcd(iface->xrcd);
        iface->xrcd = NULL;
    }

    if (iface->xrcd_fd >= 0) {
        close(iface->xrcd_fd);
        iface->xrcd_fd = -1;
    }
}

ucs_status_t
uct_xrc_verbs_iface_create_ini_qp(uct_xrc_verbs_iface_t *iface,
                                  unsigned max_send_wr,
                                  struct ibv_qp **qp_p,
                                  struct ibv_qp_cap *cap_p)
{
    uct_ib_iface_t *ib_iface = &iface->super.super;
    uct_rc_iface_t *rc_iface = &iface->super;
    struct ibv_qp_init_attr_ex attr;
    uct_ib_md_t *md          = uct_ib_iface_md(ib_iface);
    uct_ib_device_t *dev     = uct_ib_iface_device(ib_iface);
    struct ibv_qp *qp;

    memset(&attr, 0, sizeof(attr));
    attr.qp_type             = IBV_QPT_XRC_SEND;
    attr.send_cq             = ib_iface->cq[UCT_IB_DIR_TX];
    attr.recv_cq             = ib_iface->cq[UCT_IB_DIR_RX];
    attr.sq_sig_all          = !rc_iface->config.tx_moderation;
    attr.cap.max_send_wr     = (max_send_wr != 0) ? max_send_wr :
                               rc_iface->config.tx_qp_len;
    attr.cap.max_recv_wr     = 0;
    attr.cap.max_send_sge    = rc_iface->config.tx_min_sge;
    attr.cap.max_recv_sge    = 0;
    attr.cap.max_inline_data = rc_iface->config.tx_min_inline;
    attr.comp_mask           = IBV_QP_INIT_ATTR_PD;
    attr.pd                  = md->pd;

    qp = ibv_create_qp_ex(dev->ibv_context, &attr);
    if (qp == NULL) {
        /* An oversized send queue is rejected rather than clamped, and the
         * caller of a shared initiator QP lowers its request until it is
         * granted, so only the interface probe is worth an error here. */
        uct_ib_check_memlock_limit_msg(dev->ibv_context,
                                      (max_send_wr != 0) ? UCS_LOG_LEVEL_DEBUG :
                                                           UCS_LOG_LEVEL_ERROR,
                                      "ibv_create_qp_ex(XRC_SEND)");
        return UCS_ERR_IO_ERROR;
    }

    if (cap_p != NULL) {
        *cap_p = attr.cap;
    }

    *qp_p = qp;
    return UCS_OK;
}

ucs_status_t
uct_xrc_verbs_iface_create_tgt_qp(uct_xrc_verbs_iface_t *iface,
                                  struct ibv_qp **qp_p)
{
    uct_ib_iface_t *ib_iface = &iface->super.super;
    uct_ib_md_t *md          = uct_ib_iface_md(ib_iface);
    uct_ib_device_t *dev     = uct_ib_iface_device(ib_iface);
    struct ibv_qp_init_attr_ex attr;
    struct ibv_qp *qp;

    /* Preferred form: the target QP is bound to the interface XRC SRQ, so any
     * message which arrives on it is delivered to this process. */
    memset(&attr, 0, sizeof(attr));
    attr.qp_type   = IBV_QPT_XRC_RECV;
    attr.send_cq   = ib_iface->cq[UCT_IB_DIR_TX];
    attr.recv_cq   = ib_iface->cq[UCT_IB_DIR_RX];
    attr.srq       = iface->srq;
    attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
    attr.xrcd      = iface->xrcd;
    if (iface->config.tgt_pd) {
        attr.comp_mask |= IBV_QP_INIT_ATTR_PD;
        attr.pd         = md->pd;
    }

    qp = ibv_create_qp_ex(dev->ibv_context, &attr);
    if (qp == NULL) {
        /* Some stacks only accept an SRQ-less target QP, in which case the SRQ
         * is selected by the remote SRQ number carried in each work request. */
        ucs_debug("ibv_create_qp_ex(XRC_RECV) with an SRQ failed (%m), "
                  "retrying without a bound SRQ");
        memset(&attr, 0, sizeof(attr));
        attr.qp_type   = IBV_QPT_XRC_RECV;
        attr.comp_mask = IBV_QP_INIT_ATTR_XRCD;
        attr.xrcd      = iface->xrcd;

        qp = ibv_create_qp_ex(dev->ibv_context, &attr);
        if (qp == NULL) {
            uct_ib_check_memlock_limit_msg(dev->ibv_context,
                                           UCS_LOG_LEVEL_ERROR,
                                           "ibv_create_qp_ex(XRC_RECV)");
            return UCS_ERR_IO_ERROR;
        }
    }

    ++iface->num_tgt_qps;
    *qp_p = qp;
    return UCS_OK;
}

static ucs_status_t
uct_xrc_iface_verbs_init_rx(uct_rc_iface_t *rc_iface,
                            const uct_rc_iface_common_config_t *rc_config)
{
    const uct_xrc_verbs_iface_config_t *config =
            ucs_derived_of(rc_config, uct_xrc_verbs_iface_config_t);
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(rc_iface,
                                                  uct_xrc_verbs_iface_t);
    uct_ib_md_t *md              = uct_ib_iface_md(&rc_iface->super);
    uct_ib_device_t *dev         = uct_ib_iface_device(&rc_iface->super);
    struct ibv_srq_init_attr_ex srq_attr;
    ucs_status_t status;

    status = uct_xrc_verbs_iface_open_xrcd(iface, config->domain_dir,
                                           config->domain_shared);
    if (status != UCS_OK) {
        return status;
    }

    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.attr.max_wr    = config->super.super.super.rx.queue_len;
    srq_attr.attr.max_sge   = 1;
    srq_attr.attr.srq_limit = 0;
    srq_attr.srq_context    = iface;
    srq_attr.comp_mask      = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD |
                              IBV_SRQ_INIT_ATTR_CQ   | IBV_SRQ_INIT_ATTR_PD;
    srq_attr.srq_type       = IBV_SRQT_XRC;
    srq_attr.xrcd           = iface->xrcd;
    srq_attr.cq             = rc_iface->super.cq[UCT_IB_DIR_RX];
    srq_attr.pd             = md->pd;

    iface->srq = ibv_create_srq_ex(dev->ibv_context, &srq_attr);
    if (iface->srq == NULL) {
        uct_ib_check_memlock_limit_msg(dev->ibv_context, UCS_LOG_LEVEL_DEBUG,
                                       "ibv_create_srq_ex(IBV_SRQT_XRC)");
        status = UCS_ERR_UNSUPPORTED;
        goto err_close_xrcd;
    }

    if (ibv_get_srq_num(iface->srq, &iface->srq_num) != 0) {
        ucs_debug("%s: ibv_get_srq_num failed: %m", uct_ib_device_name(dev));
        status = UCS_ERR_UNSUPPORTED;
        goto err_destroy_srq;
    }

    rc_iface->rx.srq.quota = srq_attr.attr.max_wr;

    ucs_trace("%s: iface %p created XRC SRQ 0x%x with %u WRs",
              uct_ib_device_name(dev), iface, iface->srq_num,
              srq_attr.attr.max_wr);
    return UCS_OK;

err_destroy_srq:
    uct_ib_destroy_srq(iface->srq);
    iface->srq = NULL;
err_close_xrcd:
    uct_xrc_verbs_iface_close_xrcd(iface);
    return status;
}

static void uct_xrc_iface_verbs_cleanup_rx(uct_rc_iface_t *rc_iface)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(rc_iface,
                                                  uct_xrc_verbs_iface_t);

    if (iface->srq != NULL) {
        uct_ib_destroy_srq(iface->srq);
        iface->srq = NULL;
    }

    uct_xrc_verbs_iface_close_xrcd(iface);
}

static void
uct_xrc_verbs_iface_qp_cleanup(uct_rc_iface_qp_cleanup_ctx_t *rc_cleanup_ctx)
{
    uct_xrc_verbs_iface_qp_cleanup_ctx_t *cleanup_ctx =
            ucs_derived_of(rc_cleanup_ctx,
                           uct_xrc_verbs_iface_qp_cleanup_ctx_t);

    /* A shared initiator QP is owned by the interface, its endpoints only hold
     * a reference to it. */
    if (cleanup_ctx->qp != NULL) {
        uct_ib_destroy_qp(cleanup_ctx->qp);
    }
}

unsigned uct_xrc_verbs_post_recv_always(uct_xrc_verbs_iface_t *iface,
                                        unsigned max)
{
    struct ibv_recv_wr *bad_wr;
    uct_ib_recv_wr_t *wrs;
    unsigned count;
    int ret;

    wrs  = ucs_alloca(sizeof *wrs * max);

    count = uct_ib_iface_prepare_rx_wrs(&iface->super.super, &iface->super.rx.mp,
                                        wrs, max);
    if (ucs_unlikely(count == 0)) {
        return 0;
    }

    ret = ibv_post_srq_recv(iface->srq, &wrs[0].ibwr, &bad_wr);
    if (ret != 0) {
        ucs_fatal("ibv_post_srq_recv() returned %d: %m", ret);
    }

    iface->super.rx.srq.available -= count;

    return count;
}

ucs_status_t uct_xrc_verbs_iface_prepost_recvs(uct_xrc_verbs_iface_t *iface)
{
    unsigned count;

    if (iface->super.rx.srq.quota == 0) {
        return UCS_OK;
    }

    iface->super.rx.srq.available = iface->super.rx.srq.quota;
    iface->super.rx.srq.quota     = 0;
    while (iface->super.rx.srq.available > 0) {
        count = ucs_min(iface->super.rx.srq.available,
                        iface->super.super.config.rx_max_batch);
        if (uct_xrc_verbs_post_recv_always(iface, count) == 0) {
            ucs_error("iface %p: failed to post receives to the XRC SRQ",
                      iface);
            return UCS_ERR_NO_MEMORY;
        }
    }

    return UCS_OK;
}

void uct_xrc_verbs_iface_progress_enable(uct_iface_h tl_iface, unsigned flags)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(tl_iface,
                                                  uct_xrc_verbs_iface_t);

    if (flags & UCT_PROGRESS_RECV) {
        /* Ignore the return value: it is not really possible to handle it
         * here, and some receives were already pre-posted at iface creation. */
        uct_xrc_verbs_iface_prepost_recvs(iface);
    }

    uct_base_iface_progress_enable_cb(&iface->super.super.super,
                                      iface->super.progress, flags);
}

/* Endpoints which share an initiator QP share its send queue too, so the queue
 * has to be able to hold the operations of all of them at once. */
#define UCT_XRC_VERBS_NODE_QP_EPS 64

static uct_xrc_verbs_node_qp_t *
uct_xrc_verbs_node_qp_lookup(uct_xrc_verbs_iface_t *iface, uint32_t qpn)
{
    uct_xrc_verbs_node_qp_t *nqp;

    nqp = iface->last_tx_nqp;
    if ((nqp != NULL) && (nqp->qpn == qpn)) {
        return nqp;
    }

    ucs_list_for_each(nqp, &iface->node_qps, list) {
        if (nqp->qpn == qpn) {
            iface->last_tx_nqp = nqp;
            return nqp;
        }
    }

    return NULL;
}

static ucs_status_t
uct_xrc_verbs_node_qp_create(uct_xrc_verbs_iface_t *iface,
                             uct_xrc_verbs_node_qp_t **nqp_p)
{
    uct_xrc_verbs_node_qp_t *nqp;
    struct ibv_qp_cap cap;
    ucs_status_t status;
    unsigned size, max_wr;

    ++iface->nqp_alive;
    iface->nqp_peak = ucs_max(iface->nqp_peak, iface->nqp_alive);

    nqp = ucs_calloc(1, sizeof(*nqp), "xrc node qp");
    if (nqp == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    /* The endpoints which share this QP share its send queue too, so it is
     * sized for all of them at once. The device does not clamp an oversized
     * request but rejects it, so the request is lowered until it is granted,
     * and the number of endpoints the QP can serve follows from what it got. */
    max_wr = ucs_max(iface->config.tx_max_wr, 1u) * UCT_XRC_VERBS_NODE_QP_EPS;
    max_wr = ucs_min(max_wr,
                     uct_ib_iface_device(&iface->super.super)->dev_attr
                     .orig_attr.max_qp_wr);
    for (;;) {
        status = uct_xrc_verbs_iface_create_ini_qp(iface, max_wr, &nqp->qp,
                                                  &cap);
        if (status == UCS_OK) {
            break;
        }

        if (max_wr <= iface->config.tx_max_wr) {
            goto err_free;
        }

        max_wr = ucs_max(max_wr / 2, iface->config.tx_max_wr);
    }

    size        = ucs_roundup_pow2(ucs_max(cap.max_send_wr, 2u));
    ucs_info("%s: iface %p initiator QP 0x%x asked for %u send entries, "
             "device allows %u, granted %u, one endpoint takes %u, so it serves "
             "up to %u endpoints",
             uct_ib_device_name(uct_ib_iface_device(&iface->super.super)),
             iface, nqp->qpn, max_wr,
             uct_ib_iface_device(&iface->super.super)->dev_attr.orig_attr
             .max_qp_wr, cap.max_send_wr, iface->config.tx_max_wr,
             cap.max_send_wr / ucs_max(iface->config.tx_max_wr, 1u));

    nqp->max_eps     = ucs_max(cap.max_send_wr / iface->config.tx_max_wr, 1u);
    /* The depth actually granted, not the one asked for: the two differ when
     * the request had to be lowered, and the number of endpoints this queue
     * can serve is derived from the granted one. */
    nqp->max_send_wr = cap.max_send_wr;
    nqp->max_refs    = 0;
    nqp->eps = ucs_malloc(sizeof(*nqp->eps) * size, "xrc node qp eps");
    if (nqp->eps == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_destroy_qp;
    }

    nqp->iface     = iface;
    nqp->qpn       = nqp->qp->qp_num;
    nqp->eps_mask  = size - 1;
    nqp->refcount  = 0;
    nqp->connected = 0;
    nqp->retired   = 0;
    nqp->stale_eps = 0;
    nqp->inited    = 0;
    nqp->shared    = 0;
    uct_xrc_verbs_txcnt_init(&nqp->txcnt);
    uct_ib_fence_info_init(&nqp->fi);
    ucs_list_head_init(&nqp->list);
    ucs_list_head_init(&nqp->eps_alive);
    ucs_list_head_init(&nqp->srqn_fcs);
    ucs_list_add_head(&iface->node_qps, &nqp->list);

    ucs_trace("iface %p created initiator QP 0x%x with %u send WRs", iface,
              nqp->qpn, cap.max_send_wr);

    *nqp_p = nqp;
    return UCS_OK;

err_destroy_qp:
    uct_ib_destroy_qp(nqp->qp);
err_free:
    ucs_free(nqp);
    return status;
}

static void uct_xrc_verbs_node_qp_destroy(uct_xrc_verbs_node_qp_t *nqp)
{
    uct_xrc_verbs_iface_t *iface = nqp->iface;

    ucs_assertv(nqp->refcount == 0, "qpn 0x%x refcount %u", nqp->qpn,
                nqp->refcount);
    ucs_trace("iface %p destroy initiator QP 0x%x", iface, nqp->qpn);

    ucs_list_del(&nqp->list);
    if (iface->last_tx_nqp == nqp) {
        iface->last_tx_nqp = NULL;
    }

    uct_ib_destroy_qp(nqp->qp);
    ucs_free(nqp->eps);
    {
        uct_xrc_verbs_srqn_fc_t *sf, *sf_tmp;

        ucs_list_for_each_safe(sf, sf_tmp, &nqp->srqn_fcs, list) {
            ucs_list_del(&sf->list);
            ucs_free(sf);
        }
    }
    ucs_free(nqp);
}

ucs_status_t
uct_xrc_verbs_iface_get_node_qp(uct_xrc_verbs_iface_t *iface,
                                const uct_xrc_verbs_node_key_t *key,
                                uct_xrc_verbs_node_qp_t **nqp_p)
{
    uct_xrc_verbs_node_qp_t *nqp;
    ucs_status_t status;

    if (key != NULL) {
        ucs_list_for_each(nqp, &iface->node_qps, list) {
            if (!memcmp(&nqp->key, key, sizeof(*key))) {
                /* The same node was seen before. Whether its queue then
                 * refused the endpoint is a different answer from never having
                 * seen the node at all, so only a scan which matched and was
                 * not admitted is counted here. */
                if (!nqp->shared || nqp->retired ||
                    (nqp->refcount >= nqp->max_eps) ||
                    ((uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci) +
                     iface->config.tx_max_wr > nqp->max_send_wr)) {
                    ++iface->nqp_gate_full;
                    ucs_info("%s: iface %p refuse initiator QP 0x%x of a known "
                             "peer: shared %d retired %d users %u cap %u "
                             "outstanding %u granted %u one endpoint needs %u",
                             uct_ib_device_name(uct_ib_iface_device(&iface->super.super)),
                             iface, nqp->qpn, nqp->shared, nqp->retired,
                             nqp->refcount, nqp->max_eps,
                             (uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci),
                             nqp->max_send_wr, iface->config.tx_max_wr);
                }
            }
            if (nqp->shared && !nqp->retired &&
                /* What the queue has to have room for is the work of one more
                 * endpoint, not a headcount of them: a queue granted only
                 * tx_max_wr entries would otherwise serve a single endpoint
                 * however idle it is, which is what made the sharing useless.
                 * The send queue itself is the limit, and it cannot be
                 * overfilled because each endpoint holds its own credits. */
                (nqp->refcount < nqp->max_eps) &&
                ((uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci) +
                 iface->config.tx_max_wr <= nqp->max_send_wr) &&
                !memcmp(&nqp->key, key, sizeof(*key))) {
                ++nqp->refcount;
                /* The peaks have to be recorded here too. Only the creation
                 * path used to do it, and a queue is created for its first
                 * endpoint alone, so the census reported one endpoint per
                 * shared queue however many actually used it. */
                nqp->max_refs = ucs_max(nqp->max_refs, nqp->refcount);
                iface->nqp_shared_refs = ucs_max(iface->nqp_shared_refs,
                                                 nqp->refcount);
                iface->eps_shared_peak = ucs_max(iface->eps_shared_peak,
                                                 nqp->refcount);
                *nqp_p = nqp;
                return UCS_OK;
            }
        }
    }

    if (key != NULL) {
        ++iface->nqp_key_miss;
    }

    status = uct_xrc_verbs_node_qp_create(iface, &nqp);
    if (status != UCS_OK) {
        return status;
    }

    if (key != NULL) {
        nqp->shared = 1;
        ++iface->nqp_shared_created;
        iface->nqp_last_send_wr = nqp->max_send_wr;
        nqp->key    = *key;
        ucs_info("%s: iface %p initiator QP 0x%x serves lid 0x%x gid %u.%u."
                 "%u.%u.%u.%u.%u.%u srqn 0x%x",
                 uct_ib_device_name(uct_ib_iface_device(&iface->super.super)),
                 iface, nqp->qpn, key->lid,
                 key->gid.raw[8], key->gid.raw[9], key->gid.raw[10],
                 key->gid.raw[11], key->gid.raw[12], key->gid.raw[13],
                 key->gid.raw[14], key->gid.raw[15], key->srqn);
    }

    ++nqp->refcount;
    nqp->max_refs = ucs_max(nqp->max_refs, nqp->refcount);
    iface->nqp_shared_refs = ucs_max(iface->nqp_shared_refs, nqp->max_refs);
    iface->eps_shared_peak   = ucs_max(iface->eps_shared_peak, nqp->refcount);
    *nqp_p = nqp;
    return UCS_OK;
}

void uct_xrc_verbs_node_qp_release(uct_xrc_verbs_node_qp_t *nqp)
{
    ucs_assertv(nqp->refcount > 0, "qpn 0x%x", nqp->qpn);

    if (--nqp->refcount > 0) {
        return;
    }

    if (nqp->shared &&
        ((uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci) <=
         (nqp->max_send_wr / 2))) {
        /* Keep it, work outstanding or not. The work which a destroyed
         * endpoint left behind is no longer a reason to poison the queue: its
         * slots name no endpoint, so its completions are popped and skipped,
         * and putting the queue into the error state only threw away a queue
         * the node still needs and forced a replacement to be created for the
         * same peer.
         *
         * The bound is what keeps that safe. Work which never completes would
         * let the distance between the posted and the taken counter grow, and
         * the ring of endpoints which follows it cannot wrap: once the gap
         * passes half the send queue, the queue is retired and drained as
         * before, so a peer which stopped answering still costs one queue
         * rather than an assertion. */
        return;
    }

    /* Nothing is sending through it and work is still outstanding, or it is a
     * queue of one endpoint alone. Move it to error state, so that the
     * operations which are still in flight complete, and destroy it when the
     * last of their completions is taken from the completion queue, because
     * the completions of a destroyed QP would name endpoints which are gone
     * with it. */
    ucs_info("%s: iface %p retire initiator QP 0x%x: shared %d users %u "
             "outstanding %u granted %u half %u",
             uct_ib_device_name(uct_ib_iface_device(&nqp->iface->super.super)),
             nqp->iface, nqp->qpn, nqp->shared, nqp->refcount,
             (uint16_t)(nqp->txcnt.pi - nqp->txcnt.ci), nqp->max_send_wr,
             nqp->max_send_wr / 2);
    nqp->retired = 1;
    (void)uct_ib_modify_qp(nqp->qp, IBV_QPS_ERR);
    if (nqp->txcnt.pi == nqp->txcnt.ci) {
        uct_xrc_verbs_node_qp_destroy(nqp);
    }
}

static void uct_xrc_verbs_tgt_destroy(uct_xrc_verbs_iface_t *iface,
                                      uct_xrc_verbs_tgt_t *tgt)
{
    ucs_list_del(&tgt->list);
    (void)uct_ib_modify_qp(tgt->qp, IBV_QPS_ERR);
    /* A target QP has no send queue and its receive queue is the interface
     * SRQ, so it owns no completion queue entries and can go right away. */
    uct_ib_destroy_qp(tgt->qp);
    --iface->tgt_alive;
    ucs_free(tgt);
}

ucs_status_t
uct_xrc_verbs_iface_get_tgt(uct_xrc_verbs_iface_t *iface,
                            const uct_xrc_verbs_node_key_t *key,
                            uct_xrc_verbs_tgt_t **tgt_p)
{
    uct_xrc_verbs_tgt_t *tgt;
    ucs_status_t status;

    if (key != NULL) {
        ucs_list_for_each(tgt, &iface->tgts, list) {
            if (tgt->shared && !memcmp(&tgt->key, key, sizeof(*key))) {
                ++tgt->refcount;
                tgt->max_refs        = ucs_max(tgt->max_refs, tgt->refcount);
                iface->tgt_refs_peak = ucs_max(iface->tgt_refs_peak,
                                               tgt->max_refs);
                *tgt_p = tgt;
                return UCS_OK;
            }
        }
    }

    tgt = ucs_calloc(1, sizeof(*tgt), "xrc tgt qp");
    if (tgt == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    status = uct_xrc_verbs_iface_create_tgt_qp(iface, &tgt->qp);
    if (status != UCS_OK) {
        ucs_free(tgt);
        return status;
    }

    if (key != NULL) {
        tgt->key    = *key;
        tgt->shared = 1;
        ++iface->tgt_shared_created;
    }

    tgt->refcount = tgt->max_refs = 1;
    iface->tgt_refs_peak = ucs_max(iface->tgt_refs_peak, 1u);
    ++iface->tgt_alive;
    ucs_list_add_tail(&iface->tgts, &tgt->list);
    *tgt_p = tgt;
    return UCS_OK;
}

void uct_xrc_verbs_tgt_release(uct_xrc_verbs_iface_t *iface,
                               uct_xrc_verbs_tgt_t *tgt)
{
    ucs_assert(tgt->refcount > 0);
    if (--tgt->refcount > 0) {
        return;
    }

    /* The initiator of the peer process stays connected to a shared target QP
     * after the last lane which paired with it is gone, and a new lane to the
     * same process will publish it again: keep it, as the shared initiator QP
     * is kept. */
    if (tgt->shared) {
        return;
    }

    uct_xrc_verbs_tgt_destroy(iface, tgt);
}

static void uct_xrc_verbs_tgt_cleanup_all(uct_xrc_verbs_iface_t *iface)
{
    uct_xrc_verbs_tgt_t *tgt, *tmp;

    ucs_list_for_each_safe(tgt, tmp, &iface->tgts, list) {
        uct_xrc_verbs_tgt_destroy(iface, tgt);
    }
}

uint32_t uct_xrc_verbs_iface_ep_token_alloc(uct_xrc_verbs_iface_t *iface,
                                            uct_xrc_verbs_ep_t *ep)
{
    unsigned idx = ucs_ptr_array_insert(&iface->ep_table, ep);
    uint32_t gen;

    if ((idx + 1) > UCT_XRC_VERBS_EP_TOKEN_IDX_MASK) {
        ucs_ptr_array_remove(&iface->ep_table, idx);
        return 0;
    }

    gen = ++iface->ep_token_gen &
          UCS_MASK(32 - UCT_XRC_VERBS_EP_TOKEN_IDX_BITS);
    return (gen << UCT_XRC_VERBS_EP_TOKEN_IDX_BITS) | (idx + 1);
}

void uct_xrc_verbs_iface_ep_token_free(uct_xrc_verbs_iface_t *iface,
                                       uint32_t token)
{
    unsigned idx = (token & UCT_XRC_VERBS_EP_TOKEN_IDX_MASK) - 1;

    ucs_ptr_array_remove(&iface->ep_table, idx);
}

/* Resolve the token carried in the immediate data of a message to the endpoint
 * it names, checking the generation, so that a message which was in flight to
 * a destroyed endpoint is not handed to the next owner of its slot. */
static uct_xrc_verbs_ep_t *
uct_xrc_verbs_iface_lookup_token(uct_xrc_verbs_iface_t *iface, uint32_t token)
{
    unsigned idx = token & UCT_XRC_VERBS_EP_TOKEN_IDX_MASK;
    uct_xrc_verbs_ep_t *ep;

    if (idx == 0) {
        return NULL;
    }

    if (!ucs_ptr_array_lookup(&iface->ep_table, idx - 1, ep)) {
        return NULL;
    }

    return (ep->ep_token == token) ? ep : NULL;
}

/* Report what the interface ended up using, which is the measurement of the QP
 * model: with SHARED_INI=y the initiator QPs are the remote processes, and
 * every endpoint to one of them is a reference on the QP of that process. */
static void uct_xrc_verbs_iface_log_qp_usage(uct_xrc_verbs_iface_t *iface)
{
    uct_xrc_verbs_iface_t *self = iface;
    uct_xrc_verbs_node_qp_t *nqp;
    char refs[192];
    unsigned shared, total;
    size_t off;

    shared = total = 0;
    off    = 0;
    ucs_list_for_each(nqp, &self->node_qps, list) {
        if (!nqp->shared) {
            continue;
        }

        off += snprintf(refs + off, sizeof(refs) - off, "%s%u", (shared ? "," : ""),
                        nqp->max_refs);
        off  = ucs_min(off, sizeof(refs) - 1);
        total += nqp->max_refs;
        ++shared;
    }

    ucs_info("%s: iface %p ib addr_size %u iface_addr_length %u",
             uct_ib_device_name(uct_ib_iface_device(&self->super.super)), self,
             (unsigned)self->super.super.addr_size,
             (unsigned)iface->super.super.addr_size);
    ucs_info("%s: iface %p QP model: %u target QPs, %u shared initiator QPs "
             "alive / %u created (peak endpoints served by one: %u, live peak "
             "sum %u, send queue granted %u, lookups unmatched %u refused %u) "
             "(alive now: %u endpoints on %u queues, peak %u endpoints on %u "
             "queues, most endpoints ever on one shared queue %u), "
             "ep decisions: "
             "%u shared, %u declined by config, %u peer address null, %u "
             "address not unpackable, %u field not passed, %u no peer process",
             uct_ib_device_name(uct_ib_iface_device(&self->super.super)), self,
             self->num_tgt_qps, shared, self->nqp_shared_created,
             self->nqp_shared_refs, total, self->nqp_last_send_wr,
             self->nqp_key_miss, self->nqp_gate_full,
             self->eps_alive, self->nqp_alive, self->eps_peak, self->nqp_peak,
             self->eps_shared_peak,
             self->ep_shared, self->ep_priv_cfg, self->ep_priv_addr,
             self->ep_priv_unpack, self->ep_priv_mask, self->ep_priv_noproc);
    ucs_info("%s: iface %p QP model v2: ini_scope %s shared_tgt %d tgt_pd %d: "
             "%u target QPs created, %u alive, %u of them shared (most lanes "
             "on one: %u), %u initiator QPs alive, %u endpoints peak, %u "
             "connect conflicts, %u FC messages naming no endpoint",
             uct_ib_device_name(uct_ib_iface_device(&self->super.super)), self,
             uct_xrc_verbs_ini_scope_names[self->config.ini_scope],
             self->config.shared_tgt, self->config.tgt_pd, self->num_tgt_qps,
             self->tgt_alive, self->tgt_shared_created, self->tgt_refs_peak,
             self->nqp_alive, self->eps_peak, self->tgt_conflicts,
             self->fc_no_ep);
}

static void uct_xrc_verbs_node_qp_cleanup_all(uct_xrc_verbs_iface_t *iface)
{
    uct_xrc_verbs_node_qp_t *nqp, *tmp;

    ucs_list_for_each_safe(nqp, tmp, &iface->node_qps, list) {
        nqp->refcount = 0;
        (void)uct_ib_modify_qp(nqp->qp, IBV_QPS_ERR);
        uct_xrc_verbs_node_qp_destroy(nqp);
    }

    iface->last_tx_nqp = NULL;
}


static UCS_CLASS_INIT_FUNC(uct_xrc_verbs_iface_t, uct_md_h tl_md,
                           uct_worker_h worker,
                           const uct_iface_params_t *params,
                           const uct_iface_config_t *tl_config)
{
    uct_xrc_verbs_iface_config_t *config =
                    ucs_derived_of(tl_config, uct_xrc_verbs_iface_config_t);
    uct_ib_iface_config_t *ib_config    = &config->super.super.super;
    uct_ib_md_t *ib_md                  = ucs_derived_of(tl_md, uct_ib_md_t);
    uct_ib_iface_init_attr_t init_attr  = {};
    struct ibv_qp_cap qp_cap;
    ucs_status_t status;
    struct ibv_qp *qp;
    uct_rc_hdr_t *hdr;

    if (ib_md->relaxed_order_required) {
        /* Unlike rc_mlx5, a verbs transport cannot set the strong-ordering
         * WQE flag. */
        ucs_debug("%s: xrc_verbs does not support relaxed-only memory keys",
                  uct_ib_device_name(&ib_md->dev));
        return UCS_ERR_UNSUPPORTED;
    }

    memset(&self->inl_sge, 0, sizeof(self->inl_sge));
    self->srq             = NULL;
    self->xrcd            = NULL;
    self->xrcd_fd         = -1;
    self->srq_num         = 0;
    self->fc_desc         = NULL;
    self->srqn_in_addr    = 0;

    init_attr.fc_req_size           = sizeof(uct_rc_pending_req_t);
    init_attr.rx_hdr_len            = sizeof(uct_rc_hdr_t);
    init_attr.qp_type               = IBV_QPT_XRC_SEND;
    init_attr.cq_len[UCT_IB_DIR_RX] = ib_config->rx.queue_len;
    init_attr.cq_len[UCT_IB_DIR_TX] = config->super.tx_cq_len;
    init_attr.seg_size              = ib_config->seg_size;
    init_attr.xport_hdr_len         = ucs_max(sizeof(uct_rc_hdr_t),
                                              UCT_IB_RETH_LEN);
    init_attr.max_rd_atomic         = IBV_DEV_ATTR(&ib_md->dev,
                                                   max_qp_rd_atom);
    init_attr.tx_moderation         = config->super.tx_cq_moderation;
    init_attr.fc_max_wnd_size       = ib_config->rx.queue_len;
    init_attr.dev_name              = params->mode.device.dev_name;

    UCS_CLASS_CALL_SUPER_INIT(uct_rc_iface_t, &uct_xrc_verbs_iface_tl_ops,
                              &uct_xrc_verbs_iface_ops, tl_md, worker, params,
                              &config->super.super, &init_attr);

    /* The process number rides behind the device address at a fixed offset,
     * which the device address has to leave room for. */
    self->srqn_in_addr = (self->super.super.addr_size <=
                          UCT_XRC_VERBS_IFACE_ADDR_SRQN_OFF);

    self->config.tx_max_wr = ucs_min(config->tx_max_wr,
                                     self->super.config.tx_qp_len);
    if (self->config.tx_max_wr == 0) {
        ucs_error("TX_MAX_WR must be positive");
        status = UCS_ERR_INVALID_PARAM;
        goto err;
    }

    self->config.tgt_state   = config->tgt_state;
    self->config.tgt_fallback = config->tgt_fallback;
    self->config.shared_ini   = config->shared_ini;
    self->config.ini_scope    = config->ini_scope;
    self->config.tgt_pd       = config->tgt_pd;
    /* A target QP can only be shared by lanes whose peers send through one
     * initiator, which needs the peer to share initiators. The target QP is
     * keyed on the peer process under either scope: under node scope the peer
     * process still sends to all of this node through one initiator, unless
     * that queue fills and is split, which the connect conflict count shows. */
    self->config.shared_tgt   = config->shared_tgt && config->shared_ini;
    ucs_list_head_init(&self->node_qps);
    ucs_list_head_init(&self->tgts);
    ucs_ptr_array_init(&self->ep_table, "xrc_verbs_eps");
    self->ep_token_gen        = 0;
    self->tgt_alive           = 0;
    self->tgt_shared_created  = 0;
    self->tgt_refs_peak       = 0;
    self->tgt_conflicts       = 0;
    self->fc_no_ep            = 0;
    self->last_tx_nqp         = NULL;
    self->num_tgt_qps         = 0;
    self->nqp_shared_created  = 0;
    self->nqp_shared_refs     = 0;
    self->nqp_last_send_wr    = 0;
    self->nqp_key_miss        = 0;
    self->poll_unknown        = 0;
    self->eps_alive           = 0;
    self->eps_peak            = 0;
    self->nqp_alive           = 0;
    self->nqp_peak            = 0;
    self->eps_shared_peak     = 0;
    self->poll_fail           = 0;
    self->credits_reported    = 0;
    self->nqp_gate_full       = 0;
    self->ep_shared           = 0;
    self->ep_priv_cfg         = 0;
    self->ep_priv_addr        = 0;
    self->ep_priv_mask        = 0;
    self->ep_priv_unpack      = 0;
    self->super.config.tx_moderation = ucs_min(self->super.config.tx_moderation,
                                               self->config.tx_max_wr / 4);
    self->super.config.fence_mode =
            (uct_rc_fence_mode_t)config->super.super.fence_mode;
    self->super.progress     = uct_xrc_verbs_iface_progress;
    self->super.super.config.sl =
            uct_ib_iface_config_select_sl(ib_config);
    uct_ib_iface_set_reverse_sl(&self->super.super, ib_config);

    if ((config->super.super.fence_mode == UCT_RC_FENCE_MODE_WEAK) ||
        (config->super.super.fence_mode == UCT_RC_FENCE_MODE_AUTO)) {
        self->super.config.fence_mode = UCT_RC_FENCE_MODE_WEAK;
    } else if (config->super.super.fence_mode == UCT_RC_FENCE_MODE_NONE) {
        self->super.config.fence_mode = UCT_RC_FENCE_MODE_NONE;
    } else {
        ucs_error("incorrect fence value: %d", self->super.config.fence_mode);
        status = UCS_ERR_INVALID_PARAM;
        goto err;
    }

    uct_rc_am_hdr_fill(&self->am_inl_hdr.rc_hdr, 0);

    /* Configuration */
    self->config.short_desc_size = ucs_max(sizeof(uct_rc_hdr_t),
                                           config->max_am_hdr);
    self->config.short_desc_size = ucs_max(UCT_IB_MAX_ATOMIC_SIZE,
                                           self->config.short_desc_size);

    /* Create AM headers and atomic mempool */
    status = uct_iface_mpool_init(&self->super.super.super,
                                  &self->short_desc_mp,
                                  sizeof(uct_rc_iface_send_desc_t) +
                                      self->config.short_desc_size,
                                  sizeof(uct_rc_iface_send_desc_t),
                                  UCS_SYS_CACHE_LINE_SIZE,
                                  &ib_config->tx.mp,
                                  self->super.config.tx_qp_len,
                                  uct_rc_iface_send_desc_init,
                                  "xrc_verbs_short_desc");
    if (status != UCS_OK) {
        goto err;
    }

    uct_xrc_verbs_iface_init_inl_wrs(self);

    /* Check FC parameters correctness */
    status = uct_rc_init_fc_thresh(&config->super, &self->super);
    if (status != UCS_OK) {
        goto err_common_cleanup;
    }

    /* Create a dummy initiator QP in order to find out the inline and SGE
     * limits which the device grants this interface. */
    status = uct_xrc_verbs_iface_create_ini_qp(self, 0, &qp, &qp_cap);
    if (status != UCS_OK) {
        goto err_common_cleanup;
    }

    uct_ib_destroy_qp(qp);

    self->config.max_inline   = qp_cap.max_inline_data;
    self->config.max_send_sge = ucs_min(UCT_IB_MAX_IOV, qp_cap.max_send_sge);
    ucs_assertv_always(self->config.max_send_sge > 1, /* need 1 iov for header */
                       "max_send_sge %zu", self->config.max_send_sge);

    if (self->config.max_inline < sizeof(*hdr)) {
        self->fc_desc = ucs_mpool_get(&self->short_desc_mp);
        ucs_assert_always(self->fc_desc != NULL);
        hdr        = (uct_rc_hdr_t*)(self->fc_desc + 1);
        hdr->am_id = UCT_RC_EP_FC_PURE_GRANT;
    }

    /* Prepost receive descriptors, so that wireup messages of a peer which
     * connects to us can be delivered. */
    status = uct_xrc_verbs_iface_prepost_recvs(self);
    if (status != UCS_OK) {
        goto err_common_cleanup;
    }

    return UCS_OK;

err_common_cleanup:
    ucs_mpool_cleanup(&self->short_desc_mp, 1);
err:
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_xrc_verbs_iface_t)
{
    uct_base_iface_progress_disable(&self->super.super.super.super,
                                    UCT_PROGRESS_SEND | UCT_PROGRESS_RECV);

    uct_xrc_verbs_iface_log_qp_usage(self);
    uct_xrc_verbs_node_qp_cleanup_all(self);
    uct_xrc_verbs_tgt_cleanup_all(self);
    uct_rc_iface_cleanup_qps(&self->super);
    ucs_ptr_array_cleanup(&self->ep_table, 0);

    if (self->fc_desc != NULL) {
        ucs_mpool_put(self->fc_desc);
    }

    ucs_mpool_cleanup(&self->short_desc_mp, 1);
}

UCS_CLASS_DEFINE(uct_xrc_verbs_iface_t, uct_rc_iface_t);
static UCS_CLASS_DEFINE_NEW_FUNC(uct_xrc_verbs_iface_t, uct_iface_t, uct_md_h,
                                 uct_worker_h, const uct_iface_params_t*,
                                 const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_xrc_verbs_iface_t, uct_iface_t);

/* Publish the address of the device and port of this interface, which is what
 * names the node it belongs to to a peer creating an endpoint. UCP does not
 * always pass the device address of the peer to uct_ep_create(), but it always
 * passes the interface address. */
static ucs_status_t
uct_xrc_verbs_iface_get_address(uct_iface_h tl_iface, uct_iface_addr_t *iface_addr)
{
    uct_xrc_verbs_iface_t *iface     = ucs_derived_of(tl_iface,
                                                      uct_xrc_verbs_iface_t);
    uct_xrc_verbs_iface_addr_t *addr = (uct_xrc_verbs_iface_addr_t*)iface_addr;
    uint32_t srqn_be;

    if (iface->srqn_in_addr) {
        /* Clear the gap left between the device address and the process. */
        memset(addr->raw, 0, UCT_XRC_VERBS_IFACE_ADDR_SRQN_OFF +
                             sizeof(srqn_be));
    }

    uct_ib_iface_address_pack(&iface->super.super,
                              (uct_ib_address_t*)addr->raw);

    if (iface->srqn_in_addr) {
        /* The process, not only the node: see uct_xrc_verbs_node_key_t. */
        srqn_be = htonl(iface->srq_num);
        memcpy(addr->raw + UCT_XRC_VERBS_IFACE_ADDR_SRQN_OFF, &srqn_be,
               sizeof(srqn_be));
    }

    return UCS_OK;
}

static uct_iface_ops_t uct_xrc_verbs_iface_tl_ops = {
    .ep_am_short              = uct_xrc_verbs_ep_am_short,
    .ep_am_short_iov          = uct_xrc_verbs_ep_am_short_iov,
    .ep_am_bcopy              = uct_xrc_verbs_ep_am_bcopy,
    .ep_am_zcopy              = uct_xrc_verbs_ep_am_zcopy,
    .ep_put_short             = uct_xrc_verbs_ep_put_short,
    .ep_put_bcopy             = uct_xrc_verbs_ep_put_bcopy,
    .ep_put_zcopy             = uct_xrc_verbs_ep_put_zcopy,
    .ep_get_bcopy             = uct_xrc_verbs_ep_get_bcopy,
    .ep_get_zcopy             = uct_xrc_verbs_ep_get_zcopy,
    .ep_atomic_cswap64        = uct_xrc_verbs_ep_atomic_cswap64,
    .ep_atomic64_post         = uct_xrc_verbs_ep_atomic64_post,
    .ep_atomic64_fetch        = uct_xrc_verbs_ep_atomic64_fetch,
    .ep_atomic_cswap32        = (uct_ep_atomic_cswap32_func_t)
                                ucs_empty_function_return_unsupported,
    .ep_atomic32_post         = (uct_ep_atomic32_post_func_t)
                                ucs_empty_function_return_unsupported,
    .ep_atomic32_fetch        = (uct_ep_atomic32_fetch_func_t)
                                ucs_empty_function_return_unsupported,
    .ep_pending_add           = uct_xrc_verbs_ep_pending_add,
    .ep_pending_purge         = uct_rc_ep_pending_purge,
    .ep_flush                 = uct_xrc_verbs_ep_flush,
    .ep_fence                 = uct_xrc_verbs_ep_fence,
    .ep_check                 = uct_rc_ep_check,
    .ep_create                = UCS_CLASS_NEW_FUNC_NAME(uct_xrc_verbs_ep_t),
    .ep_destroy               = UCS_CLASS_DELETE_FUNC_NAME(uct_xrc_verbs_ep_t),
    .ep_get_address           = uct_xrc_verbs_ep_get_address,
    .ep_connect_to_ep         = uct_base_ep_connect_to_ep,
    .iface_flush              = uct_rc_iface_flush,
    .iface_fence              = uct_rc_iface_fence,
    .iface_progress_enable    = uct_xrc_verbs_iface_progress_enable,
    .iface_progress_disable   = uct_base_iface_progress_disable,
    .iface_progress           = uct_rc_iface_do_progress,
    .iface_event_fd_get       = uct_ib_iface_event_fd_get,
    .iface_event_arm          = uct_rc_iface_event_arm,
    .iface_close              = UCS_CLASS_DELETE_FUNC_NAME(uct_xrc_verbs_iface_t),
    .iface_query              = uct_xrc_verbs_iface_query,
    .iface_get_address        = uct_xrc_verbs_iface_get_address,
    .iface_get_device_address = uct_ib_iface_get_device_address,
    .iface_is_reachable       = uct_base_iface_is_reachable,
};

/* Flow control in this transport carries its information in the message id byte
 * alone, so a grant names no endpoint: the generic handler attributes it to the
 * endpoint which owns the target QP the grant arrived on, restores that one
 * endpoint's window and schedules that one endpoint. It is right while one queue
 * serves one pair of endpoints. Here a queue is shared by every endpoint of this
 * process which sends to a node, so the endpoints which are not the one the grant
 * happened to reach never have their window replenished: they wait for credit
 * which will not come, without an error and without a completion.
 *
 * This handler lets the grant serve the peer it was issued for. The endpoints are
 * taken from the list which the queue keeps of the endpoints using it, which the
 * transport maintains itself: the interface's table of endpoints indexed by QP
 * number was the first idea and it crashed, because casting whatever is found
 * there to an endpoint of this transport is only sound if every entry belongs to
 * this transport and is fully constructed at that instant, and endpoints are
 * created and destroyed continuously while a run wires up and tears down.
 *
 * The credit is divided rather than given in full to each endpoint. The peer's
 * receive queue is one resource shared by all of them, so handing each a full
 * window would put several windows of traffic into it, which is not a wider flow
 * control but the overrun of it. */
static uct_xrc_verbs_srqn_fc_t *
uct_xrc_verbs_srqn_fc_find(uct_xrc_verbs_node_qp_t *nqp, uint32_t srqn)
{
    uct_xrc_verbs_srqn_fc_t *sf;

    ucs_list_for_each(sf, &nqp->srqn_fcs, list) {
        if (sf->srqn == srqn) {
            return sf;
        }
    }
    return NULL;
}

uct_xrc_verbs_srqn_fc_t *
uct_xrc_verbs_srqn_fc_get(uct_xrc_verbs_node_qp_t *nqp, uint32_t srqn)
{
    uct_xrc_verbs_srqn_fc_t *sf;

    sf = uct_xrc_verbs_srqn_fc_find(nqp, srqn);
    if (sf != NULL) {
        ++sf->num_eps;
        return sf;
    }

    sf = ucs_malloc(sizeof(*sf), "xrc srqn fc");
    if (sf == NULL) {
        return NULL;
    }
    sf->srqn         = srqn;
    sf->fc_wnd       = nqp->iface->super.config.fc_wnd_size;
    sf->fc_solicited = 0;
    sf->num_eps      = 1;
    ucs_list_add_tail(&nqp->srqn_fcs, &sf->list);
    return sf;
}

void uct_xrc_verbs_srqn_fc_put(uct_xrc_verbs_srqn_fc_t *srqn_fc)
{
    ucs_assertv(srqn_fc->num_eps > 0, "srqn %u num_eps %u",
                srqn_fc->srqn, srqn_fc->num_eps);
    if (--srqn_fc->num_eps > 0) {
        return;
    }
    ucs_list_del(&srqn_fc->list);
    ucs_free(srqn_fc);
}

/* Answer a credit request of the peer paired with @a self by a pure grant,
 * queued at the head of its pending queue if it cannot be sent now. */
static ucs_status_t
uct_xrc_verbs_iface_fc_grant(uct_rc_iface_t *rc_iface, uct_xrc_verbs_ep_t *self)
{
    uct_rc_pending_req_t *fc_req;
    ucs_status_t status;

    fc_req = ucs_mpool_get(&rc_iface->tx.pending_mp);
    if (ucs_unlikely(fc_req == NULL)) {
        ucs_error("xrc ep %p: failed to allocate FC request", self);
        return UCS_ERR_NO_MEMORY;
    }

    fc_req->ep         = &self->super.super.super;
    fc_req->super.func = uct_rc_ep_fc_grant;
    status             = uct_rc_ep_fc_grant(&fc_req->super);
    if (status == UCS_ERR_NO_RESOURCE) {
        uct_pending_req_arb_group_push_head(&self->super.arb_group,
                                            &fc_req->super);
        ucs_arbiter_group_schedule(&rc_iface->tx.arbiter,
                                   &self->super.arb_group);
    } else if (status != UCS_OK) {
        ucs_mpool_put(fc_req);
    }

    return UCS_OK;
}

static ucs_status_t
uct_xrc_verbs_iface_fc_handler(uct_rc_iface_t *rc_iface, unsigned qp_num,
                              uct_rc_hdr_t *hdr, unsigned length,
                              uint32_t imm_data, uint16_t lid, unsigned flags)
{
    uct_xrc_verbs_iface_t *iface = ucs_derived_of(rc_iface,
                                                  uct_xrc_verbs_iface_t);
    uint8_t fc_hdr               = uct_rc_fc_get_fc_hdr(hdr->am_id);
    uint32_t token               = ntohl(imm_data);
    uct_xrc_verbs_ep_t *self, *other;
    uct_xrc_verbs_srqn_fc_t *sf;
    uct_rc_pending_req_t *fc_req;
    ucs_status_t status;
    unsigned granted = 0;
    int16_t cur_wnd;

    /* wc.qp_num names whichever target QP the sender's shared initiator is
     * connected to, which is shared by every lane of the sender, and may
     * belong to another process of this node. The sender names the endpoint
     * paired with it by the token that endpoint published. */
    self = uct_xrc_verbs_iface_lookup_token(iface, token);
    if (ucs_unlikely(self == NULL)) {
        ++iface->fc_no_ep;
    }

    ucs_trace("%s: iface %p flow control message on wc qp 0x%x, token 0x%x "
              "-> ep %p, hdr 0x%x",
              uct_ib_device_name(uct_ib_iface_device(&rc_iface->super)),
              rc_iface, qp_num, token, self, fc_hdr);

    if ((self != NULL) &&
        (self->super.flags & (UCT_RC_EP_FLAG_FLUSH_CANCEL |
                              UCT_RC_EP_FLAG_ERR_HANDLER_INVOKED))) {
        self = NULL;
    }

    if (fc_hdr == UCT_XRC_VERBS_FC_PURE_REQ) {
        /* Explicit request without payload. */
        return (self != NULL) ? uct_xrc_verbs_iface_fc_grant(rc_iface, self) :
                                UCS_OK;
    }

    if (self == NULL) {
        /* No endpoint here pairs with the sender any more (or yet): the flow
         * control part is lost, as in RC, but the payload must be delivered. */
        if (fc_hdr == UCT_RC_EP_FC_PURE_GRANT) {
            return UCS_OK;
        }
        return uct_iface_invoke_am(&rc_iface->super.super,
                                   hdr->am_id & ~UCT_RC_EP_FC_MASK, hdr + 1,
                                   length, flags);
    }

    /* The peer's SRQ is one resource, so a grant is credit for the window all
     * lanes to it share: refill it before the generic handler dispatches the
     * pending sends of this lane, which read it. */
    sf = (self->nqp != NULL) && self->nqp->shared ? self->srqn_fc : NULL;
    if ((fc_hdr & UCT_RC_EP_FLAG_FC_GRANT) && (sf != NULL)) {
        sf->fc_wnd       = rc_iface->config.fc_wnd_size;
        sf->fc_solicited = 0;
        ucs_list_for_each(other, &self->nqp->eps_alive, nqp_link) {
            if (other->remote_srqn == self->remote_srqn) {
                other->super.fc.fc_wnd = sf->fc_wnd;
                ++granted;
            }
        }
    }

    /* Soft/hard requests, the grant of this lane and the payload: what
     * uct_rc_iface_fc_handler() does, for the endpoint the token named rather
     * than the one the QP number table holds, since a target QP is shared. */
    status = UCS_OK;
    if (fc_hdr & UCT_RC_EP_FLAG_FC_GRANT) {
        UCS_STATS_UPDATE_COUNTER(self->super.fc.stats,
                                 UCT_RC_FC_STAT_RX_GRANT, 1);
        cur_wnd = self->super.fc.fc_wnd;
        uct_rc_fc_restore_wnd(rc_iface, &self->super.fc);
        if (cur_wnd <= 0) {
            ucs_arbiter_group_schedule(&rc_iface->tx.arbiter,
                                       &self->super.arb_group);
            uct_rc_iface_arbiter_dispatch(rc_iface);
        }
        if (fc_hdr == UCT_RC_EP_FC_PURE_GRANT) {
            UCS_STATS_UPDATE_COUNTER(self->super.fc.stats,
                                     UCT_RC_FC_STAT_RX_PURE_GRANT, 1);
            goto out_dispatch;
        }
    }

    if (fc_hdr & UCT_RC_EP_FLAG_FC_SOFT_REQ) {
        UCS_STATS_UPDATE_COUNTER(self->super.fc.stats,
                                 UCT_RC_FC_STAT_RX_SOFT_REQ, 1);
        self->super.flags |= UCT_RC_EP_FLAG_FC_GRANT;
    } else if (fc_hdr & UCT_RC_EP_FLAG_FC_HARD_REQ) {
        UCS_STATS_UPDATE_COUNTER(self->super.fc.stats,
                                 UCT_RC_FC_STAT_RX_HARD_REQ, 1);
        fc_req = ucs_mpool_get(&rc_iface->tx.pending_mp);
        if (ucs_unlikely(fc_req == NULL)) {
            ucs_error("fc_ep=%p: failed to allocate FC request", self);
            return UCS_ERR_NO_MEMORY;
        }
        fc_req->ep         = &self->super.super.super;
        fc_req->super.func = uct_rc_ep_fc_grant;
        status             = uct_rc_ep_fc_grant(&fc_req->super);
        if (status == UCS_ERR_NO_RESOURCE) {
            uct_pending_req_arb_group_push_head(&self->super.arb_group,
                                                &fc_req->super);
            ucs_arbiter_group_schedule(&rc_iface->tx.arbiter,
                                       &self->super.arb_group);
        } else if (status != UCS_OK) {
            ucs_diag("fc_ep=%p: failed to send FC grant msg: %s", self,
                     ucs_status_string(status));
        }
    }

    status = uct_iface_invoke_am(&rc_iface->super.super,
                                 hdr->am_id & ~UCT_RC_EP_FC_MASK, hdr + 1,
                                 length, flags);

out_dispatch:

    if (granted > 0) {
        ucs_list_for_each(other, &self->nqp->eps_alive, nqp_link) {
            if (other->remote_srqn == self->remote_srqn) {
                ucs_arbiter_group_schedule(&rc_iface->tx.arbiter,
                                           &other->super.arb_group);
            }
        }
        uct_rc_iface_arbiter_dispatch(rc_iface);
    }

    return status;
}

static uct_rc_iface_ops_t uct_xrc_verbs_iface_ops = {
    .super = {
        .super = {
            .iface_query_v2       = uct_iface_base_query_v2,
            .iface_estimate_perf  = uct_rc_iface_estimate_perf,
            .iface_vfs_refresh    = uct_rc_iface_vfs_refresh,
            .ep_query             = (uct_ep_query_func_t)
                                    ucs_empty_function_return_unsupported,
            .ep_invalidate        = (uct_ep_invalidate_func_t)
                                    ucs_empty_function_return_unsupported,
            .ep_connect_to_ep_v2  = uct_xrc_verbs_ep_connect_to_ep_v2,
            .iface_is_reachable_v2 = uct_ib_iface_is_reachable_v2,
            .ep_is_connected      = uct_xrc_verbs_ep_is_connected,
            .ep_get_device_ep     = (uct_ep_get_device_ep_func_t)
                                    ucs_empty_function_return_unsupported,
            .ep_outstanding_purge = (uct_ep_outstanding_purge_func_t)
                                    ucs_empty_function_return_unsupported
        },
        .create_cq      = uct_ib_verbs_create_cq,
        .destroy_cq     = uct_ib_verbs_destroy_cq,
        .event_cq       = (uct_ib_iface_event_cq_func_t)ucs_empty_function,
        .handle_failure = uct_xrc_verbs_handle_failure,
    },
    .init_rx         = uct_xrc_iface_verbs_init_rx,
    .cleanup_rx      = uct_xrc_iface_verbs_cleanup_rx,
    .fc_ctrl         = uct_xrc_verbs_ep_fc_ctrl,
    .fc_handler      = uct_xrc_verbs_iface_fc_handler,
    .cleanup_qp      = uct_xrc_verbs_iface_qp_cleanup,
    .ep_post_check   = uct_xrc_verbs_ep_post_check,
    .ep_vfs_populate = uct_xrc_verbs_ep_vfs_populate
};

/* Check that the device supports the XRC objects this transport needs: a
 * domain, an XRC shared receive queue and both XRC QP directions. */
static ucs_status_t
uct_xrc_verbs_can_create_qp(struct ibv_pd *pd, struct ibv_context *context)
{
    struct ibv_xrcd_init_attr xrcd_init_attr;
    struct ibv_srq_init_attr_ex srq_init_attr;
    struct ibv_qp_init_attr_ex qp_init_attr;
    char path[256];
    struct ibv_xrcd *xrcd;
    struct ibv_srq *srq;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    ucs_status_t status;
    int xrcd_fd;

    status = UCS_ERR_UNSUPPORTED;

    snprintf(path, sizeof(path), "%s/ucx-xrcd-probe-%d",
             UCT_XRC_VERBS_XRCD_DIR_DEFAULT, (int)getpid());
    xrcd_fd = open(path, O_CREAT | O_RDWR, 0600);
    if (xrcd_fd < 0) {
        return UCS_ERR_UNSUPPORTED;
    }

    memset(&xrcd_init_attr, 0, sizeof(xrcd_init_attr));
    xrcd_init_attr.comp_mask = IBV_XRCD_INIT_ATTR_FD | IBV_XRCD_INIT_ATTR_OFLAGS;
    xrcd_init_attr.fd        = xrcd_fd;
    xrcd_init_attr.oflags    = O_CREAT;

    xrcd = ibv_open_xrcd(context, &xrcd_init_attr);
    if (xrcd == NULL) {
        goto out_close;
    }

    cq = ibv_create_cq(context, 16, NULL, NULL, 0);
    if (cq == NULL) {
        goto out_close_xrcd;
    }

    memset(&srq_init_attr, 0, sizeof(srq_init_attr));
    srq_init_attr.attr.max_wr    = 1;
    srq_init_attr.attr.max_sge   = 1;
    srq_init_attr.comp_mask      = IBV_SRQ_INIT_ATTR_TYPE |
                                   IBV_SRQ_INIT_ATTR_XRCD |
                                   IBV_SRQ_INIT_ATTR_CQ   |
                                   IBV_SRQ_INIT_ATTR_PD;
    srq_init_attr.srq_type       = IBV_SRQT_XRC;
    srq_init_attr.xrcd           = xrcd;
    srq_init_attr.cq             = cq;
    srq_init_attr.pd             = pd;

    srq = ibv_create_srq_ex(context, &srq_init_attr);
    if (srq == NULL) {
        goto out_destroy_cq;
    }

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type            = IBV_QPT_XRC_SEND;
    qp_init_attr.send_cq            = cq;
    qp_init_attr.recv_cq            = cq;
    qp_init_attr.cap.max_send_wr    = 1;
    qp_init_attr.cap.max_recv_wr    = 0;
    qp_init_attr.cap.max_send_sge   = 1;
    qp_init_attr.cap.max_recv_sge   = 0;
    qp_init_attr.comp_mask          = IBV_QP_INIT_ATTR_PD;
    qp_init_attr.pd                 = pd;

    qp = ibv_create_qp_ex(context, &qp_init_attr);
    if (qp == NULL) {
        goto out_destroy_srq;
    }

    uct_ib_destroy_qp(qp);

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type    = IBV_QPT_XRC_RECV;
    qp_init_attr.comp_mask  = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_XRCD;
    qp_init_attr.pd         = pd;
    qp_init_attr.xrcd       = xrcd;
    qp_init_attr.srq        = srq;
    qp_init_attr.send_cq    = cq;
    qp_init_attr.recv_cq    = cq;

    qp = ibv_create_qp_ex(context, &qp_init_attr);
    if (qp == NULL) {
        goto out_destroy_srq;
    }

    uct_ib_destroy_qp(qp);
    status = UCS_OK;

out_destroy_srq:
    uct_ib_destroy_srq(srq);
out_destroy_cq:
    ibv_destroy_cq(cq);
out_close_xrcd:
    ibv_close_xrcd(xrcd);
out_close:
    close(xrcd_fd);
    unlink(path);
    return status;
}

static ucs_status_t
uct_xrc_verbs_query_tl_devices(uct_md_h md,
                               uct_tl_device_resource_t **tl_devices_p,
                               unsigned *num_tl_devices_p)
{
    uct_ib_md_t *ib_md = ucs_derived_of(md, uct_ib_md_t);
    ucs_status_t status;

    if (ib_md->relaxed_order_required) {
        ucs_debug("%s: xrc_verbs does not support relaxed-only memory keys",
                  uct_ib_device_name(&ib_md->dev));
        *tl_devices_p     = NULL;
        *num_tl_devices_p = 0;
        return UCS_OK;
    }

    /* The device does not support XRC if its objects cannot be created. */
    status = uct_xrc_verbs_can_create_qp(ib_md->pd,
                                         ib_md->dev.ibv_context);
    if (status != UCS_OK) {
        *tl_devices_p     = NULL;
        *num_tl_devices_p = 0;
        return UCS_OK;
    }

    return uct_ib_device_query_ports(&ib_md->dev, 0, tl_devices_p,
                                     num_tl_devices_p);
}

UCT_TL_DEFINE_ENTRY(&uct_ib_component, xrc_verbs,
                    uct_xrc_verbs_query_tl_devices, uct_xrc_verbs_iface_t,
                    "XRC_VERBS_", uct_xrc_verbs_iface_config_table,
                    uct_xrc_verbs_iface_config_t);
