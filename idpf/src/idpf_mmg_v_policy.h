/**
 * @file idpf_mmg_v_policy.h
 * @brief Single source of the MMG_V (Morganville) MVP bring-up policy constants.
 *
 * Feature 001 (MMG_V attach/detach), FR-010 / data-model.md §1. This header
 * holds only compile-time constants — no functions, no mutable state — and is
 * OS-agnostic (no kernel or FreeBSD headers). Every MMG_V (Morganville) MVP policy value
 * (hard-coded VIRTCHNL2 version, queue-count options/default, the 1 TX + 1 RX
 * attach request, the 1-vector count, and the masked-off capability request
 * set) MUST be referenced from here, not redefined at call sites.
 */

#ifndef _IDPF_MMG_V_POLICY_H_
#define _IDPF_MMG_V_POLICY_H_

/**
 * @brief Hard-coded VIRTCHNL2 protocol version for this MVP.
 *
 * Values mirror `VIRTCHNL2_VERSION_MAJOR_2` (2) / `VIRTCHNL2_VERSION_MINOR_0`
 * (0) from the shared `virtchnl2.h` (EV-002). A CP-reported major mismatch
 * fails attach closed (FR-004); a minor mismatch warns and continues.
 */
#define IDPF_MMG_V_VC_VERSION_MAJOR	2U
#define IDPF_MMG_V_VC_VERSION_MINOR	0U

/**
 * @brief Documented queue-count option set and the MVP default.
 *
 * `{2, 4, 8}` is the documented option set (spec.md Scope Classification);
 * this MVP defaults to 2. Not all options are individually exercised here.
 */
#define IDPF_MMG_V_QUEUE_COUNT_OPTION_LOW	2U
#define IDPF_MMG_V_QUEUE_COUNT_OPTION_MID	4U
#define IDPF_MMG_V_QUEUE_COUNT_OPTION_HIGH	8U
#define IDPF_MMG_V_QUEUE_COUNT_DEFAULT		2U

/**
 * @brief Attach-time queue and vector request for this MVP (FR-006, FR-007).
 *
 * Exactly one TX and one RX queue are requested via `OP_CREATE_VPORT`. The
 * single-vector count is recorded only; no `OP_ALLOC_VECTORS` is issued this
 * story (FR-007 exclusion) — the mailbox vector default from `OP_GET_CAPS` is
 * relied upon instead.
 */
#define IDPF_MMG_V_ATTACH_NUM_TXQ	1U
#define IDPF_MMG_V_ATTACH_NUM_RXQ	1U
#define IDPF_MMG_V_ATTACH_NUM_VECTORS	1U

/**
 * @brief VIRTCHNL2 transaction timing (FR-016).
 *
 * A synchronous ControlQ transaction waits up to
 * `IDPF_MMG_V_VC_TIMEOUT_MS` per attempt. `idpf_send_version()` retries a
 * failed (no-response) send at least `IDPF_MMG_V_VERSION_RETRY_MAX` times with
 * an `IDPF_MMG_V_VERSION_RETRY_MS` gap before failing attach; a protocol major
 * mismatch fails immediately and does not consume retries.
 */
#define IDPF_MMG_V_VC_TIMEOUT_MS	200U
#define IDPF_MMG_V_VERSION_RETRY_MAX	10U
#define IDPF_MMG_V_VERSION_RETRY_MS	20U

/**
 * @brief Upper bound on OP_CREATE_VPORT queue register chunks (FR-020).
 *
 * A single-queue-model 1 TX + 1 RX vport reports only a handful of chunks; a
 * response claiming zero or more than this is rejected as malformed.
 */
#define IDPF_MMG_V_MAX_QUEUE_CHUNKS	8U

/**
 * @brief GET_CAPS capability-request bitmasks (FR-005, FR-010).
 *
 * The MVP requests nothing beyond the default vport/queue floor: every
 * optional capability class is masked off (0).
 */
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_CSUM	0U
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_SEG	0U
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_HSPLIT	0U
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_RSC	0U
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_RSS	0U
#define IDPF_MMG_V_GET_CAPS_REQUEST_MASK_OTHER	0U
/**
 * @brief Default VF mailbox (ControlQ) ring depth (FR-010).
 *
 * Descriptor count for each of the ATQ (TX) and ARQ (RX) mailbox queues
 * programmed by idpf_init_controlq(). Matches the IDPF/iecm default depth.
 */
#define IDPF_MMG_V_MBX_Q_LEN            64U

/**
 * @brief VF reset (VFGEN_RSTAT.VFR_STATE) acceptance states (FR-018).
 *
 * Attach may proceed once the VF reset has reached Completed (01b) or
 * VF-Active (10b); In-Progress (00b) means keep polling.
 */
#define IDPF_MMG_V_VFR_STATE_COMPLETED  1U
#define IDPF_MMG_V_VFR_STATE_ACTIVE     2U

/**
 * @brief Bounded VF-reset-completion poll budget (FR-018).
 *
 * idpf_wait_reset_complete() polls VFGEN_RSTAT up to this many times with
 * a fixed gap, failing closed with ETIMEDOUT if the reset never completes.
 */
#define IDPF_MMG_V_RESET_POLL_MAX       100U
#define IDPF_MMG_V_RESET_POLL_MS        20U
#endif /* _IDPF_MMG_V_POLICY_H_ */
