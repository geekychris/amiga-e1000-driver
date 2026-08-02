/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * hw/rx.c — Receive packet processing & CopyToBuff hook calls.
 */

#include "virte1000.h"

typedef BOOL (*V1000CopyFn)(APTR to, APTR from, ULONG size);

BOOL v1000_invoke_copy_to(struct Virte1000Base *base,
                         struct V1000Opener *op,
                         struct IOSana2Req *ioreq,
                         APTR from, ULONG size)
{
    if (!op) return FALSE;
    base->copy_to_calls++;
    base->last_copy_to_size = size;

    if (op->sana2_hook) {
        base->last_copy_to_ptr = op->sana2_hook;
        base->last_copy_to_tag = S2_CopyToBuff;
        struct SANA2CopyHookMsg msg;
        msg.schm_Method  = S2_CopyToBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To      = ioreq->ios2_Data;
        msg.schm_From    = from;
        msg.schm_Size    = size;
        return (BOOL)(ULONG)base->IUtility->CallHookPkt(
            op->sana2_hook, ioreq, &msg);
    }

    if (!op->copy_to_buff) return FALSE;
    base->last_copy_to_ptr = op->copy_to_buff;
    base->last_copy_to_tag = op->copy_to_tag;
    if (op->copy_to_tag == S2_CopyToBuff) {
        V1000CopyFn fn = (V1000CopyFn)op->copy_to_buff;
        return fn(ioreq->ios2_Data, from, size);
    }
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = op->copy_to_tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = ioreq->ios2_Data;
    msg.schm_From    = from;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(
        (struct Hook *)op->copy_to_buff, ioreq, &msg);
}

void v1000_process_rx(struct Virte1000Base *base)
{
    if (!base->rx_ring || !base->bar0 || !base->IUtility) return;
    struct ExecIFace *IExec = base->IExec;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;

    ULONG dd_seen_this_call = 0;
    for (int slot = 0; slot < V1000_RING_ENTRIES; slot++) {
        volatile UBYTE *rd = (volatile UBYTE *)base->rx_ring + slot * V1000_DESC_SIZE;

        IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_InvalidateD);

        if (!(rd[12] & 0x01)) continue;
        dd_seen_this_call++;

        ULONG frame_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
        if (frame_len < 14 || frame_len > V1000_RX_BUFSIZE) {
            rd[12] = 0;
            IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_ClearD);
            e1000_write32(mmio, E1000_REG_RDT, slot);
            continue;
        }
        UBYTE *rxbuf = (UBYTE *)base->rx_buffers + slot * V1000_RX_BUFSIZE;
        IExec->CacheClearE(rxbuf, frame_len, CACRF_InvalidateD);

        ULONG payload_len = frame_len - 14;
        BOOL delivered_this_slot = FALSE;

        struct IOSana2Req *ioreq = NULL;
        struct V1000Opener *op   = NULL;
        IExec->ObtainSemaphore(&base->opener_lock);
        for (struct V1000Opener *o = (struct V1000Opener *)base->opener_list.mlh_Head;
             o->node.mln_Succ;
             o = (struct V1000Opener *)o->node.mln_Succ) {
            if (o->read_queue.mlh_Head && o->read_queue.mlh_Head->mln_Succ) {
                op = o;
                ioreq = (struct IOSana2Req *)IExec->RemHead((struct List *)&o->read_queue);
                break;
            }
        }
        IExec->ReleaseSemaphore(&base->opener_lock);

        if (ioreq && op && (op->copy_to_buff || op->sana2_hook) && ioreq->ios2_Data) {
            for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
            for (int i = 0; i < 6; i++) ioreq->ios2_DstAddr[i] = rxbuf[i];
            ioreq->ios2_PacketType = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];
            ioreq->ios2_DataLength = payload_len;

            BOOL ok = v1000_invoke_copy_to(base, op, ioreq, rxbuf + 14, payload_len);
            ioreq->ios2_Req.io_Error = ok ? 0 : S2ERR_NO_RESOURCES;
            IExec->ReplyMsg((struct Message *)ioreq);
            delivered_this_slot = ok;

            IExec->DebugPrintF("[virte1000] RX: slot=%d frame_len=%lu payload=%lu ptype=0x%04lx delivered=%d\n",
                               slot, frame_len, payload_len, (unsigned long)ioreq->ios2_PacketType, (int)ok);
        } else if (ioreq) {
            IExec->DebugPrintF("[virte1000] RX: slot=%d skipped (op=%p hook=%p copy_to=%p data=%p)\n",
                               slot, op, op ? op->sana2_hook : NULL,
                               op ? op->copy_to_buff : NULL, ioreq ? ioreq->ios2_Data : NULL);
        }
        if (delivered_this_slot) base->process_rx_delivered++;

        rd[12] = 0;
        IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_ClearD);
        e1000_write32(mmio, E1000_REG_RDT, slot);
    }
    base->process_rx_dd_seen = dd_seen_this_call;
}
