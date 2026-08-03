/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * hw/tx.c — Transmit packet processing & CopyFromBuff hook calls.
 */

#include "virte1000.h"

typedef BOOL (*V1000CopyFn)(APTR to, APTR from, ULONG size);

BOOL v1000_invoke_copy_from(struct Virte1000Base *base,
                           struct V1000Opener *op,
                           struct IOSana2Req *ioreq,
                           APTR to, ULONG size)
{
    if (!op) return FALSE;
    base->copy_from_calls++;
    base->last_copy_from_size = size;

    if (op->sana2_hook) {
        base->last_copy_from_ptr = op->sana2_hook;
        base->last_copy_from_tag = S2_CopyFromBuff;
        struct SANA2CopyHookMsg msg;
        msg.schm_Method  = S2_CopyFromBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To      = to;
        msg.schm_From    = ioreq->ios2_Data;
        msg.schm_Size    = size;
        return (BOOL)(ULONG)base->IUtility->CallHookPkt(
            op->sana2_hook, ioreq, &msg);
    }
    if (!op->copy_from_buff) return FALSE;
    base->last_copy_from_ptr = op->copy_from_buff;
    base->last_copy_from_tag = op->copy_from_tag;
    if (op->copy_from_tag == S2_CopyFromBuff) {
        V1000CopyFn fn = (V1000CopyFn)op->copy_from_buff;
        return fn(to, ioreq->ios2_Data, size);
    }
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = op->copy_from_tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = to;
    msg.schm_From    = ioreq->ios2_Data;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(
        (struct Hook *)op->copy_from_buff, ioreq, &msg);
}

BOOL v1000_transmit_packet(struct Virte1000Base *devBase, struct V1000Opener *op, struct IOSana2Req *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    if (devBase->state != V1000_STATE_ONLINE) {
        ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
        return FALSE;
    }

    ULONG len = ioreq->ios2_DataLength;
    if (len == 0 || len > 1514 || !ioreq->ios2_Data || !devBase->rx_ring) {
        ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        return FALSE;
    }
    ioreq->ios2_WireError = 0;

    if (!devBase->tx_scratch) {
        ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        return FALSE;
    }
    ULONG slot = devBase->tx_next_slot;
    UBYTE *dst = (UBYTE *)devBase->tx_scratch;

    if (!op && ioreq->ios2_BufferManagement) {
        IExec->ObtainSemaphore(&devBase->opener_lock);
        op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
        IExec->ReleaseSemaphore(&devBase->opener_lock);
    }
    BOOL cooked = (op != NULL) && (devBase->IUtility != NULL) &&
                  (op->sana2_hook != NULL || op->copy_from_buff != NULL);

    if (cooked) {
        for (int i = 0; i < 6; i++) dst[i]     = ioreq->ios2_DstAddr[i];
        for (int i = 0; i < 6; i++) dst[6 + i] = devBase->mac[i];
        uint16 etype = (uint16)(ioreq->ios2_PacketType & 0xFFFF);
        dst[12] = (UBYTE)(etype >> 8);
        dst[13] = (UBYTE)(etype & 0xFF);

        BOOL ok = v1000_invoke_copy_from(devBase, op, ioreq, dst + 14, len);
        if (!ok) {
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
            return FALSE;
        }
        len += 14;
    } else {
        UBYTE *src = (UBYTE *)ioreq->ios2_Data;
        for (ULONG i = 0; i < len; i++) dst[i] = src[i];
    }

    uint32 buf_phys = devBase->tx_scratch_phys;
    IExec->CacheClearE(dst, len, CACRF_ClearD);
    if (!buf_phys) {
        ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        return FALSE;
    }

    volatile UBYTE *d = (volatile UBYTE *)devBase->tx_ring + (slot * V1000_DESC_SIZE);
    poke_le32(d +  0, buf_phys);
    poke_le32(d +  4, 0);
    poke_le32(d +  8, (uint32)len
                      | ((uint32)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS
                                  | E1000_TXD_CMD_RS) << 24));
    poke_le32(d + 12, 0);

    volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
    uint32 tctl_before = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TCTL);
    if (!(tctl_before & E1000_TCTL_EN)) {
        uint32 new_tctl = E1000_TCTL_EN | E1000_TCTL_PSP
                        | E1000_TCTL_CT(0x0F) | E1000_TCTL_COLD(0x40);
        e1000_write32(devBase->pciDevice, mmio, E1000_REG_TCTL, new_tctl);
        tctl_before = new_tctl;
    }

    uint32 tdt_before = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TDT);
    uint32 tdh_before = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TDH);

    ULONG new_tdt = (slot + 1) % V1000_RING_ENTRIES;
    e1000_write32(devBase->pciDevice, mmio, E1000_REG_TDT, new_tdt);
    devBase->tx_next_slot = new_tdt;

    ULONG expected_tdh = (slot + 1) % V1000_RING_ENTRIES;
    BOOL done = FALSE;
    int iters = 0;
    for (iters = 0; iters < 50000; iters++) {
        IExec->CacheClearE((APTR)d, V1000_DESC_SIZE, CACRF_InvalidateD);
        uint32 tdh_check = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TDH);
        if ((d[12] & E1000_TXD_STAT_DD) || (tdh_check == expected_tdh)) {
            done = TRUE;
            break;
        }
        __asm__ volatile ("" : : : "memory");
    }

    uint32 tdh_now  = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TDH);
    uint32 tdt_now  = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TDT);
    uint32 tctl_now = e1000_read32(devBase->pciDevice, mmio, E1000_REG_TCTL);
    UBYTE  stat_byte = d[12];

    IExec->DebugPrintF("[virte1000] TX: slot=%lu len=%lu cooked=%d phys=0x%08lx TCTL=0x%08lx\n"
                       "            HDR: dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x ptype=0x%02x%02x\n"
                       "            REG: TDH_b=%lu TDT_b=%lu -> TDH_a=%lu TDT_a=%lu (expected=%lu) stat=0x%02x iters=%d done=%d\n",
                       slot, len, (int)cooked, (unsigned long)buf_phys, (unsigned long)tctl_now,
                       dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                       dst[6], dst[7], dst[8], dst[9], dst[10], dst[11],
                       dst[12], dst[13],
                       (unsigned long)tdh_before, (unsigned long)tdt_before,
                       (unsigned long)tdh_now, (unsigned long)tdt_now,
                       (unsigned long)expected_tdh, (unsigned int)stat_byte, iters, (int)done);

    ioreq->ios2_DataLength = (ULONG)tdh_now | ((ULONG)tdt_now << 16);
    ioreq->ios2_PacketType = tctl_now;
    ioreq->ios2_WireError  = ((ULONG)stat_byte)
                           | ((ULONG)(done ? 1 : 0) << 8)
                           | 0x7000;

    if (!done) {
        ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
        return FALSE;
    }

    return TRUE;
}
