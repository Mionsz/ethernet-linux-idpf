/*
 * idpf_osdep.h
 *
 * Bridge layer between the ported IDPF driver sources and the FreeBSD OSAL
 * (osal_*.h). The porting agent emitted provisional `os_*()` shim names while
 * translating Linux constructs; this header binds those names to the real
 * OSAL API so the ported tree compiles against a single abstraction.
 *
 * Rules:
 * - Every `os_*` name used by ported code must resolve here.
 * - Constructs with no FreeBSD equivalent (NAPI, RCU, rtnl, u64_stats_sync)
 *   are neutralised rather than emulated; see "NON-PORTABLE" below.
 * - No Linux headers. No Linux types leak upward.
 */

#ifndef IDPF_OSDEP_H
#define IDPF_OSDEP_H

#include "osal_port.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>

#include <net/ethernet.h>

/*
 * Linux supplies these transitively via netdevice.h; on FreeBSD they are
 * LinuxKPI gaps, so pull the shims in here where every ported file sees them.
 */
#include <linux/u64_stats_sync.h>
#include <linux/cpumask.h>
#include <linux/cache.h>
#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/idpf_gap_types.h>

#include "kcompat_xarray.h"

/* XDP types are never dereferenced on FreeBSD; see the NON-PORTABLE section. */
struct xdp_buff;
struct xdp_frame;

/* LinuxKPI provides set_bit/clear_bit but not the toggling variants. */
#define IDPF_BITS_PER_LONG (sizeof(unsigned long) * 8)

static inline void
__change_bit(long bit, volatile unsigned long *addr)
{
	addr[bit / IDPF_BITS_PER_LONG] ^= 1UL << (bit % IDPF_BITS_PER_LONG);
}

static inline void
change_bit(long bit, volatile unsigned long *addr)
{
	__change_bit(bit, addr);
}

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Driver-side lifecycle (implemented in idpf_osdep.c)
 * ------------------------------------------------------------------------ */

/* Maximum PCI BARs the bridge will track for register resolution. */
#define IDPF_OSDEP_MAX_BARS 6

/*
 * Bind the bridge to a device and map its register BARs. Must be called during
 * attach, before any os_read32()/os_write32()/os_request_irq() call site runs.
 */
int  idpf_osdep_attach(device_t dev, const unsigned int *bars, unsigned int nbars);
void idpf_osdep_detach(void);

/* The OSAL device / PCI device handles for the bound adapter. */
nic_osal_device_t     *idpf_osal_dev(void);
nic_osal_pci_device_t *idpf_osal_pci_dev(void);

/* ------------------------------------------------------------------------
 * Logging  (osal_log.h)
 * ------------------------------------------------------------------------ */

/* The `dev` argument is accepted and ignored; OSAL logging is device-agnostic. */
#define os_dev_info(dev, ...)       nic_os_log_info(__VA_ARGS__)
#define os_dev_err(dev, ...)        nic_os_log_error(__VA_ARGS__)
#define os_dev_warn(dev, ...)       nic_os_log_warn(__VA_ARGS__)
#define os_dev_dbg(dev, ...)        nic_os_log_debug(__VA_ARGS__)
#define os_netdev_info(ndev, ...)   nic_os_log_info(__VA_ARGS__)
#define os_netdev_err(ndev, ...)    nic_os_log_error(__VA_ARGS__)
#define os_netdev_warn(ndev, ...)   nic_os_log_warn(__VA_ARGS__)

/* ------------------------------------------------------------------------
 * Memory  (osal_mem.h)
 * ------------------------------------------------------------------------ */

/*
 * One malloc type for every driver allocation.
 *
 * free(9) asserts that the type matches the one used to allocate, so the
 * driver must not mix this with the OSAL's internal M_NIC_OSAL.
 */
MALLOC_DECLARE(M_IDPF);

#define os_malloc(sz)               malloc((sz), M_IDPF, M_NOWAIT)
#define os_zalloc(sz)               malloc((sz), M_IDPF, M_NOWAIT | M_ZERO)
#define os_calloc(n, sz)            mallocarray((n), (sz), M_IDPF, M_NOWAIT | M_ZERO)
#define os_free(p)                  free((p), M_IDPF)

/* devm_ has no FreeBSD analogue; caller owns the lifetime explicitly. */
#define os_devm_zalloc(dev, sz)     os_zalloc(sz)
#define os_devm_kzalloc(dev, sz)    os_zalloc(sz)

/* ------------------------------------------------------------------------
 * Locking  (osal_lock.h)
 * ------------------------------------------------------------------------ */

#define os_lock_init(l)             nic_os_lock_init(l)
#define os_lock_destroy(l)          nic_os_lock_destroy(l)
#define os_lock(l)                  nic_os_lock(l)
#define os_unlock(l)                nic_os_unlock(l)
#define os_lock_irqsave(l, f)       nic_os_lock_irqsave((l), &(f))
#define os_unlock_irqrestore(l, f)  nic_os_unlock_irqrestore((l), &(f))

/* The driver carries struct mutex, so use LinuxKPI rather than the OSAL type. */
#define os_mutex_init(m)            mutex_init(m)
#define os_mutex_destroy(m)         mutex_destroy(m)
#define os_mutex_lock(m)            mutex_lock(m)
#define os_mutex_unlock(m)          mutex_unlock(m)

/* ------------------------------------------------------------------------
 * Atomics and barriers  (osal_atomic.h)
 * ------------------------------------------------------------------------ */

#define os_atomic_read(a)           nic_os_atomic32_read(a)
#define os_atomic_set(a, v)         nic_os_atomic32_set((a), (v))
#define os_atomic_inc(a)            nic_os_atomic32_inc(a)
#define os_atomic_dec(a)            nic_os_atomic32_dec(a)

#define os_mb()                     nic_os_mb()
#define os_rmb()                    nic_os_rmb()
#define os_wmb()                    nic_os_wmb()

#define os_test_bit(bs, b)          nic_os_bit_test((bs), (b))
#define os_set_bit(bs, b)           nic_os_bit_set((bs), (b))
#define os_clear_bit(bs, b)         nic_os_bit_clear((bs), (b))
#define os_test_and_set_bit(bs, b)  nic_os_bit_test_and_set((bs), (b))
#define os_test_and_clear_bit(bs, b) nic_os_bit_test_and_clear((bs), (b))

/* ------------------------------------------------------------------------
 * MMIO  (osal_mmio.h)
 *
 * Ported code reaches registers two ways:
 *   os_read32(hw, REG)        -- hw handle + register offset
 *   os_read32(addr)           -- pre-computed mapped address
 * Both forms funnel to the OSAL mmio object carried on the hw handle. The
 * one-argument form derives its offset from the mapped BAR base, so callers
 * must only pass addresses obtained from idpf_get_reg_addr().
 * ------------------------------------------------------------------------ */

/* Supplied by the driver: resolves a hw handle to its OSAL mmio mapping. */
nic_osal_mmio_t *idpf_hw_to_mmio(void *hw);

/* Offset of a mapped register address within its BAR. */
uint32_t idpf_mmio_offset_of(const volatile void *addr);

/* Default mmio mapping, for address-only call sites. */
nic_osal_mmio_t *idpf_default_mmio(void);

/*
 * Register access.
 *
 * Call sites pass an address produced by idpf_get_reg_addr(), which resolves
 * the target region and returns a real mapped pointer, so these are plain
 * MMIO accessors. Routing them through the OSAL offset lookup instead would
 * discard that region resolution and address the wrong BAR window.
 */
#define os_read32(addr)             readl(addr)
#define os_write32(addr, value)     writel((value), (addr))
#define os_read64(addr)             readq(addr)
#define os_write64(addr, value)     writeq((value), (addr))

#define os_mmio_flush(mmio, off)    nic_os_mmio_flush((mmio), (off))

/* ------------------------------------------------------------------------
 * DMA  (osal_dma.h)
 * ------------------------------------------------------------------------ */

#define os_dma_alloc_coherent(dev, sz, buf) \
	nic_os_dma_alloc((dev), (sz), (buf))
#define os_dma_free_coherent(dev, buf) \
	nic_os_dma_free((dev), (buf))
#define os_dma_sync_for_cpu(dev, map, dir) \
	nic_os_dma_sync_for_cpu((dev), (map), (dir))
#define os_dma_sync_for_device(dev, map, dir) \
	nic_os_dma_sync_for_device((dev), (map), (dir))
#define os_dma_unmap(dev, map, dir) \
	nic_os_dma_unmap((dev), (map), (dir))

/*
 * Page-oriented DMA. FreeBSD has no page-struct DMA API; ported call sites
 * pass a kernel virtual address plus length, which maps directly onto
 * nic_os_dma_map().
 */
#define os_dma_map_page(dev, vaddr, offset, len, map) \
	nic_os_dma_map((dev), (void *)((char *)(vaddr) + (offset)), (len), \
	    NIC_OSAL_DMA_BIDIRECTIONAL, (map))

/*
 * dma_set_mask()/dma_set_coherent_mask() constrain the busdma tag, which the
 * OSAL creates internally in nic_os_dma_alloc(). Nothing to program here.
 */
#define os_dma_set_mask(dev, mask)          (0)
#define os_dma_set_coherent_mask(dev, mask) (0)

/* ------------------------------------------------------------------------
 * Deferred work  (osal_work.h)
 * ------------------------------------------------------------------------ */

/*
 * Deferred work.
 *
 * The adapter embeds struct delayed_work and passes real workqueues, so these
 * map to LinuxKPI. The callback recovers its context with container_of(), so
 * the arg carried by the OSAL form is unused.
 */
#define os_task_init(w, fn, arg)            INIT_DELAYED_WORK((w), (fn))
#define os_task_destroy(w)                  cancel_delayed_work_sync(w)
#define os_task_enqueue(tq, w)              queue_delayed_work((tq), (w), 0)
#define os_delayed_task_enqueue(tq, w, d)   queue_delayed_work((tq), (w), (d))
#define os_task_cancel(w)                   cancel_delayed_work(w)
#define os_task_drain(tq, w)                cancel_delayed_work_sync(w)
#define os_task_flush(w)                    flush_delayed_work(w)

/* ------------------------------------------------------------------------
 * Time  (osal_time.h)
 * ------------------------------------------------------------------------ */

#define os_time_ms()                nic_os_time_ms()
#define os_time_us()                nic_os_time_us()
#define os_sleep_ms(ms)             nic_os_sleep_ms(ms)
#define os_delay_us(us)             nic_os_delay_us(us)

#define os_timer_init(t, cb, arg)   nic_os_timer_init((t), (cb), (arg))
#define os_timer_destroy(t)         nic_os_timer_destroy(t)
#define os_timer_start(t, ms, per)  nic_os_timer_start((t), (ms), (per))
#define os_timer_stop(t)            nic_os_timer_stop(t)

/* Linux timer_setup()/mod_timer() spellings used by the ported code. The
 * mod_timer form passes an absolute tick deadline; the OSAL takes a relative
 * millisecond delay. */
/* The driver embeds struct timer_list, so these map to LinuxKPI. */
#define os_timer_setup(t, fn, flags)    timer_setup((t), (fn), (flags))
#define os_timer_mod(t, abs_ticks)      mod_timer((t), (abs_ticks))

/* ------------------------------------------------------------------------
 * Shims the porting pass introduced under provisional names
 * ------------------------------------------------------------------------ */

/* Both spellings appear in the ported code; LinuxKPI supplies the real macro. */
#define os_field_get(mask, val)     FIELD_GET((mask), (val))
#define os_FIELD_GET(mask, val)     FIELD_GET((mask), (val))

#define os_le64_to_cpu(x)           le64toh(x)

/* Linux spells the minimum payload-only frame length ETH_ZLEN; LinuxKPI omits it. */
#ifndef ETH_ZLEN
#define ETH_ZLEN                    (ETHER_MIN_LEN - ETHER_CRC_LEN)
#endif

/* Linux kmalloc-family names; the flags argument is already a FreeBSD M_* value. */
#define os_kmalloc(sz, flags)       malloc((sz), M_IDPF, (flags))
#define os_kcalloc(n, sz, flags)    mallocarray((n), (sz), M_IDPF, (flags) | M_ZERO)
#define os_kfree(p)                 free((p), M_IDPF)

#define os_u64_stats_inc(p)         (*(p) += 1)

#define os_wait_event_timeout(wqh, cond, to) \
	wait_event_timeout((wqh), (cond), (to))

#define os_netdev_rss_key_fill(buf, len)    arc4random_buf((buf), (len))

/* The page cache is a Linux allocator detail; a plain free is equivalent here. */
#define os_page_frag_cache_drain(page, bias) \
	do { (void)(bias); free((page), M_IDPF); } while (0)

/*
 * Map a sub-region of BAR 0.
 *
 * idpf_mem.h dereferences the result (readl(vaddr + reg)) and
 * idpf_get_*_reg_addr() does pointer arithmetic on it, so this must be a real
 * mapped address -- the Linux original is devm_ioremap() of the same window.
 */
#define os_map_region(pdev, phys, len)  ioremap((phys), (len))
#define os_unmap_region(addr, len)      iounmap(addr)

/* ------------------------------------------------------------------------
 * Interrupts  (osal_irq.h)
 *
 * Linux request_irq(irq, handler, flags, name, arg) becomes an OSAL
 * registration against an irq object keyed by vector.
 * ------------------------------------------------------------------------ */

/* Supplied by the driver: resolves a vector to its OSAL irq object. */
nic_osal_irq_t *idpf_vector_to_irq(int vector);

#define os_request_irq(vec, handler, flags, name, arg) \
	nic_os_irq_register(idpf_osal_dev(), idpf_vector_to_irq(vec), \
	    (handler), (arg), (name))
#define os_free_irq(vec, arg) \
	nic_os_irq_unregister(idpf_osal_dev(), idpf_vector_to_irq(vec))
#define os_irq_enable(vec)          nic_os_irq_enable(idpf_vector_to_irq(vec))
#define os_irq_disable(vec)         nic_os_irq_disable(idpf_vector_to_irq(vec))

/* ------------------------------------------------------------------------
 * PCI  (osal_pci.h)
 * ------------------------------------------------------------------------ */

#define os_pci_enable(p)            nic_os_pci_enable(p)
#define os_pci_disable(p)           nic_os_pci_disable(p)
#define os_pci_set_master(p)        nic_os_pci_set_master(p)
#define os_pci_read8(p, o, v)       nic_os_pci_read8((p), (o), (v))
#define os_pci_read16(p, o, v)      nic_os_pci_read16((p), (o), (v))
#define os_pci_read32(p, o, v)      nic_os_pci_read32((p), (o), (v))
#define os_pci_write8(p, o, v)      nic_os_pci_write8((p), (o), (v))
#define os_pci_write16(p, o, v)     nic_os_pci_write16((p), (o), (v))
#define os_pci_write32(p, o, v)     nic_os_pci_write32((p), (o), (v))
#define os_pci_get_bar(p, b, a, s)  nic_os_pci_get_bar((p), (b), (a), (s))

/* ------------------------------------------------------------------------
 * Network device  (osal_netdev.h)
 * ------------------------------------------------------------------------ */

#define os_register_netdev(dev, nd, ops)    nic_os_netdev_register((dev), (nd), (ops))
#define os_unregister_netdev(nd)            nic_os_netdev_unregister(nd)
#define os_netif_tx_start_all_queues(nd)    nic_os_netif_start_queue(nd)
#define os_netif_tx_stop_all_queues(nd)     nic_os_netif_stop_queue(nd)
#define os_netif_tx_wake_all_queues(nd)     nic_os_netif_wake_queue(nd)
#define os_netif_start_queue(nd)            nic_os_netif_start_queue(nd)
#define os_netif_stop_queue(nd)             nic_os_netif_stop_queue(nd)
#define os_netif_wake_queue(nd)             nic_os_netif_wake_queue(nd)
#define os_netif_rx(nd, pkt)                nic_os_netif_rx((nd), (pkt))

#define os_pkt_buf_alloc(sz)                nic_os_pkt_buf_alloc(sz)
#define os_pkt_buf_free(p)                  nic_os_pkt_buf_free(p)
#define os_free_mbuf(m)                     m_freem(m)

/* Ported code stores the softc on the netdev's os_private slot. */
#define os_netdev_priv(nd)                  ((nd)->os_private)

/* SET_NETDEV_DEV(): FreeBSD binds ifnet to device_t at iflib attach. */
#define os_set_netdev_dev(nd, dev)          do { } while (0)

/* Pad an mbuf chain out to `len` with zero bytes. */
static inline int
os_mbuf_pad_reserve_zero(struct mbuf **mp, int len)
{
	return (m_append(*mp, len, NULL) ? 0 : ENOBUFS);
}

/* ------------------------------------------------------------------------
 * Page-granular allocation
 *
 * Linux page structs do not exist on FreeBSD. Ported call sites only need a
 * page-sized, page-aligned kernel buffer they can subsequently DMA-map.
 * ------------------------------------------------------------------------ */

#define os_alloc_page()             nic_os_zalloc(PAGE_SIZE)
#define os_free_page(p)             nic_os_free(p)

/* Linux slab LRU hint has no FreeBSD analogue; the cache/lru args are ignored. */
#define os_kmem_cache_alloc_lru(cache, lru, gfp)    nic_os_zalloc((cache)->obj_size)

/* ------------------------------------------------------------------------
 * MSI-X vector allocation
 *
 * The OSAL irq layer registers handlers against already-allocated vectors;
 * allocating the vector block itself stays with the driver's PCI attach path.
 * ------------------------------------------------------------------------ */

/* Supplied by the driver: allocates `want` MSI-X vectors, returns the count. */
int idpf_pci_alloc_msix(nic_osal_pci_device_t *pdev, int min_vectors, int want);
void idpf_pci_free_msix(nic_osal_pci_device_t *pdev);

#define os_pci_alloc_msix(p, minv, want)    idpf_pci_alloc_msix((p), (minv), (want))
#define os_pci_free_msix(p)                 idpf_pci_free_msix(p)

/* ------------------------------------------------------------------------
 * NON-PORTABLE — neutralised, not emulated
 *
 * These Linux constructs have no FreeBSD counterpart under iflib. Rather than
 * fake them, they compile to no-ops or to the nearest honest primitive, so the
 * absence is visible at the call site instead of hidden behind a fake shim.
 * ------------------------------------------------------------------------ */

/* Link state: iflib publishes carrier via iflib_link_state_change(). */
#define os_netif_carrier_on(nd)     do { } while (0)
#define os_netif_carrier_off(nd)    do { } while (0)
#define os_netif_carrier_ok(nd)     (1)

/*
 * PCIe link-speed reporting is Linux diagnostic sugar with no FreeBSD
 * equivalent. Report "unknown" rather than inventing capability numbers.
 */
#define os_pcie_speed_to_str(s)             "unknown"
#define os_pcie_bandwidth_capable(d, s, w)  (0)
#define os_pcie_bandwidth_available(d, l, s, w) (0)

/* ethtool does not exist on FreeBSD; statistics are exposed via sysctl(9). */
typedef struct os_ethtool_ops os_ethtool_ops_t;

/* NAPI: iflib owns RX/TX polling. There is no NAPI object to drive. */
#define os_napi_enable(n)           do { } while (0)
#define os_napi_disable(n)          do { } while (0)
#define os_napi_complete(n)         (1)
#define os_napi_schedule(n)         do { } while (0)
#define os_napi_gro_receive(n, m)   nic_os_netif_rx(NULL, (m))

/* RTNL: FreeBSD serialises ifnet configuration through the driver's own lock. */
#define os_rtnl_lock()              do { } while (0)
#define os_rtnl_unlock()            do { } while (0)

/* RCU pointer publication reduces to an ordered store on FreeBSD. */
#define os_rcu_init_pointer(p, v)   do { (p) = (v); nic_os_wmb(); } while (0)
#define os_rcu_assign_pointer(p, v) do { nic_os_wmb(); (p) = (v); } while (0)
#define os_rcu_dereference(p)       (p)

/* u64_stats_sync is a 32-bit-platform seqlock; unnecessary on amd64. */
#define os_u64_stats_fetch_begin(s)     (0)
#define os_u64_stats_fetch_retry(s, v)  (0)
#define os_u64_stats_read(v)            (v)
#define os_u64_stats_update_begin(s)    do { } while (0)
#define os_u64_stats_update_end(s)      do { } while (0)

/* eth_type_trans(): iflib classifies received frames itself. */
#define os_eth_type_trans(m, nd)    (m)

#ifdef __cplusplus
}
#endif

#endif /* IDPF_OSDEP_H */
