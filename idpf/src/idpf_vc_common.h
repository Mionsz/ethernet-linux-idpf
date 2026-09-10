/**
 * @file idpf_vc_common.h
 * @brief Control-plane (virtchnl2) placeholder declarations.
 *
 * Declares the named, stub-level control-plane message-processing
 * surface for the idpf VF driver. No virtchnl2 message layout, no
 * message-encode/decode logic, and no functional adminq/mailbox
 * behavior is implemented at this skeleton stage (spec.md S7 Phase A;
 * R-01: iavf/idpf control-plane mismatch caution).
 *
 * A real Virtchnl2 protocol header set is NOT currently vendored in
 * this repository (the `virtchnl` submodule referenced by spec.md's
 * Clarification Addendum #2 / S3.4 item 1 has been removed pending an
 * unspecified future expansion). Any functional use of Virtchnl2
 * message layouts requires re-adding that submodule (or an equivalent
 * source) plus the Co-Design constant-validation discipline
 * (Contract 3) applied during a later, traceable expansion -- header
 * availability alone is never a validation.
 *
 * Feature 015 (User Story 2, FR-040..FR-048): also declares the
 * abstract, mockable one-outstanding-request transport contract
 * (struct idpf_vc_transport_ops) and its received-message record, per
 * contracts/transport-adapter-contract.md. Both are front-end- and
 * iflib-agnostic (FR-080): neither this header nor its includes below
 * pull in any iflib type or taskqueue call, so this header remains
 * compilable with iflib headers absent from the include path. No real
 * (hardware-backed) transport implementation is declared or
 * authorized here -- only the interface shape; a CppUTest test double
 * (idpf/tdd_tests/) is the sole implementation permitted at this
 * feature's scope (FR-047, FR-048, FR-061).
 */
#ifndef _IDPF_VC_COMMON_H_
#define _IDPF_VC_COMMON_H_

#include <sys/param.h>
#include <sys/types.h>

#include "idpf_drv.h"
#include "idpf_osdep.h"

int	    idpf_vc_common_init(struct idpf_sc *sc);
void	idpf_vc_common_deinit(struct idpf_sc *sc);
int	    idpf_vc_glue_attach(struct idpf_sc *sc);
void	idpf_vc_glue_detach(struct idpf_sc *sc);
/*
 * VIRTCHNL2 bring-up wrappers (spec.md FR-011), each submitting through the
 * shared idpf_ctlq_xn_* transport and returning a FreeBSD errno.
 */
int	idpf_send_version(struct idpf_sc *sc);
int	idpf_get_caps(struct idpf_sc *sc);
int	idpf_create_vport(struct idpf_sc *sc);
/*
 * Detach-time teardown wrappers (FR-003): both log a warning on a non-success
 * status and return it, but the caller (idpf_if_detach) never aborts on it.
 */
int	idpf_destroy_vport(struct idpf_sc *sc);
int	idpf_reset_vf(struct idpf_sc *sc);
/**
 * @struct idpf_vc_rx
 * @brief Received-message record (Feature 015, FR-044; production doc
 *        struct idpf_vc_rx).
 *
 * Produced by struct idpf_vc_transport_ops's poll_receive() operation.
 * The `matched` flag is `false` for unsolicited/uncorrelated messages
 * -- the (not-yet-specified, later-phase) portable VC core MUST NOT
 * treat a same-opcode message as a match solely because the opcode
 * aligns; correlation is by request cookie/token only (FR-044,
 * FR-045). `payload` reuses IDPF_IOVEC (Entity 3) as its opaque
 * buffer reference and is valid only until the transport reposts the
 * underlying receive buffer (FR-043).
 */
struct idpf_vc_rx {
	bool matched; /**< false for unsolicited/uncorrelated messages (FR-044). */
	IDPF_IOVEC
		payload; /**< Opaque buffer reference; valid only until repost (FR-043). */
};

/**
 * @struct idpf_vc_transport_ops
 * @brief Abstract, mockable one-outstanding-request transport contract
 *        (Feature 015, FR-040; contracts/transport-adapter-contract.md).
 *
 * Consumed by the (not-yet-specified, later-phase) portable VC core;
 * implemented in this feature ONLY by a CppUTest test double
 * (idpf/tdd_tests/), never by a real hardware-backed transport
 * (FR-061). No iflib types and no taskqueue calls appear in this
 * interface (FR-080).
 *
 * Binding contract rules (documentation-only here; enforced by the
 * test double per FR-047/FR-048):
 *   - One outstanding request (FR-041): exactly one synchronous
 *     request may be in flight; a second submit() while one is
 *     outstanding MUST be rejected (non-zero return), not queued.
 *   - Ownership transfer (FR-042): submit() transfers no ownership of
 *     the request payload until it returns success; after success,
 *     reclaim_tx() is the only path that releases transport-owned
 *     payloads.
 *   - Buffer lifetime (FR-043): poll_receive() MUST NOT expose a
 *     receive buffer past the point the transport reposts it.
 *   - Correlation, not opcode-matching (FR-044, FR-045): replies are
 *     correlated to the active request by cookie/token; non-matching
 *     replies are delivered with idpf_vc_rx.matched == false.
 *   - Execution-context separation (FR-046): see
 *     contracts/transport-adapter-contract.md, Invariant 5.
 */
struct idpf_vc_transport_ops {
	/** start(transport): begin transport operation. Returns 0 on success. */
	int (*start)(void *transport);
	/** stop(transport): halt transport operation. */
	void (*stop)(void *transport);
	/**
	 * submit(transport, opcode, request, request_length,
	 * request_cookie): issue the one outstanding synchronous
	 * request (FR-041). Returns 0 on success; non-zero if a
	 * request is already outstanding, the send ring is full, or a
	 * reset is pending. Ownership of `request` transfers to the
	 * transport only on success (FR-042).
	 */
	int (*submit)(void *transport, uint16_t opcode, const void *request,
		      size_t request_length, uint32_t request_cookie);
	/**
	 * poll_receive(transport, messages, message_capacity,
	 * message_count): drain up to message_capacity received
	 * messages into `messages`, reporting the count delivered in
	 * `*message_count`. Returns 0 on success. Never exposes a
	 * buffer past the point it is reposted (FR-043).
	 */
	int (*poll_receive)(void *transport, struct idpf_vc_rx *messages,
			    size_t message_capacity, size_t *message_count);
	/** reclaim_tx(transport): sole release path for a payload transferred by a successful submit() (FR-042). */
	void (*reclaim_tx)(void *transport);
	/** is_reset_pending(transport): true if a reset cancels the active request and blocks new submissions. */
	bool (*is_reset_pending)(void *transport);
};

#endif /* _IDPF_VC_COMMON_H_ */
