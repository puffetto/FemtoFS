// File: include/femtofs/format.h
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Shared FemtoFS version 0x0100 on-disk definitions and encoding helpers.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace femtofs {

inline constexpr uint32_t kPageSize = 4096;
inline constexpr uint32_t kHeaderSize = 128;
inline constexpr uint32_t kCellSize = 16;
inline constexpr uint32_t kMaxCellCount = 65535;
inline constexpr uint32_t kMaxTableSize = 65521;
inline constexpr uint32_t kMaxDirectoryEntries = 1u << 15;
inline constexpr uint16_t kVersion = 0x0100;
inline constexpr uint32_t kFnv1Init = 0x811c9dc5u;
inline constexpr uint32_t kFnv1Prime = 0x01000193u;

inline constexpr uint8_t kTypeNull = 0;
inline constexpr uint8_t kTypeFile = 1;
inline constexpr uint8_t kTypeDirectory = 2;
inline constexpr uint8_t kTypeSymlink = 3;
inline constexpr uint8_t kTypeHardlink = 4;
inline constexpr uint8_t kTypeFifo = 5;
inline constexpr uint8_t kTypeAux = 0x80;
inline constexpr uint8_t kTypeAttr = kTypeAux | 1;

inline constexpr uint8_t kHashModeSingle = 0;
inline constexpr uint8_t kHashModeDual = 1;
inline constexpr uint8_t kHashModeShift = 6;
inline constexpr uint8_t kHashPrimeMask = 0x3f;
inline constexpr uint16_t kParentRoot = 0xffff;

inline constexpr std::array<uint32_t, 53> kSmallPrimes{
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167,
    173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251
};

struct HeaderLayout {
    uint8_t magic[4];
    uint8_t format[2];
    uint16_t version;
    uint32_t imageHash;
    uint32_t metaHash;
    uint32_t uuid[4];
    uint32_t imageSize;
    uint32_t cellCount;
    uint32_t publicOff;
    uint32_t privateOff;
    uint32_t metaSize;
    uint32_t rootFirst;
    uint32_t rootSize;
    uint32_t rootReal;
    uint16_t rootAttr;
    uint8_t rootP;
    uint8_t rootPad;
    uint32_t hash2Base;
    uint8_t author[56];
};

union ObjectRoleLayout {
    struct {
        uint32_t dataOff;
        uint32_t size;
        uint32_t realSize;
    } raw;
    struct {
        uint32_t bucketFirst;
        uint32_t bucketCount;
        uint32_t parentN;
    } directory;
    struct {
        uint32_t imageOff;
        uint32_t contentSize;
        uint32_t reservedFlags;
    } filelike;
    struct {
        uint32_t targetIndex;
        uint32_t reserved0;
        uint32_t reserved1;
    } hardlink;
    struct {
        uint32_t objectIndex;
        uint32_t nameOff;
        uint8_t reserved;
        uint8_t nameLen;
        uint16_t nextIndex;
    } bucket;
};

struct ObjectLayout {
    uint8_t type;
    uint8_t hashP;
    uint16_t attrIndex;
    ObjectRoleLayout role;
};

struct AttrLayout {
    uint8_t type;
    uint8_t reserved0;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t extOff;
};

static_assert(sizeof(HeaderLayout) == kHeaderSize);
static_assert(offsetof(HeaderLayout, uuid) == 16);
static_assert(offsetof(HeaderLayout, imageSize) == 32);
static_assert(sizeof(ObjectRoleLayout) == 12);
static_assert(sizeof(ObjectLayout) == kCellSize);
static_assert(offsetof(ObjectLayout, role.bucket.reserved) == 12);
static_assert(offsetof(ObjectLayout, role.bucket.nameLen) == 13);
static_assert(offsetof(ObjectLayout, role.bucket.nextIndex) == 14);
static_assert(sizeof(AttrLayout) == kCellSize);

[[nodiscard]] constexpr uint32_t packParentEntries(uint16_t parent, uint16_t entries)
{
    return static_cast<uint32_t>(parent) |
           (static_cast<uint32_t>(entries & 0x7fffu) << 16u);
}

inline void store16(std::span<uint8_t> bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

inline void store32(std::span<uint8_t> bytes, size_t offset, uint32_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24u);
}

[[nodiscard]] inline uint16_t load16(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8u;
}

[[nodiscard]] inline uint32_t load32(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           static_cast<uint32_t>(bytes[offset + 1]) << 8u |
           static_cast<uint32_t>(bytes[offset + 2]) << 16u |
           static_cast<uint32_t>(bytes[offset + 3]) << 24u;
}

[[nodiscard]] constexpr uint32_t hashName(std::string_view name,
                                          uint32_t base,
                                          uint32_t tableSize)
{
    if (tableSize <= 1)
        return 0;

    uint32_t hash = 0;
    for (const unsigned char byte : name)
        hash = hash * base + byte;
    return hash % tableSize;
}

[[nodiscard]] inline uint32_t fnv1(std::span<const uint8_t> bytes,
                                   uint32_t state = kFnv1Init)
{
    for (const uint8_t byte : bytes) {
        state *= kFnv1Prime;
        state ^= byte;
    }
    return state;
}

} // namespace femtofs

// END File: include/femtofs/format.h
