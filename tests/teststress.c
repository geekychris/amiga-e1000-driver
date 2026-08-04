/*
 * teststress.c — High-throughput stress testing utility for virte1000.device.
 *
 * Tests ring buffer wraparound, interrupt handling under load, and packet throughput.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virte1000.h"

static ULONG my_copy_from_buff(struct Hook *hook, APTR object, APTR message)
{
    (void)hook; (void)object;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)message;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    return 1;
}

int main(int argc, char **argv)
{
    LONG num_packets = 1000;
    if (argc > 1 && argv[1]) {
        IDOS->StrToLong(argv[1], &num_packets);
        if (num_packets <= 0) num_packets = 1000;
    }

    IDOS->Printf("=== STRESS TEST: Sending %ld packets via virte1000.device ===\n", num_packets);

    struct Hook copy_from_hook;
    copy_from_hook.h_Entry = (HOOKFUNC)my_copy_from_buff;
    copy_from_hook.h_SubEntry = NULL;
    copy_from_hook.h_Data = NULL;

    struct TagItem bm_tags[] = {
        { S2_CopyFromBuff, (ULONG)&copy_from_hook },
        { TAG_DONE, 0 }
    };

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) {
        IDOS->Printf("FAIL: AllocSysObjectTags(ASOT_PORT) failed\n");
        return 20;
    }

    struct IOSana2Req *req = (struct IOSana2Req *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_DONE);
    if (!req) {
        IDOS->Printf("FAIL: AllocSysObjectTags(ASOT_IOREQUEST) failed\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    req->ios2_BufferManagement = (APTR)bm_tags;
    LONG open_rc = IExec->OpenDevice("virte1000.device", 0, (struct IORequest *)req, 0);
    if (open_rc != 0) {
        IDOS->Printf("FAIL: OpenDevice failed rc=%ld\n", open_rc);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    /* Configure interface online */
    req->ios2_Req.io_Command = S2_CONFIGINTERFACE;
    for (int i = 0; i < 6; i++) req->ios2_SrcAddr[i] = 0;
    IExec->DoIO((struct IORequest *)req);

    UBYTE payload[64];
    for (int i = 0; i < 64; i++) payload[i] = (UBYTE)(i & 0xFF);

#include <time.h>
#include <stdint.h>

    clock_t start_clk = clock();

    LONG success_cnt = 0, fail_cnt = 0;
    BOOL aborted = FALSE;
    LONG i;
    for (i = 0; i < num_packets; i++) {
        /* Check for Ctrl-C every 1024 packets. Cheap enough at 40 kpps
         * (~40 checks/sec) that it doesn't skew the throughput number,
         * and it means picking a too-large count is recoverable without
         * rebooting the machine. */
        if ((i & 0x3FF) == 0) {
            ULONG sigs = IExec->SetSignal(0L, 0L);
            if (sigs & SIGBREAKF_CTRL_C) {
                IExec->SetSignal(0L, SIGBREAKF_CTRL_C);
                aborted = TRUE;
                break;
            }
        }

        req->ios2_Req.io_Command = CMD_WRITE;
        req->ios2_Req.io_Error   = 0;
        req->ios2_WireError      = 0;
        req->ios2_Data           = payload;
        req->ios2_DataLength     = 64;
        req->ios2_PacketType     = 0x0800;
        for (int k = 0; k < 6; k++) req->ios2_DstAddr[k] = 0xFF;

        LONG rc = IExec->DoIO((struct IORequest *)req);
        if (rc == 0 && req->ios2_Req.io_Error == 0) {
            success_cnt++;
        } else {
            fail_cnt++;
        }
    }
    if (aborted) {
        IDOS->Printf("*** interrupted at packet %ld — printing partial stats ***\n", i);
        num_packets = i;
    }

    clock_t end_clk = clock();
    ULONG total_ms = (ULONG)(((uint64_t)(end_clk - start_clk) * 1000ULL) / (uint64_t)CLOCKS_PER_SEC);
    if (total_ms == 0) total_ms = 1;

    ULONG pps = (ULONG)(((uint64_t)success_cnt * 1000ULL) / (uint64_t)total_ms);
    ULONG kbps = (ULONG)(((uint64_t)success_cnt * 78ULL * 1000ULL) / ((uint64_t)total_ms * 1024ULL));

    IDOS->Printf("=== STRESS TEST RESULTS ===\n");
    IDOS->Printf("Sent: %ld | Success: %ld | Fail: %ld\n", num_packets, success_cnt, fail_cnt);
    IDOS->Printf("Elapsed Time: %lu.%03lu seconds\n", total_ms / 1000UL, total_ms % 1000UL);
    IDOS->Printf("Throughput: %lu packets/sec (%lu KB/sec)\n", pps, kbps);

    /* Get Global Stats */
    struct Sana2DeviceStats stats;
    req->ios2_Req.io_Command = S2_GETGLOBALSTATS;
    req->ios2_StatData       = &stats;
    IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("Driver Stats: PacketsSent=%lu PacketsReceived=%lu\n",
                 stats.PacketsSent, stats.PacketsReceived);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    return 0;
}
