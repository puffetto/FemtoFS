// File: units/test-format.cpp
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Verify the shared FemtoFS on-disk layout and primitive encodings.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <femtofs/format.h>

TEST_CASE("version 0x0100 structures have the specified ABI", "[format]")
{
    STATIC_REQUIRE(sizeof(femtofs::HeaderLayout) == 128);
    STATIC_REQUIRE(offsetof(femtofs::HeaderLayout, uuid) == 16);
    STATIC_REQUIRE(offsetof(femtofs::HeaderLayout, imageSize) == 32);
    STATIC_REQUIRE(sizeof(femtofs::ObjectRoleLayout) == 12);
    STATIC_REQUIRE(sizeof(femtofs::ObjectLayout) == 16);
    STATIC_REQUIRE(sizeof(femtofs::AttrLayout) == 16);
}

TEST_CASE("little-endian helpers round trip unaligned fields", "[format]")
{
    std::array<uint8_t, 9> bytes{};
    femtofs::store16(bytes, 1, 0xabcd);
    femtofs::store32(bytes, 3, 0x89abcdef);

    CHECK(bytes[1] == 0xcd);
    CHECK(bytes[2] == 0xab);
    CHECK(bytes[3] == 0xef);
    CHECK(bytes[6] == 0x89);
    CHECK(femtofs::load16(bytes, 1) == 0xabcd);
    CHECK(femtofs::load32(bytes, 3) == 0x89abcdef);
}

TEST_CASE("fixed prime map and full-width hash are stable", "[format]")
{
    STATIC_REQUIRE(femtofs::kSmallPrimes.size() == 53);
    CHECK(femtofs::kSmallPrimes.front() == 3);
    CHECK(femtofs::kSmallPrimes.back() == 251);
    CHECK(femtofs::hashName("collision", 0x00fffffbu, 65521) == 64228);
    CHECK(femtofs::hashName("anything", 251, 1) == 0);
}

TEST_CASE("FNV-1 uses multiply then xor", "[format]")
{
    constexpr std::array<uint8_t, 1> input{'a'};
    const uint32_t expected = femtofs::kFnv1Init * femtofs::kFnv1Prime ^ 'a';
    CHECK(femtofs::fnv1(input) == expected);
    CHECK(femtofs::fnv1(std::span<const uint8_t>{}) == femtofs::kFnv1Init);
}

// END File: units/test-format.cpp
