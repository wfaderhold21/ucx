/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2001-2026. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCT_XRC_VERBS_H
#define UCT_XRC_VERBS_H

#include <uct/ib/rc/base/rc_iface.h>
#include <uct/ib/rc/base/rc_ep.h>
#include <ucs/datastruct/ptr_array.h>
#include <ucs/type/class.h>


/* Software overhead of the verbs-based XRC data path, in seconds. */
#define UCT_XRC_VERBS_IFACE_OVERHEAD 90e-9

/* Maximum number of endpoints (target QPs) an interface can hold. Each
 * endpoint owns one XRC initiator QP and one XRC target QP. */
#define UCT_XRC_VERBS_MAX_EPS_DEFAULT  8192


typedef struct uct_xrc_verbs_iface        uct_xrc_verbs_iface_t;
typedef struct uct_xrc_verbs_node_qp      uct_xrc_verbs_node_qp_t;
typedef struct uct_xrc_verbs_srqn_fc      uct_xrc_verbs_srqn_fc_t;
typedef struct uct_xrc_verbs_tgt          uct_xrc_verbs_tgt_t;
typedef struct uct_xrc_verbs_ep           uct_xrc_verbs_ep_t;


/**
 * Endpoint address flags.
 */
enum {
    UCT_XRC_VERBS_ADDR_HAS_ATOMIC_MR    = UCS_BIT(0),
    UCT_XRC_VERBS_ADDR_NO_ATOMIC_OFFSET = UCS_BIT(1)
};


/**
 * Reachability state of the locally-created QPs of an endpoint.
 */
enum {
    UCT_XRC_VERBS_EP_FLAG_INI_CONNECTED = UCS_BIT(0), /* initiator in RTS   */
    UCT_XRC_VERBS_EP_FLAG_TGT_CONNECTED = UCS_BIT(1)  /* target   connected */
};


/**
 * Target QP bring-up sequence.
 */
typedef enum {
    UCT_XRC_VERBS_TGT_STATE_RTR, /* INIT -> RTR (enough for RDMA WRITE)     */
    UCT_XRC_VERBS_TGT_STATE_SQD, /* INIT -> RTR -> SQD                      */
    UCT_XRC_VERBS_TGT_STATE_SQT, /* INIT -> RTR -> SQD -> SQT               */
    UCT_XRC_VERBS_TGT_STATE_LAST
} uct_xrc_verbs_tgt_state_t;


static const char *uct_xrc_verbs_tgt_state_names[] = {
    [UCT_XRC_VERBS_TGT_STATE_RTR] = "rtr",
    [UCT_XRC_VERBS_TGT_STATE_SQD] = "sqd",
    [UCT_XRC_VERBS_TGT_STATE_SQT] = "sqt",
    [UCT_XRC_VERBS_TGT_STATE_LAST] = NULL
};


/**
 * What one shared initiator QP serves.
 */
typedef enum {
    UCT_XRC_VERBS_INI_SCOPE_PROCESS, /* the lanes to one remote process     */
    UCT_XRC_VERBS_INI_SCOPE_NODE,    /* the lanes to every process of a node */
    UCT_XRC_VERBS_INI_SCOPE_LAST
} uct_xrc_verbs_ini_scope_t;


static const char *uct_xrc_verbs_ini_scope_names[] = {
    [UCT_XRC_VERBS_INI_SCOPE_PROCESS] = "process",
    [UCT_XRC_VERBS_INI_SCOPE_NODE]    = "node",
    [UCT_XRC_VERBS_INI_SCOPE_LAST]    = NULL
};


/* An endpoint token names an endpoint of the interface which issued it: the
 * slot of the endpoint in the interface table plus one in the low bits, and a
 * generation in the high bits, so that a late message for a destroyed endpoint
 * does not resolve to the endpoint which reuses its slot. Zero is no token. */
#define UCT_XRC_VERBS_EP_TOKEN_IDX_BITS 20
#define UCT_XRC_VERBS_EP_TOKEN_IDX_MASK UCS_MASK(UCT_XRC_VERBS_EP_TOKEN_IDX_BITS)


/**
 * XRC endpoint address.
 *
 * An XRC connection is asymmetric: the active side sends through an XRC
 * initiator QP, and the passive side receives on an XRC target QP which is
 * bound to the owner's XRC shared receive queue. Both QP numbers are published
 * because each side has to connect its own pair: the local initiator is
 * connected to the remote target, and the local target is connected to the
 * remote initiator.
 */
typedef struct uct_xrc_verbs_ep_addr {
    uint8_t          flags;
    uct_ib_uint24_t  tgt_qp_num; /* XRC target QP which receives our sends  */
    uct_ib_uint24_t  ini_qp_num; /* XRC initiator QP which sends to us      */
    uint32_t         srqn;       /* SRQ the remote target QP delivers to    */
    uint32_t         ep_token;   /* names this endpoint in the imm. data    */
} UCS_S_PACKED uct_xrc_verbs_ep_addr_t;


/**
 * XRC endpoint address with the atomic/flush memory key of the owner.
 */
typedef struct uct_xrc_verbs_ep_flush_addr {
    uct_xrc_verbs_ep_addr_t super;
    uint8_t                 atomic_mr_id;
    uint16_t                flush_rkey_hi;
} UCS_S_PACKED uct_xrc_verbs_ep_flush_addr_t;


/**
 * Producer/consumer counters of the initiator QP send queue.
 */
typedef struct uct_xrc_verbs_txcnt {
    uint16_t pi; /* posted work requests   */
    uint16_t ci; /* completed work requests */
} uct_xrc_verbs_txcnt_t;


/* An explicit credit request: no payload, answered with a pure grant. No
 * active message carries both request bits, so the pair is unambiguous. */
#define UCT_XRC_VERBS_FC_PURE_REQ \
    (UCT_RC_EP_FLAG_FC_HARD_REQ | UCT_RC_EP_FLAG_FC_SOFT_REQ)

/* Flow control state of one destination shared receive queue, which is one
 * remote process. Every lane which sends to that process delivers into the
 * same SRQ, so the credit is a property of the destination queue, not of any
 * one lane: the lanes share one window. The generic per-endpoint flow control
 * checks and decrements the endpoint's window, so each lane mirrors the shared
 * window into its own before a send and writes it back after, which the single
 * UCP worker thread makes safe. */
typedef struct uct_xrc_verbs_srqn_fc {
    uint32_t          srqn;
    int16_t           fc_wnd;       /* credit left for the destination SRQ   */
    int               fc_solicited; /* a grant was asked for, not yet got    */
    unsigned          num_eps;      /* lanes sharing it now                  */
    ucs_list_link_t   list;         /* entry in nqp->srqn_fcs                */
} uct_xrc_verbs_srqn_fc_t;


/**
 * Identity of the group of peers which may share an initiator QP: the NIC port
 * of the peer which the address of the peer interface points at, and the shared
 * receive queue which names the process of that node the traffic is for.
 *
 * The process belongs in the key, and not only the node, because an XRC
 * initiator is bound to the target QP it was connected to. That target QP is
 * where the responder validates an rkey against its own protection domain, and
 * where it sends acknowledgements and read responses. The lanes which send to
 * one process may therefore share a queue - their rkeys all belong to that
 * process and their traffic is served by target QPs of it - while the lanes
 * which send to two processes of one node may not, however close together
 * those processes live. Measured: with a key of the node alone, a put to a
 * process other than the owner of the connected target QP is refused by the
 * responder with a remote access error.
 */
typedef struct {
    union ibv_gid gid;   /* destination GID, when the address is global */
    uint32_t      lid;   /* destination LID, when it is not             */
    uint32_t      srqn;  /* shared receive queue of the peer process    */
} UCS_S_PACKED uct_xrc_verbs_node_key_t;


/**
 * Shared XRC initiator QP.
 *
 * One initiator QP serves every endpoint which sends to the same remote
 * process, because an XRC work request names the destination shared receive
 * queue itself (wr.qp_type.xrc.remote_srqn). A SEND is steered by that number
 * and so may reach any process the queue is bound to, but a work request is
 * not enough on its own: the QP is connected to one target QP, and the
 * responder runs an RDMA or atomic request in the context of that target QP,
 * whose protection domain validates the rkey and whose address receives the
 * acknowledgements and read responses. The key is therefore the device address
 * of the peer together with its shared receive queue, and the endpoints which
 * talk to one process share a single QP, which is the QP-count saving XRC
 * exists for. With SHARED_INI=n the key is made unique per endpoint, which
 * reproduces the one-initiator-QP-per-endpoint behaviour.
 *
 * A send completion names the QP, not the endpoint, so the endpoints are
 * recorded in post order and completed in that order, which the in-order send
 * queue of a reliable connection guarantees.
 */
typedef struct uct_xrc_verbs_node_qp {
    uct_xrc_verbs_iface_t            *iface;
    ucs_list_link_t                   list;        /* entry in iface->node_qps    */
    ucs_list_link_t                   eps_alive;   /* endpoints using it now      */
    ucs_list_link_t                   srqn_fcs;    /* destination SRQ windows     */
    struct ibv_qp                    *qp;
    uint32_t                          qpn;      /* qp->qp_num, cached          */
    uct_xrc_verbs_node_key_t          key;      /* peer NIC port and SRQ      */
    int                               shared;   /* looked up by key            */
    unsigned                          refcount;
    int                               connected; /* initiator reached RTS       */
    int                               inited;    /* initiator reached INIT      */
    int                               retired;   /* no user, draining CQEs      */
    int                               stale_eps; /* its endpoints are gone      */
    unsigned                          max_eps;   /* endpoints it can serve      */
    unsigned                          max_send_wr;/* granted send queue depth   */
    unsigned                          max_refs;  /* peak simultaneous users     */
    uct_xrc_verbs_txcnt_t             txcnt;     /* send queue of the QP       */
    uct_ib_fence_info_t               fi;        /* fence state of the QP      */
    uct_xrc_verbs_ep_t              **eps;       /* endpoints in post order    */
    uint32_t                          eps_mask;  /* ring size - 1              */
    uct_rc_iface_qp_cleanup_ctx_t     cleanup;   /* used while being destroyed */
} uct_xrc_verbs_node_qp_t;


/**
 * XRC target QP.
 *
 * A target QP is connected to exactly one remote initiator QP. With a shared
 * initiator, every lane of the peer process which sends to us goes through the
 * same initiator QP, and that QP is connected to the first target QP it learns
 * of: one target per lane leaves all but one of them idle forever. So the
 * lanes of this process which pair with one peer process share one target QP,
 * keyed exactly as the peer keys its initiator, and every lane publishes the
 * same target QP number. The receiving endpoint is not named by the target QP
 * but by the endpoint token carried in the immediate data.
 *
 * A shared target QP is kept until the interface is closed, as the shared
 * initiator QP is, because the initiator of the peer stays connected to it.
 */
struct uct_xrc_verbs_tgt {
    ucs_list_link_t           list;         /* entry in iface->tgts           */
    struct ibv_qp            *qp;
    uct_xrc_verbs_node_key_t  key;          /* peer process it receives from  */
    int                       shared;
    int                       inited;       /* reached INIT                   */
    int                       connected;    /* reached RTR                    */
    uint32_t                  dest_ini_qpn; /* remote initiator it serves     */
    unsigned                  refcount;
    unsigned                  max_refs;
};


/**
 * XRC verbs endpoint.
 */
typedef struct uct_xrc_verbs_ep {
    uct_rc_ep_t            super;
    uct_xrc_verbs_txcnt_t  txcnt;       /* operations posted on the node QP   */
    uct_ib_fence_info_t    fi;
    uct_xrc_verbs_node_qp_t *nqp;       /* shared initiator QP of the node    */
    struct ibv_qp         *ini_qp;      /* = nqp->qp, outbound operations     */
    uct_xrc_verbs_tgt_t   *tgt;         /* target QP, shared or private       */
    struct ibv_qp         *tgt_qp;      /* = tgt->qp, inbound operations      */
    uint32_t               remote_srqn; /* destination SRQ number             */
    uint32_t               remote_tgt_qpn; /* target QP of the paired peer ep */
    uint32_t               ep_token;    /* names this endpoint to its peer    */
    uint32_t               remote_ep_token; /* names the paired peer endpoint */
    ucs_list_link_t        nqp_link;     /* on the initiator QP's endpoint list */
    uct_xrc_verbs_srqn_fc_t *srqn_fc;    /* window of the destination SRQ       */
    uint8_t                conn_flags;  /* UCT_XRC_VERBS_EP_FLAG_xx           */
} uct_xrc_verbs_ep_t;


/* Context for deferred destruction of a cleaned up initiator QP. */
typedef struct {
    uct_rc_iface_qp_cleanup_ctx_t super;
    struct ibv_qp                *qp;
} uct_xrc_verbs_iface_qp_cleanup_ctx_t;


/**
 * XRC verbs interface configuration.
 */
typedef struct uct_xrc_verbs_iface_config {
    uct_rc_iface_config_t              super;
    size_t                             max_am_hdr;
    unsigned                           tx_max_wr;
    uct_xrc_verbs_tgt_state_t          tgt_state;
    unsigned                           tgt_fallback;
    char                               *domain_dir;
    int                                domain_shared;
    int                                shared_ini;
    int                                shared_tgt;
    int                                tgt_pd;
    unsigned long                      node_qp_eps;
    uct_xrc_verbs_ini_scope_t          ini_scope;
} uct_xrc_verbs_iface_config_t;


/**
 * XRC verbs interface.
 *
 * The interface owns the XRC domain and the single XRC shared receive queue
 * which every target QP of this interface is bound to, so all active messages
 * of all endpoints of the interface are delivered through one receive queue.
 */
/* Interface address: the address of the device and port of the interface, so
 * that an endpoint can tell which node its peer lives on already when it is
 * created, followed at a fixed offset by the number of the shared receive queue
 * of the interface, which names the process it belongs to. UCP does not always
 * pass the device address of the peer to uct_ep_create(), but it always passes
 * the interface address, so this is the channel both are keyed on.
 *
 * The number is at a fixed offset rather than appended because the reader is
 * given no length to work from: it is handed the address of the peer as it was
 * packed. Where a device address is too wide to leave room for it, the address
 * carries no process number at all and the endpoints of the interface use
 * initiators of their own, which is correct and merely more expensive. */
#define UCT_XRC_VERBS_IFACE_ADDR_SRQN_OFF    24
#define UCT_XRC_VERBS_IFACE_ADDR_SRQN_ABSENT UINT32_MAX
/* The srqn of an initiator key which serves every process of a node. */
#define UCT_XRC_VERBS_KEY_ANY_PROCESS        (UINT32_MAX - 1)

typedef struct {
    /* uct_ib_address_t is a variable-length placeholder type, so the bytes are
     * held here at their full width and cast when used. */
    uint8_t                 raw[32];
} UCS_S_PACKED uct_xrc_verbs_iface_addr_t;

#define UCT_XRC_VERBS_IFACE_ADDR_MAX sizeof(uct_xrc_verbs_iface_addr_t)


typedef struct uct_xrc_verbs_iface {
    uct_rc_iface_t              super;
    struct ibv_xrcd            *xrcd;
    struct ibv_srq             *srq;
    uint32_t                    srq_num;
    int                         xrcd_fd;
    char                        xrcd_path[256];
    ucs_list_link_t             node_qps;     /* shared initiator QPs          */
    uct_xrc_verbs_node_qp_t    *last_tx_nqp;  /* cache for send completions    */
    unsigned                    num_tgt_qps;  /* target QPs created, for stats */
    int                         srqn_in_addr; /* address carries the SRQ number */
    unsigned                    nqp_shared_created; /* shared QPs ever created */
    unsigned                    nqp_shared_refs;    /* peak users of one of them */
    unsigned                    nqp_last_send_wr;   /* depth of the last one     */
    unsigned                    poll_unknown;       /* completions of no queue   */
    unsigned                    eps_alive;          /* endpoints on this iface   */
    unsigned                    eps_peak;           /* most ever alive at once   */
    unsigned                    nqp_alive;          /* initiator queues alive    */
    unsigned                    nqp_peak;           /* most ever alive at once   */
    unsigned                    eps_shared_peak;    /* on one shared queue       */
    unsigned                    poll_fail;          /* failed completions        */
    int                         credits_reported;   /* the report was made       */
    unsigned                    nqp_key_miss;       /* lookups with no such node */
    unsigned                    nqp_gate_full;      /* found the node, queue full*/
    unsigned                    ep_shared;    /* eps given a shared initiator  */
    unsigned                    ep_priv_cfg;  /* eps declined by the config    */
    unsigned                    ep_priv_addr; /* peer address present but null */
    unsigned                    ep_priv_mask; /* peer address not passed at all*/
    unsigned                    ep_priv_unpack;/* eps whose address was opaque */
    unsigned                    ep_priv_noproc; /* no process in the address    */
    ucs_list_link_t             tgts;         /* target QPs                    */
    ucs_ptr_array_t             ep_table;     /* endpoints by token slot       */
    uint32_t                    ep_token_gen; /* generation of the next token  */
    unsigned                    tgt_alive;          /* target QPs alive now    */
    unsigned                    tgt_shared_created; /* shared target QPs made  */
    unsigned                    tgt_refs_peak;      /* lanes on one of them    */
    unsigned                    tgt_conflicts;      /* peer used 2 initiators  */
    unsigned                    fc_no_ep;           /* FC naming no endpoint   */
    struct ibv_send_wr          inl_am_wr;
    struct ibv_send_wr          inl_rwrite_wr;
    struct ibv_sge              inl_sge[UCT_IB_MAX_IOV];
    uct_rc_am_short_hdr_t       am_inl_hdr;
    ucs_mpool_t                 short_desc_mp;
    uct_rc_iface_send_desc_t   *fc_desc; /* used when max_inline is zero */
    struct {
        size_t                  short_desc_size;
        size_t                  max_inline;
        size_t                  max_send_sge;
        unsigned                tx_max_wr;
        uct_xrc_verbs_tgt_state_t tgt_state;
        unsigned                tgt_fallback;
        int                     domain_shared;
        int                     shared_ini;
        int                     shared_tgt;
        int                     tgt_pd;
        unsigned                node_qp_eps;
        uct_xrc_verbs_ini_scope_t ini_scope;
    } config;
} uct_xrc_verbs_iface_t;


/* Look up the shared initiator QP of the peer process which @a key names,
 * create it if this is the first endpoint to that process, and take a
 * reference.
 * With SHARED_INI=n, @a key is ignored and a QP of its own is given to every
 * endpoint. @a key must be unique per endpoint in that case. */
ucs_status_t
uct_xrc_verbs_iface_get_node_qp(uct_xrc_verbs_iface_t *iface,
                                const uct_xrc_verbs_node_key_t *key,
                                uct_xrc_verbs_node_qp_t **nqp_p);

/* Drop a reference to a shared initiator QP, destroy it when unused. */
void uct_xrc_verbs_node_qp_release(uct_xrc_verbs_node_qp_t *nqp);


/* Find or create the flow control window of the destination SRQ @a srqn on
 * @a nqp, and take a reference. Returns NULL on allocation failure. */
uct_xrc_verbs_srqn_fc_t *
uct_xrc_verbs_srqn_fc_get(uct_xrc_verbs_node_qp_t *nqp, uint32_t srqn);

/* Drop a reference to a destination SRQ window; destroy it when the last lane
 * which shared it leaves. */
void uct_xrc_verbs_srqn_fc_put(uct_xrc_verbs_srqn_fc_t *srqn_fc);


UCS_CLASS_DECLARE(uct_xrc_verbs_ep_t, const uct_ep_params_t *);
UCS_CLASS_DECLARE_NEW_FUNC(uct_xrc_verbs_ep_t, uct_ep_t,
                           const uct_ep_params_t *);
UCS_CLASS_DECLARE_DELETE_FUNC(uct_xrc_verbs_ep_t, uct_ep_t);


void uct_xrc_verbs_txcnt_init(uct_xrc_verbs_txcnt_t *txcnt);


/* Create an XRC initiator (send side) QP on the interface's completion
 * queues, with a send queue of @a max_send_wr work requests (the interface
 * default when zero). When @a cap_p is not NULL, the granted queue
 * capabilities are returned there. */
ucs_status_t
uct_xrc_verbs_iface_create_ini_qp(uct_xrc_verbs_iface_t *iface,
                                  unsigned max_send_wr,
                                  struct ibv_qp **qp_p,
                                  struct ibv_qp_cap *cap_p);

/* Take the completed send work requests of the interface off its send
 * completion queue. Used to retire an endpoint whose operations are still in
 * flight on a shared initiator QP. */
unsigned uct_xrc_verbs_iface_poll_tx_wqes(uct_xrc_verbs_iface_t *iface);

/* Create an XRC target (receive side) QP bound to the interface SRQ. */
ucs_status_t
uct_xrc_verbs_iface_create_tgt_qp(uct_xrc_verbs_iface_t *iface,
                                  struct ibv_qp **qp_p);

/* Look up the target QP which receives from the peer process @a key names,
 * create it if this is the first lane paired with that process, and take a
 * reference. A NULL @a key gives the endpoint a target QP of its own. */
ucs_status_t
uct_xrc_verbs_iface_get_tgt(uct_xrc_verbs_iface_t *iface,
                            const uct_xrc_verbs_node_key_t *key,
                            uct_xrc_verbs_tgt_t **tgt_p);

/* Drop a reference to a target QP. A private one is destroyed with its last
 * user, a shared one is kept until the interface is closed. */
void uct_xrc_verbs_tgt_release(uct_xrc_verbs_iface_t *iface,
                               uct_xrc_verbs_tgt_t *tgt);

/* Issue the token which names @a ep to its peer, 0 on failure. */
uint32_t uct_xrc_verbs_iface_ep_token_alloc(uct_xrc_verbs_iface_t *iface,
                                            uct_xrc_verbs_ep_t *ep);

void uct_xrc_verbs_iface_ep_token_free(uct_xrc_verbs_iface_t *iface,
                                       uint32_t token);

ucs_status_t uct_xrc_verbs_iface_prepost_recvs(uct_xrc_verbs_iface_t *iface);

unsigned uct_xrc_verbs_post_recv_always(uct_xrc_verbs_iface_t *iface,
                                        unsigned max);

void uct_xrc_verbs_iface_progress_enable(uct_iface_h tl_iface, unsigned flags);


ucs_status_t uct_xrc_verbs_ep_put_short(uct_ep_h tl_ep, const void *buffer,
                                        unsigned length, uint64_t remote_addr,
                                        uct_rkey_t rkey);

ssize_t uct_xrc_verbs_ep_put_bcopy(uct_ep_h tl_ep, uct_pack_callback_t pack_cb,
                                   void *arg, uint64_t remote_addr,
                                   uct_rkey_t rkey);

ucs_status_t uct_xrc_verbs_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                        size_t iovcnt, uint64_t remote_addr,
                                        uct_rkey_t rkey, uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_get_bcopy(uct_ep_h tl_ep,
                                        uct_unpack_callback_t unpack_cb,
                                        void *arg, size_t length,
                                        uint64_t remote_addr, uct_rkey_t rkey,
                                        uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov,
                                        size_t iovcnt, uint64_t remote_addr,
                                        uct_rkey_t rkey, uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t hdr,
                                       const void *buffer, unsigned length);

ucs_status_t uct_xrc_verbs_ep_am_short_iov(uct_ep_h tl_ep, uint8_t id,
                                           const uct_iov_t *iov,
                                           size_t iovcnt);

ssize_t uct_xrc_verbs_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                  uct_pack_callback_t pack_cb, void *arg,
                                  unsigned flags);

ucs_status_t uct_xrc_verbs_ep_am_zcopy(uct_ep_h tl_ep, uint8_t id,
                                       const void *header,
                                       unsigned header_length,
                                       const uct_iov_t *iov, size_t iovcnt,
                                       unsigned flags, uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_atomic_cswap64(uct_ep_h tl_ep, uint64_t compare,
                                             uint64_t swap, uint64_t remote_addr,
                                             uct_rkey_t rkey, uint64_t *result,
                                             uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_atomic64_post(uct_ep_h tl_ep, unsigned opcode,
                                            uint64_t value, uint64_t remote_addr,
                                            uct_rkey_t rkey);

ucs_status_t uct_xrc_verbs_ep_atomic64_fetch(uct_ep_h tl_ep,
                                             uct_atomic_op_t opcode,
                                             uint64_t value, uint64_t *result,
                                             uint64_t remote_addr,
                                             uct_rkey_t rkey,
                                             uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_flush(uct_ep_h tl_ep, unsigned flags,
                                    uct_completion_t *comp);

ucs_status_t uct_xrc_verbs_ep_fence(uct_ep_h tl_ep, unsigned flags);

ucs_status_t uct_xrc_verbs_ep_post_check(uct_ep_h tl_ep,
                                         uct_completion_t *comp);

void uct_xrc_verbs_ep_vfs_populate(uct_rc_ep_t *rc_ep);

ucs_status_t uct_xrc_verbs_ep_pending_add(uct_ep_h tl_ep,
                                          uct_pending_req_t *n, unsigned flags);

ucs_status_t uct_xrc_verbs_ep_fc_ctrl(uct_ep_t *tl_ep, unsigned op,
                                      uct_rc_pending_req_t *req);

ucs_status_t uct_xrc_verbs_ep_get_address(uct_ep_h tl_ep, uct_ep_addr_t *addr);

int uct_xrc_verbs_ep_is_connected(const uct_ep_h tl_ep,
                                  const uct_ep_is_connected_params_t *params);

ucs_status_t
uct_xrc_verbs_ep_connect_to_ep_v2(uct_ep_h tl_ep,
                                  const uct_device_addr_t *dev_addr,
                                  const uct_ep_addr_t *ep_addr,
                                  const uct_ep_connect_to_ep_params_t *param);

#endif /* UCT_XRC_VERBS_H */
