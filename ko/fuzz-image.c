// File: ko/fuzz-image.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// libFuzzer entry point for the exact mount-time image decoder.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct ff_image im = {0};
    uint8_t *copy;
    uint32_t sum;
    if (size < 128 || size > 1024 * 1024 || ff_header(data, size, &im)) return 0;
    copy = malloc(size);
    if (!copy) return 0;
    memcpy(copy, data, size);
    /* Let mutations reach structural checks rather than stop at the checksum. */
    sum = ff_fnv(copy + 128, im.count * 16);
    copy[12] = sum; copy[13] = sum >> 8; copy[14] = sum >> 16; copy[15] = sum >> 24;
    im.bytes = copy;
    (void)ff_validate(&im);
    ff_destroy(&im);
    free(copy);
    return 0;
}
// END File: ko/fuzz-image.c
