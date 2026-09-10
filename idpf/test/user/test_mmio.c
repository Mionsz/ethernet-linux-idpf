#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint64_t bus_size_t;
typedef uint8_t u8;
struct idpf_mmio_reg {
	bus_size_t addr_start;
	bus_size_t addr_len;
	void *vaddr;
};
struct idpf_hw {
	struct idpf_mmio_reg mbx, rstat;
	struct idpf_mmio_reg *lan_regs;
	int num_lan_regs;
};
struct resource {
	bus_size_t size;
	u8 *base;
};
struct idpf_adapter {
	struct idpf_hw hw;
	struct { struct resource *static_reg_info[1]; } dev_ops;
};

static void
idpf_lan_mmio_regs_rel(struct idpf_adapter *adapter)
{
	free(adapter->hw.lan_regs);
	adapter->hw.lan_regs = NULL;
	adapter->hw.num_lan_regs = 0;
}

#define IDPF_MMIO_MAP_FALLBACK_MAX_REMAINING 3
#define rman_get_size(resource) ((resource)->size)
#define rman_get_virtual(resource) ((resource)->base)
#define device_printf(device, ...) ((void)0)
#define malloc(size, type, flags) calloc(1, (size))
#include "mmio_under_test.inc"
#undef malloc

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
	failures++; } } while (0)

static void
check_windows(bus_size_t first, bus_size_t first_len,
    bus_size_t second, bus_size_t second_len, int expected)
{
	u8 bytes[256];
	struct resource bar = { sizeof(bytes), bytes };
	struct idpf_adapter adapter = { 0 };
	int result;

	adapter.dev_ops.static_reg_info[0] = &bar;
	adapter.hw.mbx = (struct idpf_mmio_reg){ first, first_len, NULL };
	adapter.hw.rstat = (struct idpf_mmio_reg){ second, second_len, NULL };
	result = idpf_calc_remaining_mmio_regs(&adapter);
	CHECK(result == expected);
	if (result == 0 && expected == 0) {
		CHECK(idpf_map_lan_mmio_regs(&adapter) == 0);
		for (int index = 0; index < adapter.hw.num_lan_regs; index++) {
			struct idpf_mmio_reg *region = &adapter.hw.lan_regs[index];
			CHECK(region->addr_start <= bar.size);
			CHECK(region->addr_len <= bar.size - region->addr_start);
			if (region->addr_len != 0)
				CHECK(region->vaddr == bytes + region->addr_start);
		}
	}
	idpf_lan_mmio_regs_rel(&adapter);
}

int
main(void)
{
	u8 bytes[256];
	struct resource bar = { sizeof(bytes), bytes };
	struct idpf_adapter adapter = { 0 };

	check_windows(32, 16, 128, 16, 0);
	check_windows(128, 16, 32, 16, 0);
	check_windows(0, 32, 32, 224, 0);
	check_windows(32, 64, 64, 16, EINVAL);
	check_windows(32, 0, 128, 16, EINVAL);
	check_windows(32, 16, 240, 32, EINVAL);
	check_windows(32, 16, UINT64_MAX - 7, 16, EINVAL);
	check_windows(32, UINT64_MAX, 128, 16, EINVAL);
	adapter.dev_ops.static_reg_info[0] = &bar;
	adapter.hw.num_lan_regs = 1;
	adapter.hw.lan_regs = calloc(1, sizeof(*adapter.hw.lan_regs));
	if (adapter.hw.lan_regs == NULL)
		return (1);
	adapter.hw.lan_regs[0].addr_start = 128;
	adapter.hw.lan_regs[0].addr_len = UINT64_MAX - 63;
	CHECK(idpf_map_lan_mmio_regs(&adapter) == EINVAL);
	idpf_lan_mmio_regs_rel(&adapter);
	printf("MMIO regression: %d failures\n", failures);
	return (failures != 0);
}