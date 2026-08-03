/*
 * virte1000.device — SANA-II network device driver for Intel e1000
 *
 * BeginIO.c — BeginIO and AbortIO implementations + command routing.
 */

#include "virte1000.h"

#include <exec/errors.h>
#include <exec/ports.h>
#include <devices/newstyle.h>
#include <stddef.h>

/* NSCMD_DEVICEQUERY supported-commands list. */
static const UWORD v1000_supported_cmds[] = {
    NSCMD_DEVICEQUERY,
    CMD_READ,
    CMD_WRITE,
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ONLINE,
    S2_OFFLINE,
    S2_BROADCAST,
    S2_READORPHAN,
    S2_ONEVENT,
    S2_GETGLOBALSTATS,
    S2_TRACKTYPE,
    S2_UNTRACKTYPE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,
    0
};

struct V1000Opener *v1000_find_opener(struct Virte1000Base *base, APTR bm_cookie)
{
    struct V1000Opener *op;
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        if (op->bm_cookie == bm_cookie) return op;
    }
    return NULL;
}

void v1000_signal_event(struct Virte1000Base *base, ULONG event_mask)
{
    struct ExecIFace *IExec = base->IExec;
    struct V1000Opener *op;

    IExec->ObtainSemaphore(&base->opener_lock);
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ)
    {
        struct Node *curr = op->event_queue.mlh_Head;
        while (curr->ln_Succ) {
            struct Node *next = curr->ln_Succ;
            struct IOSana2Req *ioreq = (struct IOSana2Req *)curr;
            if (ioreq->ios2_WireError & event_mask) {
                IExec->Remove(curr);
                ioreq->ios2_Req.io_Error = 0;
                ioreq->ios2_WireError    = event_mask;
                IExec->ReplyMsg((struct Message *)ioreq);
            }
            curr = next;
        }
    }
    IExec->ReleaseSemaphore(&base->opener_lock);
}

void v1000_dispatch_ioreq(struct Virte1000Base *devBase, struct IOSana2Req *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;
    devBase->last_dispatched_cmd = ioreq->ios2_Req.io_Command;

    switch (ioreq->ios2_Req.io_Command) {

    case S2_DEVICEQUERY: {
        struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) q = (struct Sana2DeviceQuery *)ioreq->ios2_Data;
        if (!q) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }

        devBase->deviceq_call_count++;
        devBase->deviceq_sizeavailable = q->SizeAvailable;

        ULONG avail  = q->SizeAvailable;
        ULONG supply = sizeof(struct Sana2DeviceQuery);
        if (avail > 0 && supply > avail) supply = avail;

        q->SizeSupplied   = supply;
        q->DevQueryFormat = 0;
        q->DeviceLevel    = 0;
        if (supply >= offsetof(struct Sana2DeviceQuery, AddrFieldSize) + sizeof(UWORD))
            q->AddrFieldSize = 48;
        if (supply >= offsetof(struct Sana2DeviceQuery, MTU) + sizeof(ULONG))
            q->MTU = 1500;
        if (supply >= offsetof(struct Sana2DeviceQuery, BPS) + sizeof(ULONG))
            q->BPS = 1000000000UL;
        if (supply >= offsetof(struct Sana2DeviceQuery, HardwareType) + sizeof(ULONG))
            q->HardwareType = S2WireType_Ethernet;
        if (supply >= offsetof(struct Sana2DeviceQuery, RawMTU) + sizeof(ULONG))
            q->RawMTU = 1514;

        IExec->DebugPrintF("[virte1000] S2_DEVICEQUERY: avail=%lu supply=%lu HWType=%lu MTU=%lu\n",
                           avail, supply, (unsigned long)q->HardwareType, (unsigned long)q->MTU);
        break;
    }

    case NSCMD_DEVICEQUERY: {
        struct IOStdReq *sreq = (struct IOStdReq *)ioreq;
        ULONG want = sizeof(struct NSDeviceQueryResult);
        if (!sreq->io_Data || sreq->io_Length < want) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
            break;
        }
        struct NSDeviceQueryResult *r = (struct NSDeviceQueryResult *)sreq->io_Data;
        r->DevQueryFormat    = 0;
        r->SizeAvailable     = want;
        r->DeviceType        = NSDEVTYPE_SANA2;
        r->DeviceSubType     = 0;
        r->SupportedCommands = (UWORD *)v1000_supported_cmds;
        sreq->io_Actual      = want;
        break;
    }

    case V1000_DBG_RECV: {
        if (devBase->state != V1000_STATE_ONLINE || !devBase->rx_ring) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile UBYTE *rr = (volatile UBYTE *)devBase->rx_ring;
        volatile UBYTE *rd = rr + (0 * V1000_DESC_SIZE);
        if (!(rd[12] & 0x01)) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
            break;
        }
        ULONG frame_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
        if (frame_len < 14 || frame_len > V1000_RX_BUFSIZE) {
            rd[12] = 0;
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            break;
        }
        UBYTE *rxbuf = (UBYTE *)devBase->rx_buffers + (0 * V1000_RX_BUFSIZE);

        for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
        ioreq->ios2_PacketType = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];
        ULONG payload_len = frame_len - 14;

        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        if (op && op->copy_to_buff && devBase->IUtility && ioreq->ios2_Data) {
            BOOL ok = v1000_invoke_copy_to(devBase, op, ioreq, rxbuf + 14, payload_len);
            if (!ok) {
                ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
            }
        }

        ioreq->ios2_DataLength = payload_len;
        rd[12] = 0;
        break;
    }

    case V1000_DBG_STATUS: {
        ioreq->ios2_DataLength = devBase->irq_counter;
        ioreq->ios2_WireError  = devBase->last_icr;
        ioreq->ios2_PacketType = devBase->state;
        if (devBase->state == V1000_STATE_ONLINE && devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            uint32 tpt = e1000_read32(mmio, E1000_REG_TPT);
            uint32 rdh = e1000_read32(mmio, E1000_REG_RDH);
            ioreq->ios2_SrcAddr[0] = (UBYTE)(tpt >> 24);
            ioreq->ios2_SrcAddr[1] = (UBYTE)(tpt >> 16);
            ioreq->ios2_SrcAddr[2] = (UBYTE)(tpt >> 8);
            ioreq->ios2_SrcAddr[3] = (UBYTE)(tpt);
            ioreq->ios2_SrcAddr[4] = (UBYTE)(rdh >> 24);
            ioreq->ios2_SrcAddr[5] = (UBYTE)(rdh >> 16);
            ioreq->ios2_SrcAddr[6] = (UBYTE)(rdh >> 8);
            ioreq->ios2_SrcAddr[7] = (UBYTE)(rdh);

            ULONG dd_count = 0;
            if (devBase->rx_ring) {
                volatile UBYTE *rr = (volatile UBYTE *)devBase->rx_ring;
                for (int i = 0; i < V1000_RING_ENTRIES; i++) {
                    volatile UBYTE *rd = rr + (i * V1000_DESC_SIZE);
                    if (rd[12] & 0x01) {
                        dd_count++;
                    }
                }
            }
            ioreq->ios2_DstAddr[0] = (UBYTE)(dd_count >> 24);
            ioreq->ios2_DstAddr[1] = (UBYTE)(dd_count >> 16);
            ioreq->ios2_DstAddr[2] = (UBYTE)(dd_count >> 8);
            ioreq->ios2_DstAddr[3] = (UBYTE)(dd_count);
        }
        break;
    }

    case V1000_DBG_CMDLOG: {
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        if (out && ioreq->ios2_DataLength >= 32 * 40) {
            ULONG head = devBase->cmdlog_head;
            for (int i = 0; i < 32; i++) {
                UBYTE *p = out + (i * 40);
                p[0]  = (UBYTE)(devBase->cmdlog[i].cmd >> 8);
                p[1]  = (UBYTE)(devBase->cmdlog[i].cmd);
                p[2]  = devBase->cmdlog[i].flags_in;
                p[3]  = 0;
                p[4]  = (UBYTE)(devBase->cmdlog[i].ioerr >> 8);
                p[5]  = (UBYTE)(devBase->cmdlog[i].ioerr);
                p[6]  = (UBYTE)(devBase->cmdlog[i].ptype_in >> 8);
                p[7]  = (UBYTE)(devBase->cmdlog[i].ptype_in);
                p[8]  = (UBYTE)(devBase->cmdlog[i].wire >> 24);
                p[9]  = (UBYTE)(devBase->cmdlog[i].wire >> 16);
                p[10] = (UBYTE)(devBase->cmdlog[i].wire >> 8);
                p[11] = (UBYTE)(devBase->cmdlog[i].wire);
                p[12] = (UBYTE)(devBase->cmdlog[i].data_in >> 24);
                p[13] = (UBYTE)(devBase->cmdlog[i].data_in >> 16);
                p[14] = (UBYTE)(devBase->cmdlog[i].data_in >> 8);
                p[15] = (UBYTE)(devBase->cmdlog[i].data_in);
                p[16] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 24);
                p[17] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 16);
                p[18] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 8);
                p[19] = (UBYTE)(devBase->cmdlog[i].datalen_out);
                p[20] = (UBYTE)(devBase->cmdlog[i].bm_in >> 24);
                p[21] = (UBYTE)(devBase->cmdlog[i].bm_in >> 16);
                p[22] = (UBYTE)(devBase->cmdlog[i].bm_in >> 8);
                p[23] = (UBYTE)(devBase->cmdlog[i].bm_in);
                for (int b = 0; b < 6; b++) {
                    p[24 + b] = devBase->cmdlog[i].src_out[b];
                    p[30 + b] = devBase->cmdlog[i].dst_out[b];
                }
                p[36] = p[37] = p[38] = p[39] = 0;
            }
            ioreq->ios2_DataLength = head;
        }
        break;
    }

    case V1000_DBG_DUMPTX: {
        if (devBase->tx_scratch) {
            UBYTE *s = (UBYTE *)devBase->tx_scratch;
            for (int i = 0; i < 16; i++) ioreq->ios2_SrcAddr[i] = s[i];
        }
        break;
    }

    case V1000_DBG_CFGBUF: {
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        ULONG c = devBase->config_call_count;
        ioreq->ios2_SrcAddr[0] = (UBYTE)(c >> 24);
        ioreq->ios2_SrcAddr[1] = (UBYTE)(c >> 16);
        ioreq->ios2_SrcAddr[2] = (UBYTE)(c >>  8);
        ioreq->ios2_SrcAddr[3] = (UBYTE)c;
        for (int i = 4; i < 16; i++) ioreq->ios2_SrcAddr[i] = 0;
        if (out && ioreq->ios2_DataLength >= 160) {
            volatile UBYTE *bsrc = devBase->config_before;
            volatile UBYTE *asrc = devBase->config_after;
            for (int i = 0; i < 80; i++) out[i]      = bsrc[i];
            for (int i = 0; i < 80; i++) out[i + 80] = asrc[i];
        }
        break;
    }

    case V1000_DBG_DQBUF: {
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        ULONG c = devBase->deviceq_call_count;
        ULONG s = devBase->deviceq_sizeavailable;
        ioreq->ios2_SrcAddr[0] = (UBYTE)(c >> 24);
        ioreq->ios2_SrcAddr[1] = (UBYTE)(c >> 16);
        ioreq->ios2_SrcAddr[2] = (UBYTE)(c >>  8);
        ioreq->ios2_SrcAddr[3] = (UBYTE)c;
        ioreq->ios2_SrcAddr[4] = (UBYTE)(s >> 24);
        ioreq->ios2_SrcAddr[5] = (UBYTE)(s >> 16);
        ioreq->ios2_SrcAddr[6] = (UBYTE)(s >>  8);
        ioreq->ios2_SrcAddr[7] = (UBYTE)s;
        for (int i = 8; i < 16; i++) ioreq->ios2_SrcAddr[i] = 0;
        if (out && ioreq->ios2_DataLength >= 96) {
            volatile UBYTE *bsrc = devBase->deviceq_before;
            volatile UBYTE *asrc = devBase->deviceq_after;
            for (int i = 0; i < 48; i++) out[i]      = bsrc[i];
            for (int i = 0; i < 48; i++) out[i + 48] = asrc[i];
        }
        break;
    }

    case V1000_DBG_FIRE_IRQ: {
        if (devBase->state != V1000_STATE_ONLINE || !devBase->bar0) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_ICS, E1000_ICR_LSC);
        ioreq->ios2_DataLength = devBase->irq_counter;
        break;
    }

    case S2_CONFIGINTERFACE: {
        IExec->DebugPrintF("[virte1000] S2_CONFIGINTERFACE: state=%lu\n", devBase->state);
        devBase->config_call_count++;

        BOOL non_zero_mac = FALSE;
        for (int i = 0; i < 6; i++) {
            if (ioreq->ios2_SrcAddr[i] != 0) { non_zero_mac = TRUE; break; }
        }
        if (non_zero_mac) {
            for (int i = 0; i < 6; i++) devBase->mac[i] = ioreq->ios2_SrcAddr[i];
        }

        /* Write MAC to hardware RAL/RAH registers */
        if (devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            uint32 ral = (uint32)devBase->mac[0]
                       | ((uint32)devBase->mac[1] << 8)
                       | ((uint32)devBase->mac[2] << 16)
                       | ((uint32)devBase->mac[3] << 24);
            uint32 rah = (uint32)devBase->mac[4]
                       | ((uint32)devBase->mac[5] << 8)
                       | (1U << 31); /* AV (Address Valid) bit */
            e1000_write32(mmio, E1000_REG_RAL, ral);
            e1000_write32(mmio, E1000_REG_RAH, rah);
        }

        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = devBase->mac[i];
            ioreq->ios2_DstAddr[i] = 0xFF;
        }

        v1000_online_hw(devBase);
        devBase->state = V1000_STATE_ONLINE;
        v1000_signal_event(devBase, S2EVENT_ONLINE);
        IExec->DebugPrintF("[virte1000] S2_CONFIGINTERFACE: MAC=%02x:%02x:%02x:%02x:%02x:%02x -> ONLINE\n",
                           devBase->mac[0], devBase->mac[1], devBase->mac[2],
                           devBase->mac[3], devBase->mac[4], devBase->mac[5]);
        break;
    }

    case S2_ONLINE: {
        IExec->DebugPrintF("[virte1000] S2_ONLINE: state=%lu\n", devBase->state);
        v1000_online_hw(devBase);
        devBase->state = V1000_STATE_ONLINE;
        v1000_signal_event(devBase, S2EVENT_ONLINE);
        break;
    }

    case S2_OFFLINE: {
        IExec->DebugPrintF("[virte1000] S2_OFFLINE: state=%lu\n", devBase->state);
        if (devBase->state == V1000_STATE_ONLINE) {
            v1000_offline_hw(devBase);
            devBase->state = V1000_STATE_CONFIGURED;
        }
        v1000_signal_event(devBase, S2EVENT_OFFLINE);
        break;
    }

    case V1000_DBG_SEND:
    case CMD_WRITE:
    case S2_BROADCAST: {
        if (v1000_transmit_packet(devBase, NULL, ioreq)) {
            ioreq->ios2_Req.io_Error = 0;
        }
        break;
    }

    case S2_READORPHAN:
    case CMD_READ: {
        if (devBase->state != V1000_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
        } else {
            IExec->ObtainSemaphore(&devBase->opener_lock);
        }
        if (!op) {
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        IExec->AddTail((struct List *)&op->read_queue,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->ReleaseSemaphore(&devBase->opener_lock);

        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }

    case S2_GETSTATIONADDRESS: {
        if (!devBase->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = devBase->mac[i];
            ioreq->ios2_DstAddr[i] = devBase->mac[i];
        }
        break;
    }

    case S2_GETGLOBALSTATS: {
        struct Sana2DeviceStats *s = (struct Sana2DeviceStats *)ioreq->ios2_StatData;
        if (!s) s = (struct Sana2DeviceStats *)ioreq->ios2_Data;
        if (!s) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        s->PacketsReceived      = devBase->copy_to_calls;
        s->PacketsSent          = devBase->copy_from_calls;
        s->BadData              = 0;
        s->Overruns             = 0;
        s->UnknownTypesReceived = 0;
        s->Reconfigurations     = devBase->config_call_count;
        IExec->DebugPrintF("[virte1000] S2_GETGLOBALSTATS: rx=%lu tx=%lu\n",
                           devBase->copy_to_calls, devBase->copy_from_calls);
        break;
    }

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        break;

    case S2_SANA2HOOK: {
        struct Sana2Hook *hook = (struct Sana2Hook *)ioreq->ios2_Data;
        if (!hook) hook = (struct Sana2Hook *)ioreq->ios2_StatData;
        if (!hook) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        if (!op) {
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        op->sana2_hook = &hook->s2h_Hook;
        break;
    }

    case S2_ONEVENT: {
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
        } else {
            IExec->ObtainSemaphore(&devBase->opener_lock);
        }
        if (!op) {
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }

        ULONG want_mask = ioreq->ios2_WireError;
        if ((want_mask & S2EVENT_ONLINE) && devBase->state == V1000_STATE_ONLINE) {
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            ioreq->ios2_Req.io_Error = 0;
            ioreq->ios2_WireError    = S2EVENT_ONLINE;
            break;
        }
        if ((want_mask & S2EVENT_OFFLINE) && devBase->state == V1000_STATE_OFFLINE) {
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            ioreq->ios2_Req.io_Error = 0;
            ioreq->ios2_WireError    = S2EVENT_OFFLINE;
            break;
        }

        IExec->AddTail((struct List *)&op->event_queue,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->ReleaseSemaphore(&devBase->opener_lock);
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }

    default:
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        break;
    }

    /* Reply back to caller */
    IExec->ReplyMsg((struct Message *)ioreq);
}

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;

    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    UWORD cmd = ioreq->ios2_Req.io_Command;
    IExec->DebugPrintF("[virte1000] BeginIO: cmd=%u ioreq=%p\n", (unsigned int)cmd, ioreq);
    if (cmd == V1000_DBG_STATUS || cmd == V1000_DBG_CMDLOG ||
        cmd == V1000_DBG_DQBUF   || cmd == V1000_DBG_CFGBUF ||
        cmd == V1000_DBG_DUMPTX) {
        v1000_dispatch_ioreq(devBase, ioreq);
        return;
    }

    if (devBase->begin_port) {
        IExec->PutMsg(devBase->begin_port, (struct Message *)ioreq);
    } else {
        v1000_dispatch_ioreq(devBase, ioreq);
    }
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;

    if (!ioreq) return -1;

    IExec->DebugPrintF("[virte1000] AbortIO: ioreq=%p cmd=%u\n", ioreq, ioreq->ios2_Req.io_Command);

    IExec->ObtainSemaphore(&devBase->opener_lock);
    struct V1000Opener *op;
    for (op = (struct V1000Opener *)devBase->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ)
    {
        struct Node *curr;
        for (curr = op->read_queue.mlh_Head; curr->ln_Succ; curr = curr->ln_Succ) {
            if (curr == (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove(curr);
                IExec->ReleaseSemaphore(&devBase->opener_lock);
                ioreq->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)ioreq);
                IExec->DebugPrintF("[virte1000] AbortIO: aborted read_queue ioreq=%p\n", ioreq);
                return 0;
            }
        }
        for (curr = op->event_queue.mlh_Head; curr->ln_Succ; curr = curr->ln_Succ) {
            if (curr == (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove(curr);
                IExec->ReleaseSemaphore(&devBase->opener_lock);
                ioreq->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)ioreq);
                IExec->DebugPrintF("[virte1000] AbortIO: aborted event_queue ioreq=%p\n", ioreq);
                return 0;
            }
        }
    }
    IExec->ReleaseSemaphore(&devBase->opener_lock);

    /* Also check begin_port message list under Forbid */
    if (devBase->begin_port) {
        IExec->Forbid();
        struct Node *curr;
        for (curr = devBase->begin_port->mp_MsgList.lh_Head;
             curr->ln_Succ;
             curr = curr->ln_Succ) {
            if (curr == (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove(curr);
                IExec->Permit();
                ioreq->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)ioreq);
                IExec->DebugPrintF("[virte1000] AbortIO: aborted begin_port ioreq=%p\n", ioreq);
                return 0;
            }
        }
        IExec->Permit();
    }

    IExec->DebugPrintF("[virte1000] AbortIO: ioreq=%p not found\n", ioreq);
    return -1;
}
