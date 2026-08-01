/*
 * virte1000.device — SANA-II network device driver for the Intel e1000
 * (82540EM) as emulated by QEMU on the sam460ex AmigaOS 4.1 PPC target.
 *
 * Core device entry, Resident Tag, vector table, and Obtain/Release methods.
 */

#include "virte1000.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/emulation.h>

#include <devices/newstyle.h>

#include <dos/dos.h>
#include <interfaces/dos.h>

#include <exec/ports.h>

#include <stdarg.h>
#include <stddef.h>

#define VIRTE1000_TRACE_FILE   "RAM:virte1000-trace.log"

void v1000_trace_open(struct Virte1000Base *base)
{
    if (base->trace_fh) return;   /* already open */
    if (!base->DOSBase) {
        base->DOSBase = base->IExec->OpenLibrary("dos.library", 51);
        if (!base->DOSBase) return;
        base->IDOS = base->IExec->GetInterface(base->DOSBase, "main", 1, NULL);
        if (!base->IDOS) {
            base->IExec->CloseLibrary(base->DOSBase);
            base->DOSBase = NULL;
            return;
        }
    }
    struct DOSIFace *IDOS = (struct DOSIFace *)base->IDOS;
    base->trace_fh = IDOS->Open((CONST_STRPTR)VIRTE1000_TRACE_FILE, MODE_NEWFILE);
}

/*
 * Manager Obtain/Release — reference counting on the interface itself.
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

/* OS4 vector table for DeviceManagerInterface. */
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

/* 68k-compat jump table. */
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

/* Resident struct */
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

/* Shell entry point stub. */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to SYS:Kickstart/ or "
                       "OpenDevice() from a test program. Cannot run from shell.\n",
                       DEVNAME);
    return 20; /* RETURN_FAIL */
}
