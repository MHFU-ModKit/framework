/*
 * MWo3 monster-overlay relocator — C port of tools/ovl_reloc.py.
 * Validated byte-identical to the Python reference (see the host self-test:
 *   g++ -DOVL_RELOC_TEST src/core/ovl_reloc.cpp -o /tmp/ovlr && /tmp/ovlr <em.ovl>
 * then diff against `python tools/ovl_reloc.py <em.ovl> --newbase <b> -o ...`).
 *
 * Algorithm (matches docs/BIG_MONSTER_OVERLAY_RELOCATION.md §3):
 *  - footprint = [slot_base, load_address + image_size + bss_size)
 *  - text [0x40, 0x40+text_size): j/jal in-range -> retarget; lui+lo (register-write
 *    dataflow) forming in-range addr -> rewrite pair
 *  - data [0x40+text, +data_size): 32-bit words in-range -> += delta
 *  - delta must be 64KB-aligned (then %lo never changes; %hi shifts uniformly)
 */
#include "mhfu/ovl_reloc.h"
#include <string.h>

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* GPR written by an instruction (for %hi-holder invalidation), or -1. */
static int gpr_dest(uint32_t w, uint32_t op) {
    uint32_t rt = (w >> 16) & 0x1F;
    uint32_t rd = (w >> 11) & 0x1F;
    if (op == 0) {                 /* SPECIAL: writes rd (jr writes nothing) */
        if ((w & 0x3F) == 0x08) return -1;   /* jr */
        return (int)rd;
    }
    if (op == 1) return (((w >> 16) & 0x1F) == 0x10 || ((w >> 16) & 0x1F) == 0x11) ? 31 : -1;
    if (op == 3) return 31;        /* jal */
    if (op >= 0x08 && op <= 0x0F) return (int)rt;   /* addi..lui */
    if (op >= 0x20 && op <= 0x26) return (int)rt;   /* GPR loads */
    return -1;                     /* stores, branches, cop1 loads, etc. */
}

/* %lo-bearing opcodes (sign-extended) + ori (zero-extended). */
static int is_lo_signext(uint32_t op) {
    switch (op) {
        case 0x09: case 0x23: case 0x2B: case 0x21: case 0x25: case 0x20:
        case 0x24: case 0x28: case 0x29: case 0x31: case 0x39: case 0x35: case 0x3D:
            return 1;
    }
    return 0;
}

extern "C" mhfu_ovl_reloc_stats_t
mhfu_ovl_relocate(void *image, uint32_t image_size, uint32_t slot_base, int32_t delta)
{
    mhfu_ovl_reloc_stats_t st;
    memset(&st, 0, sizeof(st));
    uint8_t *d = (uint8_t *)image;

    if (image_size < 0x40 || memcmp(d, "MWo3", 4) != 0) return st;
    const mhfu_mwo3_header_t *h = (const mhfu_mwo3_header_t *)d;
    if (delta & 0xFFFF) return st;                 /* must be 64KB-aligned */

    uint32_t base    = h->load_address;
    uint32_t text_sz = h->text_size;
    uint32_t data_sz = h->data_size;
    uint32_t bss_sz  = h->bss_size;
    uint32_t foot_lo = slot_base;
    uint32_t foot_hi = base + image_size + bss_sz; /* end (exclusive) */
    if (0x40u + text_sz + data_sz > image_size) return st;

    uint32_t text_lo = 0x40, text_hi = 0x40 + text_sz;
    uint32_t data_lo = text_hi, data_hi = text_hi + data_sz;

#define IN_IMG(va) ((va) >= foot_lo && (va) < foot_hi)

    /* hold[reg] = file offset of the lui that set it, or 0 = none (offset 0 is the
     * header, never a lui, so 0 is a safe "empty"). hold_hi[reg] = its imm. */
    uint32_t hold_off[32]; uint16_t hold_hi[32]; uint8_t hold_set[32];
    memset(hold_set, 0, sizeof(hold_set));

    for (uint32_t o = text_lo; o < text_hi; o += 4) {
        uint32_t w  = rd32(d + o);
        uint32_t va = base + o;
        uint32_t op = w >> 26;
        int dest = gpr_dest(w, op);

        if (op == 2 || op == 3) {                  /* j / jal */
            uint32_t tgt = (va & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
            if (IN_IMG(tgt)) {
                uint32_t nt = tgt + (uint32_t)delta;
                wr32(d + o, (op << 26) | ((nt >> 2) & 0x03FFFFFFu));
                st.n_jump++;
            }
            if (dest >= 0) hold_set[dest] = 0;
        } else if (op == 0x0F) {                   /* lui -> (re)define holder */
            uint32_t rt = (w >> 16) & 0x1F;
            hold_off[rt] = o; hold_hi[rt] = (uint16_t)(w & 0xFFFF); hold_set[rt] = 1;
        } else {
            uint32_t rs = (w >> 21) & 0x1F;
            int is_ori = (op == 0x0D);
            if ((is_lo_signext(op) || is_ori) && hold_set[rs]) {
                uint32_t hi = hold_hi[rs];
                uint32_t lo = w & 0xFFFF;
                uint32_t addr = is_ori ? ((hi << 16) | lo)
                                       : (uint32_t)((hi << 16) + (int32_t)(int16_t)lo);
                if (IN_IMG(addr)) {
                    uint32_t na = addr + (uint32_t)delta;
                    uint32_t nhi, nlo;
                    if (is_ori) { nhi = (na >> 16) & 0xFFFF; nlo = na & 0xFFFF; }
                    else {
                        nlo = na & 0xFFFF;
                        nhi = ((na - (uint32_t)(int32_t)(int16_t)nlo) >> 16) & 0xFFFF;
                    }
                    uint32_t luiw = rd32(d + hold_off[rs]);
                    wr32(d + hold_off[rs], (luiw & 0xFFFF0000u) | nhi);
                    wr32(d + o, (w & 0xFFFF0000u) | nlo);
                    st.n_hilo++;
                }
            }
            if (dest >= 0) hold_set[dest] = 0;
        }
    }

    for (uint32_t o = data_lo; o < data_hi; o += 4) {
        uint32_t w = rd32(d + o);
        if (IN_IMG(w)) { wr32(d + o, w + (uint32_t)delta); st.n_data++; }
    }

    /* rebase header.load_address */
    wr32(d + 8, base + (uint32_t)delta);
    st.new_base = base + (uint32_t)delta;
    st.ok = 1;
    return st;
#undef IN_IMG
}

#ifdef OVL_RELOC_TEST
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <em.ovl> [delta_hex] [out]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n); fread(buf, 1, n, f); fclose(f);
    int32_t delta = (argc > 2) ? (int32_t)strtoll(argv[2], 0, 16) : 0x100000;
    mhfu_ovl_reloc_stats_t st = mhfu_ovl_relocate(buf, (uint32_t)n, MHFU_OVL_SLOT_EM, delta);
    fprintf(stderr, "ok=%d jump=%u hilo=%u data=%u new_base=0x%08x\n",
            st.ok, st.n_jump, st.n_hilo, st.n_data, st.new_base);
    const char *out = (argc > 3) ? argv[3] : "/tmp/em_reloc_c.bin";
    FILE *g = fopen(out, "wb"); fwrite(buf, 1, n, g); fclose(g);
    fprintf(stderr, "wrote %s\n", out);
    return st.ok ? 0 : 1;
}
#endif
