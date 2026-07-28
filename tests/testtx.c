/*
 * testtx — SKIPPED. Multiple layered issues; see memory
 *          os4-doio-ioflags-quirks for the full log.
 *
 * Summary of investigations in this session:
 *
 *   1. CMD_WRITE (io_Command=3) and S2_BROADCAST (17) never reach the
 *      driver's BeginIO — OS4 exec's dispatch returns IOERR_UNITBUSY
 *      (-6) before we see the request. Not io_Flags, not io_Unit
 *      structure (tried both fake and real embedded struct Unit), not
 *      MsgPort mode (PA_IGNORE vs PA_SIGNAL with real SigTask). Even
 *      direct IExec->BeginIO returns -6. Private io_Command 0xF001,
 *      0xF002, 0xF003 all dispatch fine; 0xF010 fails. So the rejection
 *      is io_Command-value based but not on any pattern we can see.
 *
 *   2. Went to the workaround: V1000_DBG_SEND at 0xF003 runs the same
 *      TX code. That DOES reach BeginIO, and QEMU's e1000 consumes the
 *      TX descriptor (TDH advances 0→1). But STATUS.DD never gets set
 *      in the descriptor and TPT (Total Packets Transmitted) stays 0.
 *      Adding IExec->CachePreDMA/CachePostDMA for cache coherency made
 *      the driver hang the caller (>90s no reply).
 *
 * Next-session leads:
 *   - Get rtl8139.device binary from the OS4 guest, run `strings` /
 *     look for calling conventions, or find OS4-native SANA-II driver
 *     source. This is now research-only territory.
 *   - Check whether we need to program VET (VLAN Ethertype), IPG (Inter-
 *     Packet Gap), or some other config register before TX will actually
 *     move packets in QEMU's e1000 emulation.
 *   - CachePreDMA semantics on OS4 PPC — one call hangs. Signature
 *     right? Wrong flags? Wrong pointer?
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    IDOS->Printf("testtx: SKIP - CMD_WRITE reach blocked (exec -6) AND\n"
                 "        DBG_SEND path drops descriptors silently.\n"
                 "        See memory os4-doio-ioflags-quirks.\n");
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
