/* macho_rebase - subtract a VM base from every segment/section vmaddr and every symbol
 * value in a 64-bit Mach-O, in place into a new file.
 *
 * Why: a kext carved out of a prelinked kernelcache is linked at a high kernel vmaddr
 * (e.g. 0xffffff7f81af0000). The re/ disasm/syms wrappers (and shell printf math) choke
 * on 64-bit addresses. Rebasing the carved kext to base 0 makes its functions land at the
 * same small relative offsets as a normal on-disk kext, so re/disasm/re/syms/re/vtable
 * "just work" on the ACTUAL running build.
 *
 * Usage: macho_rebase <in> <base> <out>     # base accepts 0x-hex or decimal
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LC_SEGMENT_64 0x19
#define LC_SYMTAB     0x02
struct mh64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct lc { uint32_t cmd, cmdsize; };
struct seg64 { uint32_t cmd, cmdsize; char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize; uint32_t maxprot, initprot, nsects, flags; };
struct sect64 { char sectname[16], segname[16]; uint64_t addr, size; uint32_t offset, align,
    reloff, nreloc, flags, reserved1, reserved2, reserved3; };
struct symtab { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct nlist64 { uint32_t n_strx; uint8_t n_type; uint8_t n_sect; uint16_t n_desc; uint64_t n_value; };

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s <in> <base> <out>\n", argv[0]); return 2; }
    uint64_t base = strtoull(argv[2], 0, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); return 1; }
    fclose(f);

    struct mh64 *mh = (struct mh64 *)buf;
    if (mh->magic != 0xfeedfacf) { fprintf(stderr, "not 64-bit Mach-O\n"); return 1; }
    uint8_t *p = buf + sizeof(struct mh64);
    int segs = 0, syms = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        struct lc *c = (struct lc *)p;
        if (c->cmd == LC_SEGMENT_64) {
            struct seg64 *s = (struct seg64 *)p;
            if (s->vmaddr >= base) s->vmaddr -= base;
            struct sect64 *sec = (struct sect64 *)(p + sizeof(struct seg64));
            for (uint32_t k = 0; k < s->nsects; k++) if (sec[k].addr >= base) sec[k].addr -= base;
            segs++;
        } else if (c->cmd == LC_SYMTAB) {
            struct symtab *st = (struct symtab *)p;
            struct nlist64 *nl = (struct nlist64 *)(buf + st->symoff);
            for (uint32_t k = 0; k < st->nsyms; k++) if (nl[k].n_value >= base) nl[k].n_value -= base;
            syms += st->nsyms;
        }
        p += c->cmdsize;
    }
    fprintf(stderr, "rebased %d segments, %d symbols by 0x%llx\n", segs, syms, (unsigned long long)base);
    FILE *o = fopen(argv[3], "wb");
    if (!o) { perror("open out"); return 1; }
    fwrite(buf, 1, sz, o);
    fclose(o);
    return 0;
}
