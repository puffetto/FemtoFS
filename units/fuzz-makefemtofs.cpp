// File: units/fuzz-makefemtofs.cpp
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// Bounded path normalization and builder-plan validation fuzz entry points.
#define main makefemtofs_fuzz_main
#include "../programs/makefemtofs.cpp"
#undef main

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2 || size > 4096) return 0;
    try {
#ifdef FEMTOFS_FUZZ_PATHS
        const std::string path(reinterpret_cast<const char*>(data + 2), size - 2);
        const bool directory = (data[0] & 1) != 0;
        const auto normalized = normalizeArchivePath(path, directory, data[1] % 8);
        if (normalizeArchivePath(normalized, directory, 0) != normalized)
            std::abort();
#else
        static Spool spool(FEMTOFS_TEST_ROOT);
        Options options;
        options.uuid = std::array<uint8_t, 16>{};
        InputTree tree;
        const size_t count = 1 + data[0] % 8;
        for (size_t i = 0; i < count; ++i) {
            Node node;
            node.path = node.name = std::to_string(i);
            node.attr.mode = S_IFREG | (data[1] & 0777);
            node.payload = inlineSource(std::string(
                reinterpret_cast<const char*>(data + 2), size - 2));
            tree.nodes.push_back(std::move(node));
        }
        normalizeTree(tree, options, spool);
        auto blobs = classifyBlobs(tree, spool);
        auto plan = buildPlan(std::move(tree), std::move(blobs), options, spool);
        // Mutate only fields whose ranges the plan validator owns. This is not
        // an arbitrary serialized-image parser or an unchecked object fuzzer.
        switch (data[1] % 6) {
            case 0: plan.cellCount = 65536; break;
            case 1: plan.publicOff |= 1; break;
            case 2: plan.hash2Base = 1; break;
            case 3: plan.blobs.front().imageOff = plan.imageSize; break;
            case 4: plan.directories.front().children.push_back(0); break;
            default: break;
        }
        validatePlan(plan);
#endif
    } catch (const Error&) {
        // Rejection of malformed inputs/plans is an expected result.
    }
    return 0;
}
// END File: units/fuzz-makefemtofs.cpp
