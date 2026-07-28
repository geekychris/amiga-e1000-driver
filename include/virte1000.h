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
    APTR    rx_ring;      /* 16 × 16-byte descriptors */
    APTR    tx_ring;      /* 16 × 16-byte descriptors */
    APTR    rx_buffers;   /* 16 × 2KB pool; per-descriptor slices */

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
};

#endif
