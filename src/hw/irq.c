/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * hw/irq.c — Interrupt handler and PCI INTx management.
 */

#include "virte1000.h"

uint32 v1000_isr(struct ExceptionContext *ctx, struct ExecBase *sysbase, APTR is_Data)
{
    struct Virte1000Base *base = (struct Virte1000Base *)is_Data;
    (void)ctx; (void)sysbase;

    if (!base || !base->bar0) return 0;

    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    uint32 icr = e1000_read32(mmio, E1000_REG_ICR);
    if (icr == 0)
        return 0;               /* not ours (or nothing pending) */

    base->irq_counter++;
    base->last_icr = icr;

    if (base->unit_task && base->unit_signal_mask) {
        struct ExecIFace *IExec = base->IExec;
        IExec->Signal(base->unit_task, base->unit_signal_mask);
    }
    return 1;                   /* claimed */
}
