#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint16_t u16;
typedef void *device_t;
struct resource { int rid; };
struct idpf_adapter {
	u16 num_avail_msix, num_msix_entries;
	u16 *vector_ids;
	struct resource **msix_entries;
	struct { u16 v_idx; } mb_vector;
	struct { u16 mailbox_vector_id; } caps;
	struct { int vchunks; } *req_vec_chunks;
};

static int fault, allocations, live, release_count, wrong_rid;
static struct resource irq;

static void *
test_alloc(size_t size)
{
	void *ptr;
	allocations++;
	if ((fault == 1 && allocations == 1) ||
	    (fault == 4 && allocations == 2))
		return (NULL);
	ptr = calloc(1, size);
	if (ptr != NULL)
		live++;
	return (ptr);
}

static void
test_free(void *ptr)
{
	if (ptr != NULL)
		live--;
	free(ptr);
}

static int
test_alloc_msix(device_t dev, int *count)
{
	(void)dev;
	(void)count;
	return (0);
}

static int
test_vec_ids(struct idpf_adapter *adapter, u16 *ids, int count,
    const void *chunks)
{
	(void)adapter;
	(void)chunks;
	for (int index = 0; index < count; index++)
		ids[index] = 10 + index;
	return (fault == 2 ? count - 1 : count);
}

static struct resource *
test_irq_alloc(device_t dev, int type, int *rid, int flags)
{
	(void)dev;
	(void)type;
	(void)flags;
	irq.rid = *rid;
	return (fault == 5 ? NULL : &irq);
}

static void
test_irq_release(device_t dev, int type, int rid, struct resource *res)
{
	(void)dev;
	(void)type;
	release_count++;
	if (res->rid != rid)
		wrong_rid++;
}

#define IDPF_MBX_Q_VEC 1
#define IDPF_MBX_VEC_IDX 0
#define IDPF_MIN_Q_VEC 1
#define SYS_RES_IRQ 0
#define RF_ACTIVE 1
#define RF_SHAREABLE 2
#define idpf_adapter_to_dev(adapter) ((device_t)(adapter))
#define idpf_get_default_vports(adapter) 1
#define idpf_get_reserved_vecs(adapter) 4
#define idpf_send_alloc_vectors_msg(adapter, count) ((void)(count), 0)
#define pci_alloc_msix test_alloc_msix
#define pci_release_msi(dev) ((void)0)
#define le16toh(value) (value)
#define idpf_get_vec_ids test_vec_ids
#define idpf_remap_msix_vectors(adapter, ids, count) (fault == 3 ? EINVAL : 0)
#define bus_alloc_resource_any test_irq_alloc
#define bus_release_resource test_irq_release
#define idpf_init_vector_stack(adapter) (fault == 6 ? ENOMEM : 0)
#define idpf_mb_intr_init(adapter) (fault == 7 ? EIO : 0)
#define idpf_mb_irq_enable(adapter) ((void)0)
#define idpf_deinit_vector_stack(adapter) ((void)0)
#define idpf_send_dealloc_vectors_msg(adapter) ((void)0)
#define device_printf(dev, ...) ((void)0)
#define malloc(size, type, flags) test_alloc(size)
#define free(ptr, type) test_free(ptr)
#include "intr_under_test.inc"
#undef malloc
#undef free

int
main(int argc, char **argv)
{
	struct idpf_adapter adapter = { 0 };
	int result;

	if (argc != 2)
		return (2);
	fault = atoi(argv[1]);
	adapter.req_vec_chunks = calloc(1, sizeof(*adapter.req_vec_chunks));
	if (adapter.req_vec_chunks == NULL)
		return (2);
	result = idpf_intr_req(&adapter);
	free(adapter.req_vec_chunks);
	if (result == 0 || live != 0 || wrong_rid != 0 ||
	    adapter.msix_entries != NULL || adapter.vector_ids != NULL ||
	    (fault >= 6 && release_count != 1)) {
		fprintf(stderr, "FAIL fault=%d result=%d live=%d wrong_rid=%d releases=%d\n",
		    fault, result, live, wrong_rid, release_count);
		return (1);
	}
	printf("PASS interrupt cleanup fault %d\n", fault);
	return (0);
}