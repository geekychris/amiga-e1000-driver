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

/* Forward decl — real definitions live further down with the other MMIO
 * helpers. The ISR below needs to call e1000_read32(ICR). */
static inline uint32 e1000_read32(volatile void *base, uint32 off);

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
    devBase->irq_vector    = 0;
    devBase->irq_installed = FALSE;
    devBase->irq_counter   = 0;
    devBase->last_icr      = 0;
    devBase->state         = V1000_STATE_OFFLINE;
    devBase->tx_next_slot  = 0;

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
#define V1000_RING_ENTRIES   16
#define V1000_DESC_SIZE      16
#define V1000_RX_BUFSIZE     2048
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
    if (devBase->rx_ring && devBase->rx_buffers) {
        volatile UBYTE *desc = (volatile UBYTE *)devBase->rx_ring;
        ULONG buf_lo = (ULONG)devBase->rx_buffers;
        for (int i = 0; i < V1000_RING_ENTRIES; i++) {
            volatile UBYTE *d = desc + (i * V1000_DESC_SIZE);
            poke_le32(d + 0, buf_lo + (i * V1000_RX_BUFSIZE));  /* addr low  */
            poke_le32(d + 4, 0);                                 /* addr high */
            poke_le32(d + 8, 0);                                 /* len+csum  */
            poke_le32(d + 12, 0);                                /* status/err/special */
        }

        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_RDBAL, (uint32)(ULONG)devBase->rx_ring);
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
    if (devBase->tx_ring) {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_TDBAL, (uint32)(ULONG)devBase->tx_ring);
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
     * dispatch (needed for CMD_WRITE / S2_BROADCAST / S2_MULTICAST)
     * finds a valid MsgPort. Fake devBase-cast worked for direct-
     * dispatch commands but returned -6 for anything routed through
     * unit_MsgPort. Increment unit_OpenCnt to mirror lib_OpenCnt so
     * exec sees the unit as actively used. */
    ioreq->ios2_Req.io_Unit   = &devBase->v1000_unit;
    ioreq->ios2_Req.io_Error  = 0;
    devBase->v1000_unit.unit_OpenCnt++;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    return devBase;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;

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
         * interrupt can't dereference base fields we're about to free.
         * Since IMS is only touched from Init (masked with IMC there),
         * this write may be a no-op — but the RemIntServer must happen. */
        if (devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);   /* mask all */
            (void)e1000_read32(mmio, E1000_REG_ICR);            /* clear pending */
        }
        if (devBase->irq_installed) {
            IExec->RemIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = FALSE;
        }

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
    e1000_write32(mmio, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
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
        q->BPS            = 1000000000UL;
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

    case V1000_DBG_STATUS: {
        /* virte1000 private: return runtime state to test programs.
         * Not in any SANA-II (< 0x38) or new-style (0xC000+) range.
         * Overloads reply fields:
         *   ios2_DataLength = irq_counter (total ISR fires since Init)
         *   ios2_WireError  = last_icr    (bitmap of last causes seen)
         *   ios2_PacketType = state       (V1000_STATE_*)
         * If online we also stash Total Packets Transmitted in the
         * SrcAddr[0..3] bytes so testtx can grep it — the alternative
         * would be adding another private command or another IOSana2Req
         * field abuse, and this stays cheap. */
        ioreq->ios2_DataLength = devBase->irq_counter;
        ioreq->ios2_WireError  = devBase->last_icr;
        ioreq->ios2_PacketType = devBase->state;
        if (devBase->state == V1000_STATE_ONLINE && devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            uint32 tpt = e1000_read32(mmio, E1000_REG_TPT);
            ioreq->ios2_SrcAddr[0] = (UBYTE)(tpt >> 24);
            ioreq->ios2_SrcAddr[1] = (UBYTE)(tpt >> 16);
            ioreq->ios2_SrcAddr[2] = (UBYTE)(tpt >> 8);
            ioreq->ios2_SrcAddr[3] = (UBYTE)(tpt);
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
         * all-zeros means "use factory". May be called ONCE — second call
         * from any state returns BAD_STATE / IS_CONFIGURED. */
        if (devBase->state != V1000_STATE_OFFLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_IS_CONFIGURED;
            break;
        }
        /* Detect all-zeros SrcAddr → keep factory MAC. Otherwise we'd
         * write the caller-supplied MAC into RAL/RAH here; Phase 6a
         * defers the actual re-program until we exercise it from a test
         * (avoids touching hardware in the untested happy path). */
        BOOL want_factory = TRUE;
        for (int i = 0; i < 6; i++)
            if (ioreq->ios2_SrcAddr[i] != 0) { want_factory = FALSE; break; }
        if (!want_factory) {
            /* TODO Phase 6b: program RAL/RAH from ios2_SrcAddr. For now
             * we accept the request and refuse to bounce the factory MAC,
             * so callers can pass their own address without an error but
             * the physical MAC on the wire won't actually change until we
             * wire the write. */
        }
        devBase->state = V1000_STATE_CONFIGURED;
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
        ioreq->ios2_WireError = 0x3000;   /* passed len check */

        /* Copy caller's frame into the RX buffer pool at the same slot
         * index we'll use for the TX descriptor. rx_buffers is 16 × 2KB. */
        ULONG slot = devBase->tx_next_slot;
        UBYTE *dst = (UBYTE *)devBase->rx_buffers + (slot * V1000_RX_BUFSIZE);
        UBYTE *src = (UBYTE *)ioreq->ios2_Data;
        for (ULONG i = 0; i < len; i++) dst[i] = src[i];
        ioreq->ios2_WireError = 0x4000;

        /* Build descriptor at tx_ring[slot]. All 16 bytes LE. */
        volatile UBYTE *d = (volatile UBYTE *)devBase->tx_ring + (slot * V1000_DESC_SIZE);
        poke_le32(d +  0, (uint32)(ULONG)dst);
        poke_le32(d +  4, 0);
        poke_le32(d +  8, (uint32)len
                          | ((uint32)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS
                                      | E1000_TXD_CMD_RS) << 24));
        poke_le32(d + 12, 0);
        ioreq->ios2_WireError = 0x5000;

        /* Bump TDT — TX doorbell. HW starts pulling. */
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        ULONG new_tdt = (slot + 1) % V1000_RING_ENTRIES;
        e1000_write32(mmio, E1000_REG_TDT, new_tdt);
        devBase->tx_next_slot = new_tdt;
        ioreq->ios2_WireError = 0x6000;

        /* Poll for STATUS.DD. Two-phase:
         *   1. Tight loop (fast, catches near-immediate completion)
         *   2. Delay-based (gives HW real wall-clock time to process
         *      through QEMU's e1000 emulation) */
        BOOL done = FALSE;
        for (int i = 0; i < 1000000; i++) {
            if (d[12] & E1000_TXD_STAT_DD) { done = TRUE; break; }
            __asm__ volatile ("" : : : "memory");
        }
        if (!done) {
            /* Slow path via dos.library/Delay — 1/50 sec per tick.
             * Wait up to 10 ticks = 200 ms total. */
            struct Library *DOSBase = IExec->OpenLibrary("dos.library", 51);
            struct DOSIFace *IDOS = NULL;
            if (DOSBase) IDOS = (struct DOSIFace *)IExec->GetInterface(DOSBase, "main", 1, NULL);
            for (int i = 0; !done && i < 10; i++) {
                if (IDOS) IDOS->Delay(1);
                if (d[12] & E1000_TXD_STAT_DD) done = TRUE;
            }
            if (IDOS) IExec->DropInterface((struct Interface *)IDOS);
            if (DOSBase) IExec->CloseLibrary(DOSBase);
        }
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

    default:
        /* Every other command still stubs to NOCMD — Phase 4+ will wire
         * them in one by one, following the same fail-soft pattern. */
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    if (ioreq->ios2_Req.io_Flags & IOF_QUICK)
        return;

    IExec->ReplyMsg((struct Message *)ioreq);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    (void)Self; (void)ioreq;
    /* Nothing is queued yet — nothing to abort. */
    return IOERR_NOCMD;
}
