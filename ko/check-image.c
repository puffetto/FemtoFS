// File: ko/check-image.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Host executable exercising the exact kernel decoder and page-selection policy.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "image.h"

int main(int argc, char **argv)
{
    struct ff_image im = {0};
    FILE *f;
    long length;
    uint8_t *bytes;
    uint32_t i, count = 0;
    int error;
    if (argc != 2 || !(f = fopen(argv[1], "rb"))) return 2;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 128 || fseek(f, 0, SEEK_SET)) { fclose(f); return 1; }
    bytes = malloc((size_t)length);
    if (!bytes) return 2;
    if (fread(bytes, 1, (size_t)length, f) != (size_t)length) return 2;
    fclose(f);
    error = ff_header(bytes, length, &im);
    if (!error) { im.bytes = bytes; error = ff_validate(&im); }
    if (error) { free(bytes); return 1; }
    for (i = 0; i <= im.count; i++) {
        struct ff_node *n = &im.nodes[i];
        uint32_t j;
        if (n->type == FF_DIR) {
            for (j = 0; j < n->size; j++) {
                const uint8_t *b = bytes + 128 + (n->off + j) * 16;
                uint32_t target;
                const char *name;
                if (!b[0] || b[0] == FF_ATTR) continue;
                name = (const char *)bytes + ff_u32(b + 8);
                assert(!ff_lookup(&im, i, name, strlen(name), &target));
                assert(target == im.nodes[ff_u32(b + 4)].canonical);
                count++;
            }
            assert(ff_lookup(&im, i, "/absent", 7, &j) != 0);
        }
        if (n->type == FF_FILE) {
            uint32_t page;
            for (page = 2048; page <= 8192; page *= 2) {
                uint64_t off;
                for (off = 0; off < n->size; off += page) {
                    uint32_t src, copied;
                    assert(!ff_page(&im, i, off, page, 0, &src, &copied));
                    assert(src == n->off + off);
                    assert(copied == 0 ? src % page == 0 && off + page <= n->size :
                        copied == (n->size - off < page ? n->size - off : page));
                    assert(!ff_page(&im, i, off, page, 1, &src, &copied));
                    if (!copied && off + page > n->size) {
                        assert(page <= FF_PAGE && n->size >= page && src % page == 0);
                        assert(src + page <= im.private_off);
                    }
                }
                assert(ff_page(&im, i, n->size + page, page, 1, &j, &j));
                assert(ff_page(&im, i, 1, page, 1, &j, &j));
            }
        }
    }
    assert(count == im.objects);
    printf("validated %u objects, %u unique blobs; lookup and page-policy checks passed\n", count, im.nblobs);
    ff_destroy(&im); free(bytes);
    return 0;
}
// END File: ko/check-image.c
