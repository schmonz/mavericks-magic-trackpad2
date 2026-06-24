/* kc_carve - inspect a decompressed Mach-O kernel's segments, and carve a prelinked
 * kext out of it by VM address.
 *
 * Companion to kc_lzss: after decompressing the kernelcache, a prelinked kext's own
 * Mach-O lives inside __PRELINK_TEXT at its _PrelinkExecutableSourceAddr (a vmaddr).
 * To disassemble that kext (re/disasm/re/syms operate on a standalone Mach-O), we must
 * convert the vmaddr to a file offset and copy out [_PrelinkExecutableSize] bytes.
 *
 * Usage:
 *   kc_carve <kernel> segs                         # list LC_SEGMENT_64: name vmaddr vmsize fileoff filesize
 *   kc_carve <kernel> carve <vmaddr> <size> <out>  # write the slice covering [vmaddr, vmaddr+size) to <out>
 *     vmaddr/size accept 0x-hex or decimal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LC_SEGMENT_64 0x19
struct mh64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct lc { uint32_t cmd, cmdsize; };
struct seg64 {
    uint32_t cmd, cmdsize; char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
};

static uint64_t pnum(const char *s){ return (uint64_t)strtoull(s, 0, 0); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <kernel> segs | carve <vmaddr> <size> <out>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); return 1; }
    fclose(f);

    struct mh64 *mh = (struct mh64 *)buf;
    if (mh->magic != 0xfeedfacf) { fprintf(stderr, "not a 64-bit Mach-O (magic 0x%x)\n", mh->magic); return 1; }
    uint8_t *p = buf + sizeof(struct mh64);

    int do_segs = strcmp(argv[2], "segs") == 0;
    uint64_t want_va = 0, want_sz = 0; const char *outpath = 0;
    if (!do_segs) {
        if (argc != 6) { fprintf(stderr, "carve needs <vmaddr> <size> <out>\n"); return 2; }
        want_va = pnum(argv[3]); want_sz = pnum(argv[4]); outpath = argv[5];
    }
    if (do_segs) printf("# segname            vmaddr             vmsize     fileoff      filesize\n");

    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct lc *c = (struct lc *)p;
        if (c->cmd == LC_SEGMENT_64) {
            struct seg64 *s = (struct seg64 *)p;
            if (do_segs) {
                printf("%-18s 0x%016llx 0x%08llx 0x%010llx 0x%08llx\n",
                       s->segname, (unsigned long long)s->vmaddr, (unsigned long long)s->vmsize,
                       (unsigned long long)s->fileoff, (unsigned long long)s->filesize);
            } else if (want_va >= s->vmaddr && want_va < s->vmaddr + s->vmsize) {
                uint64_t off = want_va - s->vmaddr + s->fileoff;
                if (off + want_sz > (uint64_t)sz) want_sz = sz - off;
                fprintf(stderr, "carving from segment %s: vmaddr 0x%llx -> fileoff 0x%llx, %llu bytes\n",
                        s->segname, (unsigned long long)want_va, (unsigned long long)off, (unsigned long long)want_sz);
                FILE *o = fopen(outpath, "wb");
                if (!o) { perror("open out"); return 1; }
                fwrite(buf + off, 1, want_sz, o);
                fclose(o);
                return 0;
            }
        }
        p += c->cmdsize;
    }
    if (!do_segs) { fprintf(stderr, "vmaddr 0x%llx not in any segment\n", (unsigned long long)want_va); return 1; }
    return 0;
}
