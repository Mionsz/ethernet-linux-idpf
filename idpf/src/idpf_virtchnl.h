/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf_virtchnl.h - FreeBSD >= 15.0 IDPF VF-DPF control plane interface.
 *
 * Declares the virtchnl2 transaction ("xn") layer and every control-plane
 * message the driver sends or receives over the mailbox.
 *
 * CONVERSION NOTES (evidence class per Human Reference Guide Appendix A):
 *
 *  [FBSD15:A30-A34]  FreeBSD 15 kernel interfaces are authoritative.
 *  [IDPF:A13-A17]    Virtchnl2 / IDPF 1.0 protocol behaviour is authoritative.
 *  [LOCAL:A20-A25]   Shape of the ported idpf.h is preserved.
 *
 * Linux constructs replaced:
 *   struct completion   -> struct cv + struct mtx + an explicit done flag
 *   spinlock_t          -> struct mtx
 *   DECLARE_BITMAP      -> bitstr_t bit_decl() (<sys/bitstring.h>)
 *   struct kvec         -> struct iovec (same iov_base / iov_len shape)
 *   netdev_features_t   -> int (if_capenable mask)
 *
 * Removed entirely (no FreeBSD counterpart in scope):
 *   idpf_idc_rdma_vc_send_sync   IDC/RDMA is DELEGATED
 *   idpf_send_create_adi_msg,
 *   idpf_send_destroy_adi_msg    VDCM/MDEV is Linux-only
 *   idpf_sideband_flow_type_ena,
 *   idpf_sideband_action_ena     take ethtool types removed from idpf.h
 *   idpf_send_get_port_stats_msg CONFIG_UPLINK_PORT_STATS is Linux-only
 */

#ifndef _IDPF_VIRTCHNL_H_
#define _IDPF_VIRTCHNL_H_

#include <sys/bitstring.h>
#include <sys/condvar.h>
#include <sys/uio.h>

#include "idpf.h"

/* -----------------------------------------------------------------------
 * Transaction ring geometry and cookie layout  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC (120 * 1000)
#define IDPF_VC_XN_IDX_M                0x00FFu
#define IDPF_VC_XN_SALT_M               0xFF00u
#define IDPF_VC_XN_RING_LEN             UINT8_MAX

/*
 * Upper bound on the RSS key length the driver will accept from the control
 * plane.  Linux uses NETDEV_RSS_KEY_LEN; FreeBSD has no equivalent constant
 * outside the optional RSS framework, so the same value is stated here.
 * [FBSD15:A30] [IDPF:A13-A14]
 */
#define IDPF_MAX_RSS_KEY_LEN            52

/**
 * IDPF_STRUCT_VAR_LEN - byte size of a virtchnl2 struct with @n trailing elements
 * @type: struct type
 * @member: name of the trailing flexible array member
 * @n: number of elements
 *
 * Replaces Linux struct_size().  virtchnl2 declares its trailing arrays with
 * STRUCT_VAR_LEN, so sizeof(type) cannot be used directly.  [IDPF:A13-A14]
 */
#define IDPF_STRUCT_VAR_LEN(type, member, n) \
        (offsetof(type, member) + (size_t)(n) * sizeof(((type *)0)->member[0]))

/**
 * idpf_msecs_to_ticks - convert a millisecond timeout to callout/cv ticks
 * @msecs: timeout in milliseconds
 *
 * Never returns zero, which cv_timedwait() would treat as "wait forever".
 * [FBSD15:A33]
 */
static inline int
idpf_msecs_to_ticks(int msecs)
{
        int t = (int)(((int64_t)msecs * hz) / 1000);

        return (t > 0 ? t : 1);
}

/* -----------------------------------------------------------------------
 * enum idpf_vc_xn_state - virtchnl transaction status
 * @IDPF_VC_XN_IDLE: not expecting a reply, ready to be used
 * @IDPF_VC_XN_WAITING: expecting a reply, not yet received
 * @IDPF_VC_XN_COMPLETED_SUCCESS: reply received, buffer updated
 * @IDPF_VC_XN_COMPLETED_FAILED: reply received but carried an error
 * @IDPF_VC_XN_SHUTDOWN: object unusable, virtchnl torn down
 * @IDPF_VC_XN_ASYNC: sent asynchronously, no waiter; a callback may handle
 *                    the reply
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
enum idpf_vc_xn_state {
        IDPF_VC_XN_IDLE = 1,
        IDPF_VC_XN_WAITING,
        IDPF_VC_XN_COMPLETED_SUCCESS,
        IDPF_VC_XN_COMPLETED_FAILED,
        IDPF_VC_XN_SHUTDOWN,
        IDPF_VC_XN_ASYNC,
};

struct idpf_vc_xn;

/* Callback for asynchronous replies.  [IDPF:A13-A14] */
typedef int (*async_vc_cb)(struct idpf_adapter *, struct idpf_vc_xn *,
    const struct idpf_ctlq_msg *);

/**
 * struct idpf_vc_xn - one virtchnl transaction
 * @lock: protects every field below and is the condvar's mutex
 * @completed: signalled by the mailbox receive path when a reply lands
 * @done: guards against missed and spurious wakeups on @completed
 * @state: transaction state
 * @reply_sz: original size of the reply; may exceed reply.iov_len, in which
 *            case the copy into @reply is truncated
 * @reply: caller buffer the reply is copied into; may be zero-length
 * @async_handler: optional callback for asynchronous sends
 * @vc_op: opcode this transaction was sent with
 * @idx: ring index, encoded into the cookie
 * @salt: bumped every send, encoded into the cookie to detect stale replies
 *
 * Linux uses struct completion here; FreeBSD needs the condvar, its mutex and
 * an explicit predicate, so @done is added.  [FBSD15:A32-A33]
 */
struct idpf_vc_xn {
        struct mtx              lock;
        struct cv               completed;
        bool                    done;
        enum idpf_vc_xn_state   state;
        size_t                  reply_sz;
        struct iovec            reply;
        async_vc_cb             async_handler;
        uint32_t                vc_op;
        uint8_t                 idx;
        uint8_t                 salt;
};

/**
 * struct idpf_vc_xn_manager - transaction pool
 * @ring: backing storage and lookup table
 * @free_xn_bm: bitmap of free transactions
 * @xn_bm_lock: serialises @free_xn_bm
 * @salt: bumped on every allocation to make cookies unique
 * @active: false before init and after shutdown
 * [FBSD15:A32] [IDPF:A13-A14]
 */
struct idpf_vc_xn_manager {
        struct idpf_vc_xn ring[IDPF_VC_XN_RING_LEN];
        bitstr_t          bit_decl(free_xn_bm, IDPF_VC_XN_RING_LEN);
        struct mtx        xn_bm_lock;
        uint8_t           salt;
        bool              active;
};

/**
 * struct idpf_vc_xn_params - parameters for one transaction
 * @send_buf: payload to send
 * @recv_buf: buffer for the reply; may be zero-length with a NULL base
 * @timeout_ms: how long to wait for a reply
 * @async: do not wait for a reply; the caller's context is lost
 * @async_handler: optional reply callback for asynchronous sends.  The
 *                 recv_buf memory must not be on the stack when async is set.
 * @vc_op: virtchnl opcode to send
 * [IDPF:A13-A14]
 */
struct idpf_vc_xn_params {
        struct iovec send_buf;
        struct iovec recv_buf;
        int          timeout_ms;
        bool         async;
        async_vc_cb  async_handler;
        uint32_t     vc_op;
};

struct idpf_adapter;
struct idpf_netdev_priv;
struct idpf_vec_regs;
struct idpf_vport;
struct idpf_vport_config;
struct idpf_vport_max_q;
struct idpf_vport_user_config_data;

/* -----------------------------------------------------------------------
 * Transaction layer
 *
 * idpf_vc_xn_exec() is the one function in this file that does not use the
 * FreeBSD positive-errno convention: it returns the reply size on success and
 * a negated errno on failure, because a size of zero is a valid result.
 * Every idpf_send_*() wrapper converts that to a positive errno.
 * ----------------------------------------------------------------------- */
ssize_t idpf_vc_xn_exec(struct idpf_adapter *adapter,
                          const struct idpf_vc_xn_params *params);
void idpf_init_vc_xn_completion(struct idpf_vc_xn_manager *vcxn_mngr);
void idpf_deinit_vc_xn_completion(struct idpf_vc_xn_manager *vcxn_mngr);
void idpf_vc_xn_init(struct idpf_vc_xn_manager *vcxn_mngr);
void idpf_vc_xn_shutdown(struct idpf_vc_xn_manager *vcxn_mngr);

/* Mailbox transport */
int  idpf_init_dflt_mbx(struct idpf_adapter *adapter);
void idpf_deinit_dflt_mbx(struct idpf_adapter *adapter);
int  idpf_recv_mb_msg(struct idpf_adapter *adapter,
                        struct idpf_ctlq_info *arq);
int  idpf_send_mb_msg(struct idpf_adapter *adapter,
                        struct idpf_ctlq_info *asq, uint32_t op,
                        uint16_t msg_size, uint8_t *msg, uint16_t cookie);

/* Core bring-up and teardown */
int  idpf_vc_core_init(struct idpf_adapter *adapter);
void idpf_vc_core_deinit(struct idpf_adapter *adapter);

/* Register and queue-ID discovery */
int  idpf_get_reg_intr_vecs(struct idpf_adapter *adapter,
                              struct idpf_vec_regs *reg_vals);
int  idpf_queue_reg_init(struct idpf_vport *vport,
                           struct idpf_q_vec_rsrc *rsrc,
                           struct idpf_queue_id_reg_info *chunks);
int  idpf_vport_queue_ids_init(struct idpf_q_vec_rsrc *rsrc,
                                 struct idpf_queue_id_reg_info *chunks);
int  idpf_vport_init_queue_reg_chunks(struct idpf_vport_config *vport_config,
                                        struct virtchnl2_queue_reg_chunks *schunks);
int  idpf_get_vec_ids(struct idpf_adapter *adapter, uint16_t *vecids,
                        int num_vecids,
                        struct virtchnl2_vector_chunks *chunks);

/* Vport lifecycle */
int  idpf_vport_init(struct idpf_vport *vport,
                       struct idpf_vport_max_q *max_q);
uint32_t idpf_get_vport_id(struct idpf_vport *vport);
bool idpf_vport_is_cap_ena(struct idpf_vport *vport, uint16_t flag);
unsigned int idpf_fsteer_max_rules(struct idpf_vport *vport);
int  idpf_send_create_vport_msg(struct idpf_adapter *adapter,
                                  struct idpf_vport_max_q *max_q);
int  idpf_send_destroy_vport_msg(struct idpf_adapter *adapter,
                                   uint32_t vport_id);
int  idpf_send_enable_vport_msg(struct idpf_adapter *adapter,
                                  uint32_t vport_id);
int  idpf_send_disable_vport_msg(struct idpf_adapter *adapter,
                                   uint32_t vport_id);

/* Queue budgeting */
void idpf_vport_adjust_qs(struct idpf_vport *vport,
                            struct idpf_q_vec_rsrc *rsrc);
int  idpf_vport_alloc_max_qs(struct idpf_adapter *adapter,
                               struct idpf_vport_max_q *max_q);
void idpf_vport_dealloc_max_qs(struct idpf_adapter *adapter,
                                 struct idpf_vport_max_q *max_q);

/* Queue configuration */
int  idpf_send_config_queues_msg(struct idpf_adapter *adapter,
                                   struct idpf_q_vec_rsrc *rsrc,
                                   uint32_t vport_id);
int  idpf_send_enable_queues_msg(struct idpf_adapter *adapter,
                                   uint32_t vport_id,
                                   struct idpf_queue_id_reg_info *chunks);
int  idpf_send_disable_queues_msg(struct idpf_adapter *adapter,
                                    struct idpf_vport *vport,
                                    struct idpf_q_vec_rsrc *rsrc,
                                    struct idpf_queue_id_reg_info *chunks);
int  idpf_send_delete_queues_msg(struct idpf_adapter *adapter,
                                   struct idpf_queue_id_reg_info *chunks,
                                   uint32_t vport_id);
int  idpf_send_add_queues_msg(struct idpf_adapter *adapter,
                                struct idpf_vport_config *vport_config,
                                struct idpf_q_vec_rsrc *rsrc,
                                uint32_t vport_id);

/* Vectors */
int  idpf_vport_alloc_vec_indexes(struct idpf_vport *vport,
                                    struct idpf_q_vec_rsrc *rsrc);
int  idpf_send_alloc_vectors_msg(struct idpf_adapter *adapter,
                                   uint16_t num_vectors);
int  idpf_send_dealloc_vectors_msg(struct idpf_adapter *adapter);
int  idpf_send_map_unmap_queue_vector_msg(struct idpf_adapter *adapter,
                                            struct idpf_q_vec_rsrc *rsrc,
                                            uint32_t vport_id, bool map);

/* Filters and modes */
int  idpf_add_del_mac_filters(struct idpf_adapter *adapter,
                                struct idpf_vport_config *vport_config,
                                const uint8_t *default_mac_addr,
                                uint32_t vport_id, bool add, bool async);
int  idpf_add_del_fsteer_filters(struct idpf_adapter *adapter,
                                   struct virtchnl2_flow_rule_add_del *rule,
                                   enum virtchnl2_op opcode);
int  idpf_send_ena_dis_loopback_msg(struct idpf_adapter *adapter,
                                      uint32_t vport_id, bool loopback_ena);
int  idpf_send_set_sriov_vfs_msg(struct idpf_adapter *adapter,
                                   uint16_t num_vfs);

/*
 * idpf_set_vlan_features - toggle VLAN offloads after an if_capenable change
 * @vport: vport to reconfigure
 * @capmask: the IFCAP_* bits that changed
 *
 * netdev_features_t replaced by the FreeBSD interface capability mask.
 * [FBSD15:A30]
 */
int  idpf_set_vlan_features(struct idpf_vport *vport, int capmask);

/* Statistics and RSS */
int  idpf_send_get_stats_msg(struct idpf_netdev_priv *np,
                               struct idpf_port_stats *port_stats);
int  idpf_send_get_set_rss_key_msg(struct idpf_adapter *adapter,
                                     struct idpf_rss_data *rss_data,
                                     uint32_t vport_id, bool get);
int  idpf_send_get_set_rss_lut_msg(struct idpf_adapter *adapter,
                                     struct idpf_rss_data *rss_data,
                                     uint32_t vport_id, bool get);
int  idpf_send_get_set_rss_hash_msg(struct idpf_adapter *adapter,
                                      struct idpf_rss_data *rss_data,
                                      uint32_t vport_id, bool get);

/**
 * idpf_vport_deinit_queue_reg_chunks - release the queue register chunk array
 * @vport_config: vport configuration to clean up
 */
static inline void
idpf_vport_deinit_queue_reg_chunks(struct idpf_vport_config *vport_config)
{

        if (vport_config == NULL)
                return;

        free(vport_config->qid_reg_info.queue_chunks, M_DEVBUF);
        vport_config->qid_reg_info.queue_chunks = NULL;
        vport_config->qid_reg_info.num_chunks = 0;
}

#endif /* !_IDPF_VIRTCHNL_H_ */
