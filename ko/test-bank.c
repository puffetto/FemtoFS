// File: ko/test-bank.c
// Created by Andrea "Nemesi" Cocito on 17/10/2026
// Exercise control ABI errors and privilege after descriptor passing.
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "bank.h"
int main(int argc, char **argv)
{
    struct ff_bank_request r = {.version = FF_BANK_VERSION, .fd = -1};
    int c, status, fd;
    pid_t child;
    assert(argc == 2);
    c = open("/dev/femtofs", O_RDWR); assert(c >= 0);
    strcpy(r.label, "probe");
    assert(ioctl(c, FF_BANK_ADD, &r) == -1 && errno == EBADF);
    r.version++;
    assert(ioctl(c, FF_BANK_LIST, &r) == -1 && errno == EINVAL);
    r.version = FF_BANK_VERSION;
    memset(r.label, 'x', sizeof(r.label));
    assert(ioctl(c, FF_BANK_DISCARD, &r) == -1 && errno == EINVAL);
    strcpy(r.label, "probe");
    fd = open("/", O_RDONLY); assert(fd >= 0); r.fd = fd;
    assert(ioctl(c, FF_BANK_ADD, &r) == -1 && errno == EOPNOTSUPP);
    close(fd);
    fd = open(argv[1], O_RDONLY); assert(fd >= 0); r.fd = fd;
    child = fork(); assert(child >= 0);
    if (!child) {
        assert(!setgroups(0, NULL) && !setgid(65534) && !setuid(65534));
        assert(ioctl(c, FF_BANK_ADD, &r) == -1 && errno == EPERM);
        assert(ioctl(c, FF_BANK_DISCARD, &r) == -1 && errno == EPERM);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(fd); close(c);
    puts("control ABI and passed-descriptor privilege: passed");
    return 0;
}
// END File: ko/test-bank.c
