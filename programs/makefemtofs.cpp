// File: programs/makefemtofs.cpp
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Build a femtoFS image from a directory tree or libarchive input.

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

namespace {

using femtofs::kCellSize;
using femtofs::kHeaderSize;
using femtofs::kPageSize;

class Error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] bool byteLess(std::string_view lhs, std::string_view rhs)
{
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
        [](char left, char right) {
            return static_cast<unsigned char>(left) <
                   static_cast<unsigned char>(right);
        });
}

struct Options {
    bool force = false;
    bool device = false;
    bool dryRun = false;
    bool synthesizeDirs = false;
    bool traverseMounts = false;
    bool verbose = false;
    uint32_t stripComponents = 0;
    std::optional<uint16_t> rootMode;
    std::optional<uint32_t> rootUid;
    std::optional<uint32_t> rootGid;
    std::optional<std::array<uint8_t, 16>> uuid;
    std::string tempDir;
    std::string verify = "full";
    std::string input;
    std::string output;
};

[[nodiscard]] std::string errnoText(std::string_view operation,
                                    const std::string& path = {})
{
    std::string message(operation);
    if (!path.empty())
        message += " '" + path + "'";
    message += ": ";
    message += std::strerror(errno);
    return message;
}

[[nodiscard]] uint32_t parseU32(std::string_view text, int base,
                                std::string_view option)
{
    uint32_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last)
        throw Error("invalid value for " + std::string(option) + ": " +
                    std::string(text));
    return value;
}

[[nodiscard]] std::array<uint8_t, 16> parseUuid(std::string_view text)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') {
        throw Error("--uuid requires canonical 8-4-4-4-12 hexadecimal text");
    }

    std::array<uint8_t, 16> bytes{};
    size_t output = 0;
    for (size_t input = 0; input < text.size();) {
        if (text[input] == '-') {
            ++input;
            continue;
        }
        if (input + 2 > text.size() || output == bytes.size())
            throw Error("invalid --uuid value");
        const uint32_t value = parseU32(text.substr(input, 2), 16, "--uuid");
        bytes[output++] = static_cast<uint8_t>(value);
        input += 2;
    }
    if (output != bytes.size())
        throw Error("invalid --uuid value");
    return bytes;
}

[[noreturn]] void usageError(const std::string& message)
{
    throw Error(message + "\nTry 'makefemtofs --help' for usage.");
}

void printUsage(std::ostream& out)
{
    out <<
        "usage: makefemtofs [options] INPUT OUTPUT\n"
        "  -f, --force                 replace an existing regular file\n"
        "      --device                authorize a raw device output\n"
        "      --dry-run               plan and validate without writing\n"
        "      --strip-components N    remove archive path components\n"
        "      --synthesize-dirs       create omitted archive parents\n"
        "      --root-mode OCTAL       override/default root permissions\n"
        "      --root-uid UID          override/default root uid\n"
        "      --root-gid GID          override/default root gid\n"
        "      --traverse-mounts       cross mounts for directory input\n"
        "      --uuid UUID             fixed canonical UUID\n"
        "      --temp-dir DIR          payload spool directory\n"
        "      --verify MODE           full (default) or structure\n"
        "  -v, --verbose               print planning statistics\n"
        "  -V, --version               print version\n"
        "  -h, --help                  print this help\n";
}

[[nodiscard]] Options parseOptions(int argc, char** argv)
{
    Options options;
    std::vector<std::string> positional;

    auto valueAfter = [&](int& index, std::string_view option) -> std::string {
        if (++index >= argc)
            usageError("missing value after " + std::string(option));
        return argv[index];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            while (++i < argc)
                positional.emplace_back(argv[i]);
            break;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage(std::cout);
            std::exit(0);
        }
        if (arg == "-V" || arg == "--version") {
            std::cout << "makefemtofs 0.1 (femtoFS 0x0100)\n";
            std::exit(0);
        }
        if (arg == "-f" || arg == "--force") {
            options.force = true;
        } else if (arg == "--device") {
            options.device = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--synthesize-dirs") {
            options.synthesizeDirs = true;
        } else if (arg == "--traverse-mounts") {
            options.traverseMounts = true;
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--strip-components") {
            options.stripComponents = parseU32(valueAfter(i, arg), 10, arg);
        } else if (arg == "--root-mode") {
            const uint32_t value = parseU32(valueAfter(i, arg), 8, arg);
            if (value > 07777u)
                usageError("--root-mode exceeds 07777");
            options.rootMode = static_cast<uint16_t>(value);
        } else if (arg == "--root-uid") {
            options.rootUid = parseU32(valueAfter(i, arg), 10, arg);
        } else if (arg == "--root-gid") {
            options.rootGid = parseU32(valueAfter(i, arg), 10, arg);
        } else if (arg == "--uuid") {
            options.uuid = parseUuid(valueAfter(i, arg));
        } else if (arg == "--temp-dir") {
            options.tempDir = valueAfter(i, arg);
        } else if (arg == "--verify") {
            options.verify = valueAfter(i, arg);
        } else if (!arg.empty() && arg[0] == '-') {
            usageError("unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (options.verify != "full" && options.verify != "structure")
        usageError("--verify must be 'full' or 'structure'");
    if (positional.size() != 2)
        usageError("exactly INPUT and OUTPUT are required");
    options.input = positional[0];
    options.output = positional[1];
    return options;
}

void writeAllAt(int fd, std::span<const uint8_t> bytes, uint64_t offset)
{
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::pwrite(fd, bytes.data() + done,
                                         bytes.size() - done,
                                         static_cast<off_t>(offset + done));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            throw Error(errnoText("pwrite"));
        }
        if (written == 0)
            throw Error("pwrite returned zero before completing output");
        done += static_cast<size_t>(written);
    }
}

void readAllAt(int fd, std::span<uint8_t> bytes, uint64_t offset)
{
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::pread(fd, bytes.data() + done,
                                      bytes.size() - done,
                                      static_cast<off_t>(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw Error(errnoText("pread"));
        }
        if (count == 0)
            throw Error("unexpected end of file while reading output");
        done += static_cast<size_t>(count);
    }
}

void writePageAt(int fd, std::span<const uint8_t> page, uint64_t offset)
{
    if (page.size() != kPageSize || offset % kPageSize != 0)
        throw Error("internal device write is not image-page aligned");
    ssize_t written;
    do {
        written = ::pwrite(fd, page.data(), page.size(),
                           static_cast<off_t>(offset));
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        throw Error(errnoText("pwrite device page"));
    if (static_cast<size_t>(written) != page.size())
        throw Error("short device page write");
}

void readPageAt(int fd, std::span<uint8_t> page, uint64_t offset)
{
    if (page.size() != kPageSize || offset % kPageSize != 0)
        throw Error("internal device read is not image-page aligned");
    ssize_t count;
    do {
        count = ::pread(fd, page.data(), page.size(), static_cast<off_t>(offset));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        throw Error(errnoText("pread device page"));
    if (static_cast<size_t>(count) != page.size())
        throw Error("short device page read");
}

void writeImageAt(int fd, std::span<const uint8_t> bytes, uint64_t offset,
                  bool pageIo)
{
    if (!pageIo) {
        writeAllAt(fd, bytes, offset);
        return;
    }

    std::array<uint8_t, kPageSize> page{};
    while (!bytes.empty()) {
        const uint64_t pageOffset = offset & ~(static_cast<uint64_t>(kPageSize) - 1u);
        const size_t within = static_cast<size_t>(offset - pageOffset);
        const size_t count = std::min(bytes.size(), kPageSize - within);
        if (within == 0 && count == kPageSize) {
            writePageAt(fd, bytes.first(count), pageOffset);
        } else {
            readPageAt(fd, page, pageOffset);
            std::copy_n(bytes.begin(), count, page.begin() + within);
            writePageAt(fd, page, pageOffset);
        }
        bytes = bytes.subspan(count);
        offset += count;
    }
}

void readImageAt(int fd, std::span<uint8_t> bytes, uint64_t offset,
                 bool pageIo)
{
    if (!pageIo) {
        readAllAt(fd, bytes, offset);
        return;
    }

    std::array<uint8_t, kPageSize> page{};
    while (!bytes.empty()) {
        const uint64_t pageOffset = offset & ~(static_cast<uint64_t>(kPageSize) - 1u);
        const size_t within = static_cast<size_t>(offset - pageOffset);
        const size_t count = std::min(bytes.size(), kPageSize - within);
        if (within == 0 && count == kPageSize) {
            readPageAt(fd, bytes.first(count), pageOffset);
        } else {
            readPageAt(fd, page, pageOffset);
            std::copy_n(page.begin() + within, count, bytes.begin());
        }
        bytes = bytes.subspan(count);
        offset += count;
    }
}

[[nodiscard]] uint64_t fnv1a64(std::span<const uint8_t> bytes,
                               uint64_t state = 14695981039346656037ull)
{
    for (const uint8_t byte : bytes) {
        state ^= byte;
        state *= 1099511628211ull;
    }
    return state;
}

struct BlobSource {
    bool inSpool = false;
    uint64_t spoolOff = 0;
    uint32_t size = 0;
    uint64_t digest = 14695981039346656037ull;
    std::string inlineBytes;
};

class Spool final {
public:
    explicit Spool(const std::string& directory)
    {
        if (directory.empty()) {
            file_ = std::tmpfile();
            if (file_ == nullptr)
                throw Error(errnoText("tmpfile"));
            return;
        }

        std::string pattern = directory + "/makefemtofs.XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back('\0');
        const int fd = ::mkstemp(path.data());
        if (fd < 0)
            throw Error(errnoText("mkstemp", pattern));
        if (::unlink(path.data()) != 0) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            throw Error(errnoText("unlink temporary spool", path.data()));
        }
        file_ = ::fdopen(fd, "w+b");
        if (file_ == nullptr) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            throw Error(errnoText("fdopen temporary spool"));
        }
    }

    ~Spool()
    {
        if (file_ != nullptr)
            std::fclose(file_);
    }

    Spool(const Spool&) = delete;
    Spool& operator=(const Spool&) = delete;

    [[nodiscard]] BlobSource appendArchive(struct archive* input,
                                           uint32_t expectedSize)
    {
        BlobSource source;
        source.inSpool = true;
        source.spoolOff = tail_;
        source.size = expectedSize;
        std::array<uint8_t, 64 * 1024> buffer{};
        uint64_t total = 0;
        uint64_t digest = source.digest;

        for (;;) {
            const la_ssize_t count = archive_read_data(input, buffer.data(),
                                                       buffer.size());
            if (count < 0)
                throw Error(archiveMessage(input, "reading entry data"));
            if (count == 0)
                break;
            if (total + static_cast<uint64_t>(count) > expectedSize)
                throw Error("archive entry produced more data than declared");
            const auto chunk = std::span<const uint8_t>(buffer.data(),
                                                        static_cast<size_t>(count));
            writeAllAt(::fileno(file_), chunk, tail_ + total);
            digest = fnv1a64(chunk, digest);
            total += static_cast<uint64_t>(count);
        }
        if (total != expectedSize)
            throw Error("archive entry data length differs from its declared size");
        tail_ += total;
        source.digest = digest;
        return source;
    }

    [[nodiscard]] BlobSource emptySource() const
    {
        BlobSource source;
        source.inSpool = true;
        source.spoolOff = tail_;
        return source;
    }

    void read(const BlobSource& source, uint32_t sourceOffset,
              std::span<uint8_t> output) const
    {
        if (static_cast<uint64_t>(sourceOffset) + output.size() > source.size)
            throw Error("internal blob read exceeds source size");
        if (source.inSpool) {
            readAllAt(::fileno(file_), output, source.spoolOff + sourceOffset);
        } else {
            std::memcpy(output.data(), source.inlineBytes.data() + sourceOffset,
                        output.size());
        }
    }

    [[nodiscard]] static std::string archiveMessage(struct archive* input,
                                                    std::string_view operation)
    {
        const char* detail = archive_error_string(input);
        return std::string(operation) + ": " + (detail == nullptr ?
               "unknown libarchive error" : detail);
    }

private:
    std::FILE* file_ = nullptr;
    uint64_t tail_ = 0;
};

[[nodiscard]] BlobSource inlineSource(std::string bytes)
{
    BlobSource source;
    source.size = static_cast<uint32_t>(bytes.size());
    source.digest = fnv1a64(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    source.inlineBytes = std::move(bytes);
    return source;
}

[[nodiscard]] bool equalSources(const Spool& spool, const BlobSource& lhs,
                                const BlobSource& rhs)
{
    if (lhs.size != rhs.size || lhs.digest != rhs.digest)
        return false;
    std::array<uint8_t, 64 * 1024> left{};
    std::array<uint8_t, 64 * 1024> right{};
    for (uint32_t offset = 0; offset < lhs.size;) {
        const uint32_t count = std::min<uint32_t>(left.size(), lhs.size - offset);
        spool.read(lhs, offset, std::span<uint8_t>(left.data(), count));
        spool.read(rhs, offset, std::span<uint8_t>(right.data(), count));
        if (!std::equal(left.begin(), left.begin() + count, right.begin()))
            return false;
        offset += count;
    }
    return true;
}

[[nodiscard]] int compareSources(const Spool& spool, const BlobSource& lhs,
                                 const BlobSource& rhs)
{
    std::array<uint8_t, 4096> left{};
    std::array<uint8_t, 4096> right{};
    const uint32_t common = std::min(lhs.size, rhs.size);
    for (uint32_t offset = 0; offset < common;) {
        const uint32_t count = std::min<uint32_t>(left.size(), common - offset);
        spool.read(lhs, offset, std::span<uint8_t>(left.data(), count));
        spool.read(rhs, offset, std::span<uint8_t>(right.data(), count));
        const int result = std::memcmp(left.data(), right.data(), count);
        if (result != 0)
            return result;
        offset += count;
    }
    return lhs.size < rhs.size ? -1 : lhs.size > rhs.size ? 1 : 0;
}

enum class NodeKind : uint8_t {
    file = femtofs::kTypeFile,
    directory = femtofs::kTypeDirectory,
    symlink = femtofs::kTypeSymlink,
    hardlink = femtofs::kTypeHardlink,
    fifo = femtofs::kTypeFifo
};

struct AttrKey {
    uint16_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;

    [[nodiscard]] auto tie() const { return std::tie(mode, uid, gid); }
    [[nodiscard]] bool operator==(const AttrKey& other) const
    {
        return tie() == other.tie();
    }
    [[nodiscard]] bool operator<(const AttrKey& other) const
    {
        return tie() < other.tie();
    }
};

struct SourceIdentity {
    int64_t device = 0;
    int64_t inode = 0;
    bool valid = false;

    [[nodiscard]] auto tie() const { return std::tie(device, inode); }
    [[nodiscard]] bool operator<(const SourceIdentity& other) const
    {
        return tie() < other.tie();
    }
};

struct Node {
    std::string path;
    std::string parent;
    std::string name;
    NodeKind kind = NodeKind::file;
    AttrKey attr;
    std::optional<BlobSource> payload;
    std::string symlinkTarget;
    std::string explicitHardlink;
    SourceIdentity identity;
    uint64_t nlink = 1;
    uint32_t objectIndex = 0;
    uint16_t attrIndex = 0;
    uint32_t canonicalNode = 0;
    uint32_t nameBlob = 0;
    uint32_t contentBlob = 0;
    bool ignoredMetadata = false;
};

struct InputTree {
    AttrKey rootAttr{static_cast<uint16_t>(S_IFDIR | 0755), 0, 0};
    bool hasRootAttr = false;
    uint64_t ignoredMetadata = 0;
    std::vector<Node> nodes;
};

[[nodiscard]] std::pair<std::string, std::string> splitParent(std::string path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return {"", std::move(path)};
    return {path.substr(0, slash), path.substr(slash + 1)};
}

[[nodiscard]] std::string normalizeArchivePath(std::string path, bool directory,
                                               uint32_t stripComponents)
{
    if (path.find('\0') != std::string::npos)
        throw Error("archive pathname contains NUL");
    if (!path.empty() && path.front() == '/')
        throw Error("archive contains absolute pathname: " + path);

    if (directory) {
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
    } else if (!path.empty() && path.back() == '/') {
        throw Error("non-directory archive entry has a trailing slash: " + path);
    }

    while (path == "." || path.rfind("./", 0) == 0) {
        if (path == ".") {
            path.clear();
            break;
        }
        path.erase(0, 2);
    }

    std::vector<std::string> components;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            throw Error("archive pathname has an invalid component: " + path);
        if (component.size() > 255)
            throw Error("archive pathname component exceeds 255 bytes: " + path);
        components.push_back(component);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    if (stripComponents >= components.size()) {
        if (directory)
            return {};
        throw Error("--strip-components removes the complete non-directory path: " +
                    path);
    }
    components.erase(components.begin(),
                     components.begin() + static_cast<ptrdiff_t>(stripComponents));
    std::string result;
    for (const std::string& component : components) {
        if (!result.empty())
            result.push_back('/');
        result += component;
    }
    return result;
}

[[nodiscard]] uint16_t checkedMode(mode_t mode, NodeKind kind,
                                   const std::string& path)
{
    if (static_cast<uint64_t>(mode) > std::numeric_limits<uint16_t>::max())
        throw Error("mode does not fit uint16_t: " + path);
    const uint16_t narrowed = static_cast<uint16_t>(mode);
    uint16_t expected = 0;
    switch (kind) {
        case NodeKind::file:
        case NodeKind::hardlink: expected = S_IFREG; break;
        case NodeKind::directory: expected = S_IFDIR; break;
        case NodeKind::symlink: expected = S_IFLNK; break;
        case NodeKind::fifo: expected = S_IFIFO; break;
    }
    if ((narrowed & S_IFMT) != expected)
        throw Error("entry type and mode disagree: " + path);
    return narrowed;
}

[[nodiscard]] NodeKind entryKind(mode_t fileType, const std::string& path)
{
    switch (fileType) {
        case AE_IFREG: return NodeKind::file;
        case AE_IFDIR: return NodeKind::directory;
        case AE_IFLNK: return NodeKind::symlink;
        case AE_IFIFO: return NodeKind::fifo;
        default: throw Error("unsupported archive entry type: " + path);
    }
}

[[nodiscard]] bool hasIgnoredMetadata(struct archive_entry* entry)
{
    const int accessAcl = archive_entry_acl_count(
        entry, ARCHIVE_ENTRY_ACL_TYPE_ACCESS);
    const int nfs4Acl = archive_entry_acl_count(entry, ARCHIVE_ENTRY_ACL_TYPE_NFS4);
    const int defaultAcl = archive_entry_acl_count(
        entry, ARCHIVE_ENTRY_ACL_TYPE_DEFAULT);
    const int xattrs = archive_entry_xattr_count(entry);
    unsigned long setFlags = 0;
    unsigned long clearFlags = 0;
    archive_entry_fflags(entry, &setFlags, &clearFlags);
    return accessAcl > 3 || nfs4Acl > 0 || defaultAcl > 0 || xattrs > 0 ||
           setFlags != 0 || clearFlags != 0;
}

void checkArchiveStatus(struct archive* input, int status,
                        std::string_view operation)
{
    if (status != ARCHIVE_OK)
        throw Error(Spool::archiveMessage(input, operation));
}

[[nodiscard]] Node nodeFromEntry(struct archive* input,
                                 struct archive_entry* entry,
                                 std::string normalizedPath,
                                 Spool& spool,
                                 bool canSkipData)
{
    const mode_t fileType = archive_entry_filetype(entry);
    const char* hardlinkTarget = archive_entry_hardlink(entry);
    Node node;
    node.path = std::move(normalizedPath);
    std::tie(node.parent, node.name) = splitParent(node.path);
    node.kind = hardlinkTarget == nullptr ? entryKind(fileType, node.path)
                                          : NodeKind::file;
    node.ignoredMetadata = hasIgnoredMetadata(entry);

    const la_int64_t uid = archive_entry_uid(entry);
    const la_int64_t gid = archive_entry_gid(entry);
    if (uid < 0 || static_cast<uint64_t>(uid) > UINT32_MAX ||
        gid < 0 || static_cast<uint64_t>(gid) > UINT32_MAX) {
        throw Error("uid/gid cannot be represented: " + node.path);
    }
    mode_t mode = archive_entry_mode(entry);
    if (hardlinkTarget != nullptr && (mode & S_IFMT) == 0)
        mode |= S_IFREG;
    node.attr.mode = checkedMode(mode, node.kind, node.path);
    node.attr.uid = static_cast<uint32_t>(uid);
    node.attr.gid = static_cast<uint32_t>(gid);
    node.nlink = static_cast<uint64_t>(std::max<la_int64_t>(1,
                                      archive_entry_nlink(entry)));
    node.identity.device = static_cast<int64_t>(archive_entry_dev(entry));
    node.identity.inode = static_cast<int64_t>(archive_entry_ino64(entry));
    node.identity.valid = node.nlink > 1 && archive_entry_ino_is_set(entry);

    if (node.kind == NodeKind::file) {
        if (hardlinkTarget != nullptr)
            node.explicitHardlink = hardlinkTarget;

        if (node.explicitHardlink.empty()) {
            const la_int64_t declared = archive_entry_size(entry);
            if (declared < 0 || static_cast<uint64_t>(declared) > UINT32_MAX)
                throw Error("regular-file size cannot be represented: " + node.path);
            const uint64_t stored = (static_cast<uint64_t>(declared) + 4u) & ~3ull;
            if (stored > UINT32_MAX)
                throw Error("encoded regular-file size cannot be represented: " +
                            node.path);
            node.payload = spool.appendArchive(input, static_cast<uint32_t>(declared));
        } else if (canSkipData) {
            checkArchiveStatus(input, archive_read_data_skip(input),
                               "skipping hardlink entry data");
        }
    } else if (node.kind == NodeKind::symlink) {
        const char* target = archive_entry_symlink(entry);
        if (target == nullptr)
            throw Error("symlink has no target: " + node.path);
        node.symlinkTarget = target;
        if (node.symlinkTarget.find('\0') != std::string::npos)
            throw Error("symlink target contains NUL: " + node.path);
        if (canSkipData) {
            checkArchiveStatus(input, archive_read_data_skip(input),
                               "skipping symlink entry data");
        }
    } else if (canSkipData) {
        checkArchiveStatus(input, archive_read_data_skip(input),
                           "skipping non-file entry data");
    }
    return node;
}

using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

[[nodiscard]] InputTree readArchive(const Options& options, Spool& spool)
{
    ArchivePtr input(archive_read_new(), &archive_read_free);
    if (!input)
        throw Error("archive_read_new failed");
    checkArchiveStatus(input.get(), archive_read_support_filter_all(input.get()),
                       "enabling archive filters");
    checkArchiveStatus(input.get(), archive_read_support_format_all(input.get()),
                       "enabling archive formats");
    checkArchiveStatus(input.get(),
                       archive_read_open_filename(input.get(), options.input.c_str(),
                                                  64 * 1024),
                       "opening input archive");

    InputTree tree;
    struct archive_entry* entry = nullptr;
    for (;;) {
        const int status = archive_read_next_header(input.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        checkArchiveStatus(input.get(), status, "reading archive header");
        const char* raw = archive_entry_pathname(entry);
        if (raw == nullptr)
            throw Error("archive entry has no pathname");
        const bool isDirectory = archive_entry_filetype(entry) == AE_IFDIR;
        std::string path = normalizeArchivePath(raw, isDirectory,
                                                options.stripComponents);
        if (path.empty()) {
            if (!isDirectory)
                throw Error("non-directory archive entry maps to image root");
            Node root = nodeFromEntry(input.get(), entry, {}, spool, true);
            tree.ignoredMetadata += root.ignoredMetadata;
            if (tree.hasRootAttr)
                throw Error("archive contains duplicate root directory entries");
            tree.rootAttr = root.attr;
            tree.hasRootAttr = true;
            continue;
        }

        Node node = nodeFromEntry(input.get(), entry, path, spool, true);
        tree.ignoredMetadata += node.ignoredMetadata;
        if (!node.explicitHardlink.empty()) {
            node.explicitHardlink = normalizeArchivePath(
                node.explicitHardlink, false, options.stripComponents);
        }
        tree.nodes.push_back(std::move(node));
    }
    checkArchiveStatus(input.get(), archive_read_close(input.get()),
                       "closing input archive");
    return tree;
}

[[nodiscard]] InputTree readDirectory(const Options& options, Spool& spool)
{
    ArchivePtr input(archive_read_disk_new(), &archive_read_free);
    if (!input)
        throw Error("archive_read_disk_new failed");
    checkArchiveStatus(input.get(), archive_read_disk_set_symlink_physical(input.get()),
                       "setting physical symlink traversal");
    int behavior = 0;
    if (!options.traverseMounts)
        behavior |= ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS;
    checkArchiveStatus(input.get(), archive_read_disk_set_behavior(input.get(), behavior),
                       "setting disk-reader behavior");
    checkArchiveStatus(input.get(), archive_read_disk_open(input.get(),
                                                           options.input.c_str()),
                       "opening input directory");

    InputTree tree;
    std::string rootPath;
    bool first = true;
    struct archive_entry* entry = nullptr;
    for (;;) {
        const int status = archive_read_next_header(input.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        checkArchiveStatus(input.get(), status, "walking input directory");
        const char* rawPointer = archive_entry_pathname(entry);
        if (rawPointer == nullptr)
            throw Error("disk entry has no pathname");
        std::string raw = rawPointer;
        const bool isDirectory = archive_entry_filetype(entry) == AE_IFDIR;
        while (raw.size() > 1 && raw.back() == '/')
            raw.pop_back();

        if (first) {
            first = false;
            if (!isDirectory)
                throw Error("directory reader root is not a directory");
            rootPath = raw;
        }

        std::string relative;
        if (raw == rootPath) {
            relative.clear();
        } else if (raw.size() > rootPath.size() &&
                   raw.compare(0, rootPath.size(), rootPath) == 0 &&
                   raw[rootPath.size()] == '/') {
            relative = raw.substr(rootPath.size() + 1);
        } else {
            throw Error("disk reader returned a pathname outside the input root: " +
                        raw);
        }
        relative = normalizeArchivePath(relative, isDirectory, 0);
        Node node = nodeFromEntry(input.get(), entry, relative, spool, false);
        tree.ignoredMetadata += node.ignoredMetadata;
        if (relative.empty()) {
            tree.rootAttr = node.attr;
            tree.hasRootAttr = true;
        } else {
            tree.nodes.push_back(std::move(node));
        }
        if (isDirectory && archive_read_disk_can_descend(input.get()))
            checkArchiveStatus(input.get(), archive_read_disk_descend(input.get()),
                               "descending input directory");
    }
    checkArchiveStatus(input.get(), archive_read_close(input.get()),
                       "closing input directory");

    for (const Node& node : tree.nodes) {
        if (node.kind != NodeKind::file || !node.payload)
            continue;
        const std::filesystem::path source =
            std::filesystem::path(options.input) / node.path;
        struct stat current{};
        if (::lstat(source.c_str(), &current) != 0)
            throw Error(errnoText("recheck input file", source.string()));
        if (!S_ISREG(current.st_mode) ||
            static_cast<int64_t>(current.st_dev) != node.identity.device ||
            static_cast<int64_t>(current.st_ino) != node.identity.inode ||
            current.st_size < 0 ||
            static_cast<uint64_t>(current.st_size) != node.payload->size ||
            static_cast<uint16_t>(current.st_mode) != node.attr.mode ||
            static_cast<uint64_t>(current.st_uid) != node.attr.uid ||
            static_cast<uint64_t>(current.st_gid) != node.attr.gid) {
            throw Error("input file changed while being read: " + node.path);
        }
    }
    return tree;
}

void applyRootOptions(InputTree& tree, const Options& options)
{
    if (!tree.hasRootAttr)
        tree.rootAttr = AttrKey{static_cast<uint16_t>(S_IFDIR | 0755), 0, 0};
    if (options.rootMode)
        tree.rootAttr.mode = static_cast<uint16_t>(S_IFDIR | *options.rootMode);
    if (options.rootUid)
        tree.rootAttr.uid = *options.rootUid;
    if (options.rootGid)
        tree.rootAttr.gid = *options.rootGid;
}

void normalizeTree(InputTree& tree, const Options& options, const Spool& spool)
{
    std::map<std::string, size_t> byPath;
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        const Node& node = tree.nodes[i];
        if (node.path.empty())
            throw Error("internal non-root entry has an empty path");
        if (!byPath.emplace(node.path, i).second)
            throw Error("duplicate normalized pathname: " + node.path);
    }

    if (options.synthesizeDirs) {
        std::set<std::string> missing;
        for (const Node& node : tree.nodes) {
            std::string parent = node.parent;
            while (!parent.empty() && !byPath.contains(parent)) {
                missing.insert(parent);
                parent = splitParent(parent).first;
            }
        }
        for (const std::string& path : missing) {
            Node node;
            node.path = path;
            std::tie(node.parent, node.name) = splitParent(path);
            node.kind = NodeKind::directory;
            node.attr = tree.rootAttr;
            byPath.emplace(path, tree.nodes.size());
            tree.nodes.push_back(std::move(node));
            if (options.verbose)
                std::cerr << "makefemtofs: synthesized directory " << path << '\n';
        }
    }

    byPath.clear();
    for (size_t i = 0; i < tree.nodes.size(); ++i)
        byPath.emplace(tree.nodes[i].path, i);
    for (const Node& node : tree.nodes) {
        if (node.parent.empty())
            continue;
        const auto parent = byPath.find(node.parent);
        if (parent == byPath.end())
            throw Error("missing parent directory for: " + node.path);
        if (tree.nodes[parent->second].kind != NodeKind::directory)
            throw Error("parent is not a directory for: " + node.path);
    }

    std::sort(tree.nodes.begin(), tree.nodes.end(), [](const Node& lhs,
                                                       const Node& rhs) {
        return byteLess(lhs.path, rhs.path);
    });
    if (tree.nodes.size() >= femtofs::kMaxDirectoryEntries)
        throw Error("image has 2^15 or more non-root objects");

    byPath.clear();
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        tree.nodes[i].objectIndex = static_cast<uint32_t>(i);
        tree.nodes[i].canonicalNode = static_cast<uint32_t>(i);
        byPath.emplace(tree.nodes[i].path, i);
    }

    struct DisjointSet {
        explicit DisjointSet(size_t size) : parent(size), rank(size, 0)
        {
            for (size_t i = 0; i < size; ++i)
                parent[i] = i;
        }
        size_t find(size_t value)
        {
            if (parent[value] != value)
                parent[value] = find(parent[value]);
            return parent[value];
        }
        void unite(size_t a, size_t b)
        {
            a = find(a);
            b = find(b);
            if (a == b)
                return;
            if (rank[a] < rank[b])
                std::swap(a, b);
            parent[b] = a;
            if (rank[a] == rank[b])
                ++rank[a];
        }
        std::vector<size_t> parent;
        std::vector<uint8_t> rank;
    } groups(tree.nodes.size());

    std::map<SourceIdentity, size_t> firstIdentity;
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        Node& node = tree.nodes[i];
        if (!node.explicitHardlink.empty()) {
            const auto target = byPath.find(node.explicitHardlink);
            if (target == byPath.end())
                throw Error("hardlink target does not exist: " + node.explicitHardlink);
            if (tree.nodes[target->second].kind != NodeKind::file)
                throw Error("hardlink target is not a regular file: " +
                            node.explicitHardlink);
            groups.unite(i, target->second);
        }
        if (node.identity.valid) {
            const auto [it, inserted] = firstIdentity.emplace(node.identity, i);
            if (!inserted && (node.kind != NodeKind::file ||
                              tree.nodes[it->second].kind != NodeKind::file)) {
                throw Error("multiple source paths reference the same non-regular "
                            "inode: " + node.path);
            }
            if (!inserted)
                groups.unite(i, it->second);
        }
    }

    std::map<size_t, std::vector<size_t>> members;
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        if (tree.nodes[i].kind == NodeKind::file ||
            !tree.nodes[i].explicitHardlink.empty()) {
            members[groups.find(i)].push_back(i);
        }
    }

    for (auto& [unusedRoot, group] : members) {
        (void)unusedRoot;
        if (group.size() == 1 && tree.nodes[group.front()].explicitHardlink.empty())
            continue;
        std::sort(group.begin(), group.end(), [&](size_t lhs, size_t rhs) {
            return byteLess(tree.nodes[lhs].path, tree.nodes[rhs].path);
        });
        const size_t canonical = group.front();
        const AttrKey attrs = tree.nodes[canonical].attr;
        const BlobSource* payload = nullptr;
        for (const size_t index : group) {
            Node& node = tree.nodes[index];
            if (node.kind != NodeKind::file)
                throw Error("hardlink group contains a non-regular object: " + node.path);
            if (!(node.attr == attrs))
                throw Error("hardlink group has inconsistent attributes: " + node.path);
            if (node.payload && node.payload->size != 0) {
                if (payload == nullptr)
                    payload = &*node.payload;
                else if (!equalSources(spool, *payload, *node.payload))
                    throw Error("hardlink group has inconsistent payloads: " + node.path);
            }
        }
        BlobSource selected;
        if (payload != nullptr) {
            selected = *payload;
        } else {
            const auto empty = std::find_if(group.begin(), group.end(), [&](size_t index) {
                return tree.nodes[index].payload.has_value();
            });
            if (empty != group.end())
                selected = *tree.nodes[*empty].payload;
        }
        tree.nodes[canonical].kind = NodeKind::file;
        tree.nodes[canonical].payload = selected;
        tree.nodes[canonical].explicitHardlink.clear();
        tree.nodes[canonical].canonicalNode = static_cast<uint32_t>(canonical);
        for (const size_t index : group) {
            if (index == canonical)
                continue;
            Node& node = tree.nodes[index];
            node.kind = NodeKind::hardlink;
            node.payload.reset();
            node.explicitHardlink.clear();
            node.canonicalNode = static_cast<uint32_t>(canonical);
        }
    }

    for (Node& node : tree.nodes) {
        if (node.kind == NodeKind::file && !node.payload)
            node.payload = BlobSource{};
        if (node.kind == NodeKind::hardlink) {
            const Node& target = tree.nodes[node.canonicalNode];
            if (target.kind != NodeKind::file || !target.payload)
                throw Error("hardlink did not resolve to a canonical file: " + node.path);
            node.attr = target.attr;
        }
    }
}

[[nodiscard]] bool hasMode(uint16_t mode, uint16_t bits)
{
    return (mode & bits) == bits;
}

[[nodiscard]] bool pathDirectoriesAllow(const InputTree& tree,
                                        const std::map<std::string, size_t>& byPath,
                                        const std::string& parent,
                                        uint16_t required)
{
    if (!hasMode(tree.rootAttr.mode, required))
        return false;
    if (parent.empty())
        return true;
    std::string current;
    size_t start = 0;
    while (start < parent.size()) {
        const size_t slash = parent.find('/', start);
        const size_t end = slash == std::string::npos ? parent.size() : slash;
        if (!current.empty())
            current.push_back('/');
        current.append(parent, start, end - start);
        const auto found = byPath.find(current);
        if (found == byPath.end() ||
            tree.nodes[found->second].kind != NodeKind::directory ||
            !hasMode(tree.nodes[found->second].attr.mode, required)) {
            return false;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

enum class BlobDomain : uint8_t { shared, symlink };

struct BlobClass {
    BlobDomain domain = BlobDomain::shared;
    BlobSource source;
    bool publicPart = false;
    uint32_t storedSize = 0;
    uint32_t imageOff = 0;
};

struct BlobKey {
    BlobDomain domain;
    uint32_t size;
    uint64_t digest;

    [[nodiscard]] auto tie() const { return std::tie(domain, size, digest); }
    [[nodiscard]] bool operator<(const BlobKey& other) const
    {
        return tie() < other.tie();
    }
};

[[nodiscard]] uint32_t internBlob(std::vector<BlobClass>& blobs,
                                  std::map<BlobKey, std::vector<uint32_t>>& candidates,
                                  const Spool& spool, BlobDomain domain,
                                  BlobSource source, bool isPublic)
{
    const BlobKey key{domain, source.size, source.digest};
    auto& bucket = candidates[key];
    for (const uint32_t index : bucket) {
        if (equalSources(spool, blobs[index].source, source)) {
            blobs[index].publicPart = blobs[index].publicPart || isPublic;
            return index;
        }
    }
    const uint64_t stored = (static_cast<uint64_t>(source.size) + 4u) & ~3ull;
    if (stored > UINT32_MAX)
        throw Error("encoded blob size exceeds uint32_t");
    const uint32_t index = static_cast<uint32_t>(blobs.size());
    blobs.push_back(BlobClass{domain, std::move(source), isPublic,
                              static_cast<uint32_t>(stored), 0});
    bucket.push_back(index);
    return index;
}

[[nodiscard]] std::vector<BlobClass> classifyBlobs(InputTree& tree,
                                                   const Spool& spool)
{
    std::map<std::string, size_t> byPath;
    for (size_t i = 0; i < tree.nodes.size(); ++i)
        byPath.emplace(tree.nodes[i].path, i);

    std::vector<bool> canonicalPublic(tree.nodes.size(), false);
    for (const Node& occurrence : tree.nodes) {
        if (occurrence.kind != NodeKind::file &&
            occurrence.kind != NodeKind::hardlink) {
            continue;
        }
        const Node& canonical = tree.nodes[occurrence.canonicalNode];
        if (hasMode(canonical.attr.mode, S_IROTH) &&
            pathDirectoriesAllow(tree, byPath, occurrence.parent, S_IXOTH)) {
            canonicalPublic[occurrence.canonicalNode] = true;
        }
    }

    std::vector<BlobClass> blobs;
    std::map<BlobKey, std::vector<uint32_t>> candidates;
    for (Node& node : tree.nodes) {
        const bool namePublic = pathDirectoriesAllow(
            tree, byPath, node.parent, static_cast<uint16_t>(S_IROTH | S_IXOTH));
        node.nameBlob = internBlob(blobs, candidates, spool, BlobDomain::shared,
                                   inlineSource(node.name), namePublic);

        if (node.kind == NodeKind::file || node.kind == NodeKind::hardlink) {
            const Node& canonical = tree.nodes[node.canonicalNode];
            node.contentBlob = internBlob(blobs, candidates, spool,
                BlobDomain::shared, *canonical.payload,
                canonicalPublic[node.canonicalNode]);
        } else if (node.kind == NodeKind::symlink) {
            node.contentBlob = internBlob(blobs, candidates, spool,
                BlobDomain::symlink, inlineSource(node.symlinkTarget), false);
        }
    }
    return blobs;
}

using HashChoice = femtofs::hashplan::Choice;

[[nodiscard]] HashChoice bestForTable(const std::vector<std::string>& names,
                                      uint32_t tableSize)
{
    return femtofs::hashplan::bestSingleForTable(
        names, femtofs::kSmallPrimes, tableSize);
}

[[nodiscard]] HashChoice chooseHash(const std::vector<std::string>& names)
{
    return femtofs::hashplan::chooseSingle(names, femtofs::kSmallPrimes);
}

[[nodiscard]] HashChoice chooseHashDual(const std::vector<std::string>& names,
                                        uint32_t hash2Base)
{
    return femtofs::hashplan::chooseDual(
        names, femtofs::kSmallPrimes, hash2Base);
}

struct Bucket {
    uint32_t nodeIndex = 0;
    uint32_t next = 0;
};

struct DirectoryPlan {
    std::string path;
    std::optional<uint32_t> nodeIndex;
    std::vector<uint32_t> children;
    HashChoice hash;
    uint32_t first = 0;
    std::vector<std::optional<Bucket>> buckets;
};

struct AttrPlan {
    AttrKey key;
    std::vector<std::optional<uint32_t>> references;
    uint16_t cellIndex = 0;
};

struct ImagePlan {
    AttrKey rootAttr;
    std::vector<Node> nodes;
    std::vector<BlobClass> blobs;
    std::vector<DirectoryPlan> directories;
    std::vector<size_t> directoryByNode;
    std::vector<AttrPlan> attrs;
    std::map<uint32_t, size_t> attrAtCell;
    std::vector<uint8_t> metadata;
    std::array<uint8_t, kHeaderSize> header{};
    uint32_t publicOff = 0;
    uint32_t privateOff = 0;
    uint32_t imageSize = 0;
    uint32_t cellCount = 0;
    uint32_t hash2Base = 0;
};

[[nodiscard]] std::vector<DirectoryPlan> makeDirectoryPlans(
    const std::vector<Node>& nodes, bool minimumTables)
{
    std::map<std::string, std::vector<uint32_t>> children;
    children[""];
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        children[nodes[i].parent].push_back(i);
        if (nodes[i].kind == NodeKind::directory)
            children[nodes[i].path];
    }

    std::vector<DirectoryPlan> plans;
    plans.reserve(children.size());
    for (auto& [path, members] : children) {
        if (members.size() >= femtofs::kMaxDirectoryEntries)
            throw Error("directory has 2^15 or more entries: " + path);
        std::sort(members.begin(), members.end(), [&](uint32_t lhs, uint32_t rhs) {
            return byteLess(nodes[lhs].name, nodes[rhs].name);
        });
        std::vector<std::string> names;
        names.reserve(members.size());
        for (const uint32_t member : members)
            names.push_back(nodes[member].name);

        DirectoryPlan plan;
        plan.path = path;
        plan.children = std::move(members);
        if (!path.empty()) {
            const auto found = std::lower_bound(nodes.begin(), nodes.end(), path,
                [](const Node& node, const std::string& value) {
                    return byteLess(node.path, value);
                });
            if (found == nodes.end() || found->path != path ||
                found->kind != NodeKind::directory) {
                throw Error("internal directory object is missing: " + path);
            }
            plan.nodeIndex = found->objectIndex;
        }
        plan.hash = minimumTables ? bestForTable(names,
            static_cast<uint32_t>(names.size())) : chooseHash(names);
        plans.push_back(std::move(plan));
    }
    std::sort(plans.begin(), plans.end(), [](const DirectoryPlan& lhs,
                                             const DirectoryPlan& rhs) {
        if (lhs.path.empty() != rhs.path.empty())
            return lhs.path.empty();
        return byteLess(lhs.path, rhs.path);
    });
    return plans;
}

[[nodiscard]] std::vector<uint32_t> sampledHash2Primes()
{
    constexpr uint32_t minimum = (1u << 8) + 1u;
    constexpr uint32_t maximum = 1u << 24;
    constexpr size_t sampleCount = 100;
    return femtofs::hashplan::samplePrimes(
        minimum, maximum, sampleCount, 0x0F5F2026u);
}

[[nodiscard]] uint32_t applyMixedHashPolicy(
    std::vector<DirectoryPlan>& directories, const std::vector<Node>& nodes)
{
    std::vector<size_t> hard;
    for (size_t i = 0; i < directories.size(); ++i) {
        if (directories[i].hash.maxChain > 2)
            hard.push_back(i);
    }
    if (hard.empty())
        return 0;

    uint32_t bestBase = 0;
    uint32_t bestMaximum = UINT32_MAX;
    uint64_t bestSquares = UINT64_MAX;
    uint64_t bestBuckets = UINT64_MAX;
    long double bestMissCost = std::numeric_limits<long double>::infinity();
    std::vector<HashChoice> bestChoices;

    for (const uint32_t candidate : sampledHash2Primes()) {
        std::vector<HashChoice> choices;
        choices.reserve(hard.size());
        for (const size_t index : hard) {
            std::vector<std::string> names;
            names.reserve(directories[index].children.size());
            for (const uint32_t child : directories[index].children)
                names.push_back(nodes[child].name);
            HashChoice dual = chooseHashDual(names, candidate);
            choices.push_back(femtofs::hashplan::improvesQuality(
                dual, directories[index].hash) ? dual : directories[index].hash);
        }

        uint32_t maximum = 0;
        uint64_t squares = 0;
        uint64_t buckets = 0;
        long double missCost = 0;
        size_t hardCursor = 0;
        for (size_t i = 0; i < directories.size(); ++i) {
            const bool isHard = hardCursor < hard.size() && hard[hardCursor] == i;
            const HashChoice& choice = isHard ? choices[hardCursor++]
                                              : directories[i].hash;
            maximum = std::max(maximum, choice.maxChain);
            squares += choice.sumSquares;
            buckets += choice.tableSize;
            const uint64_t entries = directories[i].children.size();
            if (choice.tableSize != 0) {
                missCost += static_cast<long double>(entries * entries) /
                    choice.tableSize * (choice.dual ? 2.0L : 1.0L);
            }
        }

        const bool better = maximum < bestMaximum ||
            (maximum == bestMaximum && squares < bestSquares) ||
            (maximum == bestMaximum && squares == bestSquares &&
             buckets < bestBuckets) ||
            (maximum == bestMaximum && squares == bestSquares &&
             buckets == bestBuckets && missCost < bestMissCost) ||
            (maximum == bestMaximum && squares == bestSquares &&
             buckets == bestBuckets && missCost == bestMissCost &&
             candidate < bestBase);
        if (better) {
            bestBase = candidate;
            bestMaximum = maximum;
            bestSquares = squares;
            bestBuckets = buckets;
            bestMissCost = missCost;
            bestChoices = std::move(choices);
        }
    }

    for (size_t i = 0; i < hard.size(); ++i)
        directories[hard[i]].hash = bestChoices[i];
    return std::any_of(bestChoices.begin(), bestChoices.end(),
        [](const HashChoice& choice) { return choice.dual; }) ? bestBase : 0;
}

[[nodiscard]] std::vector<AttrPlan> makeAttributePlans(
    const AttrKey& root, const std::vector<Node>& nodes)
{
    std::vector<AttrPlan> attrs;
    std::map<AttrKey, size_t> byKey;
    auto add = [&](const AttrKey& key, std::optional<uint32_t> reference) {
        const auto [found, inserted] = byKey.emplace(key, attrs.size());
        if (inserted)
            attrs.push_back(AttrPlan{key, {}, 0});
        attrs[found->second].references.push_back(reference);
    };
    add(root, std::nullopt);
    for (const Node& node : nodes) {
        if (node.kind != NodeKind::hardlink)
            add(node.attr, node.objectIndex);
    }
    return attrs;
}

[[nodiscard]] uint64_t plannedCellCount(const std::vector<Node>& nodes,
                                        const std::vector<DirectoryPlan>& dirs,
                                        const std::vector<AttrPlan>& attrs)
{
    uint64_t buckets = 0;
    uint64_t empty = 0;
    for (const DirectoryPlan& dir : dirs) {
        buckets += dir.hash.tableSize;
        empty += dir.hash.tableSize - dir.children.size();
    }
    return nodes.size() + buckets +
           (attrs.size() > empty ? attrs.size() - empty : 0);
}

void fitDirectoryPlans(std::vector<DirectoryPlan>& directories,
                       const std::vector<Node>& nodes,
                       const std::vector<AttrPlan>& attrs,
                       uint32_t& hash2Base)
{
    if (plannedCellCount(nodes, directories, attrs) <= femtofs::kMaxCellCount)
        return;

    const std::vector<DirectoryPlan> minimum = makeDirectoryPlans(nodes, true);
    if (plannedCellCount(nodes, minimum, attrs) > femtofs::kMaxCellCount) {
        throw Error("objects, minimum hash tables, and attributes exceed "
                    "metadata limit");
    }

    std::vector<std::pair<uint32_t, size_t>> candidates;
    uint64_t buckets = 0;
    uint64_t empty = 0;
    for (size_t i = 0; i < directories.size(); ++i) {
        buckets += directories[i].hash.tableSize;
        empty += directories[i].hash.tableSize - directories[i].children.size();
        const uint32_t saving = directories[i].hash.tableSize -
                                minimum[i].hash.tableSize;
        if (saving != 0)
            candidates.emplace_back(saving, i);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs,
                                                        const auto& rhs) {
        return lhs.first > rhs.first ||
               (lhs.first == rhs.first && lhs.second < rhs.second);
    });

    uint64_t cells = plannedCellCount(nodes, directories, attrs);
    for (const auto& [saving, index] : candidates) {
        directories[index].hash = minimum[index].hash;
        buckets -= saving;
        empty -= saving;
        cells = nodes.size() + buckets +
                (attrs.size() > empty ? attrs.size() - empty : 0);
        if (cells <= femtofs::kMaxCellCount)
            break;
    }

    if (std::none_of(directories.begin(), directories.end(),
                     [](const DirectoryPlan& directory) {
                         return directory.hash.dual;
                     })) {
        hash2Base = 0;
    }
}

void constructBuckets(std::vector<DirectoryPlan>& directories,
                      const std::vector<Node>& nodes, uint32_t hash2Base)
{
    uint32_t first = static_cast<uint32_t>(nodes.size());
    for (DirectoryPlan& directory : directories) {
        directory.first = directory.hash.tableSize == 0 ? 0 : first;
        first += directory.hash.tableSize;
        directory.buckets.assign(directory.hash.tableSize, std::nullopt);
        if (directory.children.empty())
            continue;

        std::map<uint32_t, std::vector<uint32_t>> groups;
        std::vector<uint32_t> loads(directory.hash.tableSize, 0);
        for (const uint32_t child : directory.children) {
            const uint32_t h1 = femtofs::hashName(
                nodes[child].name,
                femtofs::kSmallPrimes[directory.hash.primeIndex],
                directory.hash.tableSize);
            uint32_t anchor = h1;
            if (directory.hash.dual) {
                const uint32_t h2 = femtofs::hashName(
                    nodes[child].name, hash2Base, directory.hash.tableSize);
                if (loads[h2] < loads[h1] ||
                    (loads[h2] == loads[h1] && h2 < h1)) {
                    anchor = h2;
                }
            }
            ++loads[anchor];
            groups[anchor].push_back(child);
        }

        std::set<uint32_t> reserved;
        for (const auto& [anchor, unused] : groups) {
            (void)unused;
            reserved.insert(anchor);
        }
        std::vector<uint32_t> overflow;
        for (uint32_t i = 0; i < directory.hash.tableSize; ++i) {
            if (!reserved.contains(i))
                overflow.push_back(i);
        }
        size_t nextOverflow = 0;
        for (auto& [anchor, members] : groups) {
            std::sort(members.begin(), members.end(), [&](uint32_t lhs, uint32_t rhs) {
                return byteLess(nodes[lhs].name, nodes[rhs].name);
            });
            uint32_t position = anchor;
            for (size_t i = 0; i < members.size(); ++i) {
                if (i != 0) {
                    if (nextOverflow == overflow.size())
                        throw Error("internal hash-chain placement exhausted buckets");
                    position = overflow[nextOverflow++];
                }
                const uint32_t next = i + 1 == members.size()
                    ? directory.hash.tableSize
                    : (nextOverflow == overflow.size()
                        ? UINT32_MAX : overflow[nextOverflow]);
                if (next == UINT32_MAX)
                    throw Error("internal hash-chain placement cannot reserve next cell");
                directory.buckets[position] = Bucket{members[i], next};
            }
        }
    }
}

void placeAttributes(ImagePlan& plan)
{
    auto pageForReference = [](std::optional<uint32_t> reference) {
        const uint32_t byteOffset = reference ?
            kHeaderSize + *reference * kCellSize : 0;
        return byteOffset / kPageSize;
    };
    auto pageForCell = [](uint32_t cell) {
        return (kHeaderSize + cell * kCellSize) / kPageSize;
    };

    std::set<uint32_t> emptyBuckets;
    std::map<uint32_t, std::set<uint32_t>> emptyBucketsByPage;
    for (const DirectoryPlan& directory : plan.directories) {
        for (uint32_t i = 0; i < directory.hash.tableSize; ++i) {
            if (!directory.buckets[i]) {
                const uint32_t cell = directory.first + i;
                emptyBuckets.insert(cell);
                emptyBucketsByPage[pageForCell(cell)].insert(cell);
            }
        }
    }

    uint32_t nextCell = static_cast<uint32_t>(plan.nodes.size());
    for (const DirectoryPlan& directory : plan.directories)
        nextCell += directory.hash.tableSize;

    for (size_t attrIndex = 0; attrIndex < plan.attrs.size(); ++attrIndex) {
        AttrPlan& attr = plan.attrs[attrIndex];
        std::optional<uint32_t> selected;
        for (const std::optional<uint32_t> reference : attr.references) {
            const uint32_t page = pageForReference(reference);
            const auto found = emptyBucketsByPage.find(page);
            if (found != emptyBucketsByPage.end()) {
                selected = *found->second.begin();
                break;
            }
        }
        if (!selected && !emptyBuckets.empty())
            selected = *emptyBuckets.begin();
        if (selected) {
            emptyBuckets.erase(*selected);
            const uint32_t page = pageForCell(*selected);
            auto pageCells = emptyBucketsByPage.find(page);
            pageCells->second.erase(*selected);
            if (pageCells->second.empty())
                emptyBucketsByPage.erase(pageCells);
        } else {
            selected = nextCell++;
        }
        if (*selected > std::numeric_limits<uint16_t>::max())
            throw Error("attribute index exceeds uint16_t");
        attr.cellIndex = static_cast<uint16_t>(*selected);
        plan.attrAtCell.emplace(*selected, attrIndex);
        for (const std::optional<uint32_t> reference : attr.references) {
            if (reference)
                plan.nodes[*reference].attrIndex = attr.cellIndex;
        }
    }
    plan.cellCount = nextCell;
    if (plan.cellCount == 0 || plan.cellCount > femtofs::kMaxCellCount)
        throw Error("metadata table exceeds the <2^16 cell limit");
}

struct Hole {
    uint32_t offset = 0;
    uint32_t size = 0;
};

[[nodiscard]] uint32_t checkedAlignUp(uint64_t value, uint32_t alignment,
                                      std::string_view what)
{
    const uint64_t aligned = (value + alignment - 1u) &
                             ~static_cast<uint64_t>(alignment - 1u);
    if (aligned >= (1ull << 32))
        throw Error(std::string(what) + " exceeds uint32_t after alignment");
    return static_cast<uint32_t>(aligned);
}

[[nodiscard]] uint32_t packPart(std::vector<BlobClass>& blobs,
                                const std::vector<uint32_t>& members,
                                uint32_t partStart, const Spool& spool)
{
    std::vector<uint32_t> order = members;
    std::sort(order.begin(), order.end(), [&](uint32_t lhs, uint32_t rhs) {
        const BlobClass& a = blobs[lhs];
        const BlobClass& b = blobs[rhs];
        if (a.storedSize != b.storedSize)
            return a.storedSize > b.storedSize;
        if (a.source.digest != b.source.digest)
            return a.source.digest < b.source.digest;
        const int contentOrder = compareSources(spool, a.source, b.source);
        if (contentOrder != 0)
            return contentOrder < 0;
        if (a.domain != b.domain)
            return a.domain < b.domain;
        return lhs < rhs;
    });

    uint32_t tail = 0;
    std::vector<Hole> holes;
    for (const uint32_t index : order) {
        BlobClass& blob = blobs[index];
        auto best = holes.end();
        for (auto it = holes.begin(); it != holes.end(); ++it) {
            if (it->size < blob.storedSize)
                continue;
            if (best == holes.end() || it->size < best->size ||
                (it->size == best->size && it->offset < best->offset)) {
                best = it;
            }
        }
        uint32_t relative = 0;
        if (best != holes.end()) {
            relative = best->offset;
            best->offset += blob.storedSize;
            best->size -= blob.storedSize;
            if (best->size == 0)
                holes.erase(best);
        } else {
            if (blob.storedSize >= kPageSize) {
                const uint32_t aligned = checkedAlignUp(tail, kPageSize,
                                                        "content tail");
                if (aligned > tail)
                    holes.push_back(Hole{tail, aligned - tail});
                tail = aligned;
            } else if (tail / kPageSize !=
                       (tail + blob.storedSize - 1u) / kPageSize) {
                const uint32_t aligned = checkedAlignUp(tail, kPageSize,
                                                        "content tail");
                holes.push_back(Hole{tail, aligned - tail});
                tail = aligned;
            }
            relative = tail;
            const uint64_t end = static_cast<uint64_t>(tail) + blob.storedSize;
            if (end > UINT32_MAX)
                throw Error("content part exceeds uint32_t");
            tail = static_cast<uint32_t>(end);
        }
        const uint64_t absolute = static_cast<uint64_t>(partStart) + relative;
        if (absolute > UINT32_MAX)
            throw Error("content offset exceeds uint32_t");
        blob.imageOff = static_cast<uint32_t>(absolute);
    }
    return tail;
}

void packContent(ImagePlan& plan, const Spool& spool)
{
    const uint64_t metadataEnd = kHeaderSize +
        static_cast<uint64_t>(plan.cellCount) * kCellSize;
    if (metadataEnd > UINT32_MAX)
        throw Error("metadata end exceeds uint32_t");
    plan.publicOff = checkedAlignUp(metadataEnd, kPageSize, "metadata end");

    std::vector<uint32_t> publicBlobs;
    std::vector<uint32_t> privateBlobs;
    for (uint32_t i = 0; i < plan.blobs.size(); ++i) {
        if (plan.blobs[i].publicPart)
            publicBlobs.push_back(i);
        else
            privateBlobs.push_back(i);
    }
    const uint32_t publicTail = packPart(plan.blobs, publicBlobs,
                                         plan.publicOff, spool);
    const uint64_t privateStart = static_cast<uint64_t>(plan.publicOff) + publicTail;
    if (privateStart > UINT32_MAX)
        throw Error("public content part exceeds uint32_t");
    plan.privateOff = checkedAlignUp(privateStart, kPageSize,
                                     "private content start");
    const uint32_t privateTail = packPart(plan.blobs, privateBlobs,
                                          plan.privateOff, spool);
    const uint64_t end = static_cast<uint64_t>(plan.privateOff) + privateTail;
    if (end > UINT32_MAX)
        throw Error("image content exceeds uint32_t");
    plan.imageSize = checkedAlignUp(end, kPageSize, "image size");
}

[[nodiscard]] DirectoryPlan& rootDirectory(ImagePlan& plan)
{
    const auto found = std::find_if(plan.directories.begin(),
                                    plan.directories.end(),
        [](const DirectoryPlan& directory) { return directory.path.empty(); });
    if (found == plan.directories.end())
        throw Error("internal root directory plan is missing");
    return *found;
}

[[nodiscard]] const DirectoryPlan& directoryForNode(const ImagePlan& plan,
                                                     uint32_t nodeIndex)
{
    if (nodeIndex >= plan.directoryByNode.size() ||
        plan.directoryByNode[nodeIndex] == std::numeric_limits<size_t>::max()) {
        throw Error("internal directory slice is missing");
    }
    return plan.directories[plan.directoryByNode[nodeIndex]];
}

void encodeObject(std::span<uint8_t> cell, const Node& node,
                  const ImagePlan& plan,
                  const std::map<std::string, uint32_t>& pathIndex)
{
    cell[0] = static_cast<uint8_t>(node.kind);
    if (node.kind != NodeKind::hardlink)
        femtofs::store16(cell, 2, node.attrIndex);

    switch (node.kind) {
        case NodeKind::file: {
            const BlobClass& blob = plan.blobs[node.contentBlob];
            femtofs::store32(cell, 4, blob.imageOff);
            femtofs::store32(cell, 8, blob.source.size);
            break;
        }
        case NodeKind::directory: {
            const DirectoryPlan& directory = directoryForNode(plan, node.objectIndex);
            cell[1] = directory.hash.primeIndex |
                (directory.hash.dual ?
                    femtofs::kHashModeDual << femtofs::kHashModeShift : 0);
            femtofs::store32(cell, 4, directory.first);
            femtofs::store32(cell, 8, directory.hash.tableSize);
            uint16_t parent = femtofs::kParentRoot;
            if (!node.parent.empty()) {
                const auto found = pathIndex.find(node.parent);
                if (found == pathIndex.end())
                    throw Error("internal directory parent is missing");
                parent = static_cast<uint16_t>(found->second);
            }
            femtofs::store32(cell, 12, femtofs::packParentEntries(
                parent, static_cast<uint16_t>(directory.children.size())));
            break;
        }
        case NodeKind::symlink: {
            const BlobClass& blob = plan.blobs[node.contentBlob];
            femtofs::store32(cell, 4, blob.imageOff);
            femtofs::store32(cell, 8, blob.source.size);
            break;
        }
        case NodeKind::hardlink:
            femtofs::store32(cell, 4, plan.nodes[node.canonicalNode].objectIndex);
            break;
        case NodeKind::fifo:
            break;
    }
}

void encodeMetadata(ImagePlan& plan)
{
    const uint64_t metaSize = static_cast<uint64_t>(plan.cellCount) * kCellSize;
    plan.metadata.assign(static_cast<size_t>(metaSize), 0);
    std::map<std::string, uint32_t> pathIndex;
    for (const Node& node : plan.nodes)
        pathIndex.emplace(node.path, node.objectIndex);

    auto cellAt = [&](uint32_t index) {
        return std::span<uint8_t>(plan.metadata.data() + index * kCellSize,
                                  kCellSize);
    };
    for (const Node& node : plan.nodes)
        encodeObject(cellAt(node.objectIndex), node, plan, pathIndex);

    for (const DirectoryPlan& directory : plan.directories) {
        for (uint32_t i = 0; i < directory.hash.tableSize; ++i) {
            const uint32_t absolute = directory.first + i;
            if (plan.attrAtCell.contains(absolute))
                continue;
            std::span<uint8_t> cell = cellAt(absolute);
            if (!directory.buckets[i]) {
                femtofs::store32(cell, 12, directory.hash.tableSize);
                continue;
            }
            const Bucket& bucket = *directory.buckets[i];
            const Node& target = plan.nodes[bucket.nodeIndex];
            cell[0] = static_cast<uint8_t>(target.kind);
            femtofs::store32(cell, 4, target.objectIndex);
            femtofs::store32(cell, 8, plan.blobs[target.nameBlob].imageOff);
            femtofs::store32(cell, 12, bucket.next);
        }
    }

    for (const auto& [cellIndex, attrIndex] : plan.attrAtCell) {
        const AttrPlan& attr = plan.attrs[attrIndex];
        std::span<uint8_t> cell = cellAt(cellIndex);
        cell[0] = femtofs::kTypeAttr;
        femtofs::store16(cell, 2, attr.key.mode);
        femtofs::store32(cell, 4, attr.key.uid);
        femtofs::store32(cell, 8, attr.key.gid);
    }
}

void fillRandomUuid(std::array<uint8_t, 16>& uuid)
{
#if defined(__APPLE__) || defined(__FreeBSD__)
    ::arc4random_buf(uuid.data(), uuid.size());
#elif defined(__linux__)
    size_t done = 0;
    while (done < uuid.size()) {
        const ssize_t count = ::getrandom(uuid.data() + done, uuid.size() - done, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw Error(errnoText("getrandom"));
        }
        done += static_cast<size_t>(count);
    }
#else
    std::random_device random;
    for (uint8_t& byte : uuid)
        byte = static_cast<uint8_t>(random());
#endif
}

void encodeHeader(ImagePlan& plan, const Options& options)
{
    plan.header.fill(0);
    plan.header[0] = '0';
    plan.header[1] = 'F';
    plan.header[2] = 'S';
    plan.header[4] = 0;
    plan.header[5] = 4;
    femtofs::store16(plan.header, 6, femtofs::kVersion);
    femtofs::store32(plan.header, 12, femtofs::fnv1(plan.metadata));

    std::array<uint8_t, 16> uuid{};
    if (options.uuid)
        uuid = *options.uuid;
    else
        fillRandomUuid(uuid);
    std::copy(uuid.begin(), uuid.end(), plan.header.begin() + 16);

    femtofs::store32(plan.header, 32, plan.imageSize);
    femtofs::store32(plan.header, 36, plan.cellCount);
    femtofs::store32(plan.header, 40, plan.publicOff);
    femtofs::store32(plan.header, 44, plan.privateOff);
    femtofs::store32(plan.header, 48,
                     static_cast<uint32_t>(plan.metadata.size()));

    const DirectoryPlan& root = rootDirectory(plan);
    femtofs::store32(plan.header, 52, root.first);
    femtofs::store32(plan.header, 56, root.hash.tableSize);
    femtofs::store32(plan.header, 60,
                     static_cast<uint32_t>(root.children.size()));
    const auto rootAttr = std::find_if(plan.attrs.begin(), plan.attrs.end(),
        [&](const AttrPlan& attr) { return attr.key == plan.rootAttr; });
    if (rootAttr == plan.attrs.end())
        throw Error("internal root attribute was not placed");
    femtofs::store16(plan.header, 64, rootAttr->cellIndex);
    plan.header[66] = root.hash.primeIndex |
        (root.hash.dual ? femtofs::kHashModeDual << femtofs::kHashModeShift : 0);
    femtofs::store32(plan.header, 68, plan.hash2Base);

    constexpr std::string_view author =
        "Andrea \"Nemesi\" Cocito - blackye at gmail dot com";
    static_assert(author.size() == 49);
    std::copy(author.begin(), author.end(), plan.header.begin() + 72);
}

void validatePlan(const ImagePlan& plan)
{
    if (plan.cellCount == 0 || plan.cellCount >= (1u << 16))
        throw Error("internal validation: invalid metadata cell count");
    if (plan.metadata.size() != static_cast<size_t>(plan.cellCount) * kCellSize)
        throw Error("internal validation: metadata size mismatch");
    if (plan.publicOff % kPageSize || plan.privateOff % kPageSize ||
        plan.imageSize % kPageSize)
        throw Error("internal validation: unaligned image boundary");

    const bool anyDual = std::any_of(plan.directories.begin(),
        plan.directories.end(), [](const DirectoryPlan& directory) {
            return directory.hash.dual;
        });
    if (anyDual) {
        if (plan.hash2Base <= (1u << 8) || plan.hash2Base >= (1u << 24) ||
            !utilities::isPrime(plan.hash2Base)) {
            throw Error("internal validation: invalid dual-hash base");
        }
    } else if (plan.hash2Base != 0) {
        throw Error("internal validation: unused dual-hash base is nonzero");
    }

    std::vector<uint32_t> incoming(plan.nodes.size(), 0);
    for (const DirectoryPlan& directory : plan.directories) {
        if (directory.children.size() >= femtofs::kMaxDirectoryEntries ||
            directory.children.size() > directory.hash.tableSize)
            throw Error("internal validation: invalid directory size");
        uint32_t occupied = 0;
        for (uint32_t position = 0; position < directory.buckets.size(); ++position) {
            if (!directory.buckets[position])
                continue;
            ++occupied;
            const Bucket& bucket = *directory.buckets[position];
            if (bucket.nodeIndex >= plan.nodes.size())
                throw Error("internal validation: bucket target out of range");
            ++incoming[bucket.nodeIndex];
            std::array<uint32_t, 2> anchors{};
            anchors[0] = femtofs::hashName(
                plan.nodes[bucket.nodeIndex].name,
                femtofs::kSmallPrimes[directory.hash.primeIndex],
                directory.hash.tableSize);
            size_t anchorCount = 1;
            if (directory.hash.dual) {
                const uint32_t h2 = femtofs::hashName(
                    plan.nodes[bucket.nodeIndex].name, plan.hash2Base,
                    directory.hash.tableSize);
                if (h2 != anchors[0])
                    anchors[anchorCount++] = h2;
            }
            bool reached = false;
            for (size_t anchorIndex = 0; anchorIndex < anchorCount; ++anchorIndex) {
                uint32_t current = anchors[anchorIndex];
                if (!directory.buckets[current])
                    continue;
                bool terminated = false;
                for (uint32_t steps = 0; steps < directory.hash.tableSize; ++steps) {
                    if (!directory.buckets[current])
                        break;
                    if (current == position)
                        reached = true;
                    const uint32_t next = directory.buckets[current]->next;
                    if (next == directory.hash.tableSize) {
                        terminated = true;
                        break;
                    }
                    if (next >= directory.hash.tableSize)
                        throw Error("internal validation: chain link out of range");
                    current = next;
                }
                if (!terminated)
                    throw Error("internal validation: hash chain does not terminate");
            }
            if (!reached)
                throw Error("internal validation: bucket is unreachable from its hash anchor");
        }
        if (occupied != directory.children.size())
            throw Error("internal validation: occupied bucket count mismatch");
    }
    if (std::any_of(incoming.begin(), incoming.end(),
                    [](uint32_t count) { return count != 1; })) {
        throw Error("internal validation: object does not have exactly one bucket");
    }

    std::vector<std::pair<uint32_t, uint32_t>> intervals;
    intervals.reserve(plan.blobs.size());
    for (const BlobClass& blob : plan.blobs) {
        const uint64_t end = static_cast<uint64_t>(blob.imageOff) + blob.storedSize;
        if (end > plan.imageSize || blob.imageOff < plan.publicOff)
            throw Error("internal validation: blob range is outside image");
        if (blob.publicPart != (blob.imageOff < plan.privateOff))
            throw Error("internal validation: blob visibility part mismatch");
        if (blob.storedSize < kPageSize &&
            blob.imageOff / kPageSize != (end - 1u) / kPageSize)
            throw Error("internal validation: sub-page blob crosses a page");
        if (blob.storedSize >= kPageSize && blob.imageOff % kPageSize)
            throw Error("internal validation: large blob is not page-aligned");
        intervals.emplace_back(blob.imageOff, static_cast<uint32_t>(end));
    }
    std::sort(intervals.begin(), intervals.end());
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].first < intervals[i - 1].second)
            throw Error("internal validation: content intervals overlap");
    }
}

[[nodiscard]] ImagePlan buildPlan(InputTree tree, std::vector<BlobClass> blobs,
                                  const Options& options, const Spool& spool)
{
    ImagePlan plan;
    plan.rootAttr = tree.rootAttr;
    plan.nodes = std::move(tree.nodes);
    plan.blobs = std::move(blobs);
    plan.attrs = makeAttributePlans(plan.rootAttr, plan.nodes);
    plan.directories = makeDirectoryPlans(plan.nodes, false);
    plan.directoryByNode.assign(plan.nodes.size(),
                                std::numeric_limits<size_t>::max());
    for (size_t i = 0; i < plan.directories.size(); ++i) {
        if (plan.directories[i].nodeIndex)
            plan.directoryByNode[*plan.directories[i].nodeIndex] = i;
    }
    plan.hash2Base = applyMixedHashPolicy(plan.directories, plan.nodes);
    fitDirectoryPlans(plan.directories, plan.nodes, plan.attrs, plan.hash2Base);
    constructBuckets(plan.directories, plan.nodes, plan.hash2Base);
    placeAttributes(plan);
    packContent(plan, spool);
    encodeMetadata(plan);
    encodeHeader(plan, options);
    validatePlan(plan);
    return plan;
}

[[nodiscard]] uint32_t fnv1Fd(int fd, uint64_t offset, uint64_t length,
                              bool pageIo)
{
    std::array<uint8_t, 64 * 1024> buffer{};
    uint32_t state = femtofs::kFnv1Init;
    uint64_t done = 0;
    while (done < length) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(
            buffer.size(), length - done));
        readImageAt(fd, std::span<uint8_t>(buffer.data(), count), offset + done,
                    pageIo);
        state = femtofs::fnv1(std::span<const uint8_t>(buffer.data(), count), state);
        done += count;
    }
    return state;
}

void writeSource(int output, uint32_t imageOff, const BlobSource& source,
                 const Spool& spool, bool pageIo)
{
    std::array<uint8_t, 64 * 1024> buffer{};
    for (uint32_t offset = 0; offset < source.size;) {
        const uint32_t count = std::min<uint32_t>(buffer.size(),
                                                  source.size - offset);
        spool.read(source, offset, std::span<uint8_t>(buffer.data(), count));
        writeImageAt(output, std::span<const uint8_t>(buffer.data(), count),
                     static_cast<uint64_t>(imageOff) + offset, pageIo);
        offset += count;
    }
}

void zeroImage(int fd, uint32_t imageSize, bool pageIo)
{
    std::array<uint8_t, kPageSize> zero{};
    uint32_t offset = 0;
    if (pageIo) {
        writeImageAt(fd, zero, 0, true);
        if (::fsync(fd) != 0)
            throw Error(errnoText("flushing invalid device header"));
        offset = kPageSize;
    }
    for (; offset < imageSize; offset += kPageSize)
        writeImageAt(fd, zero, offset, pageIo);
}

void verifyImage(int fd, const ImagePlan& plan, const Spool& spool,
                 const std::string& mode, bool pageIo);

void writeImage(int fd, ImagePlan& plan, const Spool& spool, bool device)
{
    zeroImage(fd, plan.imageSize, device);

    writeImageAt(fd, plan.metadata, kHeaderSize, device);
    for (const BlobClass& blob : plan.blobs)
        writeSource(fd, blob.imageOff, blob.source, spool, device);
    writeImageAt(fd, std::span<const uint8_t>(plan.header).subspan(16), 16,
                 device);
    if (device && ::fsync(fd) != 0)
        throw Error(errnoText("flushing device image data"));

    const uint32_t imageHash = fnv1Fd(fd, 16, plan.imageSize - 16u, device);
    femtofs::store32(plan.header, 8, imageHash);

    std::array<uint8_t, kPageSize> firstPage{};
    readImageAt(fd, firstPage, 0, device);
    std::copy(plan.header.begin(), plan.header.end(), firstPage.begin());
    try {
        writeImageAt(fd, firstPage, 0, device);
        if (::fsync(fd) != 0)
            throw Error(errnoText("flushing completed image"));
    } catch (const Error& error) {
        if (!device)
            throw;
        // A failed commit may already have written a valid header. Observe the
        // provider without another write; readable bytes do not prove durability.
        std::string observed;
        try {
            verifyImage(fd, plan, spool, "full", true);
            observed = "current intended image passes full verification";
        } catch (const std::exception& verificationError) {
            observed = "current image could not be verified: " +
                       std::string(verificationError.what());
        }
        throw Error(std::string(error.what()) + "; " + observed +
                    "; persistence is uncertain");
    }
}

void verifyBlob(int fd, const BlobClass& blob, const Spool& spool, bool pageIo)
{
    std::array<uint8_t, 64 * 1024> expected{};
    std::array<uint8_t, 64 * 1024> actual{};
    for (uint32_t offset = 0; offset < blob.source.size;) {
        const uint32_t count = std::min<uint32_t>(expected.size(),
                                                  blob.source.size - offset);
        spool.read(blob.source, offset,
                   std::span<uint8_t>(expected.data(), count));
        readImageAt(fd, std::span<uint8_t>(actual.data(), count),
                    static_cast<uint64_t>(blob.imageOff) + offset, pageIo);
        if (!std::equal(expected.begin(), expected.begin() + count,
                        actual.begin())) {
            throw Error("output verification found mismatched blob bytes");
        }
        offset += count;
    }
    const uint32_t suffix = blob.storedSize - blob.source.size;
    std::array<uint8_t, 4> zeros{};
    readImageAt(fd, std::span<uint8_t>(actual.data(), suffix),
                static_cast<uint64_t>(blob.imageOff) + blob.source.size,
                pageIo);
    if (!std::equal(actual.begin(), actual.begin() + suffix, zeros.begin()))
        throw Error("output verification found nonzero blob suffix");
}

void verifyExactDedup(const ImagePlan& plan, const Spool& spool)
{
    std::map<BlobKey, std::vector<uint32_t>> candidates;
    std::array<uint8_t, 64 * 1024> buffer{};
    for (uint32_t i = 0; i < plan.blobs.size(); ++i) {
        const BlobClass& blob = plan.blobs[i];
        uint64_t digest = 14695981039346656037ull;
        for (uint32_t offset = 0; offset < blob.source.size;) {
            const uint32_t count = std::min<uint32_t>(buffer.size(),
                                                      blob.source.size - offset);
            spool.read(blob.source, offset,
                       std::span<uint8_t>(buffer.data(), count));
            digest = fnv1a64(std::span<const uint8_t>(buffer.data(), count), digest);
            offset += count;
        }
        if (digest != blob.source.digest)
            throw Error("full verification found an incorrect blob digest");

        const BlobKey key{blob.domain, blob.source.size, digest};
        auto& bucket = candidates[key];
        for (const uint32_t other : bucket) {
            if (equalSources(spool, plan.blobs[other].source, blob.source)) {
                throw Error("full verification found duplicate exact blob classes");
            }
        }
        bucket.push_back(i);
    }

    auto checkReference = [&](uint32_t index, BlobDomain domain,
                              const BlobSource& source,
                              const std::string& path) {
        if (index >= plan.blobs.size() || plan.blobs[index].domain != domain ||
            !equalSources(spool, plan.blobs[index].source, source)) {
            throw Error("full verification found an incorrect blob reference: " + path);
        }
    };

    for (const Node& node : plan.nodes) {
        checkReference(node.nameBlob, BlobDomain::shared,
                       inlineSource(node.name), node.path);
        switch (node.kind) {
            case NodeKind::file:
                if (!node.payload)
                    throw Error("full verification found a file without a source: " +
                                node.path);
                checkReference(node.contentBlob, BlobDomain::shared,
                               *node.payload, node.path);
                break;
            case NodeKind::hardlink:
                if (node.canonicalNode >= plan.nodes.size() ||
                    plan.nodes[node.canonicalNode].kind != NodeKind::file ||
                    node.contentBlob != plan.nodes[node.canonicalNode].contentBlob) {
                    throw Error("full verification found an incorrect hardlink blob: " +
                                node.path);
                }
                break;
            case NodeKind::symlink:
                checkReference(node.contentBlob, BlobDomain::symlink,
                               inlineSource(node.symlinkTarget), node.path);
                break;
            case NodeKind::directory:
            case NodeKind::fifo:
                break;
        }
    }
}

void verifyZeroHoles(int fd, const ImagePlan& plan, bool pageIo)
{
    std::vector<std::pair<uint32_t, uint32_t>> occupied;
    occupied.emplace_back(0, kHeaderSize + static_cast<uint32_t>(plan.metadata.size()));
    for (const BlobClass& blob : plan.blobs)
        occupied.emplace_back(blob.imageOff, blob.imageOff + blob.storedSize);
    std::sort(occupied.begin(), occupied.end());

    std::array<uint8_t, 64 * 1024> buffer{};
    uint32_t cursor = 0;
    for (const auto& [begin, end] : occupied) {
        if (begin > cursor) {
            uint32_t offset = cursor;
            while (offset < begin) {
                const uint32_t count = std::min<uint32_t>(buffer.size(), begin - offset);
                readImageAt(fd, std::span<uint8_t>(buffer.data(), count), offset,
                            pageIo);
                if (std::any_of(buffer.begin(), buffer.begin() + count,
                                [](uint8_t byte) { return byte != 0; })) {
                    throw Error("output verification found nonzero packing hole");
                }
                offset += count;
            }
        }
        cursor = std::max(cursor, end);
    }
    if (cursor < plan.imageSize) {
        uint32_t offset = cursor;
        while (offset < plan.imageSize) {
            const uint32_t count = std::min<uint32_t>(buffer.size(),
                                                       plan.imageSize - offset);
            readImageAt(fd, std::span<uint8_t>(buffer.data(), count), offset,
                        pageIo);
            if (std::any_of(buffer.begin(), buffer.begin() + count,
                            [](uint8_t byte) { return byte != 0; })) {
                throw Error("output verification found nonzero final padding");
            }
            offset += count;
        }
    }
}

void verifyImage(int fd, const ImagePlan& plan, const Spool& spool,
                 const std::string& mode, bool pageIo)
{
    struct stat status{};
    if (::fstat(fd, &status) != 0)
        throw Error(errnoText("fstat output"));
    if (S_ISREG(status.st_mode) &&
        static_cast<uint64_t>(status.st_size) != plan.imageSize) {
        throw Error("output file size differs from header.image_size");
    }

    std::array<uint8_t, kHeaderSize> header{};
    readImageAt(fd, header, 0, pageIo);
    if (header != plan.header)
        throw Error("output header differs from planned header");

    std::vector<uint8_t> metadata(plan.metadata.size());
    readImageAt(fd, metadata, kHeaderSize, pageIo);
    if (metadata != plan.metadata)
        throw Error("output metadata differs from planned metadata");
    if (femtofs::fnv1(metadata) != femtofs::load32(header, 12))
        throw Error("output meta_hash verification failed");
    if (mode == "full" &&
        fnv1Fd(fd, 16, plan.imageSize - 16u, pageIo) !=
            femtofs::load32(header, 8)) {
        throw Error("output image_hash verification failed");
    }
    for (const BlobClass& blob : plan.blobs)
        verifyBlob(fd, blob, spool, pageIo);
    if (mode == "full") {
        verifyExactDedup(plan, spool);
        verifyZeroHoles(fd, plan, pageIo);
    }
}

[[nodiscard]] bool sameFile(const struct stat& lhs, const struct stat& rhs)
{
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
}

[[nodiscard]] bool pathIsInside(const std::filesystem::path& child,
                                const std::filesystem::path& parent)
{
    auto childIt = child.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end();
         ++parentIt, ++childIt) {
        if (childIt == child.end() || *childIt != *parentIt)
            return false;
    }
    return true;
}

void checkOutputAlias(const Options& options, const struct stat& inputStatus)
{
    struct stat outputStatus{};
    if (::lstat(options.output.c_str(), &outputStatus) == 0) {
        if (S_ISLNK(outputStatus.st_mode))
            throw Error("output path is a symlink");
        if (sameFile(inputStatus, outputStatus))
            throw Error("input and output identify the same file");
    } else if (errno != ENOENT) {
        throw Error(errnoText("lstat output", options.output));
    }

    if (S_ISDIR(inputStatus.st_mode)) {
        std::error_code error;
        const std::filesystem::path input =
            std::filesystem::weakly_canonical(options.input, error);
        if (error)
            throw Error("cannot resolve input directory: " + error.message());
        const std::filesystem::path outputPath(options.output);
        const std::filesystem::path parent = outputPath.has_parent_path() ?
            outputPath.parent_path() : std::filesystem::path(".");
        const std::filesystem::path outputParent =
            std::filesystem::weakly_canonical(parent, error);
        if (error)
            throw Error("cannot resolve output directory: " + error.message());
        const std::filesystem::path output =
            (outputParent / outputPath.filename()).lexically_normal();
        if (pathIsInside(output, input))
            throw Error("output is located inside the input directory tree");
    }
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const { return fd_; }
private:
    int fd_;
};

void fsyncParent(const std::filesystem::path& output)
{
    const std::filesystem::path parent = output.has_parent_path() ?
        output.parent_path() : std::filesystem::path(".");
    FileDescriptor directory(::open(parent.c_str(), O_RDONLY));
    if (directory.get() < 0)
        throw Error(errnoText("open output directory", parent.string()));
    if (::fsync(directory.get()) != 0)
        throw Error(errnoText("fsync output directory", parent.string()));
}

void writeRegularOutput(const Options& options, ImagePlan& plan,
                        const Spool& spool, const struct stat& inputStatus)
{
    struct stat existing{};
    const bool exists = ::lstat(options.output.c_str(), &existing) == 0;
    if (!exists && errno != ENOENT)
        throw Error(errnoText("lstat output", options.output));
    if (exists) {
        if (!S_ISREG(existing.st_mode))
            throw Error("existing output is not a regular file");
        if (!options.force)
            throw Error("output exists; use --force to replace it");
    }

    const std::filesystem::path output(options.output);
    const std::filesystem::path parent = output.has_parent_path() ?
        output.parent_path() : std::filesystem::path(".");
    std::string pattern = (parent / ("." + output.filename().string() +
                           ".makefemtofs.XXXXXX")).string();
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');
    const int raw = ::mkstemp(temporary.data());
    if (raw < 0)
        throw Error(errnoText("mkstemp output", pattern));
    FileDescriptor fd(raw);
    bool published = false;
    try {
        writeImage(fd.get(), plan, spool, false);
        FileDescriptor verification(::open(temporary.data(),
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (verification.get() < 0)
            throw Error(errnoText("reopen temporary output", temporary.data()));
        verifyImage(verification.get(), plan, spool, options.verify, false);
        if (::fchmod(fd.get(), 0644) != 0)
            throw Error(errnoText("chmod temporary output", temporary.data()));
        if (::fsync(fd.get()) != 0)
            throw Error(errnoText("fsync temporary output", temporary.data()));

        // This recheck narrows, but cannot eliminate, the TOCTOU window. The
        // documented contract requires an immutable source throughout creation.
        checkOutputAlias(options, inputStatus);
        if (options.force) {
            struct stat current{};
            if (::lstat(options.output.c_str(), &current) == 0) {
                if (!S_ISREG(current.st_mode)) {
                    throw Error(
                        "output changed to a non-regular file before publication");
                }
            } else if (errno != ENOENT) {
                throw Error(errnoText("recheck output", options.output));
            }
            if (::rename(temporary.data(), options.output.c_str()) != 0)
                throw Error(errnoText("rename output", options.output));
        } else {
            if (::link(temporary.data(), options.output.c_str()) != 0)
                throw Error(errnoText("publish output", options.output));
            if (::unlink(temporary.data()) != 0)
                throw Error(errnoText("unlink temporary output", temporary.data()));
        }
        published = true;
        fsyncParent(output);
    } catch (...) {
        if (!published)
            ::unlink(temporary.data());
        throw;
    }
}

#ifdef __FreeBSD__
void verifyDeviceIsUnmounted(const std::string& path)
{
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr)
        throw Error(errnoText("realpath device", path));
    struct statfs* mounts = nullptr;
    const int count = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0)
        throw Error(errnoText("getmntinfo"));
    for (int i = 0; i < count; ++i) {
        if (path == mounts[i].f_mntfromname || resolved ==
            std::string(mounts[i].f_mntfromname)) {
            throw Error("output device is mounted: " + path);
        }
    }
}
#endif

void writeDeviceOutput(const Options& options, ImagePlan& plan,
                       const Spool& spool)
{
#ifndef __FreeBSD__
    (void)options;
    (void)plan;
    (void)spool;
    throw Error("raw-device output is implemented only on FreeBSD");
#else
    verifyDeviceIsUnmounted(options.output);
    FileDescriptor fd(::open(options.output.c_str(), O_RDWR | O_CLOEXEC));
    if (fd.get() < 0)
        throw Error(errnoText("open output device", options.output));
    off_t mediaSize = 0;
    u_int sectorSize = 0;
    if (::ioctl(fd.get(), DIOCGMEDIASIZE, &mediaSize) != 0 ||
        ::ioctl(fd.get(), DIOCGSECTORSIZE, &sectorSize) != 0) {
        throw Error(errnoText("query output device", options.output));
    }
    if (mediaSize < 0 || static_cast<uint64_t>(mediaSize) < plan.imageSize)
        throw Error("output device is smaller than the image");
    if (sectorSize == 0 || kPageSize % sectorSize != 0)
        throw Error("output device sector size does not divide 4096");
    writeImage(fd.get(), plan, spool, true);
    verifyImage(fd.get(), plan, spool, options.verify, true);
#endif
}

void printPlan(const ImagePlan& plan)
{
    size_t publicBlobs = 0;
    size_t dualDirectories = 0;
    uint64_t publicBytes = 0;
    uint64_t privateBytes = 0;
    for (const BlobClass& blob : plan.blobs) {
        if (blob.publicPart) {
            ++publicBlobs;
            publicBytes += blob.storedSize;
        } else {
            privateBytes += blob.storedSize;
        }
    }
    for (const DirectoryPlan& directory : plan.directories)
        dualDirectories += directory.hash.dual;
    std::cout << "objects: " << plan.nodes.size() << '\n'
              << "directories: " << plan.directories.size() << '\n'
              << "dual-hash directories: " << dualDirectories;
    if (dualDirectories != 0)
        std::cout << " (hash2_base " << plan.hash2Base << ')';
    std::cout << '\n'
              << "attributes: " << plan.attrs.size() << '\n'
              << "metadata cells: " << plan.cellCount << '\n'
              << "content blobs: " << plan.blobs.size() << " ("
              << publicBlobs << " public, "
              << plan.blobs.size() - publicBlobs << " private)\n"
              << "stored blob bytes: " << publicBytes << " public, "
              << privateBytes << " private\n"
              << "public_off: " << plan.publicOff << '\n'
              << "private_off: " << plan.privateOff << '\n'
              << "image_size: " << plan.imageSize << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        struct stat inputStatus{};
        if (::lstat(options.input.c_str(), &inputStatus) != 0)
            throw Error(errnoText("lstat input", options.input));
        if (!S_ISDIR(inputStatus.st_mode) && !S_ISREG(inputStatus.st_mode))
            throw Error("input must be a directory or regular archive file");
        if (!options.dryRun)
            checkOutputAlias(options, inputStatus);

        Spool spool(options.tempDir);
        InputTree tree = S_ISDIR(inputStatus.st_mode)
            ? readDirectory(options, spool)
            : readArchive(options, spool);
        if (tree.ignoredMetadata != 0) {
            std::cerr << "makefemtofs: warning: ignored unrepresentable metadata on "
                      << tree.ignoredMetadata << " input entr"
                      << (tree.ignoredMetadata == 1 ? "y" : "ies") << '\n';
        }
        applyRootOptions(tree, options);
        normalizeTree(tree, options, spool);
        std::vector<BlobClass> blobs = classifyBlobs(tree, spool);
        ImagePlan plan = buildPlan(std::move(tree), std::move(blobs), options, spool);

        if (options.verbose || options.dryRun)
            printPlan(plan);
        if (options.dryRun)
            return 0;

        struct stat outputStatus{};
        if (::lstat(options.output.c_str(), &outputStatus) == 0 &&
            (S_ISCHR(outputStatus.st_mode) || S_ISBLK(outputStatus.st_mode))) {
            if (!options.device)
                throw Error("raw device output requires --device");
            writeDeviceOutput(options, plan, spool);
        } else {
            if (options.device)
                throw Error("--device requires an existing block or character device");
            writeRegularOutput(options, plan, spool, inputStatus);
        }
        if (options.verbose)
            std::cout << "wrote and verified " << options.output << '\n';
        return 0;
    } catch (const Error& error) {
        std::cerr << "makefemtofs: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "makefemtofs: unexpected error: " << error.what() << '\n';
        return 1;
    }
}

// END File: programs/makefemtofs.cpp
