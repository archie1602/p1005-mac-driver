// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rastertoxqx - native (arm64) CUPS raster filter for the HP LaserJet P1005
 *               and its XQX siblings (P1006, P1007, P1008).
 *
 *   PDF -> cgpdftoraster (macOS built-in, 1-bit black raster)
 *       -> rastertoxqx (this file: firmware check + JBIG + XQX framing)
 *       -> usb backend -> printer
 *
 * The XQX/JBIG encoding follows foo2xqx.c from foo2zjs by Rick Richardson
 * (vendored from OpenPrinting/foo2zjs, see vendor/foo2zjs).  Page header
 * values, padding and PJL framing match what HP's own macOS driver
 * (rasterToHPZJS) emits for the same input; see tests/golden_hp_matrix.txt.
 *
 * The P1005 has no firmware in flash: it must be downloaded after every
 * power-on.  Like the foo2zjs hotplug script, we read the IEEE-1284 device
 * ID (here through the CUPS side channel); without "FWVER:" the firmware
 * is sent before the job, using HP's ACL download framing.
 *
 * PPD options arrive in the raster header (see ppd/p1005-native.ppd):
 *   cupsInteger1  print density 1..5      cupsInteger2  EconoMode 0/1
 *   cupsInteger3  firmware: 0 auto, 1 always, 2 never
 *   cupsMediaType passed through as the XQX media type (as HP does)
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/sidechannel.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jbig.h"
#include "xqx.h"

#ifndef FIRMWARE_FILE
#define FIRMWARE_FILE "/Library/Printers/p1005-native/P1005-firmware.acl"
#endif

enum { FW_AUTO = 0, FW_ALWAYS = 1, FW_NEVER = 2 };

#define UEL "\033%-12345X"
#define FW_WAIT_SECONDS   45.0      /* engine start-up after a download */
#define USB_WAIT_SECONDS 600.0      /* usb backend waits for a switched-off printer */
#define NET_WAIT_SECONDS   5.0

static volatile sig_atomic_t Canceled;

/* JBIG parameters: identical to foo2xqx and to HP's driver output. */
static const long JbgOptions[5] = {
    JBG_ILEAVE | JBG_SMID,
    JBG_DELAY_AT | JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON,
    128,    /* L0 */
    0,      /* MX */
    0       /* MY */
};

static void
cancel_job(int sig)
{
    (void)sig;
    Canceled = 1;
}

static void
die(const char *fmt, ...)
{
    va_list ap;

    fputs("ERROR: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void
put_be32(unsigned long v, FILE *fp)
{
    unsigned char b[4] = { v >> 24, v >> 16, v >> 8, v };

    if (fwrite(b, 1, 4, fp) != 4)
        die("write failed: %s", strerror(errno));
}

static void
chunk_write(unsigned long type, unsigned long items, FILE *fp)
{
    put_be32(type, fp);
    put_be32(items, fp);
}

static void
item_uint32_write(unsigned long item, unsigned long value, FILE *fp)
{
    put_be32(item, fp);
    put_be32(sizeof(DWORD), fp);
    put_be32(value, fp);
}

static void
item_bytelut_write(unsigned long item, unsigned long len, const unsigned char *buf, FILE *fp)
{
    put_be32(item, fp);
    put_be32(len, fp);
    if (fwrite(buf, 1, len, fp) != len)
        die("write failed: %s", strerror(errno));
}

/* ---------------------------------------------------------------------
 * Firmware download
 * ------------------------------------------------------------------- */

static double
now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Side-channel request that tolerates a slow backend: the usb backend only
 * starts answering once the printer is switched on and opened.  The request
 * is written once; replies to earlier, abandoned requests are skipped.
 */
static cups_sc_status_t
sc_request(cups_sc_command_t cmd, char *data, int *datalen, double max_wait)
{
    cups_sc_command_t rcmd;
    cups_sc_status_t st;
    double start = now();

    if (cupsSideChannelWrite(cmd, CUPS_SC_STATUS_NONE, NULL, 0, 1.0))
        return CUPS_SC_STATUS_IO_ERROR;
    while (!Canceled && now() - start < max_wait) {
        double t0 = now();
        int len = *datalen;

        if (cupsSideChannelRead(&rcmd, &st, data, &len, 1.0) == 0) {
            if (rcmd != cmd)
                continue;
            *datalen = len;
            return st;
        }
        if (now() - t0 < 0.5)
            return CUPS_SC_STATUS_IO_ERROR;     /* channel closed, not slow */
    }
    return CUPS_SC_STATUS_TIMEOUT;
}

/* Returns 1 = firmware loaded, 0 = not loaded, -1 = unknown. */
static int
firmware_state(double max_wait)
{
    char id[1024];
    double start = now();

    do {
        int len = sizeof(id) - 1;
        cups_sc_status_t st = sc_request(CUPS_SC_CMD_GET_DEVICE_ID, id, &len, max_wait - (now() - start));

        if (st != CUPS_SC_STATUS_OK)
            return -1;
        id[len > 0 ? len : 0] = '\0';
        fprintf(stderr, "DEBUG: rastertoxqx: device ID \"%s\"\n", id);
        if (strstr(id, "MDL:"))
            return strstr(id, "FWVER:") != NULL;
        sleep(1);                               /* answered, but device not ready */
    } while (!Canceled && now() - start < max_wait);
    return -1;
}

/*
 * Firmware files from HP (HP1005.acl, sihpP1005.img) are an 8-digit date
 * stamp followed by an ARM ELF image.  HP's ACL download wraps it as
 *   UEL "@PJL ENTER LANGUAGE=ACL\r\n" 00 AC C0 DE <image size, BE32>
 *   <date stamp + image> UEL
 * (same bytes as foo2zjs' arm2hpdl produces for such files).
 */
static void
send_firmware(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *data;
    long size;

    if (!fp)
        die("Printer firmware not found at %s - reinstall the p1005-native driver.", path);
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    if (size < 64 || !(data = malloc(size)) || fread(data, 1, size, fp) != (size_t)size)
        die("Cannot read printer firmware %s", path);
    fclose(fp);
    for (int i = 0; i < 8; i++)
        if (data[i] < '0' || data[i] > '9')
            die("%s is not an HP firmware (.acl) file", path);
    if (memcmp(data + 8, "\177ELF", 4) != 0)
        die("%s is not an HP firmware (.acl) file", path);

    fprintf(stderr, "INFO: Loading firmware into printer (version %.8s)\n", (char *)data);
    fputs(UEL "@PJL ENTER LANGUAGE=ACL\r\n", stdout);
    fputc(0x00, stdout); fputc(0xac, stdout); fputc(0xc0, stdout); fputc(0xde, stdout);
    put_be32(size - 8, stdout);
    if (fwrite(data, 1, size, stdout) != (size_t)size)
        die("write failed: %s", strerror(errno));
    fputs(UEL, stdout);
    fflush(stdout);
    free(data);
}

static void
load_firmware_if_needed(int mode)
{
    const char *path = getenv("P1005_FIRMWARE") ? getenv("P1005_FIRMWARE") : FIRMWARE_FILE;
    const char *uri = getenv("DEVICE_URI");
    int state;
    char dummy[8];
    int len;

    if (mode == FW_NEVER) {
        fputs("DEBUG: rastertoxqx: firmware check disabled\n", stderr);
        return;
    }
    if (!uri) {
        /* Not run by cupsd (e.g. cupsfilter): no backend to talk to. */
        fputs("DEBUG: rastertoxqx: no DEVICE_URI, firmware check skipped\n", stderr);
        if (mode == FW_ALWAYS)
            send_firmware(path);
        return;
    }

    state = firmware_state(strncmp(uri, "usb:", 4) == 0 ? USB_WAIT_SECONDS : NET_WAIT_SECONDS);
    if (Canceled)
        return;

    if (state == 1 && mode == FW_AUTO) {
        fputs("DEBUG: rastertoxqx: printer reports firmware loaded\n", stderr);
        return;
    }
    if (state < 0 && mode == FW_AUTO) {
        /* Never download blindly: a second download can hang the engine. */
        fputs("WARNING: Cannot read printer status; skipping firmware check. "
              "If nothing prints, set \"Firmware Upload\" to Always once.\n", stderr);
        return;
    }

    send_firmware(path);

    len = sizeof(dummy);
    if (sc_request(CUPS_SC_CMD_DRAIN_OUTPUT, dummy, &len, 120.0) != CUPS_SC_STATUS_OK) {
        fputs("DEBUG: rastertoxqx: no drain support; waiting a fixed time\n", stderr);
        sleep(10);
        return;
    }
    fputs("INFO: Waiting for printer to start\n", stderr);
    double start = now();
    while (!Canceled && now() - start < FW_WAIT_SECONDS) {
        sleep(1);
        if (firmware_state(5.0) == 1) {
            fprintf(stderr, "DEBUG: rastertoxqx: firmware running after %.0f s\n", now() - start);
            return;
        }
    }
    fputs("WARNING: Printer did not confirm the firmware download; printing anyway\n", stderr);
}

/* ---------------------------------------------------------------------
 * XQX stream
 * ------------------------------------------------------------------- */

/* Copies s into dst without characters that would break a PJL string. */
static void
pjl_string(char *dst, size_t size, const char *s)
{
    size_t n = 0;

    for (; s && *s && n + 1 < size; s++)
        if (*s >= ' ' && *s < 0x7f && *s != '"' && *s != '\\')
            dst[n++] = *s;
    dst[n] = '\0';
}

static void
start_doc(FILE *fp, const char *job, const char *user, const char *title,
          int density, int econo)
{
    char u[64], t[80];

    pjl_string(u, sizeof(u), user);
    pjl_string(t, sizeof(t), title);
    fputs(UEL "@PJL\n", fp);
    fprintf(fp, "@PJL JOB NAME = \"%s\" DISPLAY = \"%s %s %s\"\n", t, job, u, t);
    fprintf(fp, "@PJL SET USERNAME = \"%s\"\n", u);
    fprintf(fp, "@PJL SET DENSITY=%d \n", density);
    fprintf(fp, "@PJL SET JAMRECOVERY=OFF \n");
    fprintf(fp, "@PJL SET ECONOMODE=%s \n", econo ? "ON" : "OFF");
    fprintf(fp, "@PJL SET RET=MEDIUM \n");
    fputs(UEL ",XQX", fp);

    chunk_write(XQX_START_DOC, 7, fp);
    item_uint32_write(0x80000000, 84, fp);
    item_uint32_write(0x10000005, 1, fp);
    item_uint32_write(0x10000001, 0, fp);
    item_uint32_write(XQXI_DMDUPLEX, 0, fp);
    item_uint32_write(0x10000000, 0, fp);
    item_uint32_write(0x10000003, 1, fp);
    item_uint32_write(XQXI_END, 0xdeadbeef, fp);
}

static void
end_doc(FILE *fp)
{
    chunk_write(XQX_END_DOC, 0, fp);
    fputs(UEL "@PJL\n@PJL RDYMSG DISPLAY = \"\"\n@PJL EOJ\n" UEL, fp);
    fflush(fp);
}

/* Compressed data: first link is the 20-byte BIH, the rest are <= 64 KiB. */
typedef struct chain {
    unsigned char *data;
    size_t len, cap;
    struct chain *next;
} chain_t;

static void
output_jbig(unsigned char *start, size_t len, void *arg)
{
    chain_t **root = arg, *cur;

    if (!*root) {
        if (len != 20)
            die("JBIG: first chunk must be the 20-byte BIH");
        *root = calloc(1, sizeof(chain_t));
        (*root)->cap = 20;
    }
    for (cur = *root; cur->next; cur = cur->next)
        ;
    while (len > 0) {
        if (cur->len == cur->cap) {
            cur->next = calloc(1, sizeof(chain_t));
            cur = cur->next;
            cur->cap = 65536;           /* "Printer does strange things otherwise." */
        }
        if (!cur->data && !(cur->data = malloc(cur->cap)))
            die("out of memory");
        size_t amt = cur->cap - cur->len < len ? cur->cap - cur->len : len;
        memcpy(cur->data + cur->len, start, amt);
        cur->len += amt;
        start += amt;
        len -= amt;
    }
}

static void
write_plane(chain_t *root, FILE *fp)
{
    int first = 1;

    if (!root || root->len != 20 || !root->next)
        die("JBIG: encoder produced no data");
    for (chain_t *cur = root->next; cur && cur->len; cur = cur->next) {
        chunk_write(XQX_START_PLANE, 4, fp);
        item_uint32_write(0x80000000, first ? 64 : 48, fp);
        item_uint32_write(0x40000000, 0, fp);
        if (first)
            item_bytelut_write(XQXI_BIH, 20, root->data, fp);
        else
            item_uint32_write(0x40000003, 1, fp);
        item_uint32_write(XQXI_END, 0xdeadbeef, fp);
        chunk_write(XQX_JBIG, cur->len, fp);
        if (fwrite(cur->data, 1, cur->len, fp) != cur->len)
            die("write failed: %s", strerror(errno));
        chunk_write(XQX_END_PLANE, 0, fp);
        first = 0;
    }
    while (root) {
        chain_t *next = root->next;
        free(root->data);
        free(root);
        root = next;
    }
}

/* XQX paper codes as sent by HP's driver; every other size is 512 ("custom"). */
static int
paper_code(const cups_page_header2_t *h)
{
    static const struct { const char *name; unsigned w, l; int code; } sizes[] = {
        { "Letter", 612, 792, 1 },     { "Legal", 612, 1008, 5 },
        { "Executive", 522, 756, 7 },  { "A4", 595, 842, 9 },
        { "A5", 420, 595, 11 },        { "Env10", 297, 684, 20 },
        { "EnvDL", 312, 624, 27 },     { "B5", 516, 729, 357 },
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        if (strcmp(h->cupsPageSizeName, sizes[i].name) == 0)
            return sizes[i].code;
    if (!h->cupsPageSizeName[0])
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (h->PageSize[0] == sizes[i].w && h->PageSize[1] == sizes[i].l)
                return sizes[i].code;
    return 512;
}

static void
write_page(cups_raster_t *ras, const cups_page_header2_t *h, int econo, int pageno, FILE *fp)
{
    unsigned resx = h->HWResolution[0], resy = h->HWResolution[1];
    unsigned w = h->cupsWidth, ht = h->cupsHeight;
    int bpp;

    if (h->cupsBitsPerPixel != 1 || (h->cupsColorSpace != CUPS_CSPACE_K && h->cupsColorSpace != CUPS_CSPACE_W))
        die("Unsupported raster (%u bits, color space %u); this driver needs 1-bit black",
            h->cupsBitsPerPixel, h->cupsColorSpace);
    if ((resx != 600 && resx != 1200) || resy != 600)
        die("Unsupported resolution %ux%u dpi", resx, resy);
    bpp = resx / 600;

    /* HP pads the raster to 128 bits per row and to 8 rows. */
    unsigned rx = (w + 127) & ~127u, ry = (ht + 7) & ~7u;
    size_t bpl = rx / 8;
    unsigned char *buf = calloc(bpl, ry);
    if (!buf)
        die("out of memory for %ux%u page", rx, ry);

    for (unsigned y = 0; y < ht; y++) {
        unsigned char *row = buf + y * bpl;
        if (cupsRasterReadPixels(ras, row, h->cupsBytesPerLine) != h->cupsBytesPerLine)
            die("short raster data on page %d", pageno);
        if (h->cupsColorSpace == CUPS_CSPACE_W)
            for (unsigned i = 0; i < h->cupsBytesPerLine; i++)
                row[i] ^= 0xff;
        if (w & 7)                                  /* clear padding bits */
            row[w / 8] &= 0xff << (8 - (w & 7));
    }

    chain_t *chain = NULL;
    struct jbg_enc_state se;
    unsigned char *planes[1] = { buf };
    jbg_enc_init(&se, rx, ry, 1, planes, output_jbig, &chain);
    jbg_enc_options(&se, JbgOptions[0], JbgOptions[1], JbgOptions[2], JbgOptions[3], JbgOptions[4]);
    jbg_enc_out(&se);
    jbg_enc_free(&se);
    free(buf);

    chunk_write(XQX_START_PAGE, 15, fp);
    item_uint32_write(0x80000000, 180, fp);
    item_uint32_write(XQXI_COPIES, 1, fp);          /* cgpdftoraster makes copies */
    item_uint32_write(XQXI_DMDEFAULTSOURCE, 7, fp); /* auto: the P1005 has one tray */
    item_uint32_write(XQXI_DMMEDIATYPE, h->cupsMediaType, fp);
    item_uint32_write(0x20000007, 1, fp);
    item_uint32_write(XQXI_RESOLUTION_X, 600, fp);
    item_uint32_write(XQXI_RESOLUTION_Y, 600, fp);
    item_uint32_write(XQXI_RASTER_X, rx, fp);
    item_uint32_write(XQXI_RASTER_Y, ry, fp);
    item_uint32_write(XQXI_VIDEO_BPP, bpp, fp);
    item_uint32_write(XQXI_VIDEO_X, rx / bpp, fp);
    item_uint32_write(XQXI_VIDEO_Y, ry, fp);
    item_uint32_write(XQXI_ECONOMODE, econo, fp);
    item_uint32_write(XQXI_DMPAPER, paper_code(h), fp);
    item_uint32_write(XQXI_END, 0xdeadbeef, fp);
    write_plane(chain, fp);
    chunk_write(XQX_END_PAGE, 0, fp);
    fflush(fp);

    fprintf(stderr, "DEBUG: rastertoxqx: page %d: %ux%u px at %ux%u dpi, paper %s (%d), media %u\n",
            pageno, w, ht, resx, resy, h->cupsPageSizeName, paper_code(h), h->cupsMediaType);
    fprintf(stderr, "PAGE: %d 1\n", pageno);
}

int
main(int argc, char *argv[])
{
    cups_page_header2_t header;
    cups_raster_t *ras;
    int fd = 0, pages = 0;

    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
        return 1;
    }
    setbuf(stderr, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, cancel_job);

    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0)
        die("Unable to open raster file %s: %s", argv[6], strerror(errno));
    if (!(ras = cupsRasterOpen(fd, CUPS_RASTER_READ)))
        die("Unable to read raster data");

    if (!cupsRasterReadHeader2(ras, &header)) {
        fputs("INFO: No pages to print\n", stderr);
        return 0;
    }

    int density = (header.cupsInteger[1] >= 1 && header.cupsInteger[1] <= 5) ? header.cupsInteger[1] : 3;
    int econo = header.cupsInteger[2] ? 1 : 0;
    int fwmode = header.cupsInteger[3] <= FW_NEVER ? (int)header.cupsInteger[3] : FW_AUTO;

    load_firmware_if_needed(fwmode);
    if (Canceled)
        return 1;

    start_doc(stdout, argv[1], argv[2], argv[3], density, econo);
    do {
        write_page(ras, &header, econo, ++pages, stdout);
    } while (!Canceled && cupsRasterReadHeader2(ras, &header));
    end_doc(stdout);

    cupsRasterClose(ras);
    if (fd)
        close(fd);
    fprintf(stderr, "INFO: %s\n", Canceled ? "Job canceled" : "Ready to print.");
    return Canceled ? 1 : 0;
}
