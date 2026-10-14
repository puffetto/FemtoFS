// File: ko/test-directory.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Directory cookies, rewind, dot entries and parent traversal regression.
#include <sys/stat.h>
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

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
    printf("directory cookies, rewind and parent traversal: %u entries passed\n", count);
    return 0;
}
// END File: ko/test-directory.c
