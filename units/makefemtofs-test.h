// File: units/makefemtofs-test.h
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Test-only access to the builder and deterministic descriptor-scoped I/O faults.
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdarg>
#include <fstream>
#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

#ifdef __FreeBSD__
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#endif

#ifdef __linux__
#include <sys/random.h>
#endif

#include <femtofs/format.h>
#include <femtofs/hash_plan.h>
#include <isPrime.h>

namespace builder_test {
struct IoState {
    int output = -1;
    int verification = -1;
    int operations = 0;
    int failAt = 0;
    int readsAfterFailure = 0;
    int verificationReads = 0;
    int chmodCalls = 0;
    bool failed = false;
    bool privateReads = true;
    bool aligned = true;
    bool pageIo = false;
    bool failCommit = false;
    bool failFinalFlush = false;
    bool committed = false;
    bool unreadable = false;
    bool corruptOnReopen = false;
    bool failReopen = false;
    bool failChmod = false;
    bool failParentFlush = false;
    bool failPublish = false;
    ssize_t partial = -1;
    off_t corruptAt = -1;
    int writeErrno = EIO;
    std::string temporary;
    std::string changeSource;
};
inline IoState io;

inline bool fault(int fd)
{
    if (fd != io.output) return false;
    ++io.operations;
    if (io.failAt != io.operations) return false;
    io.failed = true;
    errno = ENOSPC;
    return true;
}
}

inline int review_lstat(const char* path, struct stat* status)
{
    auto& changed = builder_test::io.changeSource;
    if (!changed.empty() && changed == path) {
        if (::chmod(path, 0600) != 0) std::abort();
        changed.clear();
    }
    return ::lstat(path, status);
}
inline int review_mkstemp(char* path)
{
    const int fd = ::mkstemp(path);
    if (std::string_view(path).find(".makefemtofs.") != std::string_view::npos) {
        builder_test::io.output = fd;
        builder_test::io.temporary = path;
    }
    return fd;
}
inline int review_open(const char* path, int flags, ...)
{
    using namespace builder_test;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    const bool verification = !io.temporary.empty() && io.temporary == path;
    if (verification && io.failReopen) { errno = EACCES; return -1; }
    if (verification && io.corruptOnReopen) {
        const uint8_t bad = 0xff;
        if (::pwrite(io.output, &bad, 1, 0) != 1) std::abort();
    }
    const int fd = ::open(path, flags, mode);
    if (verification) io.verification = fd;
    return fd;
}
inline ssize_t review_pread(int fd, void* bytes, size_t size, off_t offset)
{
    using namespace builder_test;
    if (fd == io.verification) {
        struct stat st{};
        if (::fstat(fd, &st) != 0) std::abort();
        io.privateReads &= (st.st_mode & 0777) == 0600;
        ++io.verificationReads;
    }
    if (fd == io.output) {
        if (io.pageIo) io.aligned &= size == 4096 && offset % 4096 == 0;
        if (io.failed) {
            ++io.readsAfterFailure;
            if (io.unreadable) { errno = EIO; return -1; }
        }
    }
    return ::pread(fd, bytes, size, offset);
}
inline ssize_t review_pwrite(int fd, const void* bytes, size_t size, off_t offset)
{
    using namespace builder_test;
    if (fd == io.output) {
        if (io.pageIo) io.aligned &= size == 4096 && offset % 4096 == 0;
        const bool commit = offset == 0 && size >= 3 &&
            std::memcmp(bytes, "0FS", 3) == 0;
        if (commit) io.committed = true;
        if (fault(fd) || (commit && io.failCommit)) {
            io.failed = true;
            if (io.partial >= 0) {
                const auto count = std::min(size, static_cast<size_t>(io.partial));
                if (::pwrite(fd, bytes, count, offset) != static_cast<ssize_t>(count))
                    std::abort();
                if (count < size) return static_cast<ssize_t>(count);
            }
            errno = io.writeErrno;
            return -1;
        }
    }
    return ::pwrite(fd, bytes, size, offset);
}
inline int review_fsync(int fd)
{
    using namespace builder_test;
    struct stat st{};
    if (::fstat(fd, &st) != 0) return -1;
    if (fault(fd) || (fd == io.output && io.committed && io.failFinalFlush) ||
        (S_ISDIR(st.st_mode) && io.failParentFlush)) {
        io.failed = true;
        if (io.corruptAt >= 0) {
            uint8_t byte = 0;
            if (::pread(fd, &byte, 1, io.corruptAt) != 1) std::abort();
            byte ^= 0xff;
            if (::pwrite(fd, &byte, 1, io.corruptAt) != 1) std::abort();
        }
        errno = EIO;
        return -1;
    }
    return ::fsync(fd);
}
inline int review_fchmod(int fd, mode_t mode)
{
    if (fd == builder_test::io.output) {
        ++builder_test::io.chmodCalls;
        if (builder_test::io.failChmod) { errno = EACCES; return -1; }
    }
    return ::fchmod(fd, mode);
}
inline int review_link(const char* from, const char* to)
{
    if (builder_test::io.failPublish) { errno = EACCES; return -1; }
    return ::link(from, to);
}
inline int review_rename(const char* from, const char* to)
{
    if (builder_test::io.failPublish) { errno = EACCES; return -1; }
    return ::rename(from, to);
}

#define main makefemtofs_test_main
#define lstat review_lstat
#define mkstemp review_mkstemp
#define open review_open
#define pread review_pread
#define pwrite review_pwrite
#define fsync review_fsync
#define fchmod review_fchmod
#define link review_link
#define rename review_rename
#include "../programs/makefemtofs.cpp"
#undef main
#undef lstat
#undef mkstemp
#undef open
#undef pread
#undef pwrite
#undef fsync
#undef fchmod
#undef link
#undef rename

namespace builder_test {
struct Fixture {
    std::filesystem::path directory;
    Options options;
    struct stat inputStatus{};
    Fixture()
    {
        std::string pattern = std::string(REVIEW_TEST_ROOT) + "/builder.XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back(0);
        if (!::mkdtemp(path.data())) throw std::runtime_error("mkdtemp test fixture");
        directory = path.data();
        std::filesystem::create_directory(directory / "source");
        options.input = (directory / "source").string();
        options.output = (directory / "output.img").string();
        options.tempDir = directory.string();
        options.uuid = parseUuid("00112233-4455-6677-8899-aabbccddeeff");
        if (::lstat(options.input.c_str(), &inputStatus) != 0) std::abort();
        io = {};
    }
    ~Fixture() { io = {}; std::filesystem::remove_all(directory); }
    ImagePlan plan(Spool& spool, std::vector<Node> nodes = {}) const
    {
        InputTree tree;
        tree.nodes = std::move(nodes);
        normalizeTree(tree, options, spool);
        auto blobs = classifyBlobs(tree, spool);
        return buildPlan(std::move(tree), std::move(blobs), options, spool);
    }
};
inline Node file(std::string path, std::string bytes)
{
    Node node;
    node.path = std::move(path);
    std::tie(node.parent, node.name) = splitParent(node.path);
    node.attr.mode = S_IFREG | 0644;
    node.payload = inlineSource(std::move(bytes));
    return node;
}
inline std::string contents(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
}
// END File: units/makefemtofs-test.h
