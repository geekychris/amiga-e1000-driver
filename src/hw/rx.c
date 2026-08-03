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
    if (!op || !base->IUtility) return FALSE;
    base->copy_to_calls++;
    base->last_copy_to_size = size;

    struct Hook *hook = op->sana2_hook ? op->sana2_hook : (struct Hook *)op->copy_to_buff;
    if (!hook) return FALSE;

    ULONG tag = op->sana2_hook ? S2_CopyToBuff : op->copy_to_tag;
    base->last_copy_to_ptr = hook;
    base->last_copy_to_tag = tag;

    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = ioreq->ios2_Data;
    msg.schm_From    = from;
    msg.schm_Size    = size;

    return (BOOL)(ULONG)base->IUtility->CallHookPkt(hook, ioreq, &msg);
}

void v1000_process_rx(struct Virte1000Base *base)
{
    if (!base->rx_ring || !base->bar0 || !base->IUtility) return;
    struct ExecIFace *IExec = base->IExec;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;

    uint32 rctl_now = e1000_read32(mmio, E1000_REG_RCTL);
    if (!(rctl_now & E1000_RCTL_EN)) {
        uint32 new_rctl = E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE
                        | E1000_RCTL_BAM | E1000_RCTL_SECRC;
        e1000_write32(mmio, E1000_REG_RCTL, new_rctl);
    }

#ifdef DEBUG
    uint32 rdh = e1000_read32(mmio, E1000_REG_RDH);
    uint32 rdt = e1000_read32(mmio, E1000_REG_RDT);
    uint32 rctl = e1000_read32(mmio, E1000_REG_RCTL);
#endif

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

        ULONG pkt_type = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];

        struct IOSana2Req *ioreq = NULL;
        struct V1000Opener *op   = NULL;
        IExec->ObtainSemaphore(&base->opener_lock);
        for (struct V1000Opener *o = (struct V1000Opener *)base->opener_list.mlh_Head;
             o->node.mln_Succ;
             o = (struct V1000Opener *)o->node.mln_Succ) {
            for (struct Node *n = (struct Node *)o->read_queue.mlh_Head;
                 n->ln_Succ;
                 n = n->ln_Succ) {
                struct IOSana2Req *r = (struct IOSana2Req *)n;
                if (r->ios2_PacketType == pkt_type || r->ios2_PacketType == 0 ||
                    (r->ios2_Req.io_Flags & SANA2IOF_RAW)) {
                    op = o;
                    ioreq = r;
                    IExec->Remove((struct Node *)r);
                    break;
                }
            }
            if (ioreq) break;
        }
        IExec->ReleaseSemaphore(&base->opener_lock);

        if (ioreq && op && (op->copy_to_buff || op->sana2_hook) && ioreq->ios2_Data) {
            for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
            for (int i = 0; i < 6; i++) ioreq->ios2_DstAddr[i] = rxbuf[i];
            ioreq->ios2_PacketType = pkt_type;

            BOOL is_raw = (ioreq->ios2_Req.io_Flags & SANA2IOF_RAW) != 0;
            APTR copy_ptr = is_raw ? (APTR)rxbuf : (APTR)(rxbuf + 14);
            ULONG copy_len = is_raw ? frame_len : payload_len;

            ioreq->ios2_DataLength = copy_len;

            BOOL ok = v1000_invoke_copy_to(base, op, ioreq, copy_ptr, copy_len);
            ioreq->ios2_Req.io_Error = ok ? 0 : S2ERR_NO_RESOURCES;
            IExec->ReplyMsg((struct Message *)ioreq);
            delivered_this_slot = ok;

#ifdef DEBUG
            IExec->DebugPrintF("[virte1000] RX: slot=%d frame_len=%lu payload=%lu ptype=0x%04lx\n"
                               "            HDR: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x delivered=%d\n",
                               slot, frame_len, payload_len, (unsigned long)ioreq->ios2_PacketType,
                               rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3], rxbuf[4], rxbuf[5],
                               rxbuf[6], rxbuf[7], rxbuf[8], rxbuf[9], rxbuf[10], rxbuf[11],
                               (int)ok);
        } else if (ioreq) {
            IExec->DebugPrintF("[virte1000] RX: slot=%d skipped (op=%p hook=%p copy_to=%p data=%p)\n",
                               slot, op, op ? op->sana2_hook : NULL,
                               op ? op->copy_to_buff : NULL, ioreq ? ioreq->ios2_Data : NULL);
#endif
        }
        if (delivered_this_slot) base->process_rx_delivered++;

        rd[12] = 0;
        IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_ClearD);
        e1000_write32(mmio, E1000_REG_RDT, slot);
    }
#ifdef DEBUG
    if (dd_seen_this_call > 0) {
        IExec->DebugPrintF("[virte1000] RX: SUMMARY dd_seen=%lu rdh=%lu rdt=%lu rctl=0x%08lx\n",
                           (unsigned long)dd_seen_this_call, (unsigned long)rdh, (unsigned long)rdt, (unsigned long)rctl);
    }
#endif
    base->process_rx_dd_seen = dd_seen_this_call;
}
