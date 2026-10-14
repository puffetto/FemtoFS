// File: ko/mount_femtofs.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// nmount frontend; accepts the standard mount helper calling convention.
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct iovec iov[40];
    char path[MAXPATHLEN], device[MAXPATHLEN], message[256] = {0};
    char *options = NULL, *option;
    int ch, n = 0;
    while ((ch = getopt(argc, argv, "o:")) != -1) {
        if (ch != 'o' || options) errx(1, "usage: mount_femtofs [-o options] device directory");
        options = optarg;
    }
    if (argc - optind != 2) errx(1, "usage: mount_femtofs [-o options] device directory");
    if (!realpath(argv[optind], device) || !realpath(argv[optind+1], path)) err(1, "realpath");
#define ARG(k, v, len) do { iov[n++] = (struct iovec){(void *)(k), sizeof(k)}; \
    iov[n++] = (struct iovec){(void *)(v), (len)}; } while (0)
    ARG("fstype", "femtofs", sizeof("femtofs"));
    ARG("fspath", path, strlen(path)+1);
    ARG("from", device, strlen(device)+1);
    ARG("errmsg", message, sizeof(message));
    while (options && (option = strsep(&options, ","))) {
        if (!*option || n == 40) errx(1, "invalid mount options");
        iov[n++] = (struct iovec){option, strlen(option)+1};
        iov[n++] = (struct iovec){NULL, 0};
    }
    if (nmount(iov, n, MNT_RDONLY)) err(1, "nmount: %s", message);
    return 0;
}
// END File: ko/mount_femtofs.c
