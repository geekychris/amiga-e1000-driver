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

#include <dos/dos.h>
#include <interfaces/dos.h>

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

/* e1000 register offsets we currently touch. Full map lives in the QEMU
 * source at hw/net/e1000_regs.h + the 82540EM datasheet; only the offsets
 * we actually read/write get named here. Cross-referenced with the design
 * agent's docs/DESIGN.md §2. */
#define E1000_REG_CTRL     0x00000
#define E1000_REG_STATUS   0x00008
#define E1000_REG_ICR      0x000C0   /* Interrupt Cause Read (RO/read-to-clear) */
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
#define E1000_REG_RAL      0x05400   /* Receive Address Low  */
#define E1000_REG_RAH      0x05404   /* Receive Address High */

/* Little-endian store to main memory. Descriptor entries live in host RAM
 * but the e1000 reads them as LE, so we byte-reverse on the store side. */
static inline void poke_le32(volatile void *p, uint32 v)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(v), "r"(p) : "memory");
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

    /* -------- Phase 4e: enable RX (single RCTL write) ----------
     * DESIGN.md §3 flagged QEMU's flush_queue_timer: writing RCTL stalls
     * RX for 1 s. Do it ONCE, last, so we don't re-arm the timer.
     * Flags used:
     *   EN     = bit 1 (Receiver Enable)
     *   BAM    = bit 15 (Broadcast Accept Mode — pass broadcast frames)
     *   BSIZE  = 00 (bits 16-17) → 2048 B, matching our rx_buffers stride
     *   BSEX   = 0  (bit 25, BSIZE not extended)
     *   SECRC  = bit 26 (Strip Ethernet CRC on RX, so lengths match L2)
     * Everything else (LBM, LPE, promisc, threshold) stays default 0. */
#define E1000_RCTL_EN     (1U << 1)
#define E1000_RCTL_BAM    (1U << 15)
#define E1000_RCTL_SECRC  (1U << 26)
    if (devBase->rx_ring && devBase->rx_buffers) {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        uint32 rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;
        e1000_write32(mmio, E1000_REG_RCTL, rctl);

        uint32 rctl_readback = e1000_read32(mmio, E1000_REG_RCTL);
        uint32 rdh = e1000_read32(mmio, E1000_REG_RDH);
        uint32 rdt = e1000_read32(mmio, E1000_REG_RDT);
        LOGF(log, (CONST_STRPTR)"RX enabled: RCTL=%08lx (written %08lx) RDH=%lu RDT=%lu\n",
             (ULONG)rctl_readback, (ULONG)rctl, (ULONG)rdh, (ULONG)rdt);
        if (rctl_readback != rctl) {
            LOGF(log, (CONST_STRPTR)"RX enabled: WARNING - RCTL readback mismatch "
                 "(reserved bits or QEMU stubbed field)\n");
        }
    }

    /* -------- Phase 4f: enable TX ----------
     * TCTL bits:
     *   EN   = bit 1  (Transmit Enable)
     *   PSP  = bit 3  (Pad Short Packets to 64 B)
     *   CT   = bits 4-11 (Collision Threshold; 0x0F standard, ignored on
     *                     full-duplex which QEMU always is)
     *   COLD = bits 12-21 (Collision Distance; 0x40 for full-duplex)
     * Everything else stays 0. Value = 0x0004_00FA. */
#define E1000_TCTL_EN     (1U << 1)
#define E1000_TCTL_PSP    (1U << 3)
#define E1000_TCTL_CT(v)   (((v) & 0xff)  << 4)
#define E1000_TCTL_COLD(v) (((v) & 0x3ff) << 12)
    if (devBase->tx_ring) {
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        uint32 tctl = E1000_TCTL_EN | E1000_TCTL_PSP
                    | E1000_TCTL_CT(0x0F) | E1000_TCTL_COLD(0x40);
        e1000_write32(mmio, E1000_REG_TCTL, tctl);

        uint32 tctl_readback = e1000_read32(mmio, E1000_REG_TCTL);
        LOGF(log, (CONST_STRPTR)"TX enabled: TCTL=%08lx (written %08lx)\n",
             (ULONG)tctl_readback, (ULONG)tctl);
        if (tctl_readback != tctl) {
            LOGF(log, (CONST_STRPTR)"TX enabled: WARNING - TCTL readback mismatch\n");
        }
    }

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

    ioreq->ios2_Req.io_Unit   = (struct Unit *)devBase; /* placeholder */
    ioreq->ios2_Req.io_Error  = 0;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    return devBase;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct Virte1000Base *devBase = (struct Virte1000Base *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;

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

        /* Release PCI-side resources in reverse-acquire order. Every field
         * may be NULL if Init bailed early — check each.
         *
         * Free rings + buffers first so if the device is still bus-mastering
         * against them (shouldn't be, IRQs are masked and RX/TX disabled)
         * we at least tear down the ring pointers before the memory gets
         * handed back to the pool. Ideal order needs a full HW quiesce
         * first; that's Phase 4a's job at Init and we'll add a matching
         * quiesce-before-teardown once we're actually running the device. */
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
