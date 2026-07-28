/*
 * virte1000.device — SANA-II network device driver for the Intel e1000
 * (82540EM) as emulated by QEMU on the sam460ex AmigaOS 4.1 PPC target.
 *
 * PHASE 1 — SKELETON ONLY.
 *
 * This build produces a valid loadable OS4 device with a resident tag,
 * Init/Open/Close/Expunge, and a BeginIO that always replies IOERR_NOCMD.
 * No PCI enumeration, no descriptor rings, no SANA-II command dispatch,
 * no unit tasks. Purpose: prove the docker → push → OpenDevice loop.
 *
 * Shape borrowed from derfsss/VirtualSCSIDevice — most of the hard-won
 * OS4 device-driver conventions (resident-in-.data, 68k jump table,
 * DeviceManagerInterface vector layout, RTF_AUTOINIT semantics) come
 * from studying that project. See docs/DESIGN.md for the register-level
 * design; this file is deliberately minimal.
 */

#include "virte1000.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>

#include <devices/newstyle.h>   /* NSCMD_DEVICEQUERY + NSDeviceQueryResult */

#include <dos/dos.h>
#include <interfaces/dos.h>

#include <exec/ports.h>    /* PA_IGNORE, MsgPort init */

#include <stdarg.h>

/* Status-file path. Init appends one line per attempt; each line begins
 * with a monotonic tag so tests can grep for a specific run. Writing to
 * a file rather than DebugPrintF because DebugPrintF on the QEMU sam460ex
 * target isn't wired to the kernel serial channel and disappears; a file
 * we can read back via devbench's /api/file endpoint. */
#define VIRTE1000_STATUS_FILE  "RAM:virte1000-init.log"

/*
 * Manager Obtain/Release — reference counting on the interface itself.
 * Standard OS4 idiom; every device driver's manager interface has this.
 */
uint32 _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

uint32 _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* Forward decls for the vector table. */
extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);
extern struct Virte1000Base *_manager_Open(struct DeviceManagerInterface *Self,
                                           struct IOSana2Req *ioreq,
                                           ULONG unitNum, ULONG flags);
extern BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
extern void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);

/* OS4 vector table for DeviceManagerInterface. Order and terminator are
 * mandated by exec/interfaces.h — do not reorder. */
static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,             /* Expunge-slot on Interface — unused */
    (APTR)NULL,             /* Clone — unused */
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,             /* Reserved */
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name,        (ULONG)"__device"},
    {MIT_VectorTable, (ULONG)_manager_Vectors},
    {MIT_Version,     1},
    {TAG_END,         0},
};

const APTR devInterfaces[] = { (APTR)_manager_Tags, (APTR)NULL };

/*
 * 68k-compat jump table. Not strictly required for a SANA-II device
 * (stack callers are all OS4-native), but including it costs nothing
 * and matches the pattern of every shipping OS4 .device — safer
 * default. See VSD device.c for the incident that established the
 * need for this on trackdisk-style drivers.
 */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,     /* -6  */
    (APTR)_manager_Close,    /* -12 */
    (APTR)_manager_Expunge,  /* -18 */
    (APTR)NULL,              /* -24 Reserved */
    (APTR)_manager_BeginIO,  /* -30 */
    (APTR)_manager_AbortIO,  /* -36 */
    (APTR)-1,
};

/* Version cookie for the AmigaDOS `Version` command. */
static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

/* Init tag list — CLT_DataSize tells the kernel how big our libBase is. */
static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct Virte1000Base)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

/*
 * Resident struct — NOT const. Every working OS4 kickstart device places
 * this in writable .data because the kernel/DOS may patch fields during
 * boot binding. Putting it in .rodata is a footgun documented on the
 * VirtualSCSIDevice side. Keep it here, keep it writable, done.
 */
static struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    DEVVER,
    NT_DEVICE,
    0,
    DEVNAME,
    DEVVERSIONSTRING,
    (APTR)dev_init_tags,
};

/*
 * Shell entry point. This file is not a runnable executable, but the
 * linker still needs an _start. Print an explanatory line via
 * DebugPrintF (no dos.library needed) and return failure.
 */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to SYS:Kickstart/ or "
                       "OpenDevice() from a test program. Cannot run from shell.\n",
                       DEVNAME);
    return 20; /* RETURN_FAIL */
}

/* ------------------------------------------------------------------ */
/* Init / Open / Close / Expunge / BeginIO / AbortIO — skeleton bodies */
/* ------------------------------------------------------------------ */

/* Intel 82540EM as emulated by QEMU's -device e1000-82540em. */
#define E1000_PCI_VENDOR   0x8086
#define E1000_PCI_DEVICE   0x100E

/* NSCMD_DEVICEQUERY supported-commands list. Standard NSD idiom
 * (see daynaport-amiga/device.c for the pattern). OS4 exec MAY be
 * pre-checking this list before dispatching CMD_WRITE / S2_BROADCAST
 * / other "standard-range" io_Commands to us — that's the current
 * theory for why those cases returned IOERR_UNITBUSY (-6) before
 * BeginIO ran. Terminated by 0.
 *
 * If any client asks for a command not in this list, we still
 * dispatch it (BeginIO's switch handles it or falls to
 * IOERR_NOCMD) — the list is just what we ADVERTISE. */
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
    /* Phase 7a: advertise the commands Roadshow issues at bind time
     * even though we treat them as no-ops. Roadshow's DHCP failure with
     * "broadcast access not supported" is likely because it calls
     * S2_ADDMULTICASTADDRESSES(broadcast MAC) and gets IOERR_NOCMD. */
    S2_TRACKTYPE,
    S2_UNTRACKTYPE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,
    0
};

/* e1000 register offsets we currently touch. Full map lives in the QEMU
 * source at hw/net/e1000_regs.h + the 82540EM datasheet; only the offsets
 * we actually read/write get named here. Cross-referenced with the design
 * agent's docs/DESIGN.md §2. */
#define E1000_REG_CTRL     0x00000
#define E1000_REG_STATUS   0x00008
#define E1000_REG_ICR      0x000C0   /* Interrupt Cause Read (RO/read-to-clear) */
#define E1000_REG_ICS      0x000C8   /* Interrupt Cause Set (WO, forces IRQ) */
#define E1000_REG_IMS      0x000D0   /* Interrupt Mask Set */
#define E1000_REG_IMC      0x000D8   /* Interrupt Mask Clear (WO, write 1 to mask) */
#define E1000_REG_RCTL     0x00100   /* Receive Control */
#define E1000_REG_TCTL     0x00400   /* Transmit Control */
#define E1000_REG_RDBAL    0x02800   /* RX Descriptor Base Address Low */
#define E1000_REG_RDBAH    0x02804   /* RX Descriptor Base Address High */
#define E1000_REG_RDLEN    0x02808   /* RX Descriptor Length (in bytes) */
#define E1000_REG_RDH      0x02810   /* RX Descriptor Head */
#define E1000_REG_RDT      0x02818   /* RX Descriptor Tail */
#define E1000_REG_TDBAL    0x03800   /* TX Descriptor Base Address Low */
#define E1000_REG_TDBAH    0x03804
#define E1000_REG_TDLEN    0x03808
#define E1000_REG_TDH      0x03810
#define E1000_REG_TDT      0x03818
#define E1000_REG_TPT      0x040D4   /* Total Packets Transmitted */

/* Legacy TX descriptor CMD bits (byte 11 of the 16-byte descriptor). */
#define E1000_TXD_CMD_EOP  (1U << 0)   /* End Of Packet */
#define E1000_TXD_CMD_IFCS (1U << 1)   /* Insert FCS (CRC) */
#define E1000_TXD_CMD_RS   (1U << 3)   /* Report Status — set STATUS.DD */

/* Legacy TX descriptor STATUS bits (byte 12). */
#define E1000_TXD_STAT_DD  (1U << 0)   /* Descriptor Done */
#define E1000_REG_RAL      0x05400   /* Receive Address Low  */
#define E1000_REG_RAH      0x05404   /* Receive Address High */

/* Little-endian store to main memory. Descriptor entries live in host RAM
 * but the e1000 reads them as LE, so we byte-reverse on the store side. */
static inline void poke_le32(volatile void *p, uint32 v)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(v), "r"(p) : "memory");
}

/* Full memory barrier — ensures all prior stores complete before any
 * subsequent memory ops. Required after building a descriptor and
 * before writing the doorbell (TDT) so HW sees a consistent descriptor.
 * pa6t_eth calls this pasemi_wmb; same instruction. */
static inline void v1000_wmb(void)
{
    __asm__ volatile ("sync" : : : "memory");
}

/* -------- Phase 6k: unit task ---------- */

/* Forward decl for use by ISR. */
static void v1000_process_rx(struct Virte1000Base *base);

/* Task entry function. Called by CreateTaskTags. Args-less; we recover
 * our own Task struct via classic-Amiga SysBase-at-4 → FindTask(NULL),
 * then read tc_UserData for the Virte1000Base pointer. */
static void v1000_task_body(void)
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
    base->task_ready_ok = TRUE;
    /* Handshake — Init is Wait'ing on parent_ready_mask. */
    IExec->Signal(base->parent_task, base->parent_ready_mask);

    ULONG wait_mask = sig_mask | SIGBREAKF_CTRL_C;
    while (!base->task_shutdown) {
        (void)IExec->Wait(wait_mask);
        if (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;
        IExec->SetSignal(0, sig_mask);   /* consume all pending IRQ signals */
        base->task_wake_count++;
        v1000_process_rx(base);
    }

    /* Cleanup — task exits after this returns. */
    IExec->FreeSignal(sig);
    base->unit_signal_mask = 0;
    base->unit_task = NULL;
}

/* Called from Init. Spawns the task; waits until task signals ready. */
static BOOL v1000_task_start(struct Virte1000Base *base, struct ExecIFace *iexec)
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

/* Called from Expunge. */
static void v1000_task_stop(struct Virte1000Base *base, struct ExecIFace *IExec)
{
    if (!base->unit_task) return;
    base->task_shutdown = TRUE;
    IExec->Signal(base->unit_task, SIGBREAKF_CTRL_C);
    /* Yield until the task clears itself. */
    ULONG patience = 100000;
    while (base->unit_task && patience-- > 0) {
        IExec->Forbid();
        IExec->Permit();
    }
}

/* Find the opener matching this request's bm_cookie. NULL if none —
 * unusual (Open would have added one), but Close-race-safe. Caller
 * should hold opener_lock (unless within Init/Expunge). */
static struct V1000Opener *v1000_find_opener(struct Virte1000Base *base,
                                              APTR bm_cookie)
{
    struct V1000Opener *op;
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        if (op->bm_cookie == bm_cookie) return op;
    }
    return NULL;
}

/* Resolve a CPU-virtual allocation to its PCI-bus physical address
 * via the OS4-blessed StartDMA + GetDMAList path (pa6t_eth pattern).
 * Returns 0 on failure. Caller should have kept the allocation in the
 * DMA state via StartDMA — this function releases via EndDMA before
 * returning, which is fine for one-shot alloc-and-forget or when
 * we're doing StartDMA per operation. For the driver's long-lived
 * ring memory we keep the mapping alive until Expunge (see Init). */
static uint32 v1000_dma_phys(struct ExecIFace *IExec, void *virt,
                             uint32 size, uint32 flags)
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
    if (nentries > 1) {
        IExec->DebugPrintF("[virte1000] dma_phys: WARNING non-contiguous virt=%p size=%lu nentries=%lu\n",
                           virt, size, nentries);
    }
    IExec->FreeSysObject(ASOT_DMAENTRY, dlist);
    return phys;
}

/* Forward decls — real definitions live further down with the other
 * MMIO helpers. The ISR below needs to call e1000_read32(ICR); the
 * Phase 6k v1000_process_rx also uses e1000_write32(RDT). */
static inline uint32 e1000_read32(volatile void *base, uint32 off);
static inline void   e1000_write32(volatile void *base, uint32 off, uint32 val);

/* Ring sizing — used by v1000_process_rx (which is above Init where
 * these are also referenced during ring alloc). Moved here so both
 * users see them; keeping the values single-source-of-truth. */
#define V1000_RING_ENTRIES   16
#define V1000_DESC_SIZE      16
#define V1000_RX_BUFSIZE     2048

/* Walk the RX ring, deliver frames to queued CMD_READ requests via the
 * opener's CopyToBuff hook, reset descriptors + advance RDT so HW can
 * refill the slot. Called from the unit task on IRQ signal. */
static void v1000_process_rx(struct Virte1000Base *base)
{
    if (!base->rx_ring || !base->bar0 || !base->IUtility) return;
    struct ExecIFace *IExec = base->IExec;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;

    ULONG dd_seen_this_call = 0;
    for (int slot = 0; slot < V1000_RING_ENTRIES; slot++) {
        volatile UBYTE *rd = (volatile UBYTE *)base->rx_ring + slot * V1000_DESC_SIZE;

        /* PPC 460EX PCI DMA is not snoopy on sam460ex — HW writes to
         * descriptor + rx_buffer in RAM bypass the CPU L1 cache. Using
         * `volatile` re-issues the load instruction but doesn't force
         * a cache miss. Invalidate (no writeback) so our next read
         * fetches HW's fresh value from RAM. CACRF_InvalidateD, not
         * CACRF_ClearD — Clear does writeback-then-invalidate, which
         * would push our previous-round `rd[12]=0` back to RAM ON TOP
         * of HW's just-set DD=1, losing every frame. */
        IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_InvalidateD);

        if (!(rd[12] & 0x01)) continue;    /* not done */
        dd_seen_this_call++;

        ULONG frame_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
        if (frame_len < 14 || frame_len > V1000_RX_BUFSIZE) {
            rd[12] = 0;
            /* Flush our clear to RAM so HW starts this slot fresh
             * next time it writes here — CACRF_ClearD is writeback+
             * invalidate, safe here because HW hasn't touched the
             * slot again yet (we hold it until we bump RDT below). */
            IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_ClearD);
            e1000_write32(mmio, E1000_REG_RDT, slot);
            continue;
        }
        UBYTE *rxbuf = (UBYTE *)base->rx_buffers + slot * V1000_RX_BUFSIZE;
        /* Same story for the frame buffer: HW wrote frame bytes to
         * RAM, our cache still holds stale bytes from init or a
         * previous shorter frame. Bytes past the first cache line
         * come back wrong without this — ping data corrupts at
         * offset 8 (which lands past the first 32-byte cache line
         * inside the ICMP payload region). */
        IExec->CacheClearE(rxbuf, frame_len, CACRF_InvalidateD);

        ULONG payload_len = frame_len - 14;

        /* Find any opener with a queued CMD_READ. First-come, first-serve
         * for now — no ethertype filtering (Phase 6l). */
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

        BOOL delivered_this_slot = FALSE;
        if (ioreq && op && op->copy_to_buff && ioreq->ios2_Data) {
            /* Populate ios2 fields, invoke CopyToBuff hook, reply. */
            for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
            for (int i = 0; i < 6; i++) ioreq->ios2_DstAddr[i] = rxbuf[i];
            ioreq->ios2_PacketType = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];
            ioreq->ios2_DataLength = payload_len;

            struct SANA2CopyHookMsg msg;
            msg.schm_Method  = S2_CopyToBuff;
            msg.schm_MsgSize = sizeof(msg);
            msg.schm_To      = ioreq->ios2_Data;
            msg.schm_From    = rxbuf + 14;
            msg.schm_Size    = payload_len;
            BOOL ok = (BOOL)(ULONG)base->IUtility->CallHookPkt(
                (struct Hook *)op->copy_to_buff, ioreq, &msg);
            ioreq->ios2_Req.io_Error = ok ? 0 : S2ERR_NO_RESOURCES;
            IExec->ReplyMsg((struct Message *)ioreq);
            delivered_this_slot = ok;
        }
        if (delivered_this_slot) base->process_rx_delivered++;
        /* Whether we delivered or not, free the slot for HW reuse.
         * MUST flush the write to RAM (CACRF_ClearD) before bumping
         * RDT — otherwise HW can DMA a new frame + set DD=1 in RAM
         * while our cached DD=0 sits dirty. Next iteration's
         * InvalidateD would then discard the (unflushed) 0, but that
         * only masks the issue; a well-timed HW write between our
         * dirty-cache and the invalidate could still be seen twice. */
        rd[12] = 0;
        IExec->CacheClearE((APTR)rd, V1000_DESC_SIZE, CACRF_ClearD);
        /* Bump RDT to hand this slot back. */
        e1000_write32(mmio, E1000_REG_RDT, slot);
    }
    base->process_rx_dd_seen = dd_seen_this_call;
}

/* -------- Phase 5b: ISR ----------
 * Runs at exec interrupt level on the shared PCI INTx chain. Contract per
 * exec IntServer autodoc + VirtualSCSIDevice's virtio_irq.c:
 *   - No memory allocation, no blocking, no library calls beyond IExec
 *     essentials (Signal/Cause/similar).
 *   - No DebugPrintF (deadlock risk).
 *   - Return non-zero if we recognised and claimed this interrupt;
 *     return zero to let the next server on the chain look at it.
 *
 * Reading ICR both TELLS us what fired AND acknowledges the causes
 * (read-to-clear per DESIGN.md §6). We stash the last non-zero value
 * in devBase->last_icr for post-hoc inspection by test programs;
 * they read it via the driver-private query command added in Phase 5e.
 *
 * When IMS is 0 (Phase 5b) no cause is unmasked so ICR reads here
 * should be 0 and we always return 0 (not-ours). That's the safest
 * starting point: we're on the chain but never claim, so an install
 * bug can't storm. */
static uint32 v1000_isr(struct ExceptionContext *ctx,
                        struct ExecBase *sysbase, APTR is_Data)
{
    struct Virte1000Base *base = (struct Virte1000Base *)is_Data;
    (void)ctx; (void)sysbase;

    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    uint32 icr = e1000_read32(mmio, E1000_REG_ICR);
    if (icr == 0)
        return 0;               /* not ours (or nothing pending) */

    base->irq_counter++;
    base->last_icr = icr;

    /* Phase 6k: on RX (RXT0 bit 7), wake the unit task so it can
     * ProcessRX in task context. ISR context is limited to Signal-safe
     * ops — no ring walking here. Also on link-status change (LSC)
     * we could signal, but nothing consumes that yet. */
    if ((icr & (1U << 7)) && base->unit_task && base->unit_signal_mask) {
        struct ExecIFace *IExec = base->IExec;
        IExec->Signal(base->unit_task, base->unit_signal_mask);
    }
    return 1;                   /* claimed */
}

/* STATUS register bits (§2 of DESIGN.md, e1000x_regs.h:STATUS_*) */
#define E1000_STATUS_FD    (1U << 0)   /* Full Duplex */
#define E1000_STATUS_LU    (1U << 1)   /* Link Up */
#define E1000_STATUS_SPEED_MASK   (3U << 6)
#define E1000_STATUS_SPEED_SHIFT  6

/* PPC MMIO helpers. e1000 registers are little-endian; PPC 460EX is
 * big-endian; we use the byte-reversed load/store insns (lwbrx/stwbrx)
 * so we can dereference LE registers without an explicit bswap step.
 * `volatile` on the pointer stops GCC from caching or reordering. */
static inline uint32 e1000_read32(volatile void *base, uint32 off)
{
    volatile uint8 *p = (volatile uint8 *)base + off;
    uint32 v;
    __asm__ volatile ("lwbrx %0, 0, %1; eieio" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline void e1000_write32(volatile void *base, uint32 off, uint32 val)
{
    volatile uint8 *p = (volatile uint8 *)base + off;
    __asm__ volatile ("stwbrx %0, 0, %1; eieio" : : "r"(val), "r"(p) : "memory");
}

/* Diagnostic scope — held open across Init so we can FPrintf status lines
 * inline. We use FPrintf (varargs, works with C promotion rules) rather
 * than VFPrintf (takes a ULONG* argarray in the AmigaDOS convention, which
 * fights with PPC va_list ABI and garbled our earlier attempts).
 *
 * DebugPrintF on this QEMU target isn't wired to the kernel serial port,
 * so a status file is the only observability channel that works before
 * PCI/MMIO is up. Test scripts read it back via /api/file. */
struct V1000LogCtx {
    struct Library *DOSBase;
    struct DOSIFace *IDOS;
    BPTR fh;
};

static void v1000_log_open(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    ctx->DOSBase = NULL; ctx->IDOS = NULL; ctx->fh = (BPTR)NULL;
    ctx->DOSBase = IExec->OpenLibrary("dos.library", 51);
    if (!ctx->DOSBase) return;
    ctx->IDOS = (struct DOSIFace *)IExec->GetInterface(ctx->DOSBase, "main", 1, NULL);
    if (!ctx->IDOS) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; return; }
    /* MODE_NEWFILE truncates on each Init so we only ever see the latest
     * run's status. If you want the history, tail /api/logs instead. */
    ctx->fh = ctx->IDOS->Open((CONST_STRPTR)VIRTE1000_STATUS_FILE, MODE_NEWFILE);
}

static void v1000_log_close(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    if (ctx->fh) { ctx->IDOS->Close(ctx->fh); ctx->fh = (BPTR)NULL; }
    if (ctx->IDOS) { IExec->DropInterface((struct Interface *)ctx->IDOS); ctx->IDOS = NULL; }
    if (ctx->DOSBase) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; }
}

/* Emit one line — no-op if any part of the log context failed to open. */
#define LOGF(ctx, ...) \
    do { if ((ctx).fh) (ctx).IDOS->FPrintf((ctx).fh, __VA_ARGS__); } while (0)

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[virte1000] Init: %s\n", DEVVERSIONSTRING_FULL);

    struct V1000LogCtx log;
    v1000_log_open(iexec, &log);
    LOGF(log, (CONST_STRPTR)"=== Init %s ===\n", (STRPTR)DEVVERSIONSTRING_FULL);

    devBase->IExec = iexec;
    devBase->dev_SegList = seglist;
    iexec->InitSemaphore(&devBase->io_lock);

    /* All PCI-side fields start NULL. Any partial-init failure below leaves
     * them NULL, and Expunge unconditionally frees only what is non-NULL.
     * hw_present tracks "PCI enum + BAR mapping both succeeded" as one
     * flag so BeginIO can gate on a single boolean instead of null-checking
     * every pointer. */
    devBase->ExpansionBase = NULL;
    devBase->IPCI          = NULL;
    devBase->pciDevice     = NULL;
    devBase->bar0          = NULL;
    devBase->hw_present    = FALSE;
    devBase->rx_ring       = NULL;
    devBase->tx_ring       = NULL;
    devBase->rx_buffers    = NULL;
    devBase->rx_ring_phys  = 0;
    devBase->tx_ring_phys  = 0;
    devBase->rx_buffers_phys = 0;
    devBase->tx_scratch    = NULL;
    devBase->tx_scratch_phys = 0;
    devBase->irq_vector    = 0;
    devBase->irq_installed = FALSE;
    devBase->irq_counter   = 0;
    devBase->last_icr      = 0;
    devBase->state         = V1000_STATE_OFFLINE;
    devBase->tx_next_slot  = 0;
    devBase->UtilityBase   = NULL;
    devBase->IUtility      = NULL;
    iexec->NewList((struct List *)&devBase->opener_list);
    iexec->InitSemaphore(&devBase->opener_lock);

    /* Phase 6d: init the embedded unit's MsgPort. PA_IGNORE tells exec
     * "don't queue+signal, just call BeginIO synchronously". That's the
     * behavior we already implement — BeginIO handles everything inline
     * and calls ReplyMsg when IOF_QUICK is clear. mp_MsgList must still
     * be a valid empty List so nothing dereferences a NULL head/tail. */
    {
        struct Unit *u = &devBase->v1000_unit;
        u->unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        u->unit_MsgPort.mp_Node.ln_Name = (STRPTR)DEVNAME;
        u->unit_MsgPort.mp_Node.ln_Pri  = 0;
        /* PA_IGNORE: our BeginIO handles requests synchronously; no
         * consumer task is needed. Empirically neither PA_IGNORE nor
         * PA_SIGNAL with a real SigTask unblocks CMD_WRITE via DoIO —
         * exec has a deeper check we haven't identified yet (returns
         * IOERR_UNITBUSY before BeginIO runs). The other IO paths
         * (DEVICEQUERY, GETSTATIONADDRESS, ONLINE/OFFLINE, private
         * DBG commands) work fine with either mode. */
        u->unit_MsgPort.mp_Flags        = PA_IGNORE;
        u->unit_MsgPort.mp_SigBit       = 0;
        u->unit_MsgPort.mp_SigTask      = NULL;
        iexec->NewList(&u->unit_MsgPort.mp_MsgList);
        u->unit_flags   = UNITF_ACTIVE;
        u->unit_pad     = 0;
        u->unit_OpenCnt = 0;
    }
    for (int i = 0; i < 6; i++) devBase->mac[i] = 0;

    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version      = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision     = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    /* -------- Phase 3a: safe PCI enumeration ----------
     * Try to open expansion.library, get IPCI, and locate the e1000. Any
     * failure at any step is logged and left as NULL — Init STILL returns
     * OK so the loop test (testopen) keeps passing. Hardware ops added in
     * later phases will refuse cleanly if pciDevice is NULL. */

    /* utility.library — needed for GetTagData at Open time and
     * CallHookPkt for cooked-mode TX/RX copies. Open before PCI stuff
     * so we can bail cleanly if it's not available. */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (devBase->UtilityBase) {
        devBase->IUtility = (struct UtilityIFace *)
            iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    }
    LOGF(log, (CONST_STRPTR)"utility.library: %s IUtility=%p\n",
         devBase->UtilityBase ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED",
         devBase->IUtility);

    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 53);
    if (!devBase->ExpansionBase) {
        LOGF(log, (CONST_STRPTR)"expansion.library v53 open FAILED - no-hardware mode\n");
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"expansion.library v53 OK\n");

    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase,
                                                            "pci", 1, NULL);
    if (!devBase->IPCI) {
        LOGF(log, (CONST_STRPTR)"GetInterface(pci,1) FAILED - no-hardware mode\n");
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"IPCI v1 acquired\n");

    devBase->pciDevice = devBase->IPCI->FindDeviceTags(
        FDT_VendorID, E1000_PCI_VENDOR,
        FDT_DeviceID, E1000_PCI_DEVICE,
        TAG_END);

    if (!devBase->pciDevice) {
        LOGF(log, (CONST_STRPTR)"FindDeviceTags(%04lx:%04lx): NOT FOUND - "
             "add '-device e1000-82540em' to QEMU\n",
             (ULONG)E1000_PCI_VENDOR, (ULONG)E1000_PCI_DEVICE);
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    {
        uint8 bus = 0, dev = 0, fn = 0;
        devBase->pciDevice->GetAddress(&bus, &dev, &fn);
        LOGF(log, (CONST_STRPTR)"FindDeviceTags(%04lx:%04lx): FOUND at PCI %02lx:%02lx.%lu\n",
             (ULONG)E1000_PCI_VENDOR, (ULONG)E1000_PCI_DEVICE,
             (ULONG)bus, (ULONG)dev, (ULONG)fn);
    }

    /* -------- Phase 3c: dump PCI config space ----------
     * All READS. Safe. Values verified against the QEMU e1000 emulation:
     *   vendor=8086, device=100E, class=0200 (Ethernet), rev=03.
     * Command should have MEM_SPACE=1 and BUS_MASTER=1 for the driver to
     * function. If BUS_MASTER isn't set yet we'll turn it on in Phase 3e
     * before touching descriptor rings. */
    {
        struct PCIDevice *pd = devBase->pciDevice;
        UWORD vendor  = pd->ReadConfigWord(PCI_VENDOR_ID);
        UWORD devid   = pd->ReadConfigWord(PCI_DEVICE_ID);
        UWORD cmd     = pd->ReadConfigWord(PCI_COMMAND);
        UWORD stat    = pd->ReadConfigWord(PCI_STATUS);
        UBYTE rev     = pd->ReadConfigByte(PCI_REVISION_ID);
        UBYTE clsprog = pd->ReadConfigByte(PCI_CLASS_PROG);
        UBYTE clsdev  = pd->ReadConfigByte(PCI_CLASS_DEVICE);
        UBYTE intline = pd->ReadConfigByte(PCI_INTERRUPT_LINE);
        UBYTE intpin  = pd->ReadConfigByte(PCI_INTERRUPT_PIN);
        ULONG bar0raw = pd->ReadConfigLong(PCI_BASE_ADDRESS_0);

        LOGF(log, (CONST_STRPTR)"config: vendor=%04lx device=%04lx rev=%02lx class=%02lx.%02lx\n",
             (ULONG)vendor, (ULONG)devid, (ULONG)rev, (ULONG)clsdev, (ULONG)clsprog);
        LOGF(log, (CONST_STRPTR)"config: command=%04lx status=%04lx\n",
             (ULONG)cmd, (ULONG)stat);
        LOGF(log, (CONST_STRPTR)"config: interrupt line=%lu pin=%lu\n",
             (ULONG)intline, (ULONG)intpin);
        LOGF(log, (CONST_STRPTR)"config: BAR0 raw=%08lx  (type=%s, prefetch=%s)\n",
             (ULONG)bar0raw,
             (bar0raw & 1) ? (CONST_STRPTR)"IO" : (CONST_STRPTR)"MEM",
             (bar0raw & 8) ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no");
    }

    /* -------- Phase 3d/e: hold BAR0 + read STATUS/CTRL ----------
     * GetResourceRange resolves the raw config-space BAR to a CPU-visible
     * address the driver can dereference. Hold it in devBase->bar0 across
     * Init; Expunge frees it back. First MMIO reads use the byte-reversed
     * lwbrx primitive so we can dereference LE registers without an
     * explicit swap step. STATUS.LU tells us whether QEMU's virtual link
     * is up (should be 1 with user-mode net) and STATUS.SPEED reports
     * the negotiated link speed. Pure reads — safe. */
    devBase->bar0 = devBase->pciDevice->GetResourceRange(0);
    if (!devBase->bar0) {
        LOGF(log, (CONST_STRPTR)"GetResourceRange(0): FAILED - BAR0 not mappable\n");
        v1000_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"BAR0 held: cpu=%08lx phys=%08lx size=%lu\n",
         (ULONG)devBase->bar0->BaseAddress, (ULONG)devBase->bar0->Physical,
         (ULONG)devBase->bar0->Size);

    {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        uint32 ctrl   = e1000_read32(mmio, E1000_REG_CTRL);
        uint32 status = e1000_read32(mmio, E1000_REG_STATUS);
        ULONG speed_code = (status & E1000_STATUS_SPEED_MASK) >> E1000_STATUS_SPEED_SHIFT;
        CONST_STRPTR speed_name = (CONST_STRPTR)
            (speed_code == 0 ? "10Mb" :
             speed_code == 1 ? "100Mb" :
             speed_code == 2 ? "1000Mb" : "1000Mb-alt");

        LOGF(log, (CONST_STRPTR)"MMIO CTRL   = %08lx\n", (ULONG)ctrl);
        LOGF(log, (CONST_STRPTR)"MMIO STATUS = %08lx  (link %s, %s duplex, speed %s)\n",
             (ULONG)status,
             (status & E1000_STATUS_LU) ? (CONST_STRPTR)"UP"   : (CONST_STRPTR)"down",
             (status & E1000_STATUS_FD) ? (CONST_STRPTR)"full" : (CONST_STRPTR)"half",
             speed_name);
    }

    /* -------- Phase 3f: read MAC from RAL/RAH ----------
     * The 82540EM Receive Address Registers hold the primary MAC. RAL is
     * the low 32 bits (bytes 0-3); RAH is 16 bits (bytes 4-5) plus a
     * valid flag in bit 31. QEMU pre-fills these from the -device's
     * mac= option; default is 52:54:00:12:34:56. */
    {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        uint32 ral = e1000_read32(mmio, E1000_REG_RAL);
        uint32 rah = e1000_read32(mmio, E1000_REG_RAH);
        devBase->mac[0] = (UBYTE)(ral        & 0xff);
        devBase->mac[1] = (UBYTE)((ral >> 8) & 0xff);
        devBase->mac[2] = (UBYTE)((ral >> 16)& 0xff);
        devBase->mac[3] = (UBYTE)((ral >> 24)& 0xff);
        devBase->mac[4] = (UBYTE)(rah        & 0xff);
        devBase->mac[5] = (UBYTE)((rah >> 8) & 0xff);
        BOOL valid = (rah & (1U << 31)) != 0;
        LOGF(log, (CONST_STRPTR)"MMIO RAL    = %08lx\n", (ULONG)ral);
        LOGF(log, (CONST_STRPTR)"MMIO RAH    = %08lx  (valid=%s)\n",
             (ULONG)rah, valid ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no");
        LOGF(log, (CONST_STRPTR)"MAC         = %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
             (ULONG)devBase->mac[0], (ULONG)devBase->mac[1], (ULONG)devBase->mac[2],
             (ULONG)devBase->mac[3], (ULONG)devBase->mac[4], (ULONG)devBase->mac[5]);

        /* All the acquire steps succeeded — flip the flag so BeginIO
         * dispatch knows S2_DEVICEQUERY / S2_GETSTATIONADDRESS can
         * answer with real values instead of S2ERR_OUTOFSERVICE. */
        devBase->hw_present = TRUE;
    }

    /* -------- Phase 4a: quiesce ----------
     * Mask all interrupt sources and clear anything pending. Deliberately
     * DON'T touch RCTL/TCTL yet — writing RCTL triggers QEMU's 1-second
     * flush_queue_timer per DESIGN.md §3, so we do it once at ring-enable
     * time only. Reads here just verify the post-reset state is idle
     * (RCTL/TCTL/IMS all zero, ICR whatever) so we know Phase 4c-f start
     * from a known-good baseline. */
    {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;

        /* IMC is write-1-to-mask; 0xFFFFFFFF masks every cause. */
        e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);

        /* ICR is read-to-clear on this device (per QEMU set_interrupt_cause
         * and DESIGN.md §6). Reading it drops all pending causes. */
        uint32 icr_at_reset = e1000_read32(mmio, E1000_REG_ICR);

        /* Now read back the state we expect to be idle. */
        uint32 ims  = e1000_read32(mmio, E1000_REG_IMS);
        uint32 rctl = e1000_read32(mmio, E1000_REG_RCTL);
        uint32 tctl = e1000_read32(mmio, E1000_REG_TCTL);

        LOGF(log, (CONST_STRPTR)"quiesce: ICR-at-reset=%08lx IMS=%08lx RCTL=%08lx TCTL=%08lx\n",
             (ULONG)icr_at_reset, (ULONG)ims, (ULONG)rctl, (ULONG)tctl);

        if (ims != 0 || rctl != 0 || tctl != 0) {
            /* Not fatal — but log loudly. Something else has touched this
             * NIC (unlikely in a fresh QEMU boot, but possible if a prior
             * driver Init run left state around). */
            LOGF(log, (CONST_STRPTR)"quiesce: WARNING - one of IMS/RCTL/TCTL "
                 "was non-zero at Init; device may not be in cold state\n");
        }
    }

    /* -------- Phase 4b: allocate DMA rings + RX buffer pool ----------
     * MEMF_SHARED = usable by DMA masters (including PCI bus). Contiguous
     * + PhysicalAlignment + Alignment=16 satisfies the 82540EM's ring
     * alignment requirement. Buffers: 16 × 2KB, one per RX descriptor.
     * If any alloc fails we log, leave hw_present TRUE (basic Init worked)
     * but skip ring programming — later phases refuse to enable RX/TX. */
    {
        ULONG ring_bytes = V1000_RING_ENTRIES * V1000_DESC_SIZE;
        ULONG buf_bytes  = V1000_RING_ENTRIES * V1000_RX_BUFSIZE;

        devBase->rx_ring = iexec->AllocVecTags(ring_bytes,
            AVT_Type, MEMF_SHARED,
            AVT_Contiguous, TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment, 16,
            AVT_ClearWithValue, 0,
            TAG_END);
        devBase->tx_ring = iexec->AllocVecTags(ring_bytes,
            AVT_Type, MEMF_SHARED,
            AVT_Contiguous, TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment, 16,
            AVT_ClearWithValue, 0,
            TAG_END);
        devBase->rx_buffers = iexec->AllocVecTags(buf_bytes,
            AVT_Type, MEMF_SHARED,
            AVT_Contiguous, TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment, 16,
            AVT_ClearWithValue, 0,
            TAG_END);

        LOGF(log, (CONST_STRPTR)"rings: rx_ring=%p tx_ring=%p rx_buffers=%p (each %lu, buf pool %lu)\n",
             devBase->rx_ring, devBase->tx_ring, devBase->rx_buffers,
             (ULONG)ring_bytes, (ULONG)buf_bytes);

        if (!devBase->rx_ring || !devBase->tx_ring || !devBase->rx_buffers) {
            LOGF(log, (CONST_STRPTR)"rings: ALLOC FAILED - device will not be enabled\n");
        }

        /* Resolve CPU-virtual ring addresses to PCI-bus physical
         * addresses via StartDMA + GetDMAList. These physical values
         * are what we write to RDBAL/TDBAL and per-descriptor
         * buffer_addr fields. On sam460ex they often match the CPU
         * virt (identity mapping) but we can't rely on that — the
         * pa6t_eth reference driver does this per-allocation. */
        if (devBase->rx_ring) {
            devBase->rx_ring_phys = v1000_dma_phys(iexec, devBase->rx_ring,
                                                    ring_bytes, 0);
        }
        if (devBase->tx_ring) {
            devBase->tx_ring_phys = v1000_dma_phys(iexec, devBase->tx_ring,
                                                    ring_bytes, DMA_ReadFromRAM);
        }
        if (devBase->rx_buffers) {
            devBase->rx_buffers_phys = v1000_dma_phys(iexec, devBase->rx_buffers,
                                                       buf_bytes, 0);
        }
        LOGF(log, (CONST_STRPTR)"rings phys: rx_ring=%08lx tx_ring=%08lx rx_buffers=%08lx\n",
             (ULONG)devBase->rx_ring_phys, (ULONG)devBase->tx_ring_phys,
             (ULONG)devBase->rx_buffers_phys);

        /* Dedicated TX scratch — separate from rx_buffers so HW's
         * TX-read DMA doesn't race with RX-write DMA at the same phys
         * address. 2 KB is enough for one full-size Ethernet frame. */
        devBase->tx_scratch = iexec->AllocVecTags(V1000_RX_BUFSIZE,
            AVT_Type,           MEMF_SHARED,
            AVT_Contiguous,     TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment,      16,
            AVT_ClearWithValue, 0,
            TAG_END);
        if (devBase->tx_scratch) {
            devBase->tx_scratch_phys = v1000_dma_phys(iexec, devBase->tx_scratch,
                                                      V1000_RX_BUFSIZE, DMA_ReadFromRAM);
        }
        LOGF(log, (CONST_STRPTR)"tx_scratch: cpu=%p phys=%08lx\n",
             devBase->tx_scratch, (ULONG)devBase->tx_scratch_phys);
    }

    /* -------- Phase 4c: populate RX descriptors + program RX ring regs ----
     * Legacy 16-byte RX descriptor layout, little-endian:
     *   [0..7]  buffer_addr (64-bit)
     *   [8..9]  length       (HW writes on RX)
     *   [10..11] csum        (HW writes)
     *   [12]    status       (HW writes; bit0 = DD, done)
     *   [13]    errors
     *   [14..15] special
     * For "empty, ready to receive": buffer_addr = per-descriptor slice
     * of rx_buffers, everything else zero. RX is NOT enabled here — that's
     * Phase 4e. We only want to verify the register writes take. */
    if (devBase->rx_ring && devBase->rx_buffers && devBase->rx_buffers_phys) {
        volatile UBYTE *desc = (volatile UBYTE *)devBase->rx_ring;
        ULONG buf_lo = devBase->rx_buffers_phys;   /* PCI-bus phys */
        for (int i = 0; i < V1000_RING_ENTRIES; i++) {
            volatile UBYTE *d = desc + (i * V1000_DESC_SIZE);
            poke_le32(d + 0, buf_lo + (i * V1000_RX_BUFSIZE));  /* addr low  */
            poke_le32(d + 4, 0);                                 /* addr high */
            poke_le32(d + 8, 0);                                 /* len+csum  */
            poke_le32(d + 12, 0);                                /* status/err/special */
        }
        v1000_wmb();

        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_RDBAL, devBase->rx_ring_phys);
        e1000_write32(mmio, E1000_REG_RDBAH, 0);
        e1000_write32(mmio, E1000_REG_RDLEN, V1000_RING_ENTRIES * V1000_DESC_SIZE);
        e1000_write32(mmio, E1000_REG_RDH,   0);
        e1000_write32(mmio, E1000_REG_RDT,   V1000_RING_ENTRIES - 1);

        /* Read back to verify — writes to reserved bits get dropped, so
         * a mismatch would flag a wrong offset or a QEMU quirk. */
        LOGF(log, (CONST_STRPTR)"RX ring: RDBAL=%08lx RDBAH=%08lx RDLEN=%lu RDH=%lu RDT=%lu\n",
             (ULONG)e1000_read32(mmio, E1000_REG_RDBAL),
             (ULONG)e1000_read32(mmio, E1000_REG_RDBAH),
             (ULONG)e1000_read32(mmio, E1000_REG_RDLEN),
             (ULONG)e1000_read32(mmio, E1000_REG_RDH),
             (ULONG)e1000_read32(mmio, E1000_REG_RDT));
    }

    /* -------- Phase 4d: program TX ring regs ----------
     * TX descriptors get filled at CMD_WRITE time, not at Init — so leave
     * the ring memory as-is (zeros). Just program the base/length so the
     * NIC knows where to look, and set HEAD=TAIL=0 for "no work queued".
     * TX still disabled (TCTL.EN=0). */
    if (devBase->tx_ring && devBase->tx_ring_phys) {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_TDBAL, devBase->tx_ring_phys);
        e1000_write32(mmio, E1000_REG_TDBAH, 0);
        e1000_write32(mmio, E1000_REG_TDLEN, V1000_RING_ENTRIES * V1000_DESC_SIZE);
        e1000_write32(mmio, E1000_REG_TDH,   0);
        e1000_write32(mmio, E1000_REG_TDT,   0);

        LOGF(log, (CONST_STRPTR)"TX ring: TDBAL=%08lx TDBAH=%08lx TDLEN=%lu TDH=%lu TDT=%lu\n",
             (ULONG)e1000_read32(mmio, E1000_REG_TDBAL),
             (ULONG)e1000_read32(mmio, E1000_REG_TDBAH),
             (ULONG)e1000_read32(mmio, E1000_REG_TDLEN),
             (ULONG)e1000_read32(mmio, E1000_REG_TDH),
             (ULONG)e1000_read32(mmio, E1000_REG_TDT));
    }

    /* Phase 4e/f (RX/TX enable) + Phase 5c/d (IMS unmask) moved to
     * v1000_online() and only run when S2_ONLINE fires. Init leaves us
     * with rings programmed but the device idle — that's the SANA-II
     * spec's OFFLINE state.
     *
     * -------- Phase 5a+b: map + install IRQ handler ----------
     * MapInterrupt resolves config-space InterruptLine to a system vector.
     * Then AddIntServer chains us on the shared PCI INTx server list.
     * IMS still 0 (all sources masked) — Phase 5c unmasks LSC below. */
    if (devBase->pciDevice) {
        devBase->irq_vector = devBase->pciDevice->MapInterrupt();
        LOGF(log, (CONST_STRPTR)"MapInterrupt: vector=%lu\n", devBase->irq_vector);

        if (devBase->irq_vector != 0) {
            devBase->irq_node.is_Node.ln_Type = NT_INTERRUPT;
            devBase->irq_node.is_Node.ln_Pri  = 0;
            devBase->irq_node.is_Node.ln_Name = (STRPTR)DEVNAME;
            devBase->irq_node.is_Data         = (APTR)devBase;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            devBase->irq_node.is_Code = (VOID (*)())v1000_isr;
#pragma GCC diagnostic pop

            BOOL ok = iexec->AddIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = ok;
            LOGF(log, (CONST_STRPTR)"AddIntServer: vec=%lu result=%s\n",
                 devBase->irq_vector, ok ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED");
        } else {
            LOGF(log, (CONST_STRPTR)"MapInterrupt returned 0 — no IRQ hooked\n");
        }
    }

    /* Phase 6k/l: unit task processes queued CMD_READ on ISR signal. */
    BOOL task_ok = v1000_task_start(devBase, iexec);
    LOGF(log, (CONST_STRPTR)"unit task: %s (task=%p mask=%08lx)\n",
         task_ok ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED",
         devBase->unit_task, (ULONG)devBase->unit_signal_mask);

    /* State starts OFFLINE — caller must invoke S2_CONFIGINTERFACE then
     * S2_ONLINE (or the debug V1000_DBG_FIRE_IRQ, once online) to make
     * the device live. */
    LOGF(log, (CONST_STRPTR)"Init done: state=OFFLINE (need S2_CONFIGINTERFACE + S2_ONLINE)\n");

    v1000_log_close(iexec, &log);
    return (struct Library *)devBase;
}

struct Virte1000Base *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    (void)flags;

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

    /* Phase-1: only unit 0 is meaningful (single-NIC), but there is no
     * hardware behind it yet. Accept the open so the test app can
     * exercise the shell, then immediately treat BeginIO as no-op. */
    if (unitNum != 0) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
        ioreq->ios2_Req.io_Device = (struct Device *)-1;
        devBase->dev_Base.dd_Library.lib_OpenCnt--;
        return NULL;
    }

    /* Phase 6d: hand out the real unit pointer so exec's async-path
     * dispatch finds a valid MsgPort. Increment unit_OpenCnt to mirror
     * lib_OpenCnt so exec sees the unit as actively used. */
    ioreq->ios2_Req.io_Unit   = &devBase->v1000_unit;
    ioreq->ios2_Req.io_Error  = 0;
    devBase->v1000_unit.unit_OpenCnt++;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    /* -------- Phase 6j-1: per-opener state ----------
     * Allocate a V1000Opener for this Open. Parse ios2_BufferManagement
     * (SANA-II Rev 4 tag list) for S2_CopyFromBuff and S2_CopyToBuff —
     * the caller-supplied function pointers we'll invoke via
     * IUtility->CallHookPkt on TX and RX. NULL is fine (RAW-only
     * clients like testtx don't set them). */
    struct V1000Opener *op = (struct V1000Opener *)devBase->IExec->AllocVecTags(
        sizeof(struct V1000Opener),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!op) {
        /* Non-fatal for legacy RAW-only callers, but flag the error field
         * to signal the caller that cooked-mode won't work. */
        devBase->IExec->DebugPrintF("[virte1000] Open: opener alloc failed\n");
        /* Continue anyway — testopen etc. don't rely on opener state. */
    } else {
        /* SANA-II Rev 4: the caller passes a TAG LIST in
         * ios2_BufferManagement at OpenDevice; the driver parses it,
         * allocates per-opener state, and *replaces*
         * ios2_BufferManagement with an opaque driver-side cookie
         * that the caller carries back on every subsequent IORequest.
         * The Rev 7 Roadshow client stops issuing IO if the driver
         * doesn't do this rewrite — that was the "No buffer space
         * available" AddNetInterface failure. Use our own opener
         * pointer as the cookie so lookup is O(1) and unambiguous. */
        struct TagItem *tags = (struct TagItem *)ioreq->ios2_BufferManagement;
        if (tags && devBase->IUtility) {
            /* Roadshow can supply hook pointers for either the plain
             * S2_CopyToBuff / S2_CopyFromBuff (byte-oriented) OR the
             * 32-bit-aligned variants (S2_CopyToBuff32 etc). It always
             * supplies AT LEAST ONE. We treat them all the same at the
             * call site — CallHookPkt hands off a byte pointer, and
             * Roadshow's hooks internally cope with either alignment.
             * Falling back through the variants means we can bind to
             * a Roadshow that only provides the 32/16-bit forms. */
            op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyFromBuff, 0, tags);
            if (!op->copy_from_buff)
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff32, 0, tags);
            if (!op->copy_from_buff)
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff16, 0, tags);

            op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyToBuff, 0, tags);
            if (!op->copy_to_buff)
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff32, 0, tags);
            if (!op->copy_to_buff)
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff16, 0, tags);
        }
        op->bm_cookie = (APTR)op;   /* self-referential cookie */
        ioreq->ios2_BufferManagement = (APTR)op;
        devBase->IExec->NewList((struct List *)&op->read_queue);
        devBase->IExec->ObtainSemaphore(&devBase->opener_lock);
        devBase->IExec->AddTail((struct List *)&devBase->opener_list,
                                 (struct Node *)&op->node);
        devBase->IExec->ReleaseSemaphore(&devBase->opener_lock);
    }

    return devBase;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;

    /* Find + remove this caller's Opener from the list. Matching by
     * bm_cookie — since Phase 7a's BM-cookie rewrite, cookie is now
     * a pointer to the opener itself (self-referential). Roadshow
     * calls CloseDevice with the same cookie it received at Open.
     *
     * CRITICAL: drain the opener's read_queue BEFORE freeing. Roadshow
     * (and any well-behaved caller) is supposed to AbortIO its pending
     * requests first, but if any survive to Close, we MUST reply them
     * — otherwise the caller's WaitIO deadlocks forever and their
     * request memory leaks. Reply with IOERR_ABORTED, same as if
     * AbortIO had been called. Also removes the opener from the
     * driver-side list atomically so the unit_task's process_rx
     * doesn't race and try to deliver into a freed opener. */
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
            /* Drain queued CMD_READs while still holding opener_lock —
             * this locks out process_rx from adding/removing anything.
             * A Node is at offset 0 of IOSana2Req (through io_Message
             * at offset 0 of ios2_Req at offset 0), so cast directly. */
            struct Node *node;
            while ((node = IExec->RemHead(
                        (struct List *)&found->read_queue)) != NULL) {
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
    /* Development builds: force expunge on last-close so each testopen
     * run re-runs Init and picks up freshly pushed binaries. In release
     * builds we behave normally (stay resident until LIBF_DELEXP set). */
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

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;
    BPTR seglist = (BPTR)NULL;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = devBase->dev_SegList;
        IExec->Remove((struct Node *)devBase);

        /* Release resources in reverse-acquire order.
         *
         * FIRST: mask interrupts + remove the IntServer so a late-arriving
         * interrupt can't dereference base fields we're about to free. */
        if (devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
            (void)e1000_read32(mmio, E1000_REG_ICR);
        }
        if (devBase->irq_installed) {
            IExec->RemIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = FALSE;
        }

        /* Phase 6k: stop the unit task before freeing anything it may
         * still reference. Order: quiesce HW (above) → RemIntServer
         * (so ISR can't run) → stop task (so it can't process late-
         * arriving signals against freed memory). */
        v1000_task_stop(devBase, IExec);

        /* Free any Openers still on the list (there shouldn't be any at
         * Expunge time — Close should have cleaned up — but defensive). */
        {
            struct V1000Opener *op;
            IExec->ObtainSemaphore(&devBase->opener_lock);
            while ((op = (struct V1000Opener *)
                    IExec->RemHead((struct List *)&devBase->opener_list)) != NULL) {
                IExec->FreeVec(op);
            }
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }

        if (devBase->tx_scratch) { IExec->FreeVec(devBase->tx_scratch); devBase->tx_scratch = NULL; }
        if (devBase->rx_buffers) { IExec->FreeVec(devBase->rx_buffers); devBase->rx_buffers = NULL; }
        if (devBase->tx_ring)    { IExec->FreeVec(devBase->tx_ring);    devBase->tx_ring    = NULL; }
        if (devBase->rx_ring)    { IExec->FreeVec(devBase->rx_ring);    devBase->rx_ring    = NULL; }
        if (devBase->bar0) {
            devBase->pciDevice->FreeResourceRange(devBase->bar0);
            devBase->bar0 = NULL;
        }
        if (devBase->pciDevice) {
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }
        if (devBase->IPCI) {
            IExec->DropInterface((struct Interface *)devBase->IPCI);
            devBase->IPCI = NULL;
        }
        if (devBase->ExpansionBase) {
            IExec->CloseLibrary(devBase->ExpansionBase);
            devBase->ExpansionBase = NULL;
        }
        if (devBase->IUtility) {
            IExec->DropInterface((struct Interface *)devBase->IUtility);
            devBase->IUtility = NULL;
        }
        if (devBase->UtilityBase) {
            IExec->CloseLibrary(devBase->UtilityBase);
            devBase->UtilityBase = NULL;
        }

        IExec->DebugPrintF("[virte1000] Expunge: goodbye.\n");
        IExec->DeleteLibrary((struct Library *)devBase);
    } else {
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }
    return seglist;
}

/* -------- Phase 6a: online/offline hardware helpers ----------
 * Called from BeginIO's S2_ONLINE / S2_OFFLINE cases (below). Keeping
 * the actual register writes here rather than inline in the dispatch
 * so the state-machine logic stays readable and the hardware sequence
 * can be reused by future code paths (S2_CONFIGINTERFACE that changes
 * MAC while online would need offline-then-online semantics, for
 * example). */
#define E1000_RCTL_EN     (1U << 1)
#define E1000_RCTL_UPE    (1U << 3)   /* Unicast Promiscuous — accept any dst MAC */
#define E1000_RCTL_MPE    (1U << 4)   /* Multicast Promiscuous — accept any multicast */
#define E1000_RCTL_BAM    (1U << 15)
#define E1000_RCTL_SECRC  (1U << 26)
#define E1000_TCTL_EN     (1U << 1)
#define E1000_TCTL_PSP    (1U << 3)
#define E1000_TCTL_CT(v)   (((v) & 0xff)  << 4)
#define E1000_TCTL_COLD(v) (((v) & 0x3ff) << 12)
#define E1000_INTR_TXDW    (1U << 0)
#define E1000_INTR_LSC     (1U << 2)
#define E1000_INTR_RXT0    (1U << 7)

static void v1000_online_hw(struct Virte1000Base *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    /* Phase 6l: enable UPE/MPE to eliminate RCTL filtering as a suspect —
     * MAC-filter miss would silently drop the ARP reply from QEMU. With
     * promiscuous on, any frame QEMU sends to the wire lands in the ring. */
    e1000_write32(mmio, E1000_REG_RCTL,
                  E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE
                  | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write32(mmio, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP
                                       | E1000_TCTL_CT(0x0F) | E1000_TCTL_COLD(0x40));
    e1000_write32(mmio, E1000_REG_IMS, E1000_INTR_LSC | E1000_INTR_RXT0 | E1000_INTR_TXDW);
}

static void v1000_offline_hw(struct Virte1000Base *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
    (void)e1000_read32(mmio, E1000_REG_ICR);
    e1000_write32(mmio, E1000_REG_RCTL, 0);
    e1000_write32(mmio, E1000_REG_TCTL, 0);
}

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;

    /* Default result — commands that we handle overwrite these below. */
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    switch (ioreq->ios2_Req.io_Command) {

    case S2_DEVICEQUERY: {
        /* SANA-II-NOTES §3.1 + §5. The caller passes a pointer to a
         * Sana2DeviceQuery in ios2_StatData with SizeAvailable set;
         * we fill in the OUT fields and set SizeSupplied. Values
         * hardcoded for a QEMU-emulated 82540EM — BPS reports 1 Gbps
         * because QEMU never negotiates below max. */
        struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)ioreq->ios2_Data;
        /* Note: spec places the struct pointer in ios2_StatData per §3.1,
         * but historic drivers accept it in ios2_Data too because
         * Roadshow has been observed to set both. We honor ios2_Data
         * first; if NULL, fall back to ios2_StatData. */
        if (!q) q = (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        ULONG size_full = (ULONG)sizeof(struct Sana2DeviceQuery);
        ULONG size_out  = (q->SizeAvailable < size_full) ? q->SizeAvailable : size_full;
        q->SizeSupplied  = size_out;
        q->DevQueryFormat = 0;
        q->DeviceLevel    = 0;
        q->AddrFieldSize  = 48;
        q->MTU            = 1500;
        /* BPS: report 100 Mbps rather than 1 Gbps. QEMU's e1000 model
         * always says "link UP full duplex 1000Mb" via STATUS, but
         * Roadshow may pre-allocate scaled buffer pools based on BPS;
         * 1G is a well-known ceiling that trips old TCP/IP stacks.
         * 100Mbps is well-inside every stack's comfort zone and still
         * accurate to what QEMU's user-mode NAT actually delivers. */
        q->BPS            = 100000000UL;
        q->HardwareType   = S2WireType_Ethernet;
        q->RawMTU         = 1514;
        break;
    }

    case NSCMD_DEVICEQUERY: {
        /* New-style device query — Roadshow and other stack layers call
         * this to enumerate supported commands. OS4 exec may ALSO be
         * calling it internally before dispatching commands, then rejecting
         * anything not in the list with IOERR_UNITBUSY. Format from
         * <devices/newstyle.h>: caller sets ios2_Data (via IOStdReq view:
         * io_Data at offset 40 = ios2_SrcAddr[0..3] here — but Roadshow
         * uses IOStdReq, not IOSana2Req, for this call, so we access
         * fields through the IOStdReq layout). */
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
        /* Phase 6j-3: reap one RX descriptor and deliver via the
         * caller's CopyToBuff hook.
         *
         * Contract:
         *   IN:  ios2_Data           = target cookie (opaque, passed to
         *                              CopyToBuff as schm_To)
         *        ios2_BufferManagement = same as at Open (opener lookup)
         *   OUT: ios2_DataLength     = frame length (payload after 14-byte
         *                              Ethernet header stripped)
         *        ios2_PacketType     = ethertype from the frame
         *        ios2_SrcAddr[0..5]  = source MAC from the frame
         *        io_Error            = 0 on success, IOERR_UNITBUSY if
         *                              no DD-set descriptor
         *
         * Simplification: only walks slot 0 for this first cut. Real
         * driver needs rx_next_clean tracking + RDT refill. */
        if (devBase->state != V1000_STATE_ONLINE || !devBase->rx_ring) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile UBYTE *rr = (volatile UBYTE *)devBase->rx_ring;
        volatile UBYTE *rd = rr + (0 * V1000_DESC_SIZE);  /* slot 0 */
        if (!(rd[12] & 0x01)) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
            break;
        }
        /* Length in bytes 8-9 (LE). */
        ULONG frame_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
        if (frame_len < 14 || frame_len > V1000_RX_BUFSIZE) {
            /* Malformed length — clear descriptor + refuse. */
            rd[12] = 0;
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            break;
        }
        UBYTE *rxbuf = (UBYTE *)devBase->rx_buffers + (0 * V1000_RX_BUFSIZE);

        /* Fill SrcAddr from the frame (bytes 6..11). Ethertype = 12..13. */
        for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
        ioreq->ios2_PacketType = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];
        ULONG payload_len = frame_len - 14;

        /* Try cooked delivery via CopyToBuff hook. If we have an
         * opener with a hook + IUtility, invoke it with schm_To =
         * caller's cookie (ios2_Data), schm_From = payload start
         * (after Ethernet header). */
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        if (op && op->copy_to_buff && devBase->IUtility && ioreq->ios2_Data) {
            struct SANA2CopyHookMsg msg;
            msg.schm_Method  = S2_CopyToBuff;
            msg.schm_MsgSize = sizeof(msg);
            msg.schm_To      = ioreq->ios2_Data;
            msg.schm_From    = rxbuf + 14;      /* payload after Ethernet header */
            msg.schm_Size    = payload_len;
            BOOL ok = (BOOL)(ULONG)devBase->IUtility->CallHookPkt(
                (struct Hook *)op->copy_to_buff, ioreq, &msg);
            if (!ok) {
                ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                /* fall through — clear descriptor either way */
            }
        }
        /* If no hook installed, caller can inspect payload directly:
         * we've populated ios2_PacketType / SrcAddr, and they know
         * where their own buffer is. This is for testrx-style callers. */

        ioreq->ios2_DataLength = payload_len;

        /* Reset descriptor status so HW can reuse this slot on wrap.
         * Clean would be: advance RDT to hand the slot back to HW.
         * For the first-cut single-slot demo we just clear DD; the
         * RDT stays where it is. Enough to prove the round-trip. */
        rd[12] = 0;
        break;
    }

    case V1000_DBG_STATUS: {
        /* virte1000 private diagnostic. Field overloads:
         *   ios2_DataLength = irq_counter (total ISR fires since Init)
         *   ios2_WireError  = last_icr    (bitmap of last causes seen)
         *   ios2_PacketType = state       (V1000_STATE_*)
         *   ios2_SrcAddr[0..3] = TPT (Total Packets Transmitted)
         *   ios2_SrcAddr[4..7] = RDH (RX Descriptor Head — advances on each
         *                       packet HW DMAs into our ring; also
         *                       accumulates count of DD-set descriptors)
         *   ios2_DstAddr[0..3] = count of RX descriptors whose STATUS.DD
         *                       bit is currently set (unclaimed packets)
         *   ios2_DstAddr[4..5] = length of first DD-set RX descriptor
         */
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

            /* Scan RX descriptors for STATUS.DD set. Reset SrcAddr[0..3]
             * ← wait we just used those for TPT. Use DstAddr for the DD
             * scan instead. */
            ULONG dd_count = 0;
            ULONG first_len = 0;
            if (devBase->rx_ring) {
                volatile UBYTE *rr = (volatile UBYTE *)devBase->rx_ring;
                for (int i = 0; i < V1000_RING_ENTRIES; i++) {
                    volatile UBYTE *rd = rr + (i * V1000_DESC_SIZE);
                    /* Legacy RX descriptor: bytes 8-9 = length (LE),
                     * byte 12 = status (bit 0 = DD, done). */
                    if (rd[12] & 0x01) {
                        if (dd_count == 0) {
                            first_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
                        }
                        dd_count++;
                    }
                }
            }
            ioreq->ios2_DstAddr[0] = (UBYTE)(dd_count >> 24);
            ioreq->ios2_DstAddr[1] = (UBYTE)(dd_count >> 16);
            ioreq->ios2_DstAddr[2] = (UBYTE)(dd_count >> 8);
            ioreq->ios2_DstAddr[3] = (UBYTE)(dd_count);
            ioreq->ios2_DstAddr[4] = (UBYTE)(first_len >> 8);
            ioreq->ios2_DstAddr[5] = (UBYTE)(first_len);
            /* Phase 6l task-path diagnostics: */
            ioreq->ios2_DstAddr[6]  = (UBYTE)(devBase->task_wake_count >> 8);
            ioreq->ios2_DstAddr[7]  = (UBYTE)(devBase->task_wake_count);
            ioreq->ios2_DstAddr[8]  = (UBYTE)(devBase->process_rx_dd_seen >> 8);
            ioreq->ios2_DstAddr[9]  = (UBYTE)(devBase->process_rx_dd_seen);
            ioreq->ios2_DstAddr[10] = (UBYTE)(devBase->process_rx_delivered >> 8);
            ioreq->ios2_DstAddr[11] = (UBYTE)(devBase->process_rx_delivered);
        }
        break;
    }

    case V1000_DBG_CMDLOG: {
        /* Dump the cmdlog ring into caller's buffer. Layout per entry
         * (8 bytes): 2 cmd, 2 ioerr, 4 wire — 32 entries × 8 = 256 B.
         * Returns the ring's head index (mod 32) so caller can walk in
         * chronological order: (head - 32 + i) & 31. */
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        if (out && ioreq->ios2_DataLength >= 32 * 8) {
            ULONG head = devBase->cmdlog_head;
            for (int i = 0; i < 32; i++) {
                UBYTE *p = out + (i * 8);
                p[0] = (UBYTE)(devBase->cmdlog[i].cmd >> 8);
                p[1] = (UBYTE)(devBase->cmdlog[i].cmd);
                p[2] = (UBYTE)(devBase->cmdlog[i].ioerr >> 8);
                p[3] = (UBYTE)(devBase->cmdlog[i].ioerr);
                p[4] = (UBYTE)(devBase->cmdlog[i].wire >> 24);
                p[5] = (UBYTE)(devBase->cmdlog[i].wire >> 16);
                p[6] = (UBYTE)(devBase->cmdlog[i].wire >> 8);
                p[7] = (UBYTE)(devBase->cmdlog[i].wire);
            }
            ioreq->ios2_DataLength = head;   /* return head so caller can compute order */
        }
        break;
    }

    case V1000_DBG_DUMPTX: {
        /* Dump first 16 bytes of the last-built cooked TX frame so callers
         * can eyeball the on-wire framing (Ethernet dst/src/etype + start
         * of payload). Overloads ios2_SrcAddr[0..15]. */
        if (devBase->tx_scratch) {
            UBYTE *s = (UBYTE *)devBase->tx_scratch;
            for (int i = 0; i < 16; i++) ioreq->ios2_SrcAddr[i] = s[i];
        }
        break;
    }

    case V1000_DBG_FIRE_IRQ: {
        /* Force one ISR fire via ICS. Only meaningful when we're ONLINE
         * (IMS unmask must have already happened) — otherwise ICR bits
         * set here are just masked and never delivered. Used by testirq
         * to verify the IRQ path end-to-end after state transitions. */
        if (devBase->state != V1000_STATE_ONLINE || !devBase->bar0) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_ICS, E1000_INTR_LSC);
        /* Return the (probably updated) counter for convenience. */
        ioreq->ios2_DataLength = devBase->irq_counter;
        break;
    }

    case S2_CONFIGINTERFACE: {
        /* SANA-II-NOTES §3.3: caller sets ios2_SrcAddr = MAC to program;
         * all-zeros means "use factory".
         *
         * Phase 7a: Roadshow calls S2_CONFIGINTERFACE TWICE at bind time
         * (once to probe, once to actually configure — cmdlog trace
         * proved this). Under the strict "only from OFFLINE" reading of
         * the spec, the second call returned S2ERR_BAD_STATE and made
         * Roadshow report "No buffer space available" for the whole
         * add. Real drivers (rtl8139.device etc.) accept the second call
         * idempotently as long as no MAC change is requested. */
        BOOL want_factory = TRUE;
        for (int i = 0; i < 6; i++)
            if (ioreq->ios2_SrcAddr[i] != 0) { want_factory = FALSE; break; }

        if (devBase->state != V1000_STATE_OFFLINE) {
            /* Second (or later) config call — accept as no-op IF caller
             * wants factory MAC (or the same MAC we already have). Refuse
             * only actual MAC changes while online / already-configured. */
            if (!want_factory) {
                BOOL same_mac = TRUE;
                for (int i = 0; i < 6; i++)
                    if (ioreq->ios2_SrcAddr[i] != devBase->mac[i]) {
                        same_mac = FALSE; break;
                    }
                if (!same_mac) {
                    ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
                    ioreq->ios2_WireError    = S2WERR_IS_CONFIGURED;
                    break;
                }
            }
            /* Same MAC (or factory) — success, no state change. Still
             * fill in SrcAddr/DstAddr per SANA-II §3.3, since Roadshow
             * reads these on every CONFIGINTERFACE reply, not just the
             * first. */
            for (int i = 0; i < 6; i++) {
                ioreq->ios2_SrcAddr[i] = devBase->mac[i];
                ioreq->ios2_DstAddr[i] = 0xFF;
            }
            break;
        }
        if (!want_factory) {
            /* TODO Phase 6b: program RAL/RAH from ios2_SrcAddr. For now
             * we accept the request and refuse to bounce the factory MAC,
             * so callers can pass their own address without an error but
             * the physical MAC on the wire won't actually change until we
             * wire the write. */
        }
        devBase->state = V1000_STATE_CONFIGURED;
        /* SANA-II §3.3 (fell through): on return, ios2_SrcAddr must
         * hold the interface's hardware address (its own MAC) and
         * ios2_DstAddr must hold the broadcast hardware address. This
         * is how Roadshow discovers the device's broadcast MAC — if
         * it's not set here, Roadshow doesn't know how to send to
         * broadcast and quietly abandons config after this call. */
        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = devBase->mac[i];
            ioreq->ios2_DstAddr[i] = 0xFF;
        }
        break;
    }

    case S2_ONLINE: {
        /* SANA-II-NOTES §3.4: requires CONFIGURED. On success enables
         * RX/TX and unmasks IRQs, transitions state to ONLINE. */
        if (devBase->state == V1000_STATE_ONLINE) {
            /* Idempotent success — no state change, no hardware toggle. */
            break;
        }
        if (devBase->state != V1000_STATE_CONFIGURED) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_NOT_CONFIGURED;
            break;
        }
        v1000_online_hw(devBase);
        devBase->state = V1000_STATE_ONLINE;
        break;
    }

    case S2_OFFLINE: {
        /* SANA-II-NOTES §3.5: always succeeds. Disables hardware and
         * demotes to CONFIGURED (config retained; caller can S2_ONLINE
         * again without re-configuring). */
        if (devBase->state == V1000_STATE_ONLINE) {
            v1000_offline_hw(devBase);
            devBase->state = V1000_STATE_CONFIGURED;
        }
        /* Phase 6c will: reply all pending IO requests with S2ERR_OUTOFSERVICE
         * and signal S2EVENT_OFFLINE to ONEVENT queues. Nothing queued yet. */
        break;
    }

    case V1000_DBG_SEND:
    case CMD_WRITE:
    case S2_BROADCAST: {
        /* Phase 6b/e/f — TX PARTIALLY IMPLEMENTED, blocked on two issues:
         *
         *   1. CMD_WRITE (io_Command=3) and S2_BROADCAST (17) never reach
         *      this case via DoIO/SendIO/direct-BeginIO — OS4 exec rejects
         *      with IOERR_UNITBUSY (-6) before dispatch. Discriminator is
         *      io_Command-value based; not io_Flags, not io_Unit struct,
         *      not MsgPort mode. Private cmds 0xF001..0xF003 dispatch
         *      fine, 0xF010 doesn't. See memory os4-doio-ioflags-quirks.
         *
         *   2. Even when reached via V1000_DBG_SEND (0xF003), the full TX
         *      code path either drops the descriptor silently (TDH advances
         *      but STATUS.DD never sets, TPT stays 0) or hangs the caller
         *      when we add IExec->CachePreDMA/CachePostDMA. Root cause
         *      not identified.
         *
         * Current behavior: report dispatch success but do nothing. Real
         * TX code lives in the #if 0 block below, preserved for the next
         * debugging session. */

        if (devBase->state != V1000_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }

        /* Frame length sanity: 60 bytes minimum (pad-to-min handled by
         * PSP in TCTL) up to RawMTU=1514. */
        ULONG len = ioreq->ios2_DataLength;
        if (len == 0 || len > 1514 || !ioreq->ios2_Data ||
            !devBase->rx_ring /* proxy for "rings alloc succeeded" */) {
            ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        ioreq->ios2_WireError = 0x3000;

        /* Two shapes: COOKED (opener has hook) or RAW (memcpy).
         * Use the dedicated tx_scratch buffer — NOT rx_buffers[slot],
         * which is what the RX ring's descriptor also points at. Sharing
         * caused HW TX-read to race with HW RX-write when a reply frame
         * arrived during outbound send. */
        if (!devBase->tx_scratch) {
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            break;
        }
        ULONG slot = devBase->tx_next_slot;         /* TX descriptor index */
        UBYTE *dst = (UBYTE *)devBase->tx_scratch;  /* dedicated frame buf */

        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = v1000_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        BOOL cooked = (op != NULL) && (op->copy_from_buff != NULL) && (devBase->IUtility != NULL);

        if (cooked) {
            /* Build 14-byte Ethernet header */
            for (int i = 0; i < 6; i++) dst[i]     = ioreq->ios2_DstAddr[i];  /* dst */
            for (int i = 0; i < 6; i++) dst[6 + i] = devBase->mac[i];         /* src */
            uint16 etype = (uint16)(ioreq->ios2_PacketType & 0xFFFF);
            dst[12] = (UBYTE)(etype >> 8);
            dst[13] = (UBYTE)(etype & 0xFF);

            /* Pull payload via the caller's CopyFromBuff hook. Per
             * SANA-II-NOTES §2 + pa6t_eth pattern, invocation goes
             * through IUtility->CallHookPkt with a SANA2CopyHookMsg. */
            struct SANA2CopyHookMsg msg;
            msg.schm_Method  = S2_CopyFromBuff;
            msg.schm_MsgSize = sizeof(msg);
            msg.schm_To      = dst + 14;
            msg.schm_From    = ioreq->ios2_Data;    /* opaque cookie */
            msg.schm_Size    = len;
            BOOL ok = (BOOL)(ULONG)devBase->IUtility->CallHookPkt(
                (struct Hook *)op->copy_from_buff, ioreq, &msg);
            if (!ok) {
                ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                break;
            }
            /* Frame length now includes the Ethernet header. Explicitly
             * zero-pad to the 60-byte Ethernet minimum. Manual byte-at-a-
             * time — a plain while-loop optimizes to memset() which pulls
             * newlib into a resident driver and fails to link. */
            len = 14 + len;
            volatile UBYTE *pad = dst;   /* volatile stops the memset
                                          * optimization */
            for (ULONG i = len; i < 60; i++) pad[i] = 0;
            if (len < 60) len = 60;
        } else {
            /* RAW: caller supplied full L2 frame in ios2_Data. */
            UBYTE *src = (UBYTE *)ioreq->ios2_Data;
            for (ULONG i = 0; i < len; i++) dst[i] = src[i];
        }

        /* Physical addr of the TX scratch. StartDMA also flushes the
         * data cache — critical since we just wrote the frame bytes.
         * Pair with EndDMA after we're sure HW is done. */
        ULONG frame_len_io = len;
        uint32 buf_phys = v1000_dma_phys(IExec, dst, len, DMA_ReadFromRAM);
        if (!buf_phys) {
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            break;
        }

        /* Build descriptor at tx_ring[slot]. buffer_addr = PHYSICAL. */
        volatile UBYTE *d = (volatile UBYTE *)devBase->tx_ring + (slot * V1000_DESC_SIZE);
        poke_le32(d +  0, buf_phys);
        poke_le32(d +  4, 0);
        poke_le32(d +  8, (uint32)len
                          | ((uint32)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS
                                      | E1000_TXD_CMD_RS) << 24));
        poke_le32(d + 12, 0);

        /* Full memory barrier — ensures descriptor writes hit RAM before
         * the doorbell tells HW to start reading. pa6t_eth does the same. */
        v1000_wmb();

        /* Bump TDT — TX doorbell. HW pulls the descriptor + frame. */
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        ULONG new_tdt = (slot + 1) % V1000_RING_ENTRIES;
        e1000_write32(mmio, E1000_REG_TDT, new_tdt);
        devBase->tx_next_slot = new_tdt;

        /* Poll for STATUS.DD. HW writes it back via DMA. */
        BOOL done = FALSE;
        for (int i = 0; i < 100000; i++) {
            if (d[12] & E1000_TXD_STAT_DD) { done = TRUE; break; }
            __asm__ volatile ("" : : : "memory");
        }

        /* Release the DMA hold on the frame buffer. */
        IExec->EndDMA(dst, frame_len_io, DMA_ReadFromRAM);
        /* Stash more diagnostic state: TDH, TDT, descriptor status byte,
         * TCTL — so testtx can see what HW did. */
        uint32 tdh_now  = e1000_read32(mmio, E1000_REG_TDH);
        uint32 tdt_now  = e1000_read32(mmio, E1000_REG_TDT);
        uint32 tctl_now = e1000_read32(mmio, E1000_REG_TCTL);
        UBYTE  stat_byte = d[12];
        /* Pack into ios2 fields:
         *   ios2_DataLength = TDH | (TDT << 16)
         *   ios2_PacketType = tctl
         *   ios2_WireError  = status_byte | (done << 8) | 0x7000
         */
        ioreq->ios2_DataLength = (ULONG)tdh_now | ((ULONG)tdt_now << 16);
        ioreq->ios2_PacketType = tctl_now;
        ioreq->ios2_WireError  = ((ULONG)stat_byte)
                               | ((ULONG)(done ? 1 : 0) << 8)
                               | 0x7000;

        if (!done) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
        }
        break;
    }

    case S2_READORPHAN:
        /* Frames not claimed by any type-tracked opener. We don't do
         * per-type filtering, so orphan reads are structurally
         * identical to CMD_READ — fall through. */
        /* fallthrough */
    case CMD_READ: {
        /* Phase 6k: queue the caller's CMD_READ on their opener's
         * read queue. Unit task delivers via CopyToBuff when a frame
         * arrives (see v1000_process_rx). Caller is expected to have
         * used SendIO + WaitIO (or DoIO — but DoIO's IOF_QUICK clear
         * means it Wait's on the reply port anyway). */
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

        /* Clear IOF_QUICK so the caller's WaitIO actually waits for the
         * reply. Also — since we did NOT set io_Error and are NOT
         * ReplyMsg'ing here, return early to skip the tail ReplyMsg. */
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }

    case S2_GETSTATIONADDRESS: {
        /* SANA-II-NOTES §3.2: write factory MAC into ios2_SrcAddr and
         * currently-configured MAC into ios2_DstAddr. Until Phase 6 wires
         * S2_CONFIGINTERFACE (which lets a caller override the MAC), both
         * addresses are the factory value we cached from RAL/RAH. */
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

    /* Phase 7a: Roadshow-required no-op handlers. Advertising these in
     * v1000_supported_cmds without a handler makes Roadshow's DHCP fail
     * with "broadcast access not supported". Returning success (0) is
     * fine because our RCTL already accepts broadcast (BAM) and all
     * multicast (MPE), so subscription requests are effectively already
     * honored at the HW level — no per-address filter table to update. */
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
        /* Success. HW is already accepting broadcast + all multicast. */
        break;

    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        /* Optional per-ethertype packet-count tracking. We don't keep
         * per-type counters yet; report success so Roadshow (which
         * tracks 0x0800/0x0806/0x86DD at bind time) proceeds. */
        break;

    case S2_ONEVENT: {
        /* SANA-II event notification. Real drivers queue the request
         * and reply when a subscribed event fires (S2EVENT_ONLINE etc).
         * Phase 7a: if the caller is subscribing to an event whose
         * current state is already true, reply immediately with the
         * event mask; otherwise reply with success but empty mask.
         * This is enough for Roadshow to proceed — it subscribes to
         * S2EVENT_ONLINE at bind time, and we're ONLINE by then. */
        ULONG mask = ioreq->ios2_WireError;   /* caller-supplied event mask */
        ULONG match = 0;
        if ((mask & S2EVENT_ONLINE)  && devBase->state == V1000_STATE_ONLINE)  match |= S2EVENT_ONLINE;
        if ((mask & S2EVENT_OFFLINE) && devBase->state != V1000_STATE_ONLINE)  match |= S2EVENT_OFFLINE;
        ioreq->ios2_WireError = match ? match : mask;
        /* io_Error stays 0 — event either matched or the caller can
         * inspect WireError. Not queuing yet; Roadshow tolerates this
         * pattern in existing drivers (rtl8139.device, etc). */
        break;
    }

    case S2_GETGLOBALSTATS: {
        /* Fill Sana2DeviceStats with zeros — we don't yet maintain
         * per-driver counters. Roadshow uses this for stats display
         * only; returning zeros is safer than IOERR_NOCMD. */
        struct Sana2DeviceStats *st = (struct Sana2DeviceStats *)ioreq->ios2_StatData;
        if (!st) st = (struct Sana2DeviceStats *)ioreq->ios2_Data;
        if (st) {
            /* volatile stops GCC optimizing the loop into memset() —
             * memset would pull newlib into a resident driver and fail
             * to link. Same trick as the TX zero-pad in CMD_WRITE. */
            volatile UBYTE *p = (volatile UBYTE *)st;
            for (ULONG i = 0; i < sizeof(struct Sana2DeviceStats); i++) p[i] = 0;
        }
        break;
    }

    default:
        /* Every other command still stubs to NOCMD — Phase 4+ will wire
         * them in one by one, following the same fail-soft pattern. */
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    /* Phase 7a: log this BeginIO invocation into the cmdlog ring so
     * DBG_CMDLOG can dump what Roadshow does. Skip if this is the
     * DBG_CMDLOG or DBG_STATUS call itself — those spam the log. */
    if (ioreq->ios2_Req.io_Command != V1000_DBG_CMDLOG
        && ioreq->ios2_Req.io_Command != V1000_DBG_STATUS
        && ioreq->ios2_Req.io_Command != V1000_DBG_DUMPTX) {
        ULONG i = devBase->cmdlog_head & 31;
        devBase->cmdlog[i].cmd   = ioreq->ios2_Req.io_Command;
        devBase->cmdlog[i].ioerr = ioreq->ios2_Req.io_Error;
        devBase->cmdlog[i].wire  = ioreq->ios2_WireError;
        devBase->cmdlog_head++;
    }

    if (ioreq->ios2_Req.io_Flags & IOF_QUICK)
        return;

    IExec->ReplyMsg((struct Message *)ioreq);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    /* Phase 6l: honest AbortIO. Walk every opener's read_queue looking
     * for this request; if found, unlink and reply with IOERR_ABORTED.
     * Without this, a caller who SendIO's CMD_READ then AbortIO's it
     * (because timeout / user cancel / test cleanup) will have their
     * WaitIO block forever — the request never gets ReplyMsg'd. */
    struct Virte1000Base *base = (struct Virte1000Base *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;
    struct V1000Opener *op;
    struct MinNode *node, *next;
    BOOL found = FALSE;

    IExec->ObtainSemaphore(&base->opener_lock);
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ && !found;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        for (node = op->read_queue.mlh_Head;
             (next = node->mln_Succ) != NULL;
             node = next) {
            if ((APTR)node == (APTR)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove((struct Node *)node);
                found = TRUE;
                break;
            }
        }
    }
    IExec->ReleaseSemaphore(&base->opener_lock);

    if (!found) return IOERR_NOCMD;   /* not on any queue */
    ioreq->ios2_Req.io_Error = IOERR_ABORTED;
    IExec->ReplyMsg((struct Message *)ioreq);
    return 0;
}
