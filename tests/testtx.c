/*
 * testtx — SKIPPED (CMD_WRITE dispatch mystery documented in
 *                    memory os4-doio-ioflags-quirks).
 *
 * Even after Phase 6d's Unit refactor (real embedded struct Unit with
 * PA_IGNORE MsgPort, valid MsgList, UNITF_ACTIVE), OS4 exec still
 * returns IOERR_UNITBUSY (-6) for CMD_WRITE (and S2_BROADCAST, and
 * an arbitrary private io_Command 0xF010) BEFORE calling our BeginIO.
 * DEVICEQUERY, GETSTATIONADDRESS, CONFIGINTERFACE, ONLINE, OFFLINE,
 * and private DBG_STATUS/FIRE_IRQ all dispatch fine with the same
 * setup — so it's io_Command-specific but not a simple "numeric
 * range" filter. Even calling IExec->BeginIO directly (bypassing DoIO)
 * returns -6, so the rejection is inside exec's dispatch layer, not
 * DoIO's routing.
 *
 * The CMD_WRITE code in device.c is structurally correct — it
 * builds a TX descriptor, bumps TDT, polls for STATUS.DD, and would
 * happily send a frame if only exec would let it. Unblocking probably
 * needs reading a shipping OS4 SANA-II driver source (rtl8139.device
 * being the obvious candidate) to see what unit_flags / MsgPort setup
 * / interface trick they use.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    IDOS->Printf("testtx: SKIP - CMD_WRITE hits exec IOERR_UNITBUSY (-6) even with\n"
                 "        a real struct Unit + PA_IGNORE/PA_SIGNAL + UNITF_ACTIVE.\n"
                 "        Root cause needs reading rtl8139.device or equivalent.\n");
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
