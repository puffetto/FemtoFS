// File: ko/test-directory.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Directory cookies, rewind, dot entries and parent traversal regression.
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/mount.h>
#endif

int main(int argc, char **argv)
{
    DIR *dir;
    struct dirent *entry;
    struct stat root, parent;
    char name[256], path[4096];
    long cookie;
    unsigned count = 0, repeat = 0, dots = 0;
    assert(argc == 2 && (dir = opendir(argv[1])) != NULL);
    assert(!stat(argv[1], &root));
    /* CREATE lookup must resolve existing names before rejecting new writes. */
    snprintf(path, sizeof(path), "%s/hidden", argv[1]);
    assert(mkdir(path, 0755) == -1 && errno == EEXIST);
    snprintf(path, sizeof(path), "%s/new-directory", argv[1]);
    assert(mkdir(path, 0755) == -1 && errno == EROFS);
    assert(mkdir(path, 0755) == -1 && errno == EROFS);
#ifdef __FreeBSD__
    struct statfs fs;
    assert(!statfs(argv[1], &fs) && fs.f_namemax == 255);
    assert(pathconf(argv[1], _PC_NAME_MAX) == 255);
    assert(pathconf(argv[1], _PC_LINK_MAX) == 32767);
    assert(pathconf(argv[1], _PC_FILESIZEBITS) == 33);
    assert(pathconf(argv[1], _PC_SYMLINK_MAX) == MAXPATHLEN);
    assert(pathconf(argv[1], _PC_NO_TRUNC) == 1);
#endif
    snprintf(path, sizeof(path), "%s/hidden/..", argv[1]);
    assert(!stat(path, &parent) && root.st_ino == parent.st_ino);
    while ((entry = readdir(dir))) {
        count++;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) dots++;
        cookie = telldir(dir);
        entry = readdir(dir);
        if (!entry) break;
        strcpy(name, entry->d_name);
        seekdir(dir, cookie);
        entry = readdir(dir);
        assert(entry && !strcmp(name, entry->d_name));
        seekdir(dir, cookie);
    }
    rewinddir(dir);
    while (readdir(dir)) repeat++;
    assert(count == repeat && dots == 2);
    assert(!closedir(dir));
#ifdef __FreeBSD__
    int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
    unsigned small = 0;
    assert(fd >= 0);
    for (;;) {
        char buffer[512];
        off_t base;
        ssize_t length = getdirentries(fd, buffer, sizeof(buffer), &base);
        assert(length >= 0);
        if (!length) break;
        for (ssize_t offset = 0; offset < length;) {
            entry = (struct dirent *)(buffer + offset);
            assert(entry->d_reclen && offset + entry->d_reclen <= length);
            small++;
            offset += entry->d_reclen;
        }
    }
    assert(!close(fd) && small == count);
#endif
    printf("directory cookies, rewind and parent traversal: %u entries passed\n", count);
    return 0;
}
// END File: ko/test-directory.c
