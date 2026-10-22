/* hfsvh.h -- describe an HFS+ / HFSX volume from its first blocks: the volume
 * header (partition offset 1024) and, from the catalog B-tree, the volume name
 * (the key of the first leaf record: parent ID 1, the root folder's name).
 * Read-only by construction: it only ever calls rd().  Header-only so a host
 * build can test it against an image made with hdiutil; iosnand uses it on a
 * partition served from the NAND.
 */
#ifndef HFSVH_H
#define HFSVH_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*hfs_read_fn)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len);

static uint16_t hfs_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t hfs_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t hfs_be64(const uint8_t *p) { return (uint64_t)hfs_be32(p) << 32 | hfs_be32(p + 4); }

/* byte offset of a catalog file offset, through the fork's first 8 extents */
static int hfs_fork_off(const uint8_t *fork, uint32_t bs, uint64_t foff, uint64_t *voff)
{
    uint64_t fblk = foff / bs;
    for (int i = 0; i < 8; i++) {
        uint32_t start = hfs_be32(fork + 16 + 8 * i), cnt = hfs_be32(fork + 20 + 8 * i);
        if (fblk < cnt) {
            *voff = ((uint64_t)start + fblk) * bs + foff % bs;
            return 0;
        }
        fblk -= cnt;
    }
    return -1;
}

/* Prints the header; returns 0 when the signature is H+ or HX. */
static int hfs_describe(hfs_read_fn rd, void *ctx, FILE *out)
{
    uint8_t vh[512], nd[512];
    char name[256];
    if (rd(ctx, 1024, vh, sizeof(vh)))
        return -1;
    uint16_t sig = hfs_be16(vh), ver = hfs_be16(vh + 2);
    uint32_t bs = hfs_be32(vh + 40), total = hfs_be32(vh + 44), freeb = hfs_be32(vh + 48);
    fprintf(out, "signature  0x%04x \"%c%c\"  version %u\n", sig,
            vh[0] >= 0x20 && vh[0] < 0x7f ? vh[0] : '.', vh[1] >= 0x20 && vh[1] < 0x7f ? vh[1] : '.', ver);
    if (sig != 0x482b && sig != 0x4858) {
        fprintf(out, "not HFS+ (H+) or HFSX (HX)\n");
        return -1;
    }
    fprintf(out, "attributes 0x%08x  last mounted by \"%.4s\"  journal info block %u\n",
            hfs_be32(vh + 4), (const char *)vh + 8, hfs_be32(vh + 12));
    fprintf(out, "block size %u  total blocks %u (%llu bytes)  free %u\n", bs, total,
            (unsigned long long)total * bs, freeb);
    fprintf(out, "files %u  folders %u  next catalog id %u  write count %u\n",
            hfs_be32(vh + 32), hfs_be32(vh + 36), hfs_be32(vh + 64), hfs_be32(vh + 68));
    const uint8_t *cat = vh + 272;
    fprintf(out, "catalog    %llu bytes, first extent block %u x %u\n",
            (unsigned long long)hfs_be64(cat), hfs_be32(cat + 16), hfs_be32(cat + 20));

    /* the volume name: catalog header node -> first leaf -> record 0's key */
    uint64_t o;
    if (bs < 512 || (bs & (bs - 1)) || hfs_fork_off(cat, bs, 0, &o) || rd(ctx, o, nd, sizeof(nd)))
        goto noname;
    uint32_t first_leaf = hfs_be32(nd + 14 + 10);
    uint16_t node_size = hfs_be16(nd + 14 + 18);
    if (node_size < 512 || node_size > 32768 || (node_size & (node_size - 1)) || !first_leaf)
        goto noname;
    static uint8_t leaf[32768];
    for (uint32_t got = 0; got < node_size; got += 512) {
        if (hfs_fork_off(cat, bs, (uint64_t)first_leaf * node_size + got, &o) || rd(ctx, o, leaf + got, 512))
            goto noname;
    }
    uint16_t r0 = hfs_be16(leaf + node_size - 2);
    if ((int8_t)leaf[8] != -1 || r0 < 14 || r0 + 8u > node_size)       /* kind: leaf */
        goto noname;
    uint32_t parent = hfs_be32(leaf + r0 + 2);
    uint16_t n = hfs_be16(leaf + r0 + 6);
    if (parent != 1 || r0 + 8u + 2u * n > node_size || n > 255)
        goto noname;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t c = hfs_be16(leaf + r0 + 8 + 2 * i);
        name[i] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    name[n] = 0;
    fprintf(out, "volume name \"%s\"\n", name);
    return 0;
noname:
    fprintf(out, "volume name: not found (catalog unreadable from here)\n");
    return 0;
}

#endif
