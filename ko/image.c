// File: ko/image.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Bounds, graph, hash-chain and visibility validation without kernel dependencies.
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/errno.h>
MALLOC_DEFINE(M_FFIMAGE, "femtofs image", "femtofs decoded metadata");
#define alloc(n, s) malloc((size_t)(n) * (s), M_FFIMAGE, M_WAITOK | M_ZERO)
#define release(p) free(p, M_FFIMAGE)
#else
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define alloc(n, s) calloc(n, s)
#define release(p) free(p)
#endif
#include "image.h"

static const uint8_t primes[] = {
    3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,
    97,101,103,107,109,113,127,131,137,139,149,151,157,163,167,173,179,
    181,191,193,197,199,211,223,227,229,233,239,241,251
};
_Static_assert(sizeof(primes) == 53, "prime index ABI");

uint16_t ff_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
uint32_t ff_u32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint32_t ff_fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811c9dc5;
    while (n--) h = h * 0x01000193 ^ *p++;
    return h;
}
static const uint8_t *cell(const struct ff_image *im, uint32_t i)
{ return im->bytes + 128 + 16 * i; }
static int occupied(const uint8_t *p) { return p[0] >= FF_FILE && p[0] <= FF_FIFO; }
static uint32_t hash(const char *s, size_t n, uint32_t base, uint32_t size)
{
    uint32_t h = 0;
    while (n--) h = h * base + (uint8_t)*s++;
    return h % size;
}
static int zero(const uint8_t *p, size_t n)
{
    while (n--) if (*p++) return 0;
    return 1;
}

int ff_header(const uint8_t *h, uint64_t capacity, struct ff_image *im)
{
    static const char author[56] = "Andrea \"Nemesi\" Cocito - blackye at gmail dot com";
    if (capacity < 128 || memcmp(h, "0FS\0\0\4", 6) || h[7] != 1 ||
        memcmp(h + 72, author, 56) || h[67]) return EINVAL;
    im->size = ff_u32(h + 32);
    im->count = ff_u32(h + 36);
    im->public_off = ff_u32(h + 40);
    im->private_off = ff_u32(h + 44);
    im->hash2 = ff_u32(h + 68);
    if (!im->count || im->count > 65535 || !im->size || im->size > capacity ||
        ff_u32(h + 48) != im->count * 16 ||
        128 + im->count * 16 > im->public_off ||
        im->public_off > im->private_off || im->private_off > im->size ||
        ((im->public_off | im->private_off | im->size) & (FF_PAGE - 1))) return EINVAL;
    return 0;
}

struct work {
    uint32_t parent, head, sibling, enter, leave;
    uint8_t slice, object, attr;
};
struct name { const char *s; uint32_t dir; };
static int namecmp(const void *a, const void *b)
{
    const struct name *x = a, *y = b;
    if (x->dir != y->dir) return x->dir < y->dir ? -1 : 1;
    return strcmp(x->s, y->s);
}
static int blobcmp(const void *a, const void *b)
{
    const struct ff_blob *x = a, *y = b;
    return x->off < y->off ? -1 : x->off != y->off;
}
static int attrcmp(const void *a, const void *b)
{ return memcmp(*(const uint8_t *const *)a + 2, *(const uint8_t *const *)b + 2, 14); }

static int attribute(struct ff_image *im, struct work *w, uint32_t id, uint32_t a)
{
    struct ff_node *n = &im->nodes[id];
    static const uint16_t kinds[] = {0, 0100000, 0040000, 0120000, 0, 0010000};
    const uint8_t *p;
    if (a >= im->count || cell(im, a)[0] != FF_ATTR) return EINVAL;
    p = cell(im, a);
    n->mode = ff_u16(p + 2); n->uid = ff_u32(p + 4); n->gid = ff_u32(p + 8);
    if ((n->mode & 0170000) != kinds[n->type]) return EINVAL;
    w[a].attr = 1;
    return 0;
}

static int blob(struct ff_image *im, uint32_t off, uint32_t size, int sym, int pub)
{
    uint64_t stored = ((uint64_t)size + 4) & ~(uint64_t)3;
    uint32_t end = off < im->private_off ? im->private_off : im->size;
    if ((off & 3) || off < im->public_off || off >= im->size ||
        stored > UINT32_MAX || (uint64_t)off + stored > end ||
        (stored >= FF_PAGE ? (off & (FF_PAGE - 1)) != 0 :
            (off & (FF_PAGE - 1)) + stored > FF_PAGE) ||
        !zero(im->bytes + off + size, (size_t)stored - size)) return EINVAL;
    if (sym && (off < im->private_off || memchr(im->bytes + off, 0, size))) return EINVAL;
    im->blobs[im->nblobs++] = (struct ff_blob){off, size, (uint32_t)stored, sym, pub};
    return 0;
}

/* A reverse-chain forest supplies O(1) anchor reachability after O(N) traversal. */
static int chains(struct ff_image *im, struct work *w, struct ff_node *d)
{
    uint32_t i, j, next, clock = 0, seen = 0, root;
    for (i = 0; i < d->size; i++) {
        const uint8_t *p = cell(im, d->off + i);
        if (!occupied(p)) continue;
        next = ff_u16(p + 14);
        if (next > d->size) return EINVAL;
        w[d->off + i].parent = next;
        if (next != d->size) {
            if (!occupied(cell(im, d->off + next))) return EINVAL;
            w[d->off + i].sibling = w[d->off + next].head;
            w[d->off + next].head = i + 1;
        }
    }
    for (root = 0; root < d->size; root++) {
        if (!occupied(cell(im, d->off + root)) ||
            w[d->off + root].parent != d->size) continue;
        i = root;
        for (;;) {
            struct work *v = &w[d->off + i];
            if (!v->enter) { v->enter = ++clock; seen++; }
            if (v->head) { j = v->head - 1; v->head = w[d->off + j].sibling; i = j; continue; }
            v->leave = clock;
            if (i == root) break;
            i = v->parent;
        }
    }
    if (seen != d->entries) return EINVAL; /* cycles have no sentinel root */
    for (i = 0; i < d->size; i++) {
        const uint8_t *p = cell(im, d->off + i);
        const char *s;
        int reachable = 0;
        if (!occupied(p)) continue;
        s = (const char *)im->bytes + ff_u32(p + 8);
        for (j = 0; j <= (uint32_t)(d->hash >> 6); j++) {
            uint32_t a = hash(s, p[13], j ? im->hash2 : primes[d->hash & 63], d->size);
            struct work *v = &w[d->off + i], *anchor = &w[d->off + a];
            if (occupied(cell(im, d->off + a)) && v->enter <= anchor->enter &&
                anchor->enter <= v->leave) reachable = 1;
        }
        if (!reachable) return EINVAL;
    }
    return 0;
}

int ff_validate(struct ff_image *im)
{
    struct work *w = NULL;
    uint32_t *queue = NULL, nq = 0, qi, i, j, nattr = 0, nnames = 0;
    struct name *names = NULL;
    const uint8_t **attrs = NULL, *h = im->bytes;
    int error = EINVAL, dual = 0;
    if (ff_u32(h + 32) != im->size || ff_header(h, im->size, im) ||
        ff_fnv(h + 16, im->size - 16) != ff_u32(h + 8) ||
        ff_fnv(h + 128, im->count * 16) != ff_u32(h + 12)) return EINVAL;
    im->metadata_size = ((size_t)im->count + 1) * sizeof(*im->nodes) +
        (size_t)im->count * 2 * sizeof(*im->blobs);
    im->nodes = alloc(im->count + 1, sizeof(*im->nodes));
    im->blobs = alloc(im->count * 2, sizeof(*im->blobs));
    w = alloc(im->count, sizeof(*w));
    queue = alloc(im->count + 1, sizeof(*queue));
    names = alloc(im->count, sizeof(*names));
    attrs = alloc(im->count, sizeof(*attrs));
    if (!im->nodes || !im->blobs || !w || !queue || !names || !attrs) { error = ENOMEM; goto out; }
    im->nodes[im->count] = (struct ff_node){.type = FF_DIR, .off = ff_u32(h + 52),
        .size = ff_u32(h + 56), .entries = ff_u32(h + 60), .hash = h[66],
        .parent = im->count, .canonical = im->count, .links = 2};
    if (attribute(im, w, im->count, ff_u16(h + 64))) goto out;
    im->nodes[im->count].search = !!(im->nodes[im->count].mode & 1);
    im->nodes[im->count].names = (im->nodes[im->count].mode & 5) == 5;
    queue[nq++] = im->count;
    for (qi = 0; qi < nq; qi++) {
        uint32_t id = queue[qi], entries = 0;
        struct ff_node *d = &im->nodes[id];
        if ((d->hash >> 6) > 1 || (d->hash & 63) >= sizeof(primes) ||
            d->size > 65521 || d->entries >= 32768 || d->entries > d->size ||
            (!d->size && (d->off || d->entries)) ||
            (uint64_t)d->off + d->size > im->count) goto out;
        dual |= d->hash >> 6;
        for (i = d->off; i < d->off + d->size; i++) {
            if (w[i].slice || w[i].object) goto out;
            w[i].slice = 1;
        }
        for (i = d->off; i < d->off + d->size; i++) {
            const uint8_t *p = cell(im, i), *o;
            struct ff_node *n;
            uint32_t target, nameoff, end, length;
            const uint8_t *nul;
            if (p[0] == FF_ATTR) continue;
            if (!p[0]) {
                if (!zero(p, 14) || ff_u16(p + 14) != d->size) goto out;
                continue;
            }
            if (!occupied(p) || p[1] || ff_u16(p + 2) || p[12]) goto out;
            entries++;
            target = ff_u32(p + 4); nameoff = ff_u32(p + 8);
            if (target >= im->count || w[target].slice || w[target].object) goto out;
            o = cell(im, target);
            if (o[0] != p[0]) goto out;
            if (++im->objects >= 32768) goto out;
            w[target].object = 1;
            n = &im->nodes[target];
            *n = (struct ff_node){.type = o[0], .hash = o[1], .off = ff_u32(o + 4),
                .size = ff_u32(o + 8), .parent = id, .canonical = target, .links = 1};
            if (n->type != FF_LINK && attribute(im, w, target, ff_u16(o + 2))) goto out;
            n->search = d->search && !!(n->mode & 1);
            n->names = d->names && (n->mode & 5) == 5;
            if (n->type == FF_DIR) {
                uint32_t packed = ff_u32(o + 12);
                if ((packed & 0x80000000) || (packed & 65535) != (id == im->count ? 65535 : id)) goto out;
                n->entries = packed >> 16; n->links = 2; d->links++;
                queue[nq++] = target;
            } else {
                if (ff_u32(o + 12)) goto out;
                if (n->type == FF_FIFO && (n->off || n->size)) goto out;
                if (n->type == FF_LINK && (ff_u16(o + 2) || n->size)) goto out;
            }
            if (nameoff < im->public_off || nameoff >= im->size) goto out;
            end = nameoff < im->private_off ? im->private_off : im->size;
            length = end - nameoff < 256 ? end - nameoff : 256;
            nul = memchr(h + nameoff, 0, length);
            if (!nul) goto out;
            length = (uint32_t)(nul - h - nameoff);
            if (!length || length != p[13] || memchr(h + nameoff, '/', length) ||
                (length == 1 && h[nameoff] == '.') ||
                (length == 2 && h[nameoff] == '.' && h[nameoff + 1] == '.')) goto out;
            if (blob(im, nameoff, length, 0, d->names)) goto out;
            names[nnames++] = (struct name){(const char *)h + nameoff, id};
        }
        if (entries != d->entries || chains(im, w, d)) goto out;
    }
    if (dual) {
        if (im->hash2 <= 256 || im->hash2 >= (1u << 24)) goto out;
        for (i = 2; i <= im->hash2 / i; i++) if (!(im->hash2 % i)) goto out;
    } else if (im->hash2) goto out;
    qsort(names, nnames, sizeof(*names), namecmp);
    for (i = 1; i < nnames; i++) if (!namecmp(&names[i-1], &names[i])) goto out;
    for (i = 0; i < im->count; i++) {
        struct ff_node *n = &im->nodes[i];
        const uint8_t *p = cell(im, i);
        if (p[0] == FF_ATTR) {
            if (!w[i].attr || p[1] || (h[6] == 0 && ff_u32(p + 12))) goto out;
            attrs[nattr++] = p;
        } else if (!w[i].slice && !w[i].object && !zero(p, 16)) goto out;
        if (!n->type) continue;
        if (n->type == FF_LINK) {
            if (n->off >= im->count || im->nodes[n->off].type != FF_FILE) goto out;
            n->canonical = n->off;
            im->nodes[n->off].links++;
        }
        if (n->type == FF_FILE || n->type == FF_LINK) {
            struct ff_node *f = &im->nodes[n->canonical];
            if ((f->mode & 4) && im->nodes[n->parent].search) f->public_file = 1;
        }
    }
    qsort(attrs, nattr, sizeof(*attrs), attrcmp);
    for (i = 1; i < nattr; i++) if (!attrcmp(&attrs[i-1], &attrs[i])) goto out;
    for (i = 0; i < im->count; i++) {
        struct ff_node *n = &im->nodes[i];
        if ((n->type == FF_FILE || n->type == FF_SYMLINK) &&
            blob(im, n->off, n->size, n->type == FF_SYMLINK, n->public_file)) goto out;
    }
    qsort(im->blobs, im->nblobs, sizeof(*im->blobs), blobcmp);
    for (i = 0, j = 0; i < im->nblobs; i++) {
        struct ff_blob *b = &im->blobs[i];
        if (j && b->off == im->blobs[j-1].off) {
            struct ff_blob *last = &im->blobs[j-1];
            if (b->size != last->size || b->symlink != last->symlink) goto out;
            last->public_ref |= b->public_ref;
        } else {
            if (j && b->off < (uint64_t)im->blobs[j-1].off + im->blobs[j-1].stored) goto out;
            im->blobs[j++] = *b;
        }
    }
    im->nblobs = j;
    for (i = 0; i < j; i++) if (!!(im->blobs[i].off < im->private_off) != !!im->blobs[i].public_ref) goto out;
    for (i = 0; i < im->count; i++) {
        struct ff_node *n = &im->nodes[i];
        uint32_t lo = 0, hi = im->nblobs;
        if (n->type != FF_FILE) continue;
        while (lo < hi) { uint32_t mid = lo + (hi-lo)/2; if (im->blobs[mid].off < n->off) lo = mid+1; else hi = mid; }
        n->blob = lo;
    }
    error = 0;
out:
    release(w); release(queue); release(names); release(attrs);
    if (error) ff_destroy(im);
    return error;
}

void ff_destroy(struct ff_image *im)
{
    release(im->nodes); release(im->blobs);
    im->nodes = NULL; im->blobs = NULL; im->objects = im->nblobs = 0;
    im->metadata_size = 0;
}

int ff_lookup(const struct ff_image *im, uint32_t dir, const char *name, size_t len, uint32_t *id)
{
    const struct ff_node *d = &im->nodes[dir];
    uint32_t pass, i, first;
    if (!d->size) return ENOENT;
    first = hash(name, len, primes[d->hash & 63], d->size);
    for (pass = 0; pass <= (uint32_t)(d->hash >> 6); pass++) {
        i = pass ? hash(name, len, im->hash2, d->size) : first;
        if (pass && i == first) break;
        while (i != d->size) {
            const uint8_t *p = cell(im, d->off + i);
            const char *s;
            if (!occupied(p)) break;
            s = (const char *)im->bytes + ff_u32(p + 8);
            if (p[13] == len && !memcmp(name, s, len)) { *id = im->nodes[ff_u32(p + 4)].canonical; return 0; }
            i = ff_u16(p + 14);
        }
    }
    return ENOENT;
}

int ff_page(const struct ff_image *im, uint32_t id, uint64_t offset, uint32_t page,
    int leaking, uint32_t *source, uint32_t *copied)
{
    const struct ff_node *n = &im->nodes[id];
    uint32_t count;
    if (!page || (page & (page-1)) || offset % page || offset >= n->size) return EINVAL;
    count = n->size - offset < page ? (uint32_t)(n->size-offset) : page;
    *source = n->off + (uint32_t)offset;
    *copied = count;
    if (!(*source % page) && (count == page ||
        (leaking && page <= FF_PAGE && n->size >= page && n->off < im->private_off &&
         (uint64_t)*source + page <= im->private_off))) *copied = 0;
    return 0;
}
// END File: ko/image.c
