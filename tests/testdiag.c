/*
 * testdiag - open virte1000 as an additional opener + dump the cmdlog ring
 * so we can see what Roadshow (or whatever else was hitting the driver)
 * has been doing.
 *
 * Prints entries in chronological order, oldest to newest.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virte1000.h"

static const char *cmd_name(UWORD c)
{
    switch (c) {
    case CMD_INVALID:            return "INVALID";
    case CMD_RESET:              return "CMD_RESET";
    case CMD_READ:               return "CMD_READ";
    case CMD_WRITE:              return "CMD_WRITE";
    case CMD_UPDATE:             return "CMD_UPDATE";
    case CMD_CLEAR:              return "CMD_CLEAR";
    case CMD_STOP:               return "CMD_STOP";
    case CMD_START:              return "CMD_START";
    case CMD_FLUSH:              return "CMD_FLUSH";
    case NSCMD_DEVICEQUERY:      return "NSCMD_DEVICEQUERY";
    case S2_DEVICEQUERY:         return "S2_DEVICEQUERY";
    case S2_GETSTATIONADDRESS:   return "S2_GETSTATIONADDRESS";
    case S2_CONFIGINTERFACE:     return "S2_CONFIGINTERFACE";
    case S2_ADDMULTICASTADDRESS: return "S2_ADDMULTICASTADDRESS";
    case S2_DELMULTICASTADDRESS: return "S2_DELMULTICASTADDRESS";
    case S2_MULTICAST:           return "S2_MULTICAST";
    case S2_BROADCAST:           return "S2_BROADCAST";
    case S2_TRACKTYPE:           return "S2_TRACKTYPE";
    case S2_UNTRACKTYPE:         return "S2_UNTRACKTYPE";
    case S2_GETTYPESTATS:        return "S2_GETTYPESTATS";
    case S2_GETSPECIALSTATS:     return "S2_GETSPECIALSTATS";
    case S2_GETGLOBALSTATS:      return "S2_GETGLOBALSTATS";
    case S2_ONEVENT:             return "S2_ONEVENT";
    case S2_READORPHAN:          return "S2_READORPHAN";
    case S2_ONLINE:              return "S2_ONLINE";
    case S2_OFFLINE:             return "S2_OFFLINE";
    case S2_ADDMULTICASTADDRESSES: return "S2_ADDMULTICASTADDRESSES";
    case S2_DELMULTICASTADDRESSES: return "S2_DELMULTICASTADDRESSES";
    default:                     return "?";
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) return 20;

    LONG err = IExec->OpenDevice("virte1000.device", 0, (struct IORequest *)req, 0);
    if (err) { IDOS->Printf("RESULT: FAIL Open=%ld\n", (long)err); return 20; }

    UBYTE buf[32 * 8];
    for (int i = 0; i < 32 * 8; i++) buf[i] = 0;
    req->ios2_Req.io_Command = V1000_DBG_CMDLOG;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    IExec->DoIO((struct IORequest *)req);
    ULONG head = req->ios2_DataLength;
    IDOS->Printf("cmdlog: head=%lu (%lu entries recorded)\n",
                 (unsigned long)head, (unsigned long)(head < 32 ? head : 32));

    /* Walk in chronological order. If head < 32, entries [0..head-1] are
     * valid. If head >= 32, all 32 are valid and oldest is at (head & 31). */
    ULONG n = head < 32 ? head : 32;
    ULONG start = head < 32 ? 0 : (head & 31);
    for (ULONG i = 0; i < n; i++) {
        ULONG slot = (start + i) & 31;
        UBYTE *p = buf + (slot * 8);
        UWORD cmd = ((UWORD)p[0] << 8) | (UWORD)p[1];
        WORD  ioerr = (WORD)(((UWORD)p[2] << 8) | (UWORD)p[3]);
        ULONG wire = ((ULONG)p[4] << 24) | ((ULONG)p[5] << 16)
                   | ((ULONG)p[6] << 8)  |  (ULONG)p[7];
        IDOS->Printf("  [%3lu] cmd=0x%04lx (%s) ioerr=%ld wire=0x%lx\n",
                     (unsigned long)i, (unsigned long)cmd, cmd_name(cmd),
                     (long)ioerr, (unsigned long)wire);
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
