// File: units/test-makefemtofs.cpp
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Exercise private publication and device commit failure handling.
#include "makefemtofs-test.h"

using namespace builder_test;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("temporary image verification precedes public permissions", "[builder][publication]")
{
    for (const std::string mode : {"full", "structure"}) {
        Fixture fixture;
        fixture.options.verify = mode;
        Spool spool(fixture.options.tempDir);
        auto plan = fixture.plan(spool, {file("hello", "payload")});
        writeRegularOutput(fixture.options, plan, spool, fixture.inputStatus);
        REQUIRE(io.verificationReads > 0);
        CHECK(io.privateReads);
        CHECK(io.chmodCalls == 1);
        struct stat st{};
        REQUIRE(::stat(fixture.options.output.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0644);
        CHECK_FALSE(std::filesystem::exists(io.temporary));
    }
}

TEST_CASE("device commit failure triggers a recovery read", "[builder][device]")
{
    Fixture fixture;
    Spool spool(fixture.options.tempDir);
    auto plan = fixture.plan(spool);
    FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDWR | O_CREAT, 0600));
    REQUIRE(fd.get() >= 0);
    io.output = fd.get();
    io.pageIo = true;
    io.failCommit = true;
    REQUIRE_THROWS_AS(writeImage(fd.get(), plan, spool, true), Error);
    CHECK(io.readsAfterFailure > 0);
    CHECK(io.aligned);
}


TEST_CASE("publication errors preserve the destination before publish", "[builder][publication]")
{
    for (int fault = 0; fault != 5; ++fault) {
        CAPTURE(fault);
        Fixture fixture;
        Spool spool(fixture.options.tempDir);
        auto plan = fixture.plan(spool, {file("hello", "payload")});
        std::ofstream(fixture.options.output) << "previous image";
        fixture.options.force = true;
        io.corruptOnReopen = fault == 0;
        io.failReopen = fault == 1;
        io.failChmod = fault == 2;
        io.failPublish = fault == 3;
        io.failParentFlush = fault == 4;
        REQUIRE_THROWS_AS(writeRegularOutput(fixture.options, plan, spool,
                                            fixture.inputStatus), Error);
        CHECK_FALSE(std::filesystem::exists(io.temporary));
        if (fault == 4) {
            FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDONLY));
            REQUIRE_NOTHROW(verifyImage(fd.get(), plan, spool, "full", false));
        } else {
            CHECK(contents(fixture.options.output) == "previous image");
        }
        if (fault < 2) CHECK(io.chmodCalls == 0);
    }
}

TEST_CASE("each regular writer write and flush failure prevents publication", "[builder][publication]")
{
    int operationCount = 0;
    for (int failAt = 0; failAt <= operationCount; ++failAt) {
        Fixture fixture;
        Spool spool(fixture.options.tempDir);
        auto plan = fixture.plan(spool, {file("payload", std::string(5000, 'x'))});
        std::ofstream(fixture.options.output) << "previous image";
        fixture.options.force = true;
        io.failAt = failAt;
        if (failAt == 0) {
            REQUIRE_NOTHROW(writeRegularOutput(fixture.options, plan, spool,
                                               fixture.inputStatus));
            operationCount = io.operations;
            REQUIRE(operationCount > 5);
        } else {
            CAPTURE(failAt, operationCount);
            REQUIRE_THROWS_AS(writeRegularOutput(fixture.options, plan, spool,
                                                fixture.inputStatus), Error);
            CHECK(contents(fixture.options.output) == "previous image");
            CHECK_FALSE(std::filesystem::exists(io.temporary));
        }
    }
}

TEST_CASE("device commit recovery reports observed bytes and retains failure", "[builder][device]")
{
    for (int fault = 0; fault != 9; ++fault) {
        CAPTURE(fault);
        Fixture fixture;
        Spool spool(fixture.options.tempDir);
        auto plan = fixture.plan(spool, {file("hello", "payload")});
        FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDWR | O_CREAT, 0600));
        REQUIRE(fd.get() >= 0);
        io.output = fd.get();
        io.pageIo = true;
        io.failCommit = fault <= 3;
        io.failFinalFlush = fault >= 4;
        if (fault == 1) io.partial = 1;
        if (fault == 2) io.partial = 128; // All changed header bytes, short page write.
        if (fault == 3) io.partial = 4096; // Write complete but syscall reports failure.
        if (fault == 5) io.unreadable = true;
        if (fault == 6) io.corruptAt = 128;
        if (fault == 7) io.corruptAt = plan.blobs[0].imageOff;
        if (fault == 8) io.corruptAt = plan.imageSize - 1;
        const bool valid = fault == 2 || fault == 3 || fault == 4;
        try {
            writeImage(fd.get(), plan, spool, true);
            FAIL("injected device failure returned success");
        } catch (const Error& error) {
            const std::string diagnostic = error.what();
            CHECK_THAT(diagnostic, ContainsSubstring("persistence is uncertain"));
            CHECK_THAT(diagnostic, ContainsSubstring(valid ?
                "passes full verification" : "current image could not be verified"));
            if (fault == 5) CHECK_THAT(diagnostic, ContainsSubstring("pread device page"));
            if (fault >= 4) CHECK_THAT(diagnostic, ContainsSubstring("flushing completed image"));
        }
        CHECK(io.readsAfterFailure > 0);
        CHECK(io.aligned);
    }
}

TEST_CASE("device precommit faults never write the final header", "[builder][device]")
{
    int operationCount = 0;
    for (int failAt = 0; failAt <= operationCount; ++failAt) {
        Fixture fixture;
        Spool spool(fixture.options.tempDir);
        auto plan = fixture.plan(spool, {file("hello", std::string(5000, 'x'))});
        FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDWR | O_CREAT, 0600));
        REQUIRE(fd.get() >= 0);
        io.output = fd.get();
        io.pageIo = true;
        io.failAt = failAt;
        if (failAt == 0) {
            REQUIRE_NOTHROW(writeImage(fd.get(), plan, spool, true));
            operationCount = io.operations - 2; // Exclude final commit and flush.
            REQUIRE_NOTHROW(verifyImage(fd.get(), plan, spool, "full", true));
            CHECK(io.readsAfterFailure == 0);
        } else {
            CAPTURE(failAt);
            REQUIRE_THROWS_AS(writeImage(fd.get(), plan, spool, true), Error);
            CHECK_FALSE(io.committed);
            CHECK(io.readsAfterFailure == 0);
            if (failAt > 1) {
                std::array<uint8_t, 16> header{};
                REQUIRE(::pread(fd.get(), header.data(), header.size(), 0) == 16);
                CHECK(std::all_of(header.begin(), header.end(), [](auto b) { return b == 0; }));
            }
        }
        CHECK(io.aligned);
    }
}

TEST_CASE("regular byte writes retry interruption and partial progress", "[builder][io]")
{
    for (int fault = 0; fault != 4; ++fault) {
        Fixture fixture;
        FileDescriptor fd(::open(fixture.options.output.c_str(), O_RDWR | O_CREAT, 0600));
        REQUIRE(fd.get() >= 0);
        io.output = fd.get();
        io.failAt = 1;
        io.writeErrno = fault == 0 ? EINTR : ENOSPC;
        if (fault == 1) io.partial = 3;
        if (fault == 2) io.partial = 0;
        const std::array<uint8_t, 8> bytes{1,2,3,4,5,6,7,8};
        if (fault < 2) {
            REQUIRE_NOTHROW(writeAllAt(fd.get(), bytes, 0));
            std::array<uint8_t, 8> actual{};
            readAllAt(fd.get(), actual, 0);
            CHECK(actual == bytes);
        } else {
            CHECK_THROWS_AS(writeAllAt(fd.get(), bytes, 0), Error);
        }
    }
}
// END File: units/test-makefemtofs.cpp
