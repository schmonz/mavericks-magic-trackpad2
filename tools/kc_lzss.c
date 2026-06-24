/* kc_lzss - decompress a 10.9 'complzss' kernelcache to a plain Mach-O kernel.
 *
 * Why in-repo: pulling the prelinked AppleUSBMultitouchDriver out of the boot kernelcache
 * is the only way to read the ACTUAL running driver build (the on-disk /S/L/E binary differs).
 * Apple's kernelcache wraps the kernel in a fat Mach-O, then 'complzss' (standard xnu LZSS).
 * This is the tiny, 10.9-native decompressor for it -- no LZMA/LIEF/python dependency.
 *
 * Usage: kc_lzss <kernelcache-in> <kernel-out>
 *   - handles a fat (cafebabe) wrapper by taking the first arch slice, or a raw complzss blob.
 *   - finds the 'complzss' header, decompresses uncompressed_size bytes, writes them out.
 *
 * complzss header (big-endian), per xnu/kext_tools compressed_kernel_header:
 *   u32 signature 'comp'; u32 compress_type 'lzss'; u32 adler32; u32 uncompressed_size;
 *   u32 compressed_size; u32 prelink_version; u32 reserved[10]; char platform[64]; char root[256];
 *   u8  data[];   // -> data begins 0x180 (384) bytes after the header start
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* xnu decompress_lzss (libkern). Returns bytes written. */
#define N 4096
#define F 18
#define THRESHOLD 2
static int decompress_lzss(uint8_t *dst, uint32_t dstlen, const uint8_t *src, uint32_t srclen) {
    uint8_t *dststart = dst, *dstend = dst + dstlen;
    const uint8_t *srcend = src + srclen;
    uint8_t text_buf[N + F - 1];
    int i, j, k, r, c;
    unsigned int flags;
    for (i = 0; i < N - F; i++) text_buf[i] = ' ';
    r = N - F; flags = 0;
    for (;;) {
        if (((flags >>= 1) & 0x100) == 0) {
            if (src < srcend) c = *src++; else break;
            flags = c | 0xFF00;
        }
        if (flags & 1) {
            if (src < srcend) c = *src++; else break;
            if (dst < dstend) *dst++ = c; else break;
            text_buf[r++] = c; r &= (N - 1);
        } else {
            if (src < srcend) i = *src++; else break;
            if (src < srcend) j = *src++; else break;
            i |= ((j & 0xF0) << 4);
            j = (j & 0x0F) + THRESHOLD;
            for (k = 0; k <= j; k++) {
                c = text_buf[(i + k) & (N - 1)];
                if (dst < dstend) *dst++ = c; else break;
                text_buf[r++] = c; r &= (N - 1);
            }
        }
    }
    return (int)(dst - dststart);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <kernelcache-in> <kernel-out>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open in"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); return 1; }
    fclose(f);

    /* locate the complzss header: scan for the 'comp''lzss' signature (handles fat wrapper). */
    long h = -1;
    for (long i = 0; i + 8 <= sz; i++) {
        if (be32(buf + i) == 0x636f6d70 /* 'comp' */ && be32(buf + i + 4) == 0x6c7a7373 /* 'lzss' */) { h = i; break; }
    }
    if (h < 0) { fprintf(stderr, "no 'complzss' signature found\n"); return 1; }

    uint32_t uncomp = be32(buf + h + 12);
    uint32_t comp   = be32(buf + h + 16);
    fprintf(stderr, "complzss @ 0x%lx: uncompressed=%u compressed=%u\n", h, uncomp, comp);

    const uint8_t *src = buf + h + 0x180;      /* data[] starts 384 bytes in */
    uint8_t *out = malloc(uncomp);
    int n = decompress_lzss(out, uncomp, src, comp);
    fprintf(stderr, "decompressed %d bytes (expected %u)\n", n, uncomp);

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("open out"); return 1; }
    fwrite(out, 1, n, o);
    fclose(o);
    return (n == (int)uncomp) ? 0 : 1;
}
