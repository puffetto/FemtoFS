// File: ko/image.h
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Portable, read-only image decoder shared by the kernel and host tests.
#ifndef FEMTOFS_IMAGE_H
#define FEMTOFS_IMAGE_H
#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define FF_FILE 1
#define FF_DIR 2
#define FF_SYMLINK 3
#define FF_LINK 4
#define FF_FIFO 5
#define FF_ATTR 129
#define FF_PAGE 4096

struct ff_node {
    uint32_t off, size, entries, parent, canonical, links, blob;
    uint32_t uid, gid;
    uint16_t mode;
    uint8_t type, hash, search, names, public_file;
};

struct ff_blob {
    uint32_t off, size, stored;
    uint8_t symlink, public_ref;
};

struct ff_image {
    const uint8_t *bytes;
    uint32_t size, count, public_off, private_off, hash2, objects;
    size_t metadata_size;
    struct ff_node *nodes; /* Root is nodes[count]; unused cells have type 0. */
    struct ff_blob *blobs;
    uint32_t nblobs;
};

uint16_t ff_u16(const uint8_t *);
uint32_t ff_u32(const uint8_t *);
uint32_t ff_fnv(const uint8_t *, size_t);
int ff_header(const uint8_t *, uint64_t, struct ff_image *);
int ff_validate(struct ff_image *);
void ff_destroy(struct ff_image *);
int ff_lookup(const struct ff_image *, uint32_t, const char *, size_t, uint32_t *);
/* Return source offset and copied-byte count; zero count denotes a direct page. */
int ff_page(const struct ff_image *, uint32_t, uint64_t, uint32_t, int,
    uint32_t *, uint32_t *);
#endif
// END File: ko/image.h
