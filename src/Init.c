/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * Init.c — Device initialization implementation (_manager_Init).
 */

#include "virte1000.h"
#include "version.h"

#include <exec/ports.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <devices/newstyle.h>

#include <stdarg.h>

/* Intel 82540EM PCI IDs */
#define E1000_PCI_VENDOR   0x8086
#define E1000_PCI_DEVICE   0x100E

/* Diagnostic log context structure */
struct V1000LogCtx {
    APTR trace_fh;
};

static void v1000_log_open(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    (void)IExec;
    ctx->trace_fh = NULL;
}

static void v1000_log_close(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    (void)IExec; (void)ctx;
}

#define LOGF(ctx, ...) do { } while (0)

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[virte1000] Init: %s\n", DEVVERSIONSTRING_FULL);

    struct V1000LogCtx log;
    v1000_log_open(iexec, &log);

    devBase->IExec = iexec;
    devBase->dev_SegList = seglist;
    iexec->InitSemaphore(&devBase->io_lock);

    devBase->ExpansionBase = NULL;
    devBase->IPCI          = NULL;
    devBase->pciDevice     = NULL;
    devBase->bar0          = NULL;
    devBase->hw_present    = FALSE;
    devBase->rx_ring       = NULL;
    devBase->tx_ring       = NULL;
    devBase->rx_buffers    = NULL;
    devBase->rx_ring_phys  = 0;
    devBase->tx_ring_phys  = 0;
    devBase->rx_buffers_phys = 0;
    devBase->tx_scratch    = NULL;
    devBase->tx_scratch_phys = 0;
    devBase->irq_vector    = 0;
    devBase->irq_installed = FALSE;
    devBase->irq_counter   = 0;
    devBase->last_icr      = 0;
    devBase->state         = V1000_STATE_OFFLINE;
    devBase->tx_next_slot  = 0;
    devBase->UtilityBase   = NULL;
    devBase->IUtility      = NULL;
    devBase->sana2hook     = NULL;
    devBase->pending_online = FALSE;
    devBase->trace_fh      = NULL;
    devBase->DOSBase       = NULL;
    devBase->IDOS          = NULL;
    devBase->trace_seq     = 0;

    iexec->NewList((struct List *)&devBase->opener_list);
    iexec->InitSemaphore(&devBase->opener_lock);

    /* Init unit struct */
    {
        struct Unit *u = &devBase->v1000_unit;
        u->unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        u->unit_MsgPort.mp_Node.ln_Name = (STRPTR)DEVNAME;
        u->unit_MsgPort.mp_Node.ln_Pri  = 0;
        u->unit_MsgPort.mp_Flags        = PA_IGNORE;
        u->unit_MsgPort.mp_SigBit       = 0;
        u->unit_MsgPort.mp_SigTask      = NULL;
        iexec->NewList(&u->unit_MsgPort.mp_MsgList);
        u->unit_flags   = UNITF_ACTIVE;
        u->unit_pad     = 0;
        u->unit_OpenCnt = 0;
    }
    for (int i = 0; i < 6; i++) devBase->mac[i] = 0;

    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version      = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision     = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    /* Open utility.library */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (devBase->UtilityBase) {
        devBase->IUtility = (struct UtilityIFace *)
            iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    }

    /* Open expansion.library */
    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 53);
    if (!devBase->ExpansionBase) {
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase,
                                                            "pci", 1, NULL);
    if (!devBase->IPCI) {
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    devBase->pciDevice = devBase->IPCI->FindDeviceTags(
        FDT_VendorID, E1000_PCI_VENDOR,
        FDT_DeviceID, E1000_PCI_DEVICE,
        TAG_END);

    if (!devBase->pciDevice) {
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    devBase->bar0 = devBase->pciDevice->GetResourceRange(0);
    if (!devBase->bar0) {
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    /* Read MAC from hardware */
    {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        uint32 ral = e1000_read32(devBase->pciDevice, mmio, E1000_REG_RAL);
        uint32 rah = e1000_read32(devBase->pciDevice, mmio, E1000_REG_RAH);
        devBase->mac[0] = (UBYTE)(ral        & 0xff);
        devBase->mac[1] = (UBYTE)((ral >> 8) & 0xff);
        devBase->mac[2] = (UBYTE)((ral >> 16)& 0xff);
        devBase->mac[3] = (UBYTE)((ral >> 24)& 0xff);
        devBase->mac[4] = (UBYTE)(rah        & 0xff);
        devBase->mac[5] = (UBYTE)((rah >> 8) & 0xff);

        if ((devBase->mac[0] == 0 && devBase->mac[1] == 0 && devBase->mac[2] == 0) ||
            (devBase->mac[0] == 0xff && devBase->mac[1] == 0xff && devBase->mac[2] == 0xff)) {
            devBase->mac[0] = 0x52; devBase->mac[1] = 0x54; devBase->mac[2] = 0x00;
            devBase->mac[3] = 0xE1; devBase->mac[4] = 0x00; devBase->mac[5] = 0x00;
        }

        devBase->hw_present = TRUE;
    }

    /* Quiesce device interrupts */
    {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
        (void)e1000_read32(devBase->pciDevice, mmio, E1000_REG_ICR);
    }

    /* Allocate DMA rings */
    {
        ULONG ring_bytes = V1000_RING_ENTRIES * V1000_DESC_SIZE;
        ULONG buf_bytes  = V1000_RING_ENTRIES * V1000_RX_BUFSIZE;

        ULONG rx_ring_alloc = ring_bytes + 16;
        ULONG tx_ring_alloc = ring_bytes + 16;
        ULONG buf_alloc     = buf_bytes  + 16;
        devBase->rx_ring_raw = iexec->AllocMem(rx_ring_alloc, MEMF_KICK | MEMF_CLEAR);
        devBase->tx_ring_raw = iexec->AllocMem(tx_ring_alloc, MEMF_KICK | MEMF_CLEAR);
        devBase->rx_buffers_raw = iexec->AllocMem(buf_alloc, MEMF_KICK | MEMF_CLEAR);
        devBase->rx_ring_raw_size = rx_ring_alloc;
        devBase->tx_ring_raw_size = tx_ring_alloc;
        devBase->rx_buffers_raw_size = buf_alloc;
        devBase->rx_ring = devBase->rx_ring_raw ? (APTR)(((ULONG)devBase->rx_ring_raw + 15UL) & ~15UL) : NULL;
        devBase->tx_ring = devBase->tx_ring_raw ? (APTR)(((ULONG)devBase->tx_ring_raw + 15UL) & ~15UL) : NULL;
        devBase->rx_buffers = devBase->rx_buffers_raw ? (APTR)(((ULONG)devBase->rx_buffers_raw + 15UL) & ~15UL) : NULL;

        if (devBase->rx_ring) {
            devBase->rx_ring_phys = v1000_dma_phys(iexec, devBase->rx_ring, ring_bytes, 0);
        }
        if (devBase->tx_ring) {
            devBase->tx_ring_phys = v1000_dma_phys(iexec, devBase->tx_ring, ring_bytes, DMA_ReadFromRAM);
        }
        if (devBase->rx_buffers) {
            devBase->rx_buffers_phys = v1000_dma_phys(iexec, devBase->rx_buffers, buf_bytes, 0);
        }

        ULONG scratch_alloc = V1000_RX_BUFSIZE + 16;
        devBase->tx_scratch_raw = iexec->AllocMem(scratch_alloc, MEMF_KICK | MEMF_CLEAR);
        devBase->tx_scratch_raw_size = scratch_alloc;
        devBase->tx_scratch = devBase->tx_scratch_raw
            ? (APTR)(((ULONG)devBase->tx_scratch_raw + 15UL) & ~15UL)
            : NULL;
        if (devBase->tx_scratch) {
            devBase->tx_scratch_phys = v1000_dma_phys(iexec, devBase->tx_scratch, V1000_RX_BUFSIZE, DMA_ReadFromRAM);
        }
    }

    /* Program RX ring */
    if (devBase->rx_ring && devBase->rx_buffers && devBase->rx_buffers_phys) {
        volatile UBYTE *desc = (volatile UBYTE *)devBase->rx_ring;
        ULONG buf_lo = devBase->rx_buffers_phys;
        for (int i = 0; i < V1000_RING_ENTRIES; i++) {
            volatile UBYTE *d = desc + (i * V1000_DESC_SIZE);
            poke_le32(d + 0, buf_lo + (i * V1000_RX_BUFSIZE));
            poke_le32(d + 4, 0);
            poke_le32(d + 8, 0);
            poke_le32(d + 12, 0);
        }
        v1000_wmb();

        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_RDBAL, devBase->rx_ring_phys);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_RDBAH, 0);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_RDLEN, V1000_RING_ENTRIES * V1000_DESC_SIZE);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_RDH,   0);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_RDT,   V1000_RING_ENTRIES - 1);
    }

    /* Program TX ring */
    if (devBase->tx_ring && devBase->tx_ring_phys) {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDBAL, devBase->tx_ring_phys);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDBAH, 0);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDLEN, V1000_RING_ENTRIES * V1000_DESC_SIZE);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDH,   0);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDT,   0);
    }

    /* Install IRQ handler */
    if (devBase->pciDevice) {
        devBase->irq_vector = devBase->pciDevice->MapInterrupt();

        if (devBase->irq_vector != 0) {
            devBase->irq_node.is_Node.ln_Type = NT_INTERRUPT;
            devBase->irq_node.is_Node.ln_Pri  = 0;
            devBase->irq_node.is_Node.ln_Name = (STRPTR)DEVNAME;
            devBase->irq_node.is_Data         = (APTR)devBase;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            devBase->irq_node.is_Code = (VOID (*)())v1000_isr;
#pragma GCC diagnostic pop

            BOOL ok = iexec->AddIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = ok;
        }
    }

    /* Start unit task */
    (void)v1000_task_start(devBase, iexec);

    v1000_log_close(iexec, &log);
    return (struct Library *)devBase;
}
