/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * Close.c — Device close implementation (_manager_Close).
 */

#include "virte1000.h"

#include <exec/errors.h>
#include <exec/ports.h>

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;

    devBase->IExec->DebugPrintF("[virte1000] Close: ioreq=%p BM=%p openCnt=%lu\n",
                                ioreq, ioreq ? ioreq->ios2_BufferManagement : NULL,
                                (unsigned long)devBase->dev_Base.dd_Library.lib_OpenCnt);

    {
        struct ExecIFace *IExec = devBase->IExec;
        struct V1000Opener *op, *found = NULL;
        APTR cookie = ioreq->ios2_BufferManagement;
        IExec->ObtainSemaphore(&devBase->opener_lock);
        for (op = (struct V1000Opener *)devBase->opener_list.mlh_Head;
             op->node.mln_Succ;
             op = (struct V1000Opener *)op->node.mln_Succ) {
            if (op->bm_cookie == cookie) { found = op; break; }
        }
        if (found) {
            struct Node *node;
            while ((node = IExec->RemHead((struct List *)&found->read_queue)) != NULL) {
                struct IOSana2Req *pending = (struct IOSana2Req *)node;
                pending->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)pending);
            }
            while ((node = IExec->RemHead((struct List *)&found->event_queue)) != NULL) {
                struct IOSana2Req *pending = (struct IOSana2Req *)node;
                pending->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)pending);
            }
            IExec->Remove((struct Node *)&found->node);
        }
        IExec->ReleaseSemaphore(&devBase->opener_lock);
        if (found) IExec->FreeVec(found);
    }

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;

    if (devBase->v1000_unit.unit_OpenCnt > 0)
        devBase->v1000_unit.unit_OpenCnt--;
    devBase->dev_Base.dd_Library.lib_OpenCnt--;

#ifdef DEBUG
    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = _manager_Expunge(Self);
    }
#else
    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0
        && (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP))
    {
        seglist = _manager_Expunge(Self);
    }
#endif

    return seglist;
}
