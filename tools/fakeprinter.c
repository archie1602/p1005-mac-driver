/*
 * fakeprinter - offline stand-in for the CUPS usb backend + an HP P1005.
 *
 * Runs a CUPS filter with the same file descriptors the real backend gives
 * it (stdout = print data, fd 3 = back channel, fd 4 = side channel) and
 * emulates the printer's firmware state:
 *
 *   - GET_DEVICE_ID answers with the P1005's IEEE-1284 ID.  "FWVER:" is
 *     only present once a complete firmware download has been received
 *     (and -w milliseconds have passed, like the real engine booting).
 *   - DRAIN_OUTPUT is answered after all pending output has been consumed.
 *   - The captured stream is checked: XQX job data must not arrive before
 *     the firmware, and firmware must not be sent twice.
 *
 * Usage: fakeprinter [-f] [-n] [-d ms] [-w ms] [-i device-id] -o out.bin -- filter args...
 *   -f  printer already has firmware loaded
 *   -n  no side channel (like cupsfilter or a network backend)
 *   -d  printer switched on late: side channel silent for the first ms
 *       (the usb backend only answers once the device is opened)
 */

#include <cups/sidechannel.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *DeviceId =
    "SERN:TEST0000000;MFG:Hewlett-Packard;MDL:HP LaserJet P1005;CMD:ACL;CLS:PRINTER;DES:HP LaserJet P1005;";
static const char ACL_ENTER[] = "\033%-12345X@PJL ENTER LANGUAGE=ACL\r\n";
static const char UEL[] = "\033%-12345X";

static unsigned char *Buf;          /* everything the filter wrote */
static size_t BufLen, BufCap, ScanPos;
static int FwUploads, FwLoaded, XqxBeforeFw, IdQueries, Drains;
static double FwReadyAt = -1, WaitSec = 3.0;

static double
now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int
move_high(int fd)
{
    int nfd = fcntl(fd, F_DUPFD, 20);
    close(fd);
    return nfd;
}

static void *
memfind(const void *hay, size_t hlen, const void *needle, size_t nlen)
{
    return nlen <= hlen ? memmem(hay, hlen, needle, nlen) : NULL;
}

/* Walk the captured stream and update the emulated printer state. */
static void
scan_stream(void)
{
    for (;;) {
        unsigned char *acl = memfind(Buf + ScanPos, BufLen - ScanPos, ACL_ENTER, strlen(ACL_ENTER));
        unsigned char *xqx = memfind(Buf + ScanPos, BufLen - ScanPos, ",XQX", 4);

        if (xqx && (!acl || xqx < acl)) {
            if (!FwLoaded && !XqxBeforeFw) {
                XqxBeforeFw = 1;
                fprintf(stderr, "fakeprinter: PROTOCOL ERROR: XQX job data arrived before firmware was loaded\n");
            }
            ScanPos = (xqx - Buf) + 4;
            continue;
        }
        if (!acl)
            return;

        size_t hdr = (acl - Buf) + strlen(ACL_ENTER);
        if (BufLen < hdr + 8)
            return;                                 /* wait for more data */
        const unsigned char *h = Buf + hdr;
        if (h[0] != 0x00 || h[1] != 0xac || h[2] != 0xc0 || h[3] != 0xde) {
            fprintf(stderr, "fakeprinter: PROTOCOL ERROR: bad ACL download header %02x %02x %02x %02x\n",
                    h[0], h[1], h[2], h[3]);
            ScanPos = hdr;
            continue;
        }
        size_t size = ((size_t)h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
        size_t end = hdr + 8 + 8 + size;            /* header, version stamp, image */
        if (BufLen < end + strlen(UEL))
            return;
        if (memcmp(Buf + end, UEL, strlen(UEL)) != 0)
            fprintf(stderr, "fakeprinter: PROTOCOL ERROR: firmware not followed by UEL trailer\n");
        FwUploads++;
        fprintf(stderr, "fakeprinter: firmware received: version %.8s, %zu image bytes\n",
                (const char *)Buf + hdr + 8, size);
        if (FwUploads > 1)
            fprintf(stderr, "fakeprinter: WARNING: firmware sent %d times\n", FwUploads);
        if (FwReadyAt < 0)
            FwReadyAt = now() + WaitSec;
        ScanPos = end + strlen(UEL);
    }
}

static void
update_fw_state(void)
{
    if (!FwLoaded && FwReadyAt >= 0 && now() >= FwReadyAt) {
        FwLoaded = 1;
        fprintf(stderr, "fakeprinter: firmware booted, FWVER now reported\n");
    }
}

/* Returns 0 on EOF, 1 if data was read, -1 if nothing available. */
static int
read_output(int fd)
{
    if (BufCap - BufLen < 65536) {
        BufCap = BufCap ? BufCap * 2 : 1 << 20;
        Buf = realloc(Buf, BufCap);
    }
    ssize_t n = read(fd, Buf + BufLen, BufCap - BufLen);
    if (n > 0) {
        BufLen += n;
        scan_stream();
        return 1;
    }
    if (n == 0)
        return 0;
    return (errno == EAGAIN || errno == EINTR) ? -1 : 0;
}

static void
handle_side_channel(int out_fd, int *out_open)
{
    cups_sc_command_t cmd;
    cups_sc_status_t st;
    char data[2048];
    int len = sizeof(data);

    if (cupsSideChannelRead(&cmd, &st, data, &len, 1.0))
        return;
    update_fw_state();
    switch (cmd) {
    case CUPS_SC_CMD_GET_DEVICE_ID:
        IdQueries++;
        snprintf(data, sizeof(data), "%s%s", DeviceId, FwLoaded ? "FWVER:20080505;" : "");
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_OK, data, strlen(data), 1.0);
        break;
    case CUPS_SC_CMD_DRAIN_OUTPUT:
        Drains++;
        fcntl(out_fd, F_SETFL, O_NONBLOCK);
        while (*out_open) {
            int r = read_output(out_fd);
            if (r == 0)
                *out_open = 0;
            if (r < 0)
                break;
        }
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_OK, NULL, 0, 1.0);
        break;
    case CUPS_SC_CMD_GET_BIDI:
        data[0] = 1;
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_OK, data, 1, 1.0);
        break;
    case CUPS_SC_CMD_GET_STATE:
        data[0] = CUPS_SC_STATE_ONLINE;
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_OK, data, 1, 1.0);
        break;
    case CUPS_SC_CMD_GET_CONNECTED:
        data[0] = 1;
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_OK, data, 1, 1.0);
        break;
    default:
        cupsSideChannelWrite(cmd, CUPS_SC_STATUS_NOT_IMPLEMENTED, NULL, 0, 1.0);
        break;
    }
}

int
main(int argc, char *argv[])
{
    const char *outpath = NULL;
    int opt, no_sc = 0, sv[2], bc[2], out[2], status;
    double online_at = 0;

    while ((opt = getopt(argc, argv, "fnd:w:i:o:")) != -1)
        switch (opt) {
        case 'f': FwLoaded = 1; break;
        case 'n': no_sc = 1; break;
        case 'd': online_at = atoi(optarg) / 1000.0; break;
        case 'w': WaitSec = atoi(optarg) / 1000.0; break;
        case 'i': DeviceId = optarg; break;
        case 'o': outpath = optarg; break;
        default: return 2;
        }
    if (!outpath || optind >= argc) {
        fprintf(stderr, "usage: fakeprinter [-f] [-n] [-w ms] [-i id] -o out -- filter args...\n");
        return 2;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(bc) || pipe(out))
        return perror("fakeprinter"), 1;
    sv[0] = move_high(sv[0]); sv[1] = move_high(sv[1]);
    bc[0] = move_high(bc[0]); bc[1] = move_high(bc[1]);
    out[0] = move_high(out[0]); out[1] = move_high(out[1]);

    pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], 1);
        dup2(bc[0], 3);
        if (no_sc)
            close(4);
        else
            dup2(sv[1], 4);
        for (int fd = 20; fd < 64; fd++)
            close(fd);
        execv(argv[optind], argv + optind);
        perror("execv");
        _exit(127);
    }
    close(sv[1]); close(bc[0]); close(out[1]);
    dup2(sv[0], CUPS_SC_FD);    /* libcups side-channel calls use fd 4 */
    close(sv[0]);

    int out_open = 1;
    online_at += now();
    while (out_open) {
        struct pollfd pfd[2] = {{out[0], POLLIN, 0}, {CUPS_SC_FD, POLLIN, 0}};
        int sc_live = !no_sc && now() >= online_at;
        int n = poll(pfd, sc_live ? 2 : 1, 200);
        update_fw_state();
        if (n <= 0)
            continue;
        if (pfd[0].revents & (POLLIN | POLLHUP))
            if (read_output(out[0]) == 0)
                out_open = 0;
        if (sc_live && (pfd[1].revents & POLLIN))
            handle_side_channel(out[0], &out_open);
    }
    waitpid(pid, &status, 0);

    FILE *fp = fopen(outpath, "wb");
    fwrite(Buf, 1, BufLen, fp);
    fclose(fp);

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    fprintf(stderr, "fakeprinter: filter exit=%d bytes=%zu firmware_uploads=%d xqx_before_fw=%s "
            "deviceid_queries=%d drains=%d\n",
            code, BufLen, FwUploads, XqxBeforeFw ? "YES" : "no", IdQueries, Drains);
    return code ? code : (XqxBeforeFw || FwUploads > 1) ? 3 : 0;
}
