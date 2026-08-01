/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * Open.c — Device open implementation (_manager_Open).
 */

#include "virte1000.h"

#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/ports.h>

struct Virte1000Base *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    (void)flags;

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

    if (unitNum != 0) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
        ioreq->ios2_Req.io_Device = (struct Device *)-1;
        devBase->dev_Base.dd_Library.lib_OpenCnt--;
        return NULL;
    }

    ioreq->ios2_Req.io_Unit   = &devBase->v1000_unit;
    ioreq->ios2_Req.io_Error  = 0;
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    devBase->v1000_unit.unit_OpenCnt++;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    /* Allocate a V1000Opener for this session */
    struct V1000Opener *op = (struct V1000Opener *)devBase->IExec->AllocVecTags(
        sizeof(struct V1000Opener),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_END);

    if (op) {
        struct TagItem *tags = (struct TagItem *)ioreq->ios2_BufferManagement;
        if (tags && devBase->IUtility) {
            op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyToBuff32, 0, tags);
            op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff32 : 0;
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff16, 0, tags);
                op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff16 : 0;
            }
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff, 0, tags);
                op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff : 0;
            }

            op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyFromBuff32, 0, tags);
            op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff32 : 0;
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff16, 0, tags);
                op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff16 : 0;
            }
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff, 0, tags);
                op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff : 0;
            }
        }
        op->bm_cookie = ioreq->ios2_BufferManagement;
        devBase->IExec->NewList((struct List *)&op->read_queue);
        devBase->IExec->NewList((struct List *)&op->event_queue);
        devBase->IExec->ObtainSemaphore(&devBase->opener_lock);
        devBase->IExec->AddTail((struct List *)&devBase->opener_list,
                                 (struct Node *)&op->node);
        devBase->IExec->ReleaseSemaphore(&devBase->opener_lock);
    }

    return devBase;
}
