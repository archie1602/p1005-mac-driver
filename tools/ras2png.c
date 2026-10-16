/*
 * ras2png - convert CUPS raster pages or a PBM (P4) bitmap to downscaled
 *           grayscale PNGs for visual inspection.
 *
 * Usage: ras2png [-s scale] input.{ras,pbm} out-prefix
 *        writes out-prefix-<page>.png, averaging scale x scale pixel blocks
 *        (default scale 4).  Black = ink.
 *        ras2png -p input.ras out-prefix
 *        writes each 1-bit raster page as out-prefix-<page>.pbm, padded the
 *        way the XQX filter pads it (width to 128, height to 8), so it can be
 *        compared bit-for-bit with xqxdecode's decoded output.
 */

#include <cups/raster.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static void
put32(unsigned char *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void
chunk(FILE *fp, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    uint32_t crc;

    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, fp);
    if (len)
        fwrite(data, 1, len, fp);
    crc = crc32(0, (const unsigned char *)type, 4);
    if (len)
        crc = crc32(crc, data, len);
    put32(hdr, crc);
    fwrite(hdr, 1, 4, fp);
}

/* ink[] holds 0..255 ink coverage per output pixel; PNG stores luminance. */
static int
write_png(const char *path, const unsigned char *ink, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    unsigned char ihdr[13];
    size_t rawlen = (size_t)(w + 1) * h;
    unsigned char *raw = malloc(rawlen);
    uLongf zlen = compressBound(rawlen);
    unsigned char *z = malloc(zlen);

    if (!fp || !raw || !z)
        return -1;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0;
        for (int x = 0; x < w; x++)
            raw[(size_t)y * (w + 1) + 1 + x] = 255 - ink[(size_t)y * w + x];
    }
    compress2(z, &zlen, raw, rawlen, 9);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, fp);
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(fp, "IHDR", ihdr, 13);
    chunk(fp, "IDAT", z, zlen);
    chunk(fp, "IEND", NULL, 0);
    fclose(fp);
    free(raw);
    free(z);
    return 0;
}

/* Accumulates one input row into the downscaled ink buffer. */
static void
accumulate(unsigned *acc, const unsigned char *row, int bits, int w, int scale)
{
    for (int x = 0; x < w; x++) {
        unsigned v;
        if (bits == 1)
            v = (row[x >> 3] >> (7 - (x & 7)) & 1) ? 255 : 0;
        else
            v = row[x];
        acc[x / scale] += v;
    }
}

static void
emit(const char *prefix, int page, unsigned *acc, unsigned char *ink,
     int ow, int oh, int scale)
{
    char path[1024];

    for (size_t i = 0; i < (size_t)ow * oh; i++)
        ink[i] = acc[i] / (scale * scale);
    snprintf(path, sizeof(path), "%s-%d.png", prefix, page);
    write_png(path, ink, ow, oh);
    printf("%s (%dx%d)\n", path, ow, oh);
}

static int
do_pbm(FILE *fp, const char *prefix, int scale)
{
    int w, h, c;

    if (fscanf(fp, "P4 %d %d", &w, &h) != 2)
        return -1;
    c = fgetc(fp);                       /* single whitespace after header */
    (void)c;
    int bpl = (w + 7) / 8, ow = w / scale, oh = h / scale;
    unsigned char *row = malloc(bpl);
    unsigned *acc = calloc((size_t)ow * oh, sizeof(unsigned));
    unsigned char *ink = malloc((size_t)ow * oh);

    for (int y = 0; y < oh * scale; y++) {
        if (fread(row, 1, bpl, fp) != (size_t)bpl)
            break;
        accumulate(acc + (size_t)(y / scale) * ow, row, 1, ow * scale, scale);
    }
    emit(prefix, 1, acc, ink, ow, oh, scale);
    return 0;
}

static int
do_pbm_dump(int fd, const char *prefix)
{
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t hdr;
    int page = 0;
    char path[1024];

    while (cupsRasterReadHeader2(ras, &hdr)) {
        unsigned w = hdr.cupsWidth, h = hdr.cupsHeight;
        unsigned rx = (w + 127) & ~127u, ry = (h + 7) & ~7u, bpl = rx / 8;
        unsigned char *row = calloc(bpl, 1);

        if (hdr.cupsBitsPerPixel != 1) {
            fprintf(stderr, "-p needs a 1-bit raster\n");
            return 1;
        }
        snprintf(path, sizeof(path), "%s-%d.pbm", prefix, ++page);
        FILE *fp = fopen(path, "wb");
        fprintf(fp, "P4\n%u %u\n", rx, ry);
        for (unsigned y = 0; y < ry; y++) {
            memset(row, 0, bpl);
            if (y < h) {
                cupsRasterReadPixels(ras, row, hdr.cupsBytesPerLine);
                if (w & 7)
                    row[w / 8] &= 0xff << (8 - (w & 7));
            }
            fwrite(row, 1, bpl, fp);
        }
        fclose(fp);
        free(row);
        printf("%s\n", path);
    }
    cupsRasterClose(ras);
    return page ? 0 : 1;
}

static int
do_raster(int fd, const char *prefix, int scale)
{
    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t hdr;
    int page = 0;

    while (cupsRasterReadHeader2(ras, &hdr)) {
        int bits = hdr.cupsBitsPerPixel, w = hdr.cupsWidth, h = hdr.cupsHeight;
        int ow = w / scale, oh = h / scale;
        unsigned char *row = malloc(hdr.cupsBytesPerLine);
        unsigned *acc = calloc((size_t)ow * oh, sizeof(unsigned));
        unsigned char *ink = malloc((size_t)ow * oh);

        if (bits != 1 && bits != 8) {
            fprintf(stderr, "unsupported %d bits per pixel\n", bits);
            return 1;
        }
        printf("page %d: %dx%d %ux%u dpi bpp=%d cspace=%u size=%s\n", page + 1, w, h,
               hdr.HWResolution[0], hdr.HWResolution[1], bits, hdr.cupsColorSpace,
               hdr.cupsPageSizeName);
        for (int y = 0; y < h; y++) {
            cupsRasterReadPixels(ras, row, hdr.cupsBytesPerLine);
            if (y < oh * scale)
                accumulate(acc + (size_t)(y / scale) * ow, row, bits, ow * scale, scale);
        }
        emit(prefix, ++page, acc, ink, ow, oh, scale);
        free(row); free(acc); free(ink);
    }
    cupsRasterClose(ras);
    return page ? 0 : 1;
}

int
main(int argc, char *argv[])
{
    int scale = 4, opt, pbm = 0;
    char magic[2];

    while ((opt = getopt(argc, argv, "ps:")) != -1)
        if (opt == 's')
            scale = atoi(optarg);
        else if (opt == 'p')
            pbm = 1;
    if (argc - optind != 2 || scale < 1) {
        fprintf(stderr, "usage: ras2png [-s scale | -p] input.{ras,pbm} out-prefix\n");
        return 2;
    }
    if (pbm)
        return do_pbm_dump(open(argv[optind], O_RDONLY), argv[optind + 1]);
    FILE *fp = fopen(argv[optind], "rb");
    if (!fp || fread(magic, 1, 2, fp) != 2) {
        perror(argv[optind]);
        return 1;
    }
    if (magic[0] == 'P' && magic[1] == '4') {
        rewind(fp);
        return do_pbm(fp, argv[optind + 1], scale);
    }
    fclose(fp);
    return do_raster(open(argv[optind], O_RDONLY), argv[optind + 1], scale);
}
