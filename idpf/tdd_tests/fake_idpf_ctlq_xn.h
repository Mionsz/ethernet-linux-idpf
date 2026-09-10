/**
 * @file fake_idpf_ctlq_xn.h
 * @brief CppUTest test double for the shared idpf_ctlq_xn_* transport
 * boundary (spec.md FR-011).
 *
 * Provides a single link-time replacement for idpf_ctlq_xn_send() so the
 * VIRTCHNL2 bring-up wrappers (idpf_send_version/get_caps/create_vport/...)
 * can be exercised without a real ControlQ. Byte-buffer based, so it serves
 * every wrapper's request/response shape.
 */
#ifndef _FAKE_IDPF_CTLQ_XN_H_
#define _FAKE_IDPF_CTLQ_XN_H_

#include <stddef.h>
#include <stdint.h>

/** Reset to defaults: rc=0, no response payload, counters cleared. */
void fake_xn_reset(void);
/** Set the value idpf_ctlq_xn_send() returns. */
void fake_xn_set_rc(int rc);
/** Set the response bytes copied into the caller's recv_buf on rc==0. */
void fake_xn_set_recv(const void *buf, size_t len);
/**
 * Register a per-opcode response. When idpf_ctlq_xn_send() sees a request
 * with this opcode, it copies these bytes into recv_buf (overriding the
 * single fake_xn_set_recv() payload). Lets one test drive a multi-message
 * flow (VERSION -> GET_CAPS -> CREATE_VPORT) with a distinct reply each.
 */
void fake_xn_set_recv_for(uint16_t opcode, const void *buf, size_t len);
/** Last request opcode seen by the fake. */
uint16_t fake_xn_last_opcode(void);
/** The idx-th request opcode seen since reset (0-based), or 0 if out of range. */
uint16_t fake_xn_opcode_at(int idx);
/** Number of idpf_ctlq_xn_send() calls since the last reset. */
int fake_xn_call_count(void);

/** Set the value the idpf_ctlq_init() double returns (default 0). */
void fake_xn_set_ctlq_init_rc(int rc);
/** Set the value the idpf_ctlq_xn_init() double returns (default 0). */
void fake_xn_set_ctlq_xn_init_rc(int rc);
/** Number of idpf_ctlq_init() double calls since reset. */
int fake_xn_ctlq_init_count(void);
/** Number of idpf_ctlq_xn_init() double calls since reset. */
int fake_xn_ctlq_xn_init_count(void);

#endif /* _FAKE_IDPF_CTLQ_XN_H_ */
