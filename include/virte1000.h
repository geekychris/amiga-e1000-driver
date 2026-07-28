#ifndef VIRTE1000_H
#define VIRTE1000_H

#include <exec/devices.h>
#include <exec/interfaces.h>
#include <exec/interrupts.h>  /* struct Interrupt */
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <dos/dos.h>          /* BPTR */
#include <devices/sana2.h>
#include <expansion/pci.h>    /* PCIDevice, PCIResourceRange */
#include <interfaces/expansion.h>  /* PCIIFace method definitions */

#include <proto/exec.h>

/*
 * Library base for virte1000.device. First field MUST be struct Device so
 * that (struct Device *)devBase is well-defined — the kernel wires up the
 * negative-offset jump table relative to &dev_Base.dd_Library.
 *
 * Phase-3a: expansion.library + IPCI + pciDevice added, plus BAR0 slot.
 * All fields may be NULL at any time (Init degrades gracefully — see
 * device.c). Expunge unconditionally frees whatever is non-NULL, so a
 * partially-completed Init still tears down cleanly.
 */
struct Virte1000Base
{
    struct Device      dev_Base;
    struct ExecIFace  *IExec;
    BPTR               dev_SegList;
    struct SignalSemaphore io_lock;

    /* PCI plumbing — populated by Init if e1000 present, else NULL. */
    struct Library         *ExpansionBase;
    struct PCIIFace        *IPCI;
    struct PCIDevice       *pciDevice;
    struct PCIResourceRange *bar0;   /* MMIO range for e1000 register file */

    /* Cached hardware state — read once at Init from RAL/RAH. Serves
     * S2_GETSTATIONADDRESS + S2_DEVICEQUERY without re-hitting MMIO on
     * every request. */
    UBYTE   mac[6];
    BOOL    hw_present;   /* TRUE iff pciDevice + bar0 valid */

    /* Phase 4: DMA-visible RX/TX descriptor rings and RX buffer pool.
     * On sam460ex CPU addresses == PCI-bus addresses for main RAM
     * (identity-mapped), so we hand these pointers directly to the NIC
     * as RDBAL/TDBAL values. NULL if ring alloc failed — subsequent
     * phases check and refuse to enable the device. */
    APTR    rx_ring;      /* 16 × 16-byte descriptors (CPU virt) */
    APTR    tx_ring;      /* 16 × 16-byte descriptors (CPU virt) */
    APTR    rx_buffers;   /* 16 × 2KB pool; per-descriptor slices (CPU virt) */

    /* PCI-bus physical addresses of the above, obtained via
     * IExec->StartDMA + GetDMAList. Written into RDBAL/TDBAL and
     * per-descriptor buffer_addr fields. On sam460ex these usually
     * equal the CPU virt addresses (identity map) but NOT reliably —
     * pa6t_eth's approach (StartDMA per allocation) is the OS4-blessed
     * path. */
    ULONG   rx_ring_phys;
    ULONG   tx_ring_phys;
    ULONG   rx_buffers_phys;

    /* Phase 5: PCI INTx interrupt handler. irq_installed guards Expunge
     * so we RemIntServer only if AddIntServer succeeded. irq_counter and
     * last_icr are updated by the ISR on every fire — read from user
     * context (test programs) via a private query command. Volatile so
     * a spinning reader sees new values. */
    struct Interrupt irq_node;
    ULONG            irq_vector;
    BOOL             irq_installed;
    volatile ULONG   irq_counter;
    volatile ULONG   last_icr;

    /* Phase 6a: SANA-II state machine. Init leaves us OFFLINE with rings
     * programmed but RX/TX disabled and IRQs masked. S2_CONFIGINTERFACE
     * promotes to CONFIGURED (one-shot). S2_ONLINE promotes to ONLINE
     * (writes RCTL.EN, TCTL.EN, unmasks IMS). S2_OFFLINE demotes back
     * to CONFIGURED (masks IMS, clears RCTL.EN, TCTL.EN, retains MAC). */
    ULONG            state;   /* V1000_STATE_* below */

    /* Phase 6b: TX ring producer index. Next TX descriptor to fill on
     * CMD_WRITE. Wraps at V1000_RING_ENTRIES. Since we currently reply
     * synchronously (poll for TDH advance) there is only ever ONE
     * outstanding descriptor at a time, so tx_next_slot doesn't really
     * need to be a full ring — but keeping it as a slot index gets the
     * bookkeeping right for the future async version. */
    ULONG            tx_next_slot;

    /* Phase 6d: real struct Unit so OS4 exec can enqueue IORequests
     * for async-path commands (CMD_WRITE, S2_BROADCAST, S2_MULTICAST).
     * Embedded (not a pointer) so lifetime tracks Virte1000Base and no
     * separate alloc/free is needed. Init sets up unit_MsgPort with
     * PA_IGNORE so exec bypasses the signal path and calls BeginIO
     * synchronously — same net effect as the old fake io_Unit but with
     * a valid MsgPort structure. Single unit only (unit 0). */
    struct Unit      v1000_unit;
};

/* Unit-state values held in Virte1000Base.state. Defined as constants
 * rather than an enum so testonline can #include this header without
 * dragging in the whole libBase struct. */
#define V1000_STATE_OFFLINE     0   /* fresh from Init or reset */
#define V1000_STATE_CONFIGURED  1   /* MAC programmed, hardware idle */
#define V1000_STATE_ONLINE      2   /* RX/TX enabled, IRQs unmasked */

/* Private debug command IDs — kept in header so tests can #include and
 * share the values. Well outside SANA-II (< 0x38) and new-style (0xC000+)
 * ranges, so no chance of colliding with a spec command. */
#define V1000_DBG_STATUS   0xF001   /* returns counter/icr/state */
#define V1000_DBG_FIRE_IRQ 0xF002   /* ICS.LSC → forces one ISR call */
#define V1000_DBG_SEND     0xF003   /* send raw L2 frame from ios2_Data
                                     * of ios2_DataLength bytes; workaround
                                     * for CMD_WRITE being blocked by
                                     * exec's dispatch layer */

#endif
