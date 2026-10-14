// File: units/test-builder-plan.cpp
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Verify builder boundaries, layout invariants, and malformed plan rejection.
#include "makefemtofs-test.h"

using namespace builder_test;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("blob and image arithmetic reject unrepresentable boundaries", "[builder][bounds]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    for (uint32_t size : {4294967291u, 4294967292u, UINT32_MAX}) {
        std::vector<BlobClass> blobs;
        std::map<BlobKey, std::vector<uint32_t>> candidates;
        BlobSource source;
        source.size = size;
        if (size == 4294967291u) {
            REQUIRE(internBlob(blobs, candidates, spool, BlobDomain::shared,
                               source, true) == 0);
            CHECK(blobs[0].storedSize == 4294967292u);
            ImagePlan plan;
            plan.cellCount = 1;
            plan.blobs = std::move(blobs);
            CHECK_THROWS_WITH(packContent(plan, spool),
                              ContainsSubstring("public content part exceeds"));
        } else {
            CHECK_THROWS_WITH(internBlob(blobs, candidates, spool, BlobDomain::shared,
                                        source, true), ContainsSubstring("encoded blob size"));
        }
    }
    CHECK(checkedAlignUp(0xfffff000u, 4096, "test") == 0xfffff000u);
    CHECK_THROWS_AS(checkedAlignUp(0xfffff001u, 4096, "test"), Error);
    CHECK_THROWS_AS(checkedAlignUp(1ull << 32, 4096, "test"), Error);
}

TEST_CASE("page edge payloads preserve bytes and zero holes", "[builder][layout]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    std::vector<Node> nodes;
    for (size_t size : {0, 1, 3, 4, 4091, 4092, 4095, 4096, 4097, 8191, 8192})
        nodes.push_back(file(std::to_string(size), std::string(size, 'x')));
    auto plan = fixture.plan(spool, std::move(nodes));
    writeRegularOutput(fixture.options, plan, spool, fixture.inputStatus);
    for (const auto& blob : plan.blobs) {
        CHECK(blob.storedSize == ((uint64_t(blob.source.size) + 4) & ~3ull));
        if (blob.storedSize >= 4096) CHECK(blob.imageOff % 4096 == 0);
        else CHECK(blob.imageOff / 4096 == (blob.imageOff + blob.storedSize - 1) / 4096);
    }
    FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDONLY));
    REQUIRE_NOTHROW(verifyImage(fd.get(), plan, spool, "full", false));
}

TEST_CASE("root and global metadata limits are independent", "[builder][bounds]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    std::vector<Node> nodes;
    for (uint32_t i = 0; i < 32767; ++i) nodes.push_back(file(std::to_string(i), ""));
    auto minimum = makeDirectoryPlans(nodes, true);
    REQUIRE(minimum.size() == 1);
    CHECK(minimum[0].children.size() == 32767);
    CHECK(minimum[0].hash.tableSize <= 65521);
    nodes.push_back(file("limit", ""));
    CHECK_THROWS_WITH(makeDirectoryPlans(nodes, true), ContainsSubstring("2^15"));
    auto plan = fixture.plan(spool);
    plan.cellCount = 65535;
    plan.metadata.resize(65535 * 16);
    // Isolate the validator's count guard, not a claim of a canonical padded image.
    CHECK_NOTHROW(validatePlan(plan));
    for (uint32_t count : {0, 65536, 65537}) {
        plan.cellCount = count;
        CHECK_THROWS_WITH(validatePlan(plan), ContainsSubstring("metadata cell count"));
    }
}

TEST_CASE("attribute reuse affects deterministic metadata fallback", "[builder][budget]")
{
    std::vector<Node> nodes;
    for (uint32_t i = 0; i < 100; ++i) nodes.push_back(file(std::to_string(i), ""));
    auto dirs = makeDirectoryPlans(nodes, true);
    const auto minimum = dirs[0].hash.tableSize;
    dirs[0].hash.tableSize = 65521;
    std::vector<AttrPlan> attrs(3);
    CHECK(plannedCellCount(nodes, dirs, attrs) == 65621);
    uint32_t hash2 = 0;
    fitDirectoryPlans(dirs, nodes, attrs, hash2);
    CHECK(dirs[0].hash.tableSize == minimum);
    CHECK(plannedCellCount(nodes, dirs, attrs) <= 65535);
    dirs[0].hash.tableSize = 65435;
    CHECK(plannedCellCount(nodes, dirs, attrs) == 65535);
    fitDirectoryPlans(dirs, nodes, attrs, hash2);
    CHECK(dirs[0].hash.tableSize == 65435); // Empty buckets absorb the attributes.
    attrs.resize(65336);
    CHECK(plannedCellCount(nodes, dirs, attrs) == 65536);
    CHECK_THROWS_WITH(fitDirectoryPlans(dirs, nodes, attrs, hash2),
                      ContainsSubstring("metadata limit"));
}

TEST_CASE("plan validator rejects chain and content defects", "[builder][validation]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    const auto valid = fixture.plan(spool, {file("alpha", "payload"), file("beta", "other")});
    REQUIRE_NOTHROW(validatePlan(valid));
    for (int fault = 0; fault < 13; ++fault) {
        CAPTURE(fault);
        auto plan = valid;
        auto& directory = plan.directories[0];
        const auto found = std::find_if(directory.buckets.begin(), directory.buckets.end(),
                                       [](const auto& b) { return b.has_value(); });
        REQUIRE(found != directory.buckets.end());
        const auto position = static_cast<uint32_t>(found - directory.buckets.begin());
        switch (fault) {
            case 0: found->value().next = position; break;
            case 1: found->value().next = directory.hash.tableSize + 1; break;
            case 2: found->value().nodeIndex = plan.nodes.size(); break;
            case 3: plan.hash2Base = 3; break;
            case 4: plan.publicOff++; break;
            case 5: plan.blobs[0].imageOff = plan.imageSize; break;
            case 6: plan.blobs[0].publicPart = !plan.blobs[0].publicPart; break;
            case 7: plan.blobs[1].imageOff = plan.blobs[0].imageOff; break;
            case 8: directory.children.push_back(0); break;
            case 9: plan.metadata.pop_back(); break;
            case 10: {
                const auto empty = std::find(directory.buckets.begin(), directory.buckets.end(),
                                             std::nullopt);
                REQUIRE(empty != directory.buckets.end());
                *empty = *found;
                found->reset();
                break;
            }
            case 11: {
                const auto empty = std::find(directory.buckets.begin(), directory.buckets.end(),
                                             std::nullopt);
                REQUIRE(empty != directory.buckets.end());
                found->value().next = static_cast<uint32_t>(empty - directory.buckets.begin());
                break;
            }
            case 12: found->reset(); directory.children.pop_back(); break;
        }
        CHECK_THROWS_AS(validatePlan(plan), Error);
    }
}

TEST_CASE("public hardlink and filename promotion preserves symlink isolation", "[builder][visibility]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    Node hidden = file("hidden", "");
    hidden.kind = NodeKind::directory;
    hidden.attr.mode = S_IFDIR | 0700;
    hidden.payload.reset();
    auto privateFile = file("hidden/a", "shared");
    auto publicLink = file("visible", "shared");
    publicLink.explicitHardlink = "hidden/a";
    auto sharedName = file("shared", "different");
    auto symlink = file("link", "");
    symlink.kind = NodeKind::symlink;
    symlink.attr.mode = S_IFLNK | 0777;
    symlink.payload.reset();
    symlink.symlinkTarget = "shared";
    auto plan = fixture.plan(spool, {hidden, privateFile, publicLink, sharedName, symlink});
    auto node = [&](const std::string& path) -> const Node& {
        return *std::find_if(plan.nodes.begin(), plan.nodes.end(),
                            [&](const Node& n) { return n.path == path; });
    };
    CHECK(node("hidden/a").contentBlob == node("visible").contentBlob);
    CHECK(node("shared").nameBlob == node("visible").contentBlob);
    CHECK(plan.blobs[node("visible").contentBlob].publicPart);
    CHECK_FALSE(plan.blobs[node("hidden/a").nameBlob].publicPart);
    CHECK_FALSE(plan.blobs[node("link").contentBlob].publicPart);
    CHECK(node("link").contentBlob != node("visible").contentBlob);
}

TEST_CASE("output verification detects altered bytes in both modes", "[builder][validation]")
{
    for (const std::string mode : {"full", "structure"}) {
        for (int fault = 0; fault < 5; ++fault) {
            Fixture fixture;
            Spool spool(fixture.options.tempDir);
            auto plan = fixture.plan(spool, {file("alpha", "payload")});
            FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDWR | O_CREAT, 0600));
            writeImage(fd.get(), plan, spool, false);
            REQUIRE_NOTHROW(verifyImage(fd.get(), plan, spool, mode, false));
            const auto& blob = plan.blobs[0];
            const off_t offsets[] = {0, 128, blob.imageOff,
                static_cast<off_t>(blob.imageOff + blob.source.size), plan.imageSize - 1};
            uint8_t byte = 0xff;
            REQUIRE(::pwrite(fd.get(), &byte, 1, offsets[fault]) == 1);
            if (mode == "structure" && fault == 4)
                CHECK_NOTHROW(verifyImage(fd.get(), plan, spool, mode, false));
            else
                CHECK_THROWS_AS(verifyImage(fd.get(), plan, spool, mode, false), Error);
        }
    }
}
TEST_CASE("directory recheck rejects a source changed after spooling", "[builder][source]")
{
    Fixture fixture;
    const auto path = std::filesystem::path(fixture.options.input) / "changing";
    std::ofstream(path) << "payload";
    REQUIRE(::chmod(path.c_str(), 0644) == 0);
    Spool spool(fixture.options.tempDir);
    REQUIRE_NOTHROW(readDirectory(fixture.options, spool));
    io.changeSource = path.string();
    CHECK_THROWS_WITH(readDirectory(fixture.options, spool),
                      ContainsSubstring("input file changed while being read"));
}

TEST_CASE("all hash prime indices and unsigned overflow follow the format", "[builder][hash]")
{
    for (uint32_t base : femtofs::kSmallPrimes) {
        const std::string name = "a-long-name-with-unsigned-\xff-bytes";
        uint32_t expected = 0;
        for (unsigned char byte : name) expected = expected * base + byte;
        CHECK(femtofs::hashName(name, base, 65521) == expected % 65521);
    }
    const std::string name = "full-width-base";
    uint32_t expected = 0;
    for (unsigned char byte : name) expected = expected * 0xfffffffbu + byte;
    CHECK(femtofs::hashName(name, 0xfffffffbu, 65521) == expected % 65521);
}
TEST_CASE("packing chooses the smallest hole and then the lowest offset", "[builder][layout]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    std::vector<BlobClass> blobs;
    for (uint32_t size : {4097, 4097, 4097, 100, 100}) {
        blobs.push_back({BlobDomain::shared, inlineSource(std::string(size, 'x')),
                         true, (size + 4u) & ~3u, 0});
    }
    CHECK(packPart(blobs, {0, 1, 2, 3, 4}, 4096, spool) == 20484);
    CHECK(blobs[3].imageOff == 8196);
    CHECK(blobs[4].imageOff == 8300);
    const auto expected = blobs;
    CHECK(packPart(blobs, {4, 3, 2, 1, 0}, 4096, spool) == 20484);
    for (size_t i = 0; i < blobs.size(); ++i)
        CHECK(blobs[i].imageOff == expected[i].imageOff);
}

TEST_CASE("maximum directory table fits when the independent cell budget permits", "[builder][bounds]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    auto plan = fixture.plan(spool, {file("only", "data")});
    plan.directories[0].hash.tableSize = 65521;
    constructBuckets(plan.directories, plan.nodes, 0);
    plan.attrAtCell.clear();
    placeAttributes(plan);
    packContent(plan, spool);
    encodeMetadata(plan);
    encodeHeader(plan, fixture.options);
    CHECK(plan.cellCount == 65522);
    CHECK(femtofs::load32(plan.header, 56) == 65521);
    CHECK(femtofs::load32(plan.header, 60) == 1);
    REQUIRE_NOTHROW(validatePlan(plan));
    writeRegularOutput(fixture.options, plan, spool, fixture.inputStatus);
}
TEST_CASE("source normalization rejects forbidden names and unsupported inode kinds", "[builder][source]")
{
    for (const auto& path : {std::string("a\0b", 3), std::string("a/"),
                             std::string("./."), std::string("a/../b"),
                             std::string(256, 'a')}) {
        CHECK_THROWS_AS(normalizeArchivePath(path, false, 0), Error);
    }
    CHECK(normalizeArchivePath(std::string(255, 'a'), false, 0) == std::string(255, 'a'));
    CHECK(normalizeArchivePath("././parent/file", false, 1) == "file");
    CHECK_THROWS_AS(normalizeArchivePath("parent/file", false, 2), Error);
    CHECK(normalizeArchivePath("./", true, 0).empty());
    for (mode_t kind : {S_IFCHR, S_IFBLK, S_IFSOCK})
        CHECK_THROWS_AS(entryKind(kind, "unsupported"), Error);
#ifdef S_IFWHT
    CHECK_THROWS_AS(entryKind(S_IFWHT, "whiteout"), Error);
#endif
}
// END File: units/test-builder-plan.cpp
