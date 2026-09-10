/**
 * @file idpf_osdep.c
 * @brief OS-shim implementation for the idpf FreeBSD VF driver.
 *
 * Feature 015 (User Story 1): implements the DMA-buffer lifecycle
 * (idpf_alloc_dma_mem/idpf_free_dma_mem, FR-014) and the generic
 * allocation shims (idpf_calloc/idpf_free, FR-015), per
 * contracts/os-shim-contract.md. idpf_osdep_init()/idpf_osdep_deinit()
 * remain structural no-ops at this stage.
 */

#include "idpf_osdep.h"
#include "idpf_drv.h" /* completes struct idpf_hw for the register accessors */

MALLOC_DEFINE(M_IDPF, "idpf", "idpf driver memory");

uint32_t idpf_rd32(struct idpf_hw *hw, bus_size_t offset)
{
	struct idpf_sc *sc = __containerof(hw, struct idpf_sc, hw);
	KASSERT(sc->pci_mem != NULL, ("idpf: BAR0 is not mapped"));
	KASSERT(offset < rman_get_size(sc->pci_mem), ("idpf: offset is greater than pci_mem size"));
	return (bus_read_4(sc->pci_mem, offset));
}

void idpf_wr32(struct idpf_hw *hw, bus_size_t offset, uint32_t value)
{
	struct idpf_sc *sc = __containerof(hw, struct idpf_sc, hw);
	KASSERT(sc->pci_mem != NULL, ("idpf: BAR0 is not mapped"));
	KASSERT(offset < rman_get_size(sc->pci_mem), ("idpf: offset is greater than pci_mem size"));
	bus_write_4(sc->pci_mem, offset, value);
}

void idpf_flush_wr(struct idpf_hw *hw)
{
	struct idpf_sc *sc = __containerof(hw, struct idpf_sc, hw);
	KASSERT(sc->pci_mem != NULL, ("idpf: BAR0 is not mapped"));
	bus_barrier(sc->pci_mem, 0, 0, BUS_SPACE_BARRIER_WRITE);
}

/**
 * @brief Allocate and zero-fill a DMA-capable buffer (FR-014).
 *
 * Acquires a bus_dma(9) tag, allocates DMA-safe memory against it, and
 * loads a map for that memory, in that order. On any failure, releases
 * whatever was already acquired (reverse order) and leaves @p mem
 * zeroed, reporting the failing step's error code.
 *
 * @param sc   Driver software context (reserved for future per-device
 *             DMA-tag parenting; unused at this feature's scope).
 * @param mem  Output DMA-buffer descriptor.
 * @param size Requested allocation size, in bytes.
 * @return Virtual address of the buffer, or NULL on failure.
 */
void *idpf_alloc_dma_mem(struct idpf_hw *hw __unused, struct idpf_dma_mem *mem,
			 u64 size)
{
	int rc;

	memset(mem, 0, sizeof(*mem));

	rc = bus_dma_tag_create(
		/* parent */ NULL,
		/* alignment */ 1,
		/* boundary */ 0,
		/* lowaddr */ BUS_SPACE_MAXADDR,
		/* highaddr */ BUS_SPACE_MAXADDR,
		/* filtfunc */ NULL,
		/* filtfuncarg */ NULL,
		/* maxsize */ size,
		/* nsegments */ 1,
		/* maxsegsz */ size,
		/* flags */ 0,
		/* lockfunc */ NULL,
		/* lockfuncarg */ NULL, &mem->tag);
	if (rc != 0)
		return (NULL);

	rc = bus_dmamem_alloc(mem->tag, &mem->va, 0, &mem->map);
	if (rc != 0) {
		bus_dma_tag_destroy(mem->tag);
		memset(mem, 0, sizeof(*mem));
		return (NULL);
	}

	rc = bus_dmamap_load(mem->tag, mem->map, mem->va, size, NULL, NULL, 0);
	if (rc != 0) {
		bus_dmamem_free(mem->tag, mem->va, mem->map);
		bus_dma_tag_destroy(mem->tag);
		memset(mem, 0, sizeof(*mem));
		return (NULL);
	}

	memset(mem->va, 0, size);
	mem->size = size;

	return (mem->va);
}

/**
 * @brief Release a DMA-capable buffer in reverse order of acquisition
 * (FR-014).
 *
 * Reverse order of idpf_alloc_dma_mem(): unload the map, free the
 * memory, then destroy the tag.
 *
 * @param sc  Driver software context (unused at this feature's scope).
 * @param mem DMA-buffer descriptor to release.
 */
void idpf_free_dma_mem(struct idpf_hw *hw __unused, struct idpf_dma_mem *mem)
{
	bus_dmamap_unload(mem->tag, mem->map);
	bus_dmamem_free(mem->tag, mem->va, mem->map);
	bus_dma_tag_destroy(mem->tag);
}

/**
 * @brief Allocate zeroed, M_IDPF-tagged memory (FR-015).
 *
 * Maps to malloc(number * size, M_IDPF, M_NOWAIT | M_ZERO). M_NOWAIT because
 * shared code allocates with the control-queue mutex held and checks for a
 * NULL return.
 *
 * @param hw     Shared-code hw context; unused, and may be NULL.
 * @param number Element count.
 * @param size   Element size, in bytes.
 * @return Pointer to zeroed memory, or NULL on allocation failure.
 */
void *idpf_calloc(struct idpf_hw *hw __unused, u32 number, size_t size)
{
	return (malloc((size_t)number * size, M_IDPF, M_NOWAIT | M_ZERO));
}

/**
 * @brief Allocate a single zeroed, M_IDPF-tagged block (FR-015).
 *
 * Zeroing matches the UEFI shim, where idpf_malloc() resolves to
 * AllocateZeroPool(); shared code (see shared/idpf_common.c) relies on the
 * returned block being cleared.
 *
 * @param hw   Shared-code hw context; unused, and may be NULL.
 * @param size Allocation size, in bytes.
 * @return Pointer to zeroed memory, or NULL on allocation failure.
 */
void *idpf_malloc(struct idpf_hw *hw __unused, size_t size)
{
	return (malloc(size, M_IDPF, M_NOWAIT | M_ZERO));
}

/**
 * @brief Release memory allocated by idpf_calloc()/idpf_malloc() (FR-015).
 *
 * Maps to free(ptr, M_IDPF).
 *
 * @param hw  Shared-code hw context; unused, and may be NULL.
 * @param ptr Pointer previously returned by idpf_calloc()/idpf_malloc().
 */
void idpf_free(struct idpf_hw *hw __unused, void *ptr)
{
	free(ptr, M_IDPF);
}

/**
 * @brief Initialize the OS-shim layer for one device instance.
 *
 * A no-op at this skeleton stage; declared for structural symmetry with
 * idpf_osdep_deinit() so that any future acquisition performed here has
 * a paired, reverse-order teardown call site already in place.
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_osdep_init(struct idpf_sc *sc __unused)
{
}

/**
 * @brief Tear down the OS-shim layer for one device instance.
 *
 * No-op at this skeleton stage. Must remain the exact reverse of
 * idpf_osdep_init() once real acquisition exists.
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_osdep_deinit(struct idpf_sc *sc __unused)
{
}
