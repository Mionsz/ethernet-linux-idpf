/**
 * @file idpf_osdep.h
 * @brief OS-shim declarations for the idpf FreeBSD VF driver.
 *
 * Feature 015 (User Story 1): closes the S1.2 OS-shim checklist --
 * 14 symbols (structs/macros/functions) isolating FreeBSD-specific
 * primitives from shared/OS-agnostic driver logic, per
 * contracts/os-shim-contract.md. This is a closed set (FR-023): no
 * VIRTCHNL2 protocol structure or hard-coded VIRTCHNL2 numeric value
 * may be added here, and no symbol outside this list may be
 * referenced by a translation unit that includes only this header
 * (see test_idpf_osdep.cpp's closed-contract test).
 */
#ifndef _IDPF_OSDEP_H_
#define _IDPF_OSDEP_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/bus.h>
#include <machine/atomic.h>
#include <machine/bus.h> /* MUST precede sys/bus_dma.h: defines BUS_DMAMAP_OP */
#include <sys/bus_dma.h> /* now sees BUS_DMAMAP_OP already resolved correctly  */
#include <netinet/in.h>

/* LIST_*_TYPE / LIST_FOR_EACH_ENTRY* used by the shared control queue code. */
#include "idpf_list.h"

/*
 * This header is what shared code resolves #include "idpf_osdep.h" to, so it
 * must stay self-contained: it supplies the FreeBSD primitives that the shared
 * struct idpf_hw is built from, and therefore cannot depend on that struct.
 */
struct idpf_hw;
struct idpf_sc;

#ifndef IDPF_TYPE_COMPAT_INCLUDED
/**
 * Linux-style type compatibility definitions for shared code.
 * The shared code uses these Linux kernel types, so we map them to
 * FreeBSD's stdint types.
 *
 * Note: These may be defined earlier via idpf_type_compat.h when
 * compiling shared code with -include flag.
 */
typedef uint8_t		u8;
typedef int8_t		s8;
typedef uint16_t	u16;
typedef int16_t		s16;
typedef uint32_t	u32;
typedef int32_t		s32;
typedef uint64_t	u64;
typedef int64_t		s64;

/* Little-endian types used by shared code */
#define __le16		u16
#define __le32		u32
#define __le64		u64

/* Big-endian types used by shared code */
#define __be16		u16
#define __be32		u32
#define __be64		u64
#endif /* IDPF_TYPE_COMPAT_INCLUDED */

/**
 * M_IDPF: driver-owned malloc(9) type tag backing idpf_calloc()/
 * idpf_free() (FR-015). Declared here (rather than assumed already
 * declared) per checklists/os-shim.md CHK033; defined exactly once,
 * in idpf_osdep.c, via MALLOC_DEFINE(9).
 */
MALLOC_DECLARE(M_IDPF);

/**
 * IDPF_DFLT_MBX_BUF_SIZE (FR-022) -- Proposed, pending review sign-off
 * (research.md R5): a named constant (not an open-coded literal) so
 * downstream code references the symbol, not a bare 4096. The value
 * itself remains a driver-policy default, not yet finalized.
 */
#define IDPF_DFLT_MBX_BUF_SIZE 4096

/**
 * IDPF_STATIC_ASSERT (FR-021): resolves to C11 _Static_assert, used
 * for the struct idpf_hw / struct idpf_sc first-member layout
 * assertion (US1-AS-3, SC-003).
 */
#define IDPF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* static_assert is not a keyword before C23; virtchnl2.h uses it directly. */
#ifndef static_assert
#define static_assert(cond, msg) _Static_assert(cond, msg)
#endif

/**
 * STATIC: internal-linkage marker on shared-code functions (e.g.
 * idpf_get_set_rss_lut() in shared/idpf_common.c). Resolves to `static`, as
 * EDK2's Base.h does for the UEFI shim; unit-test builds may predefine it
 * empty to reach those functions.
 */
#ifndef STATIC
#define STATIC static
#endif

#ifndef IDPF_TYPE_COMPAT_INCLUDED
/**
 * BIT macro for shared code compatibility.
 * Note: May be defined earlier via idpf_type_compat.h.
 */
#ifndef BIT
#define BIT(a) (1UL << (a))
#endif

#ifndef BIT_ULL
#define BIT_ULL(a) (1ULL << (a))
#endif

/**
 * GENMASK macro - generates a contiguous bitmask.
 * Used by shared code for bit field definitions.
 */
#ifndef BITS_PER_LONG
#define BITS_PER_LONG (sizeof(long) * 8)
#endif

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG (sizeof(long long) * 8)
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#endif

/**
 * DEBUGFUNC macro - debug function tracing for shared code.
 * Currently a no-op in FreeBSD driver.
 */
#ifndef DEBUGFUNC
#define DEBUGFUNC(S) do { } while (0)
#endif
#endif /* IDPF_TYPE_COMPAT_INCLUDED */

/*
 * DEBUGOUT2: shared-code trace macro, mirroring the UEFI shim's
 * DBG(DEBUG_INFO, Msg, Par1, Par2). Defined outside the type-compat guard so
 * it is visible on both the driver and shared-code compile paths.
 */
#ifndef IDPF_DBG
#define IDPF_DBG 0
#endif

#ifndef DEBUGOUT2
#if IDPF_DBG
#define DEBUGOUT2(S, A, B)	printf((S), (A), (B))
#else
#define DEBUGOUT2(S, A, B)	do { } while (0)
#endif
#endif

/**
 * Register read/write macros expected by shared code.
 * These wrap the idpf_rd32/idpf_wr32 functions defined below.
 */
#define wr32(hw, reg, value)	idpf_wr32((hw), (reg), (value))
#define rd32(hw, reg)		idpf_rd32((hw), (reg))

/**
 * idpf_wmb: write barrier the shared code issues before ringing the
 * control-queue doorbell (shared/idpf_controlq.c), so descriptor writes are
 * visible first. Release semantics cover that ordering; the UEFI shim maps it
 * to MemoryFence().
 */
#define idpf_wmb()	atomic_thread_fence_rel()

/**
 * Byte-order conversion macros (FR-019): match ntohs/htons/ntohl/
 * htonl exactly, per the original submission's shim description.
 */
#define IDPF_NTOHS(x) ntohs(x)
#define IDPF_HTONS(x) htons(x)
#define IDPF_NTOHL(x) ntohl(x)
#define IDPF_HTONL(x) htonl(x)

/**
 * Additional byte-order conversion macros used by shared code.
 * These map to FreeBSD's byte-order functions from <sys/endian.h>.
 */
#define CPU_TO_LE16(o)	htole16(o)
#define CPU_TO_LE32(s)	htole32(s)
#define CPU_TO_LE64(h)	htole64(h)
#define LE16_TO_CPU(a)	le16toh(a)
#define LE32_TO_CPU(c)	le32toh(c)
#define LE64_TO_CPU(k)	le64toh(k)

/**
 * idpf_memcpy (FR-016): thin wrapper over the standard memcpy(3).
 *
 * Shared code passes a trailing enum idpf_memcpy_type (shared/idpf_alloc.h);
 * as in the UEFI shim, it is discarded -- DMA and non-DMA buffers are both
 * plain kernel-mapped memory here.
 */
#define idpf_memcpy(dst, src, len, type) memcpy((dst), (src), (len))

/**
 * idpf_memset (FR-016): thin wrapper over the standard memset(3).
 *
 * Shared code passes a trailing enum idpf_memset_type (shared/idpf_alloc.h);
 * as in the UEFI shim, it is discarded -- DMA and non-DMA buffers are both
 * plain kernel-mapped memory here.
 */
#define idpf_memset(dst, val, len, type) memset((dst), (val), (len))

/* Shared-code lock abstraction, backed by FreeBSD mutex(9). */
typedef struct mtx	idpf_lock;
#define IDPF_LOCK	struct mtx

#define idpf_init_lock(lock)	mtx_init((lock), "idpf_lock", NULL, MTX_DEF)
#define idpf_acquire_lock(lock)	mtx_lock((lock))
#define idpf_release_lock(lock)	mtx_unlock((lock))
#define idpf_destroy_lock(lock)	mtx_destroy((lock))

#define IDPF_LOCK_INIT(lock)	mtx_init((lock), "idpf_xn", NULL, MTX_DEF)
#define IDPF_LOCK_ACQUIRE(lock)	mtx_lock((lock))
#define IDPF_LOCK_RELEASE(lock)	mtx_unlock((lock))
#define IDPF_LOCK_DESTROY(lock)	mtx_destroy((lock))

/**
 * @struct idpf_dma_mem
 * @brief OS-shim DMA buffer descriptor (FR-011).
 *
 * Produced by idpf_alloc_dma_mem() and released by
 * idpf_free_dma_mem(), backed by FreeBSD bus_dma(9).
 */
struct idpf_dma_mem {
	void *va; /**< Virtual address of the allocated buffer. */
	bus_addr_t pa; /**< Physical/bus address. */
	bus_size_t size; /**< Allocation size. */
	bus_dma_tag_t tag; /**< Owns the mapping's DMA constraints. */
	bus_dmamap_t map; /**< The loaded map handle. */
};

/**
 * IDPF_IOVEC (FR-012): send/recv buffer shape referenced by the
 * transport adapter's submit()/poll_receive() operations (Feature 015
 * User Story 2), matching shared-code usage exactly.
 */
typedef struct idpf_iovec {
	void *iov_base; /**< Buffer start. */
	size_t iov_len; /**< Buffer length. */
} IDPF_IOVEC;

/**
 * IDPF_CMD_COMPLETION (FR-013, Proposed): wraps a FreeBSD struct mtx +
 * struct cv, waited by a synchronous lifecycle caller.
 *
 * The INIT/REINIT/SIG/WAIT/DEINIT operations over this type live in
 * iecm_osdep.h, alongside the other symbols the shared idpf_xn.c
 * expects but which are not part of this closed OS-shim contract.
 */
typedef struct idpf_cmd_completion {
	struct mtx mtx; /**< Serializes wait/signal access. */
	struct cv cv; /**< Signalled on completion. */
	int done; /**< Guards against spurious cv wakeups. */
} IDPF_CMD_COMPLETION;

/**
 * idpf_rd32/idpf_wr32/idpf_flush_wr (FR-017, FR-018): register-access
 * shims resolving to bus_space_read_4()/bus_space_write_4()/
 * bus_space_barrier(..., BUS_SPACE_BARRIER_WRITE) against the BAR0
 * tag/handle stored in struct idpf_hw.
 *
 * Declared, not inlined: struct idpf_hw is still incomplete here.
 */
uint32_t idpf_rd32(struct idpf_hw *hw, bus_size_t offset);
void idpf_wr32(struct idpf_hw *hw, bus_size_t offset, uint32_t value);
void idpf_flush_wr(struct idpf_hw *hw);

/**
 * idpf_usec_delay/idpf_msec_delay (FR-020): map to DELAY(9).
 */
static __inline void idpf_usec_delay(int usecs)
{
	DELAY(usecs);
}

static __inline void idpf_msec_delay(int msecs)
{
	DELAY(msecs * 1000);
}

/*
 * IDPF_GET_TIME_MS: monotonic millisecond timestamp for shared idpf_xn
 * transaction timeout accounting (idpf_xn.c). Uses FreeBSD's fast, monotonic
 * uptime clock so wall-clock steps never perturb transaction timeouts.
 */
static __inline uint64_t idpf_get_time_ms(void)
{
	struct timeval tv;

	getmicrouptime(&tv);
	return ((uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000);
}
#define IDPF_GET_TIME_MS()	idpf_get_time_ms()

/* --- DMA buffer lifecycle (FR-014) --- */
/* Prototype is dictated by shared idpf_controlq.h: returns the VA, or NULL. */
void *idpf_alloc_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem,
			 u64 size);
void idpf_free_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem);

/* --- Generic allocation (FR-015) --- */
/*
 * Prototypes are dictated by the shared code, which calls
 * idpf_calloc(hw, count, size) / idpf_malloc(hw, size) / idpf_free(hw, ptr)
 * and may pass a NULL hw (see shared/idpf_controlq_fxps.c); hw is therefore
 * never dereferenced.
 */
void *idpf_calloc(struct idpf_hw *hw, u32 number, size_t size);
void *idpf_malloc(struct idpf_hw *hw, size_t size);
void idpf_free(struct idpf_hw *hw, void *ptr);

void	idpf_osdep_init(struct idpf_sc *sc);
void	idpf_osdep_deinit(struct idpf_sc *sc);

#endif /* _IDPF_OSDEP_H_ */
