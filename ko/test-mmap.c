// File: ko/test-mmap.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Mounted-file mmap protection, tail, lifetime and EOF regression probe.
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __FreeBSD__
#include <sys/mount.h>
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void bus(int sig) { (void)sig; _exit(0); }
int main(int argc, char **argv)
{
    struct stat st, expected;
    int fd, ef, kind, status;
    long page = sysconf(_SC_PAGESIZE);
    size_t length;
    unsigned char *bytes, *p, *shared;
    pid_t child;
    assert(argc == 4);
    fd = open(argv[1], O_RDONLY); ef = open(argv[2], O_RDONLY);
    assert(fd >= 0 && ef >= 0 && !fstat(fd, &st) && !fstat(ef, &expected));
    length = ((size_t)st.st_size + page - 1) / page * page;
    bytes = malloc(length ? length : 1);
    assert(expected.st_size == (off_t)length);
    assert(read(ef, bytes, length) == (ssize_t)length);
    close(ef);
    /* Optional pipe barrier lets independent processes fault a fresh cache together. */
    if (getenv("FEMTOFS_TEST_BARRIER")) {
        char token;
        int barrier = atoi(getenv("FEMTOFS_TEST_BARRIER"));
        int ready = atoi(getenv("FEMTOFS_TEST_READY"));
        assert(write(ready, "r", 1) == 1);
        close(ready);
        assert(read(barrier, &token, 1) == 1);
        close(barrier);
    }
#ifdef __FreeBSD__
    assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED));
    assert(!posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
#endif
    for (kind = 0; kind < 2; kind++) {
        int flags = kind ? MAP_PRIVATE : MAP_SHARED;
        p = mmap(NULL, length ? length : (size_t)page, PROT_READ | PROT_WRITE, flags, fd, 0);
        if (!kind) assert(p == MAP_FAILED && errno == EACCES);
        else {
            if (p == MAP_FAILED) perror("private writable mmap");
            assert(p != MAP_FAILED);
            /* Write before reading: exercise a COW fault on an absent page. */
            for (size_t i = 0; i < length; i += page) p[i] = bytes[i] ^ 0xff;
            assert(!munmap(p, length ? length : (size_t)page));
        }
        if (!length) continue;
        p = mmap(NULL, length, PROT_READ, flags, fd, 0);
        assert(p != MAP_FAILED && (uintptr_t)p % page == 0);
        assert(!memcmp(p, bytes, length));
        if (!kind) assert(mprotect(p, length, PROT_READ | PROT_WRITE) == -1);
        else {
            shared = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
            assert(shared != MAP_FAILED);
            assert(!mprotect(p, length, PROT_READ | PROT_WRITE));
            /* Include the partial tail and any permitted public spill. */
            for (size_t i = 0; i < length; i++) p[i] ^= 0xff;
            child = fork(); assert(child >= 0);
            if (!child) {
                for (size_t i = 0; i < length; i++) p[i] ^= 0x55;
                assert(!memcmp(shared, bytes, length));
                _exit(0);
            }
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
            for (size_t i = 0; i < length; i++) assert(p[i] == (unsigned char)(bytes[i] ^ 0xff));
            assert(!memcmp(shared, bytes, length));
            assert(!munmap(shared, length));
        }
        assert(!munmap(p, length));
    }
    p = mmap(NULL, length + page, PROT_READ, MAP_SHARED, fd, 0);
    assert(p != MAP_FAILED);
    close(fd);
    if (length) assert(!memcmp(p, bytes, length));
#ifdef __FreeBSD__
    errno = 0;
    assert(unmount(argv[3], 0) == -1 && errno == EBUSY);
#endif
    child = fork();
    assert(child >= 0);
    if (!child) {
        signal(SIGBUS, bus);
        (void)*(volatile unsigned char *)(p + length);
        _exit(1);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!munmap(p, length + page));
    free(bytes);
    puts("mmap bytes, tails, shared protection, private COW/fork, descriptor lifetime and EOF: passed");
    return 0;
}
// END File: ko/test-mmap.c
