// File: ko/test-lookup.c
// Created by Andrea "Nemesi" Cocito on 25/10/2026
// Count name scans and comparisons in single- and dual-hash lookups.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned scans, comparisons;
size_t counted_strlen(const char *s) { scans++; return strlen(s); }
static int counted_memcmp(const void *a, const void *b, size_t n)
{ comparisons++; return memcmp(a, b, n); }
#define strlen counted_strlen
#define memcmp counted_memcmp
#include "image.c"
#undef strlen
#undef memcmp

static void lookup(struct ff_image *im, const char *name, size_t len,
    int expected, uint32_t target, unsigned expected_comparisons)
{
    uint32_t id = UINT32_MAX;
    int error;
    scans = comparisons = 0;
    error = ff_lookup(im, 4, name, len, &id);
    printf("%.*s: status=%d scans=%u comparisons=%u\n", (int)len, name,
        error, scans, comparisons);
    fflush(stdout);
    assert(error == expected);
    assert(error || id == target);
    assert(comparisons == expected_comparisons);
    assert(scans == 0);
}

int main(void)
{
    uint8_t bytes[208] = {0};
    struct ff_node nodes[5] = {0};
    struct ff_image im = {.bytes = bytes, .nodes = nodes, .hash2 = 257};
    unsigned i;
    /* Modulo two, odd hash bases give both six-byte names anchor zero. */
    nodes[4] = (struct ff_node){.size = 2, .hash = 0x40};
    for (i = 0; i < 2; i++) {
        uint8_t *b = bytes + 128 + 16 * i;
        b[0] = FF_FILE;
        b[4] = i + 2;
        b[8] = 192 + 8 * i;
        b[13] = 6;
        b[14] = i + 1;
        nodes[i + 2].canonical = i + 2;
    }
    memcpy(bytes + 192, "aaaaaa", 7);
    memcpy(bytes + 200, "bbbbbb", 7);
    lookup(&im, "cccccc", 6, ENOENT, 0, 2);
    lookup(&im, "xxxx", 4, ENOENT, 0, 0);
    lookup(&im, "aaaaaa", 6, 0, 2, 1);
    lookup(&im, "bbbbbb", 6, 0, 3, 2);
    nodes[4].hash = 0;
    im.hash2 = 0;
    lookup(&im, "cccccc", 6, ENOENT, 0, 2);
    /* A distinct second anchor must still be searched after a primary miss. */
    memset(bytes + 128, 0, 48);
    nodes[4].size = 3;
    nodes[4].hash = 0x40;
    im.hash2 = 257;
    for (i = 1; i <= 2; i++) {
        uint8_t *b = bytes + 128 + 16 * i;
        b[0] = FF_FILE;
        b[4] = i + 1;
        b[8] = 184 + 8 * i;
        b[13] = 2;
        b[14] = 3;
    }
    memcpy(bytes + 192, "ab", 3);
    memcpy(bytes + 200, "zz", 3);
    lookup(&im, "ab", 2, 0, 2, 2);
    lookup(&im, "ac", 2, ENOENT, 0, 1);
    puts("lookup counters: passed");
    return 0;
}
// END File: ko/test-lookup.c
