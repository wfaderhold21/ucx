/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2025. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_MRC_IFACE_H
#define UCT_MRC_IFACE_H

#include <uct/ib/base/ib_iface.h>
#include <uct/base/uct_iface.h>
#include <ucs/datastruct/mpool.h>
#include <ucs/datastruct/arbiter.h>

#include <mrc.h>


/* Tag bit used in wr_id to distinguish PUT_BCOPY descs from PUT_ZCOPY
 * completion pointers.  Bounce-buffer descriptors are at least 4-byte
 * aligned, so bit 0 is always free to use as a flag. */
#define UCT_MRC_WR_ID_BCOPY_FLAG  1ULL

/* Maximum number of TX completions to reap in a single progress call. */
#define UCT_MRC_MAX_TX_POLL       8


/* Forward declarations */
typedef struct uct_mrc_iface        uct_mrc_iface_t;
typedef struct uct_mrc_ep           uct_mrc_ep_t;
typedef struct uct_mrc_iface_config uct_mrc_iface_config_t;


/**
 * TX bounce-buffer descriptor (placed before the payload in the pool object).
 * The lkey is set once at pool-object initialisation time.
 */
typedef struct {
    uint32_t lkey;
} uct_mrc_send_desc_t;


/**
 * Iface address advertised via iface_get_address.
 * Contains the port LID and primary GID so that the remote EP can build
 * an address-handle and drive the INIT→RTR→RTS QP state machine.
 */
typedef struct {
    uint16_t      lid;
    union ibv_gid gid;
} uct_mrc_iface_addr_t;


/**
 * EP address advertised via ep_get_address.
 * Contains only the QP number; routing is obtained from the iface address.
 */
typedef struct {
    uint32_t qp_num;
} uct_mrc_ep_addr_t;


/**
 * Wrapper used to track pending flush completions.
 * Allocated on demand; freed when the flush fires or the object is destroyed.
 */
typedef struct {
    ucs_list_link_t   list;
    uct_completion_t *comp;
} uct_mrc_flush_comp_t;


/**
 * Iface configuration (env prefix "UCX_MRC_").
 */
struct uct_mrc_iface_config {
    uct_ib_iface_config_t super;

    int      tx_queue_len;
    int      max_inline;
    int      mpr;
    int      mpr_dest;
    int      timeout;
    int      num_send_peers;
    int      num_remote_recv_peers;
    uint64_t ev_profile_id;
};


/**
 * MRC UCT iface.
 *
 * Inherits from uct_ib_iface_t (NOT uct_rc_iface_t): libmrc owns the CQ,
 * QP, and WQ; the IB base class provides PD, port, GID, pkey, async events,
 * and standard ibv_reg_mr memory registration.
 */
struct uct_mrc_iface {
    uct_ib_iface_t super;

    /* libmrc resources (iface lifetime, shared across all EPs) */
    struct mrc_context    *mrc_ctx;
    struct mrc_cq         *mrc_cq_tx;    /* TX CQ only; no RX CQ in v1 */
    struct mrc_device_attr dev_attr;      /* cached from mrc_query_device */

    /* TX resources */
    ucs_mpool_t    tx_desc_mp;            /* PUT_BCOPY bounce buffers */
    ucs_arbiter_t  tx_arbiter;            /* pending-send back-pressure */
    unsigned       tx_outstanding;        /* total signaled sends in flight */
    ucs_list_link_t flush_comp_list;      /* pending iface-level flush comps */

    /* Configuration cache */
    struct {
        int      tx_queue_len;
        int      num_send_peers;
        int      num_remote_recv_peers;
        int      mpr;
        int      mpr_dest;
        uint8_t  timeout;
        size_t   max_inline;
        uint64_t ev_profile_id;
    } config;
};


/**
 * MRC UCT endpoint.
 *
 * One per peer (per PUT lane).  Holds the libmrc QP and QP hint.
 * The hint communicates the traffic-pattern to the MRC Service at
 * RESET→INIT time.
 */
struct uct_mrc_ep {
    uct_base_ep_t       super;
    struct mrc_qp      *mrc_qp;
    struct mrc_qp_hint *mrc_hint;         /* per-EP hint (v1: one-to-one) */
    uint32_t            qp_num;
    unsigned            tx_outstanding;   /* signaled sends in flight on this EP */
    ucs_arbiter_group_t arb_group;        /* pending-send group */
    ucs_list_link_t     flush_comp_list;  /* pending EP-level flush comps */
};


UCS_CLASS_DECLARE(uct_mrc_iface_t, uct_md_h, uct_worker_h,
                  const uct_iface_params_t *, const uct_iface_config_t *);

UCS_CLASS_DECLARE(uct_mrc_ep_t, const uct_ep_params_t *);


/* Pending-send arbiter callback (called from uct_mrc_iface_progress) */
ucs_arbiter_cb_result_t
uct_mrc_ep_process_pending(ucs_arbiter_t *arbiter,
                           ucs_arbiter_group_t *group,
                           ucs_arbiter_elem_t *elem,
                           void *arg);

/* EP operations (defined in mrc_ep.c, referenced by ops tables in mrc_iface.c) */
ucs_status_t uct_mrc_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                                          const uct_device_addr_t *device_addr,
                                          const uct_ep_addr_t *ep_addr,
                                          const uct_ep_connect_to_ep_params_t *params);
int          uct_mrc_ep_is_connected(uct_ep_h tl_ep,
                                     const uct_ep_is_connected_params_t *params);
ucs_status_t uct_mrc_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr);
ucs_status_t uct_mrc_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                   unsigned length, uint64_t remote_addr,
                                   uct_rkey_t rkey);
ssize_t      uct_mrc_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                   void *arg, uint64_t remote_addr,
                                   uct_rkey_t rkey);
ucs_status_t uct_mrc_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                   size_t iovcnt, uint64_t remote_addr,
                                   uct_rkey_t rkey, uct_completion_t *comp);
ucs_status_t uct_mrc_ep_flush(uct_ep_h tl_ep, unsigned flags,
                               uct_completion_t *comp);
ucs_status_t uct_mrc_ep_pending_add(uct_ep_h tl_ep, uct_pending_req_t *req,
                                     unsigned flags);
void         uct_mrc_ep_pending_purge(uct_ep_h tl_ep,
                                      uct_pending_purge_callback_t cb,
                                      void *arg);

extern ucs_config_field_t uct_mrc_iface_config_table[];

#endif /* UCT_MRC_IFACE_H */
