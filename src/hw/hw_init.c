/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * hw/hw_init.c — Hardware initialization, ring management, and power/online states.
 */

#include "virte1000.h"

#define E1000_RCTL_EN     (1U << 1)
#define E1000_RCTL_UPE    (1U << 3)
#define E1000_RCTL_MPE    (1U << 4)
#define E1000_RCTL_BAM    (1U << 15)
#define E1000_RCTL_SECRC  (1U << 26)
#define E1000_TCTL_EN     (1U << 1)
#define E1000_TCTL_PSP    (1U << 3)
#define E1000_TCTL_CT(v)   (((v) & 0xff)  << 4)
#define E1000_TCTL_COLD(v) (((v) & 0x3ff) << 12)
#define E1000_INTR_TXDW    (1U << 0)
#define E1000_INTR_LSC     (1U << 2)
#define E1000_INTR_RXT0    (1U << 7)

uint32 v1000_dma_phys(struct ExecIFace *IExec, void *virt, ULONG size, ULONG flags)
{
    uint32 nentries = IExec->StartDMA(virt, size, flags);
    if (nentries == 0) return 0;
    struct DMAEntry *dlist = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, nentries, TAG_DONE);
    if (!dlist) {
        IExec->EndDMA(virt, size, flags | DMAF_NoModify);
        return 0;
    }
    IExec->GetDMAList(virt, size, flags, dlist);
    uint32 phys = (uint32)dlist[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, dlist);
    return phys;
}

void v1000_online_hw(struct Virte1000Base *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    e1000_write32(base->pciDevice, mmio, E1000_REG_RCTL,
                  E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE
                  | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write32(base->pciDevice, mmio, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP
                                       | E1000_TCTL_CT(0x0F) | E1000_TCTL_COLD(0x40));
    e1000_write32(base->pciDevice, mmio, E1000_REG_IMS, E1000_INTR_LSC | E1000_INTR_RXT0 | E1000_INTR_TXDW);
}

void v1000_offline_hw(struct Virte1000Base *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    e1000_write32(base->pciDevice, mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
    (void)e1000_read32(base->pciDevice, mmio, E1000_REG_ICR);
    e1000_write32(base->pciDevice, mmio, E1000_REG_RCTL, 0);
    e1000_write32(base->pciDevice, mmio, E1000_REG_TCTL, 0);
}
