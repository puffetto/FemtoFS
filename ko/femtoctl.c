// File: ko/femtoctl.c
// Created by Andrea "Nemesi" Cocito on 17/10/2026
// Administrative frontend for resident FemtoFS banks.
#include <sys/types.h>
#include <sys/ioctl.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bank.h"

int main(int argc, char **argv)
{
    struct ff_bank_request r = {.version = FF_BANK_VERSION}, *rows = NULL;
    size_t count = 0;
    int control, source = -1, attempts = 0;
    unsigned long cmd;
    if (argc == 2 && !strcmp(argv[1], "list")) cmd = FF_BANK_LIST;
    else if (argc == 3 && !strcmp(argv[1], "discard")) cmd = FF_BANK_DISCARD;
    else if (argc == 4 && !strcmp(argv[1], "add")) cmd = FF_BANK_ADD;
    else errx(1, "usage: femtoctl list | add label path | discard label (async is not implemented)");
    if (cmd != FF_BANK_LIST && strlcpy(r.label, argv[2], sizeof(r.label)) >= sizeof(r.label))
        errx(1, "label too long");
    control = open("/dev/femtofs", O_RDWR | O_CLOEXEC);
    if (control < 0) err(1, "/dev/femtofs");
    if (cmd == FF_BANK_ADD) {
        if (strlcpy(r.source, argv[3], sizeof(r.source)) >= sizeof(r.source)) errx(1, "source path too long");
        source = open(argv[3], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (source < 0) err(1, "%s", argv[3]);
        r.fd = source;
    }
    if (cmd != FF_BANK_LIST) {
        if (ioctl(control, cmd, &r)) err(1, "%s %s", argv[1], r.label);
    } else {
        for (;;) {
            if (ioctl(control, cmd, &r)) {
                if (errno == ENOENT) break;
                if (errno == EAGAIN && ++attempts < 8) { count = 0; r.index = 0; continue; }
                err(1, "list");
            }
            struct ff_bank_request *next = reallocarray(rows, count + 1, sizeof(*rows));
            if (!next) err(1, "list allocation");
            rows = next; rows[count++] = r; r.index++;
        }
        puts("LABEL\tORIGIN\tBYTES\tALLOCATED\tSTATE\tUUID\tSUPPLIED_SOURCE");
        for (size_t i = 0; i < count; i++) {
            r = rows[i];
            printf("%s\t%s\t%llu\t%llu\t%s\t", r.label,
                r.preload ? "loader" : "copy", (unsigned long long)r.size,
                (unsigned long long)r.extent, r.claimed ? "claimed" : "available");
            for (size_t j = 0; j < sizeof(r.uuid); j++) printf("%02x", r.uuid[j]);
            printf("\t%s\n", r.source);
        }
        free(rows);
    }
    if (source >= 0) close(source);
    close(control);
    return 0;
}
// END File: ko/femtoctl.c
