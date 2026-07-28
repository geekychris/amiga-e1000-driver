/*
 * testirq — verify IRQ delivery.
 *
 * Init has already forced one interrupt via ICS write (see Phase 5d in
 * device.c). testirq opens the device and calls a driver-private status
 * command (io_Command 0xF001) that returns irq_counter + last_icr in
 * ios2_DataLength / ios2_WireError. PASS iff counter > 0.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#define V1000_DBG_STATUS  0xF001

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;

    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("virte1000.device", 0, (struct IORequest *)req, 0);
    if (err != 0) {
        IDOS->Printf("testirq: OpenDevice = %ld (FAIL)\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    req->ios2_Req.io_Command = V1000_DBG_STATUS;
    req->ios2_DataLength = 0;
    req->ios2_WireError  = 0;
    LONG rc = IExec->DoIO((struct IORequest *)req);

    IDOS->Printf("V1000_DBG_STATUS  DoIO=%ld  io_Error=%ld\n",
                 (long)rc, (long)req->ios2_Req.io_Error);
    IDOS->Printf("  irq_counter = %lu\n", (unsigned long)req->ios2_DataLength);
    IDOS->Printf("  last_icr    = 0x%08lx\n", (unsigned long)req->ios2_WireError);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    /* Pass iff the ISR fired at least once during Init's ICS test.
     * If counter is 0, either the ISR install failed silently, or the
     * ICS-triggered fire was suppressed by something (unmapped IRQ line,
     * masked at the PIC, etc.). */
    BOOL pass = (rc == 0) && (req->ios2_Req.io_Error == 0)
             && (req->ios2_DataLength > 0);
    if (pass) IDOS->Printf("RESULT: PASS\n");
    else      IDOS->Printf("RESULT: FAIL rc=%ld ioerr=%ld counter=%lu\n",
                           (long)rc, (long)req->ios2_Req.io_Error,
                           (unsigned long)req->ios2_DataLength);
    return pass ? 0 : 20;
}
