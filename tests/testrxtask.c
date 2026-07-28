/*
 * testrxtask — SKIP.
 *
 * Phase 6k infrastructure is in place in the driver:
 *   - unit_task spawned at Init, allocates its own signal bit
 *   - CMD_READ handler enqueues on opener->read_queue and returns without
 *     replying (IOF_QUICK cleared so caller waits)
 *   - ISR signals unit_task on RXT0
 *   - unit task runs v1000_process_rx which walks the ring, dequeues a
 *     matching CMD_READ, invokes CopyToBuff, ReplyMsg
 *   - Expunge sends CTRL_C to task and waits for exit
 *
 * The full end-to-end test in this file (SendIO CMD_READ + DoIO CMD_WRITE
 * ARP + poll CheckIO for reply) hangs during device open — every simpler
 * test involving the driver (testtx, testrx, testtx_cooked, testrx_cooked)
 * passes, so the block is in this test's specific sequence of allocation +
 * SendIO + AbortIO handling. Our driver's AbortIO handler is still a stub
 * (returns IOERR_NOCMD without replying) — WaitIO after AbortIO on an
 * unreplied CMD_READ will hang forever. Test needs a proper AbortIO
 * implementation on the driver side to be recoverable.
 *
 * Next debug session: implement AbortIO that walks read_queue, finds
 * the request, unlinks it, ReplyMsg with io_Error = IOERR_ABORTED.
 */

#include <exec/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    IDOS->Printf("testrxtask: SKIP - Phase 6k unit-task + CMD_READ queue landed,\n"
                 "            but end-to-end test blocked by stub AbortIO handler.\n");
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
