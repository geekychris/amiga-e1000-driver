/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * Expunge.c — Device expunge implementation (_manager_Expunge).
 */

#include "virte1000.h"

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;
    BPTR seglist = (BPTR)NULL;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = devBase->dev_SegList;
        IExec->Remove((struct Node *)devBase);

        if (devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
            (void)e1000_read32(mmio, E1000_REG_ICR);
        }
        if (devBase->irq_installed) {
            IExec->RemIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = FALSE;
        }

        v1000_task_stop(devBase, IExec);

        {
            struct V1000Opener *op;
            IExec->ObtainSemaphore(&devBase->opener_lock);
            while ((op = (struct V1000Opener *)
                    IExec->RemHead((struct List *)&devBase->opener_list)) != NULL) {
                IExec->FreeVec(op);
            }
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }

        if (devBase->tx_scratch_raw) { IExec->FreeMem(devBase->tx_scratch_raw, devBase->tx_scratch_raw_size); devBase->tx_scratch_raw = NULL; devBase->tx_scratch = NULL; }
        if (devBase->rx_buffers_raw) { IExec->FreeMem(devBase->rx_buffers_raw, devBase->rx_buffers_raw_size); devBase->rx_buffers_raw = NULL; devBase->rx_buffers = NULL; }
        if (devBase->tx_ring_raw)    { IExec->FreeMem(devBase->tx_ring_raw,    devBase->tx_ring_raw_size);    devBase->tx_ring_raw    = NULL; devBase->tx_ring    = NULL; }
        if (devBase->rx_ring_raw)    { IExec->FreeMem(devBase->rx_ring_raw,    devBase->rx_ring_raw_size);    devBase->rx_ring_raw    = NULL; devBase->rx_ring    = NULL; }

        if (devBase->bar0) {
            devBase->pciDevice->FreeResourceRange(devBase->bar0);
            devBase->bar0 = NULL;
        }
        if (devBase->pciDevice) {
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }
        if (devBase->IPCI) {
            IExec->DropInterface((struct Interface *)devBase->IPCI);
            devBase->IPCI = NULL;
        }
        if (devBase->ExpansionBase) {
            IExec->CloseLibrary(devBase->ExpansionBase);
            devBase->ExpansionBase = NULL;
        }
        if (devBase->IUtility) {
            IExec->DropInterface((struct Interface *)devBase->IUtility);
            devBase->IUtility = NULL;
        }
        if (devBase->UtilityBase) {
            IExec->CloseLibrary(devBase->UtilityBase);
            devBase->UtilityBase = NULL;
        }
        if (devBase->IDOS) {
            IExec->DropInterface((struct Interface *)devBase->IDOS);
            devBase->IDOS = NULL;
        }
        if (devBase->DOSBase) {
            IExec->CloseLibrary(devBase->DOSBase);
            devBase->DOSBase = NULL;
        }
    }

    return seglist;
}
