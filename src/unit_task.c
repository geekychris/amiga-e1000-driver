/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * unit_task.c — Unit task management & event dispatch loop.
 */

#include "virte1000.h"

#include <exec/ports.h>

void v1000_task_body(void)
{
    struct ExecIFace *IExec = ((struct ExecIFace *)
        ((*(struct ExecBase **)4)->MainInterface));
    struct Task *self = IExec->FindTask(NULL);
    struct Virte1000Base *base = (struct Virte1000Base *)self->tc_UserData;

    int8 sig = IExec->AllocSignal(-1);
    if (sig < 0) {
        base->task_ready_ok = FALSE;
        IExec->Signal(base->parent_task, base->parent_ready_mask);
        return;
    }
    ULONG sig_mask = 1UL << sig;
    base->unit_signal_mask = sig_mask;

    base->begin_port = (struct MsgPort *)IExec->AllocSysObjectTags(
        ASOT_PORT, ASOPORT_AllocSig, TRUE, TAG_END);
    if (!base->begin_port) {
        IExec->FreeSignal(sig);
        base->unit_signal_mask = 0;
        base->task_ready_ok = FALSE;
        IExec->Signal(base->parent_task, base->parent_ready_mask);
        return;
    }
    base->begin_port_mask = 1UL << base->begin_port->mp_SigBit;

    base->task_ready_ok = TRUE;
    IExec->Signal(base->parent_task, base->parent_ready_mask);

    ULONG wait_mask = sig_mask | base->begin_port_mask | SIGBREAKF_CTRL_C;
    while (!base->task_shutdown) {
        ULONG got = IExec->Wait(wait_mask);
        if (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;

        if (got & base->begin_port_mask) {
            struct IOSana2Req *req;
            while ((req = (struct IOSana2Req *)IExec->GetMsg(base->begin_port)) != NULL) {
                v1000_dispatch_ioreq(base, req);
            }
        }

        if (got & sig_mask) {
            IExec->SetSignal(0, sig_mask);
            base->task_wake_count++;
            v1000_process_rx(base);
        }
    }

    if (base->begin_port) {
        IExec->FreeSysObject(ASOT_PORT, base->begin_port);
        base->begin_port = NULL;
        base->begin_port_mask = 0;
    }
    IExec->FreeSignal(sig);
    base->unit_signal_mask = 0;
    base->unit_task = NULL;
}

BOOL v1000_task_start(struct Virte1000Base *base, struct ExecIFace *iexec)
{
    int8 ready_bit = iexec->AllocSignal(-1);
    if (ready_bit < 0) return FALSE;

    base->task_shutdown     = FALSE;
    base->task_ready_ok     = FALSE;
    base->parent_task       = iexec->FindTask(NULL);
    base->parent_ready_mask = 1UL << ready_bit;

    iexec->Forbid();
    struct Task *t = iexec->CreateTaskTags("virte1000-unit", 20,
                                           v1000_task_body, 8192,
                                           TAG_END);
    if (t) t->tc_UserData = (APTR)base;
    base->unit_task = t;
    iexec->Permit();

    if (!t) { iexec->FreeSignal(ready_bit); return FALSE; }

    (void)iexec->Wait(base->parent_ready_mask);
    iexec->FreeSignal(ready_bit);
    return base->task_ready_ok;
}

void v1000_task_stop(struct Virte1000Base *base, struct ExecIFace *IExec)
{
    if (!base->unit_task) return;
    base->task_shutdown = TRUE;
    IExec->Signal(base->unit_task, SIGBREAKF_CTRL_C);
    ULONG patience = 100000;
    while (base->unit_task && patience-- > 0) {
        IExec->Forbid();
        IExec->Permit();
    }
}
