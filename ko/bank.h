// File: ko/bank.h
// Created by Andrea "Nemesi" Cocito on 17/10/2026
// Resident bank control ABI and kernel ownership interface.
#ifndef FEMTOFS_BANK_H
#define FEMTOFS_BANK_H
#include <sys/types.h>
#include <sys/ioccom.h>
#define FF_BANK_VERSION 1
#define FF_LABEL_SIZE 64
struct ff_bank_request {
    uint32_t version, index;
    uint64_t generation, size, extent;
    int32_t fd;
    uint32_t preload, claimed;
    uint8_t uuid[16];
    char label[FF_LABEL_SIZE];
    char source[1024];
    uint32_t reserved;
};
_Static_assert(sizeof(struct ff_bank_request) == 1152, "stable bank ioctl ABI");
#define FF_BANK_ADD _IOW('F', 1, struct ff_bank_request)
#define FF_BANK_LIST _IOWR('F', 2, struct ff_bank_request)
#define FF_BANK_DISCARD _IOW('F', 3, struct ff_bank_request)
#ifdef _KERNEL
#include <vm/vm.h>
#include "image.h"
struct ff_bank {
    struct ff_bank *next;
    struct ff_image image;
    size_t extent;
    vm_object_t object;
    vm_page_t *pages;
    uint8_t *loader_oflags;
    size_t owned;
    int mapped;
    int claimed, preload;
    u_int sampled;
    char label[FF_LABEL_SIZE], source[1024];
};
int ff_banks_init(void);
int ff_banks_uninit(void);
int ff_bank_claim(const char *, struct ff_bank **);
void ff_bank_release(struct ff_bank *);
#endif
#endif
// END File: ko/bank.h
