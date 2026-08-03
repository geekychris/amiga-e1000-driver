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

    clock_t start_clk = clock();

    LONG success_cnt = 0, fail_cnt = 0;
    for (LONG i = 0; i < num_packets; i++) {
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

    clock_t end_clk = clock();
    double total_secs = (double)(end_clk - start_clk) / (double)CLOCKS_PER_SEC;
    double pps = total_secs > 0.0 ? (double)success_cnt / total_secs : 0.0;
    double kbps = total_secs > 0.0 ? (double)(success_cnt * (64 + 14)) / (total_secs * 1024.0) : 0.0;

    IDOS->Printf("=== STRESS TEST RESULTS ===\n");
    IDOS->Printf("Sent: %ld | Success: %ld | Fail: %ld\n", num_packets, success_cnt, fail_cnt);
    IDOS->Printf("Elapsed Time: %.3f seconds\n", total_secs);
    IDOS->Printf("Throughput: %.2f packets/sec (%.2f KB/sec)\n", pps, kbps);

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
