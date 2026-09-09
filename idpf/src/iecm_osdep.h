/**
 * @file iecm_osdep.h
 * @brief FreeBSD shim for the shared code's pre-rename OS-dep header name.
 *
 * IECM ("Intel Ethernet Common Module") is the former name of IDPF; the
 * vendored shared/idpf_xn.h still does #include "iecm_osdep.h". The shared
 * tree is kept unmodified, so this driver-owned header resolves that include
 * (via -I${.CURDIR}, which follows the failed lookup in shared/) and supplies
 * the list and command-completion primitives idpf_xn.c expects on top of
 * idpf_osdep.h.
 */
#ifndef _IECM_OSDEP_H_
#define _IECM_OSDEP_H_

#include <sys/systm.h>

#include "idpf_list.h"
#include "idpf_osdep.h"

/* Bitfield helpers; the mask is always a compile-time constant here. */
#define __idpf_bf_shf(x)		(__builtin_ffsll(x) - 1)
#define FIELD_PREP(_mask, _val)		(((_val) << __idpf_bf_shf(_mask)) & (_mask))
#define FIELD_GET(_mask, _reg)		(((_reg) & (_mask)) >> __idpf_bf_shf(_mask))

#if IDPF_DBG
#define IDPF_DEBUG_PRINT(...)		printf(__VA_ARGS__)
#else
#define IDPF_DEBUG_PRINT(...)		do { } while (0)
#endif

/*
 * idpf_xn.c hard-codes the list link member name as `entry`, and passes only
 * the head to IDPF_LIST_DEL (it always removes the first element).
 */
#define IDPF_LIST_ENTRY(type)		LIST_ENTRY_TYPE(type)
#define IDPF_LIST_HEAD(name, type)	LIST_HEAD_TYPE(name, type)
#define IDPF_LIST_HEAD_INIT(head)	LIST_INIT(head)
#define IDPF_LIST_EMPTY(head)		LIST_EMPTY(head)
#define IDPF_LIST_FIRST(head)		LIST_FIRST(head)
#define IDPF_LIST_ADD(head, elm)	LIST_INSERT_HEAD((head), (elm), entry)
#define IDPF_LIST_DEL(head)		LIST_REMOVE(LIST_FIRST(head), entry)

/*
 * IDPF_CMD_COMPLETION_*: no reference implementation exists -- the shared code
 * only calls these, no OS layer in the shared/UEFI trees defines them. The
 * semantics below are inferred from the idpf_xn.c call sites. Verify during
 * bring-up:
 *   - Callers of idpf_ctlq_xn_send() must be sleepable: WAIT sleeps, so it
 *     must not run from an interrupt filter or under a spin/iflib CTX lock.
 *   - timeout_ms == 0 is treated as "poll once" (clamped to 1 tick); confirm
 *     the CP contract does not mean "wait forever".
 *   - Tick rounding: at hz=100 any timeout below 10 ms collapses to 1 tick.
 *   - Lock order is xn->lock -> ev->mtx (deinit SIGs while holding xn->lock);
 *     check WITNESS reports no inversion.
 *   - SIG's value argument is only ever 1 today; `done` must stay non-zero
 *     for the WAIT loop to exit.
 */
#define IDPF_CMD_COMPLETION_INIT(ev) do {				\
	mtx_init(&(ev)->mtx, "idpf_xn_cmpl", NULL, MTX_DEF);		\
	cv_init(&(ev)->cv, "idpf_xn_cmpl");				\
	(ev)->done = 0;							\
} while (0)

#define IDPF_CMD_COMPLETION_REINIT(ev) do {				\
	mtx_lock(&(ev)->mtx);						\
	(ev)->done = 0;							\
	mtx_unlock(&(ev)->mtx);						\
} while (0)

#define IDPF_CMD_COMPLETION_SIG(ev, val) do {				\
	mtx_lock(&(ev)->mtx);						\
	(ev)->done = (val);						\
	cv_broadcast(&(ev)->cv);					\
	mtx_unlock(&(ev)->mtx);						\
} while (0)

/*
 * No return value: on timeout the caller observes IDPF_CTLQ_XN_WAITING and
 * reports IDPF_ERR_CTLQ_TIMEOUT itself.
 */
#define IDPF_CMD_COMPLETION_WAIT(ev, timeout_ms) do {			\
	int _idpf_to = (int)(((uint64_t)(timeout_ms) * hz) / 1000);	\
									\
	if (_idpf_to <= 0)						\
		_idpf_to = 1;						\
	mtx_lock(&(ev)->mtx);						\
	while (!(ev)->done) {						\
		if (cv_timedwait(&(ev)->cv, &(ev)->mtx, _idpf_to) != 0)	\
			break;						\
	}								\
	mtx_unlock(&(ev)->mtx);						\
} while (0)

#define IDPF_CMD_COMPLETION_DEINIT(ev) do {				\
	cv_destroy(&(ev)->cv);						\
	mtx_destroy(&(ev)->mtx);					\
} while (0)

#endif /* _IECM_OSDEP_H_ */
