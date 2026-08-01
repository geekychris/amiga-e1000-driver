/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * hw/pci.c — PCI bus enumeration and MMIO BAR mapping.
 */

#include "virte1000.h"

#define E1000_PCI_VENDOR   0x8086
#define E1000_PCI_DEVICE   0x100E

BOOL v1000_pci_find(struct Virte1000Base *base)
{
    struct ExecIFace *IExec = base->IExec;

    base->ExpansionBase = IExec->OpenLibrary("expansion.library", 53);
    if (!base->ExpansionBase) return FALSE;

    base->IPCI = (struct PCIIFace *)IExec->GetInterface(base->ExpansionBase, "pci", 1, NULL);
    if (!base->IPCI) return FALSE;

    base->pciDevice = base->IPCI->FindDeviceTags(
        FDT_VendorID, E1000_PCI_VENDOR,
        FDT_DeviceID, E1000_PCI_DEVICE,
        TAG_END);
    if (!base->pciDevice) return FALSE;

    base->bar0 = base->pciDevice->GetResourceRange(0);
    if (!base->bar0) return FALSE;

    return TRUE;
}
