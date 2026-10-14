// File: programs/femtofsSim.cpp
// Created by Andrea "Nemesi" Cocito on 24/08/2026
// Estimate femtoFS 0x0100 metadata, hashing, packing, and mmap behavior.

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <femtofs/format.h>
#include <femtofs/hash_plan.h>

namespace {

constexpr uint32_t kBlobWord = 4u;

struct FsEntry {
    uint64_t inode = 0;
    char type = '?'; // d, -, l, p
    std::string mode;
    std::string owner;
    std::string group;
    uint64_t size = 0;
    std::string path;
    std::string parent;
    std::string name;
    std::string symlinkTarget;
};

struct HashChoice {
    uint32_t tablesize = 0;
    uint32_t p = 0;
    double score = 0.0;
    uint32_t maxChain = 0;
    uint64_t sumSquares = 0;
    uint32_t empties = 0;
    bool hitCeiling = false;
    uint32_t ceiling = 0;
    bool dual = false;
};

struct PackingStats {
    uint64_t rawBytes = 0;
    uint64_t paddedBytes = 0;
    uint64_t paddingBytes = 0;
    size_t contentCount = 0;
};

struct ClassifiedContent {
    uint32_t size = 0;
    bool isPublic = false;
};

struct VisibilitySplitPackingStats {
    PackingStats combined;
    PackingStats publicPool;
    PackingStats privatePool;
};

struct DirReport {
    std::string path;
    uint32_t n = 0;
    HashChoice choice;
};

struct HashSimulationSummary {
    std::vector<DirReport> dirs;
    uint64_t totalDirEntries = 0;
    uint64_t totalBuckets = 0;
    uint64_t totalEmptyBuckets = 0;
    uint64_t totalSumSquares = 0;
    uint32_t globalMaxChain = 0;
    uint64_t dirsPerfect = 0;
    uint64_t dirsHitCeiling = 0;
    uint64_t dirsFallback = 0;
    uint64_t dirsOverHardLimit = 0;
};

struct ProgramOptions {
    std::string inputPath = "misc/list";
    uint32_t imagePageSize = femtofs::kPageSize;
    uint32_t vmPageSize = femtofs::kPageSize;
    bool fixedBaseExperiment = false;
    size_t fixedBaseSamples = 100;
    uint32_t fixedBaseSeed = 0x0F5F2026u;
    bool dualHashExperiment = true;
    size_t dualHashSamples = 100;
    uint32_t dualHashSeed = 0x0F5F2026u;
    uint32_t dualHashHardThreshold = 2u;
    bool budgetedHashExperiment = true;
    uint32_t budgetedTargetMaxChain = 2u;
    std::vector<uint32_t> budgetedBudgetsKiB = {0u, 64u, 128u, 256u, 512u};
};

struct DirectoryHashCandidateSet {
    std::string path;
    uint32_t n = 0;
    std::vector<HashChoice> choices; // baseline policy choice is at index 0
};

struct BudgetedHashTuningResult {
    HashSimulationSummary summary;
    uint64_t budgetBuckets = 0;
    uint64_t usedBuckets = 0;
    uint64_t upgradesApplied = 0;
    uint64_t directoriesChanged = 0;
};

struct MixedDualHashResult {
    HashSimulationSummary summary;
    uint64_t hardDirectories = 0;
    uint64_t hardEntries = 0;
    long double missScore = 0.0L; // Same parameter-selection proxy as the builder.
};

std::string normalizePath(std::string path, bool directory) {
    if (path.find('\0') != std::string::npos || (!path.empty() && path.front() == '/')) {
        throw std::runtime_error("invalid pathname: " + path);
    }
    if (directory) {
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
    } else if (!path.empty() && path.back() == '/') {
        throw std::runtime_error("non-directory pathname has a trailing slash: " + path);
    }
    while (path == "." || path.rfind("./", 0) == 0) {
        if (path == ".") {
            path.clear();
            break;
        }
        path.erase(0, 2);
    }
    if (path.empty() && !directory)
        throw std::runtime_error("non-directory pathname maps to root");
    for (size_t start = 0; start < path.size();) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string_view component(path.data() + start, end - start);
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255) {
            throw std::runtime_error("invalid pathname component: " + path);
        }
        start = end + 1;
    }
    return path;
}

std::pair<std::string, std::string> splitParentName(const std::string& path) {
    if (path.empty()) {
        return {"", ""};
    }
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

bool parseFindLsLine(const std::string& line, FsEntry& out) {
    static const std::regex re(
        R"(^\s*([0-9]+)\s+([0-9]+)\s+([bcdlps-][rwxstST-]{9})\s+([0-9]+)\s+(\S+)\s+(\S+)\s+([0-9]+)\s+[A-Za-z]{3}\s+[0-9]{1,2}\s+([0-9]{2}:[0-9]{2}|[0-9]{4})\s+(.+)$)");

    std::smatch m;
    if (!std::regex_match(line, m, re)) {
        return false;
    }

    FsEntry e;
    e.inode = std::stoull(m[1].str());
    const std::string mode = m[3].str();
    e.type = mode.empty() ? '?' : mode[0];
    e.mode = mode;
    e.owner = m[5].str();
    e.group = m[6].str();
    e.size = std::stoull(m[7].str());

    std::string pathField = m[9].str();
    if (e.type == 'l') {
        const std::string arrow = " -> ";
        const auto pos = pathField.find(arrow);
        if (pos != std::string::npos) {
            e.symlinkTarget = pathField.substr(pos + arrow.size());
            pathField = pathField.substr(0, pos);
        }
    }

    e.path = normalizePath(std::move(pathField), e.type == 'd');
    const auto [parent, name] = splitParentName(e.path);
    e.parent = parent;
    e.name = name;

    out = std::move(e);
    return true;
}

const std::vector<uint32_t>& fixedSmallPrimes() {
    static const std::vector<uint32_t> kSmallPrimes(
        femtofs::kSmallPrimes.begin(), femtofs::kSmallPrimes.end());
    return kSmallPrimes;
}

uint32_t smallPrimeIndex(uint32_t prime) {
    const auto found = std::find(femtofs::kSmallPrimes.begin(),
                                 femtofs::kSmallPrimes.end(), prime);
    if (found == femtofs::kSmallPrimes.end())
        return UINT32_MAX;
    return static_cast<uint32_t>(
        std::distance(femtofs::kSmallPrimes.begin(), found));
}

uint32_t nextPrimeStrictlyGreater(uint32_t n) {
    return femtofs::hashplan::nextPrimeStrict(n);
}

using Score = femtofs::hashplan::Score;

Score scoreDirectory(const std::vector<std::string>& names, uint32_t p, uint32_t tablesize) {
    return femtofs::hashplan::scoreSingle(names, p, tablesize);
}

HashChoice simulationChoice(const femtofs::hashplan::Choice& choice,
                            uint32_t entries) {
    HashChoice out;
    out.tablesize = choice.tableSize;
    out.p = choice.prime;
    out.score = entries == 0 ? 0.0 :
        static_cast<double>(choice.sumSquares) / static_cast<double>(entries);
    out.maxChain = choice.maxChain;
    out.sumSquares = choice.sumSquares;
    out.empties = choice.tableSize - entries;
    out.hitCeiling = choice.hitCeiling;
    out.ceiling = choice.ceiling;
    out.dual = choice.dual;
    return out;
}

HashChoice chooseHash(const std::vector<std::string>& names,
                      const std::vector<uint32_t>& bases) {
    return simulationChoice(femtofs::hashplan::chooseSingle(names, bases),
                            static_cast<uint32_t>(names.size()));
}

HashChoice chooseHashWithFixedFallback(const std::vector<std::string>& names,
                                       const std::vector<uint32_t>& bases,
                                       uint32_t fallbackBase) {
    HashChoice primary = chooseHash(names, bases);
    if (!primary.hitCeiling) {
        return primary;
    }

    HashChoice fallback = chooseHash(names, {fallbackBase});
    fallback.hitCeiling = true;
    return fallback;
}

std::vector<uint32_t> parseU32Csv(const std::string& csv) {
    std::vector<uint32_t> values;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            throw std::runtime_error("invalid empty value in CSV list: " + csv);
        }
        values.push_back(static_cast<uint32_t>(std::stoul(token)));
    }
    if (values.empty()) {
        throw std::runtime_error("empty CSV list");
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

ProgramOptions parseArgs(int argc, char** argv) {
    ProgramOptions opts;
    bool inputSet = false;
    bool imagePageSizeSet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fixed-base-experiment") {
            opts.fixedBaseExperiment = true;
            continue;
        }
        if (arg.rfind("--fixed-base-samples=", 0) == 0) {
            opts.fixedBaseExperiment = true;
            opts.fixedBaseSamples = static_cast<size_t>(std::stoul(arg.substr(21)));
            continue;
        }
        if (arg.rfind("--fixed-base-seed=", 0) == 0) {
            opts.fixedBaseExperiment = true;
            opts.fixedBaseSeed = static_cast<uint32_t>(std::stoul(arg.substr(18)));
            continue;
        }
        if (arg == "--dual-hash-experiment") {
            opts.dualHashExperiment = true;
            continue;
        }
        if (arg == "--no-dual-hash-experiment") {
            opts.dualHashExperiment = false;
            continue;
        }
        constexpr std::string_view dualSamples = "--dual-hash-samples=";
        if (arg.rfind(dualSamples, 0) == 0) {
            opts.dualHashExperiment = true;
            opts.dualHashSamples = static_cast<size_t>(
                std::stoul(arg.substr(dualSamples.size())));
            continue;
        }
        constexpr std::string_view dualSeed = "--dual-hash-seed=";
        if (arg.rfind(dualSeed, 0) == 0) {
            opts.dualHashExperiment = true;
            opts.dualHashSeed = static_cast<uint32_t>(
                std::stoul(arg.substr(dualSeed.size())));
            continue;
        }
        constexpr std::string_view dualThreshold = "--dual-hash-hard-threshold=";
        if (arg.rfind(dualThreshold, 0) == 0) {
            opts.dualHashExperiment = true;
            opts.dualHashHardThreshold = static_cast<uint32_t>(
                std::stoul(arg.substr(dualThreshold.size())));
            continue;
        }
        constexpr std::string_view vmPage = "--vm-page-size=";
        if (arg.rfind(vmPage, 0) == 0) {
            opts.vmPageSize = static_cast<uint32_t>(
                std::stoul(arg.substr(vmPage.size())));
            continue;
        }
        if (arg == "--budgeted-hash-experiment") {
            opts.budgetedHashExperiment = true;
            continue;
        }
        if (arg == "--no-budgeted-hash-experiment") {
            opts.budgetedHashExperiment = false;
            continue;
        }
        if (arg.rfind("--budgeted-budgets-kib=", 0) == 0) {
            opts.budgetedHashExperiment = true;
            opts.budgetedBudgetsKiB = parseU32Csv(arg.substr(23));
            continue;
        }
        if (arg.rfind("--budgeted-target-max-chain=", 0) == 0) {
            opts.budgetedHashExperiment = true;
            opts.budgetedTargetMaxChain = static_cast<uint32_t>(std::stoul(arg.substr(28)));
            continue;
        }
        if (!inputSet) {
            opts.inputPath = arg;
            inputSet = true;
            continue;
        }
        if (!imagePageSizeSet) {
            opts.imagePageSize = static_cast<uint32_t>(std::stoul(arg));
            imagePageSizeSet = true;
            continue;
        }
        throw std::runtime_error("usage: femtofsSim [inputPath] [imagePageSize] [--vm-page-size=N] "
                                 "[--fixed-base-experiment] "
                                 "[--fixed-base-samples=N] [--fixed-base-seed=N] "
                                 "[--dual-hash-experiment|--no-dual-hash-experiment] "
                                 "[--dual-hash-samples=N] [--dual-hash-seed=N] "
                                 "[--dual-hash-hard-threshold=N] "
                                 "[--budgeted-hash-experiment|--no-budgeted-hash-experiment] "
                                 "[--budgeted-budgets-kib=a,b,c] [--budgeted-target-max-chain=N]");
    }

    const auto validPageSize = [](uint32_t value) {
        return value >= 256u && value <= (1u << 31) &&
               (value & (value - 1u)) == 0;
    };
    if (!validPageSize(opts.imagePageSize))
        throw std::runtime_error("image PAGE_SIZE must be a power of two from 256 through 2^31");
    if (!validPageSize(opts.vmPageSize))
        throw std::runtime_error("VM_PAGE_SIZE must be a power of two from 256 through 2^31");

    return opts;
}

std::vector<uint32_t> sampleRandomPrimes(uint32_t minInclusive,
                                         uint32_t maxExclusive,
                                         size_t count,
                                         uint32_t seed) {
    return femtofs::hashplan::samplePrimes(
        minInclusive, maxExclusive, count, seed);
}

HashSimulationSummary simulateHashing(const std::set<std::string>& directories,
                                      const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
                                      const std::vector<uint32_t>& bases) {
    HashSimulationSummary summary;
    summary.dirs.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= femtofs::kMaxDirectoryEntries) {
            ++summary.dirsOverHardLimit;
        }

        HashChoice hc = chooseHash(names, bases);
        if (hc.score == 1.0 && n > 0) {
            ++summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++summary.dirsFallback;
            }
        }

        summary.totalDirEntries += n;
        summary.totalBuckets += hc.tablesize;
        summary.totalEmptyBuckets += hc.empties;
        summary.totalSumSquares += hc.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain, hc.maxChain);

        summary.dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});
    }

    std::sort(summary.dirs.begin(), summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    return summary;
}

HashSimulationSummary simulateHashingWithFixedFallback(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::vector<uint32_t>& bases,
    uint32_t fallbackBase) {
    HashSimulationSummary summary;
    summary.dirs.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= femtofs::kMaxDirectoryEntries) {
            ++summary.dirsOverHardLimit;
        }

        HashChoice hc = chooseHashWithFixedFallback(names, bases, fallbackBase);
        if (hc.score == 1.0 && n > 0) {
            ++summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++summary.dirsFallback;
            }
        }

        summary.totalDirEntries += n;
        summary.totalBuckets += hc.tablesize;
        summary.totalEmptyBuckets += hc.empties;
        summary.totalSumSquares += hc.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain, hc.maxChain);

        summary.dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});
    }

    std::sort(summary.dirs.begin(), summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    return summary;
}

HashChoice chooseHashTwoChoiceBalanced(const std::vector<std::string>& names,
                                       const std::vector<uint32_t>& bases,
                                       uint32_t p2) {
    return simulationChoice(femtofs::hashplan::chooseDual(names, bases, p2),
                            static_cast<uint32_t>(names.size()));
}

std::unordered_map<std::string, HashChoice> buildSingleHashChoiceByDirectoryKey(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::vector<uint32_t>& bases) {
    std::unordered_map<std::string, HashChoice> byDir;
    byDir.reserve(directories.size() * 2);
    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        byDir.emplace(d, chooseHash(names, bases));
    }
    return byDir;
}

MixedDualHashResult simulateHashingMixedDualHash(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::unordered_map<std::string, HashChoice>& singleChoicesByDir,
    const std::vector<uint32_t>& bases,
    uint32_t p2,
    uint32_t hardThreshold) {
    MixedDualHashResult out;
    out.summary.dirs.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        if (n >= femtofs::kMaxDirectoryEntries) {
            ++out.summary.dirsOverHardLimit;
        }

        const auto singleIt = singleChoicesByDir.find(d);
        if (singleIt == singleChoicesByDir.end()) {
            throw std::runtime_error("internal error: missing single-hash choice for directory");
        }

        const HashChoice single = singleIt->second;
        const bool isHard = (single.maxChain > hardThreshold);
        const HashChoice dual = isHard ?
            chooseHashTwoChoiceBalanced(names, bases, p2) : single;
        const bool useDual = isHard && femtofs::hashplan::improvesQuality(
            Score{dual.sumSquares, dual.maxChain},
            Score{single.sumSquares, single.maxChain});
        const HashChoice& hc = useDual ? dual : single;

        if (useDual) {
            ++out.hardDirectories;
            out.hardEntries += n;
        }

        if (hc.score == 1.0 && n > 0) {
            ++out.summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++out.summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++out.summary.dirsFallback;
            }
        }

        out.summary.totalDirEntries += n;
        out.summary.totalBuckets += hc.tablesize;
        out.summary.totalEmptyBuckets += hc.empties;
        out.summary.totalSumSquares += hc.sumSquares;
        out.summary.globalMaxChain = std::max(out.summary.globalMaxChain, hc.maxChain);
        out.summary.dirs.push_back(DirReport{d.empty() ? "/" : d, n, hc});

        if (n > 0 && hc.tablesize > 0) {
            const long double nl = static_cast<long double>(n);
            const long double tl = static_cast<long double>(hc.tablesize);
            const long double unit = (nl * nl) / tl;
            out.missScore += useDual ? (2.0L * unit) : unit;
        }
    }

    std::sort(out.summary.dirs.begin(), out.summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        return a.n > b.n;
    });

    return out;
}

HashChoice chooseBestForFixedTableSize(const std::vector<std::string>& names,
                                       const std::vector<uint32_t>& bases,
                                       uint32_t tablesize,
                                       uint32_t ceiling) {
    HashChoice out;
    const uint32_t n = static_cast<uint32_t>(names.size());
    if (n == 0) {
        out.tablesize = 0;
        out.p = 0;
        out.score = 0.0;
        out.maxChain = 0;
        out.sumSquares = 0;
        out.empties = 0;
        out.ceiling = 0;
        return out;
    }
    if (n == 1) {
        const uint32_t ts = (tablesize == 0) ? 1u : tablesize;
        out.tablesize = ts;
        out.p = 0;
        out.score = 1.0;
        out.maxChain = 1;
        out.sumSquares = 1;
        out.empties = ts - 1u;
        out.ceiling = ceiling;
        out.hitCeiling = (ts == ceiling && out.score >= 1.1);
        return out;
    }

    bool hasBest = false;
    uint32_t bestP = 0;
    Score bestScore;

    for (uint32_t p : bases) {
        const Score s = scoreDirectory(names, p, tablesize);
        if (!hasBest ||
            s.maxChain < bestScore.maxChain ||
            (s.maxChain == bestScore.maxChain && s.sumSquares < bestScore.sumSquares) ||
            (s.maxChain == bestScore.maxChain && s.sumSquares == bestScore.sumSquares && p < bestP)) {
            hasBest = true;
            bestP = p;
            bestScore = s;
        }
    }

    out.tablesize = tablesize;
    out.p = bestP;
    out.score = static_cast<double>(bestScore.sumSquares) / static_cast<double>(n);
    out.maxChain = bestScore.maxChain;
    out.sumSquares = bestScore.sumSquares;
    out.empties = tablesize - n;
    out.ceiling = ceiling;
    out.hitCeiling = (tablesize == ceiling && out.score >= 1.1);
    return out;
}

HashSimulationSummary simulateMinimumHashing(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::vector<uint32_t>& bases) {
    HashSimulationSummary summary;
    summary.dirs.reserve(directories.size());
    for (const auto& directory : directories) {
        const auto& names = dirChildren.at(directory);
        const uint32_t n = static_cast<uint32_t>(names.size());
        const HashChoice choice = chooseBestForFixedTableSize(
            names, bases, n, n);
        summary.totalDirEntries += n;
        summary.totalBuckets += choice.tablesize;
        summary.totalEmptyBuckets += choice.empties;
        summary.totalSumSquares += choice.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain,
                                          choice.maxChain);
        summary.dirsPerfect += choice.score == 1.0 && n > 0;
        summary.dirsOverHardLimit += n >= femtofs::kMaxDirectoryEntries;
        summary.dirs.push_back(DirReport{
            directory.empty() ? "/" : directory, n, choice});
    }
    std::sort(summary.dirs.begin(), summary.dirs.end(),
        [](const DirReport& lhs, const DirReport& rhs) {
            if (lhs.choice.score != rhs.choice.score)
                return lhs.choice.score > rhs.choice.score;
            if (lhs.choice.maxChain != rhs.choice.maxChain)
                return lhs.choice.maxChain > rhs.choice.maxChain;
            return lhs.n > rhs.n;
        });
    return summary;
}

bool sameHashChoiceKey(const HashChoice& a, const HashChoice& b) {
    return a.tablesize == b.tablesize &&
           a.p == b.p &&
           a.maxChain == b.maxChain &&
           a.sumSquares == b.sumSquares;
}

void appendUniqueChoice(std::vector<HashChoice>* choices, const HashChoice& c) {
    for (const auto& existing : *choices) {
        if (sameHashChoiceKey(existing, c)) {
            return;
        }
    }
    choices->push_back(c);
}

std::vector<DirectoryHashCandidateSet> buildHashCandidateSets(
    const std::set<std::string>& directories,
    const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
    const std::vector<uint32_t>& bases) {
    std::vector<DirectoryHashCandidateSet> sets;
    sets.reserve(directories.size());

    for (const auto& d : directories) {
        const auto& names = dirChildren.at(d);
        const uint32_t n = static_cast<uint32_t>(names.size());
        const HashChoice baseline = chooseHash(names, bases);

        DirectoryHashCandidateSet set;
        set.path = d.empty() ? "/" : d;
        set.n = n;
        set.choices.push_back(baseline);

        if (n >= 2) {
            const uint32_t ceiling = baseline.ceiling;

            appendUniqueChoice(&set.choices, chooseBestForFixedTableSize(names, bases, n, ceiling));

            for (uint32_t tablesize = nextPrimeStrictlyGreater(n);
                 tablesize <= ceiling;
                 tablesize = nextPrimeStrictlyGreater(tablesize)) {
                appendUniqueChoice(&set.choices, chooseBestForFixedTableSize(names, bases, tablesize, ceiling));
            }

            if (set.choices.size() > 2) {
                std::sort(set.choices.begin() + 1, set.choices.end(), [](const HashChoice& a, const HashChoice& b) {
                    if (a.tablesize != b.tablesize) {
                        return a.tablesize < b.tablesize;
                    }
                    if (a.maxChain != b.maxChain) {
                        return a.maxChain < b.maxChain;
                    }
                    if (a.sumSquares != b.sumSquares) {
                        return a.sumSquares < b.sumSquares;
                    }
                    return a.p < b.p;
                });
            }
        }

        sets.push_back(std::move(set));
    }

    return sets;
}

HashSimulationSummary summarizeHashChoices(const std::vector<DirectoryHashCandidateSet>& sets,
                                          const std::vector<size_t>& selectedChoiceIndices) {
    HashSimulationSummary summary;
    summary.dirs.reserve(sets.size());

    for (size_t i = 0; i < sets.size(); ++i) {
        const auto& set = sets[i];
        const auto& hc = set.choices[selectedChoiceIndices[i]];
        const uint32_t n = set.n;

        if (n >= femtofs::kMaxDirectoryEntries) {
            ++summary.dirsOverHardLimit;
        }
        if (hc.score == 1.0 && n > 0) {
            ++summary.dirsPerfect;
        }
        if (hc.hitCeiling) {
            ++summary.dirsHitCeiling;
            if (hc.score >= 1.1) {
                ++summary.dirsFallback;
            }
        }

        summary.totalDirEntries += n;
        summary.totalBuckets += hc.tablesize;
        summary.totalEmptyBuckets += hc.empties;
        summary.totalSumSquares += hc.sumSquares;
        summary.globalMaxChain = std::max(summary.globalMaxChain, hc.maxChain);

        summary.dirs.push_back(DirReport{set.path, n, hc});
    }

    std::sort(summary.dirs.begin(), summary.dirs.end(), [](const DirReport& a, const DirReport& b) {
        if (a.choice.score != b.choice.score) {
            return a.choice.score > b.choice.score;
        }
        if (a.choice.maxChain != b.choice.maxChain) {
            return a.choice.maxChain > b.choice.maxChain;
        }
        return a.n > b.n;
    });

    return summary;
}

BudgetedHashTuningResult runBudgetedHashTuning(const std::vector<DirectoryHashCandidateSet>& sets,
                                               uint64_t budgetBuckets,
                                               uint32_t targetMaxChain) {
    BudgetedHashTuningResult result;
    result.budgetBuckets = budgetBuckets;

    if (sets.empty()) {
        return result;
    }

    std::vector<size_t> selectedChoiceIndices(sets.size(), 0);
    HashSimulationSummary current = summarizeHashChoices(sets, selectedChoiceIndices);
    uint64_t usedBuckets = 0;
    uint64_t upgradesApplied = 0;

    struct Candidate {
        bool found = false;
        size_t dirIndex = 0;
        size_t choiceIndex = 0;
        uint64_t costBuckets = 0;
        uint32_t newGlobalMaxChain = 0;
        uint64_t newTotalSumSquares = 0;
    };

    for (;;) {
        Candidate bestAny;
        Candidate bestReducer;
        const bool prioritizeMaxChainReduction =
            (targetMaxChain > 0 && current.globalMaxChain > targetMaxChain);

        auto considerCandidate = [&](Candidate* best,
                                     size_t dirIndex,
                                     size_t choiceIndex,
                                     uint64_t costBuckets,
                                     uint32_t newGlobalMaxChain,
                                     uint64_t newTotalSumSquares) {
            const auto& candidateChoice = sets[dirIndex].choices[choiceIndex];
            const auto& currentBestChoice = best->found
                ? sets[best->dirIndex].choices[best->choiceIndex]
                : candidateChoice;

            const bool take =
                !best->found ||
                (newGlobalMaxChain < best->newGlobalMaxChain) ||
                (newGlobalMaxChain == best->newGlobalMaxChain &&
                 newTotalSumSquares < best->newTotalSumSquares) ||
                (newGlobalMaxChain == best->newGlobalMaxChain &&
                 newTotalSumSquares == best->newTotalSumSquares &&
                 costBuckets < best->costBuckets) ||
                (newGlobalMaxChain == best->newGlobalMaxChain &&
                 newTotalSumSquares == best->newTotalSumSquares &&
                 costBuckets == best->costBuckets &&
                 candidateChoice.tablesize < currentBestChoice.tablesize) ||
                (newGlobalMaxChain == best->newGlobalMaxChain &&
                 newTotalSumSquares == best->newTotalSumSquares &&
                 costBuckets == best->costBuckets &&
                 candidateChoice.tablesize == currentBestChoice.tablesize &&
                 candidateChoice.p < currentBestChoice.p) ||
                (newGlobalMaxChain == best->newGlobalMaxChain &&
                 newTotalSumSquares == best->newTotalSumSquares &&
                 costBuckets == best->costBuckets &&
                 candidateChoice.tablesize == currentBestChoice.tablesize &&
                 candidateChoice.p == currentBestChoice.p &&
                 sets[dirIndex].path < sets[best->dirIndex].path);

            if (!take) {
                return;
            }

            best->found = true;
            best->dirIndex = dirIndex;
            best->choiceIndex = choiceIndex;
            best->costBuckets = costBuckets;
            best->newGlobalMaxChain = newGlobalMaxChain;
            best->newTotalSumSquares = newTotalSumSquares;
        };

        for (size_t i = 0; i < sets.size(); ++i) {
            const auto& set = sets[i];
            const auto& currentChoice = set.choices[selectedChoiceIndices[i]];

            for (size_t j = 0; j < set.choices.size(); ++j) {
                if (j == selectedChoiceIndices[i]) {
                    continue;
                }

                const auto& candidateChoice = set.choices[j];
                if (candidateChoice.tablesize < currentChoice.tablesize) {
                    continue;
                }

                const uint64_t costBuckets =
                    static_cast<uint64_t>(candidateChoice.tablesize - currentChoice.tablesize);
                if (costBuckets > (budgetBuckets - usedBuckets)) {
                    continue;
                }

                uint32_t newGlobalMaxChain = 0;
                for (size_t k = 0; k < sets.size(); ++k) {
                    const auto& choice = (k == i)
                        ? candidateChoice
                        : sets[k].choices[selectedChoiceIndices[k]];
                    newGlobalMaxChain = std::max(newGlobalMaxChain, choice.maxChain);
                }

                const uint64_t newTotalSumSquares =
                    current.totalSumSquares - currentChoice.sumSquares + candidateChoice.sumSquares;

                const bool improvesObjective =
                    (newGlobalMaxChain < current.globalMaxChain) ||
                    (newGlobalMaxChain == current.globalMaxChain && newTotalSumSquares < current.totalSumSquares);
                if (!improvesObjective) {
                    continue;
                }

                considerCandidate(&bestAny, i, j, costBuckets, newGlobalMaxChain, newTotalSumSquares);
                if (newGlobalMaxChain < current.globalMaxChain) {
                    considerCandidate(&bestReducer, i, j, costBuckets, newGlobalMaxChain, newTotalSumSquares);
                }
            }
        }

        const Candidate& best = (prioritizeMaxChainReduction && bestReducer.found) ? bestReducer : bestAny;
        if (!best.found) {
            break;
        }

        selectedChoiceIndices[best.dirIndex] = best.choiceIndex;
        usedBuckets += best.costBuckets;
        ++upgradesApplied;
        current = summarizeHashChoices(sets, selectedChoiceIndices);
    }

    uint64_t directoriesChanged = 0;
    for (size_t idx : selectedChoiceIndices) {
        if (idx != 0) {
            ++directoriesChanged;
        }
    }

    result.summary = std::move(current);
    result.usedBuckets = usedBuckets;
    result.upgradesApplied = upgradesApplied;
    result.directoriesChanged = directoriesChanged;
    return result;
}

double weightedMeanSquare(const HashSimulationSummary& summary) {
    return (summary.totalDirEntries == 0)
        ? 0.0
        : static_cast<double>(summary.totalSumSquares) / static_cast<double>(summary.totalDirEntries);
}

struct LookupStats {
    uint64_t comparisons = 0;
    uint64_t oneComparison = 0;
    uint64_t maxComparisons = 0;
    double average = 0.0;
};

LookupStats replayLookups(const HashSimulationSummary& summary,
                         const std::unordered_map<std::string, std::vector<std::string>>& dirChildren,
                         uint32_t secondBase = 0) {
    LookupStats stats;
    for (const DirReport& dir : summary.dirs) {
        const auto& names = dirChildren.at(dir.path == "/" ? "" : dir.path);
        const HashChoice& choice = dir.choice;
        if (names.empty())
            continue;

        // Reserve assigned anchors, then place overflow cells in index order,
        // matching the builder's coalesced chains over the sorted names.
        std::vector<std::vector<size_t>> groups(choice.tablesize);
        for (size_t i = 0; i < names.size(); ++i) {
            const uint32_t h1 = femtofs::hashName(names[i], choice.p, choice.tablesize);
            const uint32_t h2 = choice.dual ?
                femtofs::hashName(names[i], secondBase, choice.tablesize) : h1;
            const uint32_t anchor = groups[h2].size() < groups[h1].size() ||
                (groups[h2].size() == groups[h1].size() && h2 < h1) ? h2 : h1;
            groups[anchor].push_back(i);
        }
        std::vector<size_t> objectAt(choice.tablesize, names.size());
        std::vector<uint32_t> next(choice.tablesize, choice.tablesize);
        uint32_t overflow = 0;
        for (uint32_t anchor = 0; anchor < choice.tablesize; ++anchor) {
            uint32_t previous = choice.tablesize;
            for (const size_t object : groups[anchor]) {
                uint32_t position = anchor;
                if (previous != choice.tablesize) {
                    while (!groups[overflow].empty())
                        ++overflow;
                    position = overflow++;
                    next[previous] = position;
                }
                objectAt[position] = object;
                previous = position;
            }
        }

        for (size_t i = 0; i < names.size(); ++i) {
            const uint32_t h1 = femtofs::hashName(names[i], choice.p, choice.tablesize);
            const uint32_t h2 = choice.dual ?
                femtofs::hashName(names[i], secondBase, choice.tablesize) : h1;
            const std::array<uint32_t, 2> anchors{h1, h2};
            const size_t anchorCount = h1 == h2 ? 1 : 2;
            uint64_t comparisons = 0;
            bool found = false;
            for (size_t a = 0; a < anchorCount && !found; ++a) {
                uint32_t current = anchors[a];
                while (current != choice.tablesize && objectAt[current] != names.size()) {
                    ++comparisons;
                    if (objectAt[current] == i) {
                        found = true;
                        break;
                    }
                    current = next[current];
                }
            }
            if (!found)
                throw std::runtime_error("lookup replay could not reach an entry in " + dir.path);
            stats.comparisons += comparisons;
            stats.oneComparison += comparisons == 1;
            stats.maxComparisons = std::max(stats.maxComparisons, comparisons);
        }
    }
    if (summary.totalDirEntries != 0)
        stats.average = static_cast<double>(stats.comparisons) / summary.totalDirEntries;
    return stats;
}

double averageUnsuccessfulLookupStrcmp(const HashSimulationSummary& summary) {
    if (summary.totalDirEntries == 0) {
        return 0.0;
    }
    long double weighted = 0.0L;
    for (const auto& dir : summary.dirs) {
        if (dir.choice.tablesize == 0 || dir.n == 0) {
            continue;
        }
        const long double n = static_cast<long double>(dir.n);
        const long double t = static_cast<long double>(dir.choice.tablesize);
        // Every occupied cell can be a query anchor, including an overflow
        // cell. A chain of length L contributes L*(L+1)/2 comparisons over
        // all its starting cells. Independent uniform dual anchors coincide
        // with probability 1/t; lookup probes that anchor only once.
        const long double oneProbe = (dir.choice.sumSquares + n) / (2.0L * t);
        weighted += n * oneProbe * (dir.choice.dual ? 2.0L - 1.0L / t : 1.0L);
    }
    return static_cast<double>(weighted / static_cast<long double>(summary.totalDirEntries));
}

double globalLoadFactor(const HashSimulationSummary& summary) {
    return (summary.totalBuckets == 0)
        ? 0.0
        : static_cast<double>(summary.totalDirEntries) / static_cast<double>(summary.totalBuckets);
}

bool otherRead(const FsEntry& e) {
    return e.mode.size() >= 10 && e.mode[7] == 'r';
}

bool otherExec(const FsEntry& e) {
    return e.mode.size() >= 10 && (e.mode[9] == 'x' || e.mode[9] == 's' || e.mode[9] == 't');
}

void addHole(std::multimap<uint32_t, uint64_t>& holes, uint64_t off, uint32_t size) {
    if (size == 0) {
        return;
    }
    holes.emplace(size, off);
}

PackingStats packContents(std::vector<uint32_t> sizes, uint32_t imagePageSize) {
    PackingStats ps;
    if (imagePageSize == 0) {
        return ps;
    }

    sizes.erase(std::remove(sizes.begin(), sizes.end(), 0u), sizes.end());
    std::sort(sizes.begin(), sizes.end(), std::greater<uint32_t>());

    std::multimap<uint32_t, uint64_t> holes; // key=size, value=offset
    uint64_t tail = 0;

    for (uint32_t s : sizes) {
        ps.rawBytes += s;
        ++ps.contentCount;

        auto it = holes.lower_bound(s);
        if (it != holes.end()) {
            const uint32_t holeSize = it->first;
            const uint64_t holeOff = it->second;
            holes.erase(it);

            const uint32_t rem = holeSize - s;
            if (rem > 0) {
                addHole(holes, holeOff + s, rem);
            }
            continue;
        }

        const uint32_t pageOff = static_cast<uint32_t>(tail % imagePageSize);
        const bool needsAlignment = s >= imagePageSize ? pageOff != 0 :
            static_cast<uint64_t>(pageOff) + s > imagePageSize;
        if (needsAlignment) {
            const uint32_t gap = imagePageSize - pageOff;
            addHole(holes, tail, gap);
            tail += gap;
        }
        tail += s;
    }

    ps.paddedBytes = tail;
    ps.paddingBytes = ps.paddedBytes - ps.rawBytes;
    return ps;
}

PackingStats packClassifiedContents(const std::vector<ClassifiedContent>& contents,
                                    uint32_t imagePageSize,
                                    bool targetPublicPool) {
    std::vector<uint32_t> sizes;
    sizes.reserve(contents.size());
    for (const auto& content : contents) {
        if (content.isPublic == targetPublicPool && content.size != 0) {
            sizes.push_back(content.size);
        }
    }
    return packContents(std::move(sizes), imagePageSize);
}

VisibilitySplitPackingStats packContentsWithVisibilitySplit(const std::vector<ClassifiedContent>& contents,
                                                            uint32_t imagePageSize) {
    VisibilitySplitPackingStats stats;
    stats.publicPool = packClassifiedContents(contents, imagePageSize, true);
    stats.privatePool = packClassifiedContents(contents, imagePageSize, false);
    stats.combined.rawBytes = stats.publicPool.rawBytes + stats.privatePool.rawBytes;
    stats.combined.paddedBytes = stats.publicPool.paddedBytes + stats.privatePool.paddedBytes;
    stats.combined.paddingBytes = stats.publicPool.paddingBytes + stats.privatePool.paddingBytes;
    stats.combined.contentCount = stats.publicPool.contentCount + stats.privatePool.contentCount;
    return stats;
}

std::string prettyBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < (sizeof(units) / sizeof(units[0]))) {
        v /= 1024.0;
        ++u;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision((u == 0) ? 0 : 2) << v << ' ' << units[u];
    return oss.str();
}

uint64_t alignUp(uint64_t x, uint32_t a) {
    if (a == 0) {
        return x;
    }
    const uint64_t remainder = x % a;
    if (remainder == 0)
        return x;
    const uint64_t increment = a - remainder;
    if (x > std::numeric_limits<uint64_t>::max() - increment)
        throw std::runtime_error("alignment overflow");
    return x + increment;
}

uint32_t pageFormatCode(uint32_t pageSize) {
    uint32_t code = 0;
    for (uint32_t value = 256u; value < pageSize; value <<= 1u)
        ++code;
    return code;
}

bool encodedBlobBytes(uint64_t announcedBytes, uint32_t* outEncodedBytes) {
    if (outEncodedBytes == nullptr ||
        announcedBytes > std::numeric_limits<uint32_t>::max() - kBlobWord) {
        return false;
    }
    const uint64_t withTerminator = announcedBytes + 1u;
    const uint64_t encoded = (withTerminator + (kBlobWord - 1u)) & ~(static_cast<uint64_t>(kBlobWord) - 1u);
    *outEncodedBytes = static_cast<uint32_t>(encoded);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    try {
    const ProgramOptions options = parseArgs(argc, argv);
    const std::string& inputPath = options.inputPath;
    const uint32_t imagePageSize = options.imagePageSize;
    const uint32_t vmPageSize = options.vmPageSize;
    const auto& smallPrimes = fixedSmallPrimes();

    std::ifstream in(inputPath);
    if (!in) {
        std::cerr << "Cannot open input file: " << inputPath << "\n";
        return 1;
    }

    std::vector<FsEntry> entries;
    entries.reserve(8192);

    size_t parseErrors = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) {
            continue;
        }
        FsEntry e;
        if (!parseFindLsLine(line, e)) {
            ++parseErrors;
            continue;
        }
        entries.push_back(std::move(e));
    }

    std::unordered_map<std::string, const FsEntry*> entryByPath;
    entryByPath.reserve(entries.size());
    for (const auto& e : entries) {
        if (!entryByPath.emplace(e.path, &e).second)
            throw std::runtime_error("duplicate pathname: " + e.path);
    }
    for (const auto& e : entries) {
        if (e.path.empty())
            continue;
        const auto parent = entryByPath.find(e.parent);
        if (parent == entryByPath.end() || parent->second->type != 'd')
            throw std::runtime_error("missing or non-directory parent for: " + e.path);
    }

    std::unordered_map<std::string, bool> publicSearchableDirectoryByPath;
    publicSearchableDirectoryByPath.reserve(entries.size());
    std::unordered_map<std::string, bool> publicReadableSearchableDirectoryByPath;
    publicReadableSearchableDirectoryByPath.reserve(entries.size());

    std::function<bool(const std::string&)> isPublicSearchableDirectory = [&](const std::string& path) -> bool {
        const auto memoIt = publicSearchableDirectoryByPath.find(path);
        if (memoIt != publicSearchableDirectoryByPath.end()) {
            return memoIt->second;
        }

        const auto entryIt = entryByPath.find(path);
        if (entryIt == entryByPath.end() || entryIt->second->type != 'd') {
            publicSearchableDirectoryByPath[path] = false;
            return false;
        }

        const FsEntry& e = *entryIt->second;
        bool visible = false;
        if (path.empty()) {
            visible = otherExec(e);
        } else {
            visible = isPublicSearchableDirectory(e.parent) && otherExec(e);
        }

        publicSearchableDirectoryByPath[path] = visible;
        return visible;
    };

    std::function<bool(const std::string&)> isPublicReadableSearchableDirectory = [&](const std::string& path) -> bool {
        const auto memoIt = publicReadableSearchableDirectoryByPath.find(path);
        if (memoIt != publicReadableSearchableDirectoryByPath.end()) {
            return memoIt->second;
        }

        const auto entryIt = entryByPath.find(path);
        if (entryIt == entryByPath.end() || entryIt->second->type != 'd') {
            publicReadableSearchableDirectoryByPath[path] = false;
            return false;
        }

        const FsEntry& e = *entryIt->second;
        bool visible = false;
        if (path.empty()) {
            visible = otherRead(e) && otherExec(e);
        } else {
            visible = isPublicReadableSearchableDirectory(e.parent) && otherRead(e) && otherExec(e);
        }

        publicReadableSearchableDirectoryByPath[path] = visible;
        return visible;
    };

    // A file payload is public-eligible iff one reachable path has world-read
    // on the file and world-search on each directory along the path.
    auto isPublicRegularFilePath = [&](const FsEntry& e) -> bool {
        if (e.type != '-') {
            return false;
        }
        if (!otherRead(e)) {
            return false;
        }
        return isPublicSearchableDirectory(e.parent);
    };

    auto isPublicFilenamePath = [&](const FsEntry& e) -> bool {
        if (e.path.empty()) {
            return false;
        }
        return isPublicReadableSearchableDirectory(e.parent);
    };

    std::set<std::string> directories;
    directories.insert(""); // root

    std::unordered_map<std::string, std::vector<std::string>> dirChildren;
    dirChildren.reserve(1024);

    std::unordered_map<uint64_t, uint64_t> filePayloadByInode;
    std::unordered_map<uint64_t, uint32_t> filePathCountByInode;
    std::unordered_map<uint64_t, bool> filePayloadPublicByInode;
    std::unordered_map<uint64_t, std::string> fileAttrKeyByInode;
    std::unordered_map<uint64_t, uint32_t> symlinkPathCountByInode;
    std::unordered_map<uint64_t, uint32_t> fifoPathCountByInode;
    std::unordered_map<uint64_t, std::string> fifoAttrKeyByInode;
    filePayloadByInode.reserve(4096);
    filePathCountByInode.reserve(4096);
    filePayloadPublicByInode.reserve(4096);
    fileAttrKeyByInode.reserve(4096);
    symlinkPathCountByInode.reserve(1024);
    fifoPathCountByInode.reserve(128);
    fifoAttrKeyByInode.reserve(128);

    std::set<std::string> uniqueNames;
    std::set<std::string> uniqueSymlinkTargets;
    std::unordered_map<std::string, bool> namePublicByValue;
    namePublicByValue.reserve(4096);

    uint64_t countDir = 0;
    uint64_t rootDirectoryCount = 0;
    uint64_t countFilePath = 0;
    uint64_t countSymlink = 0;
    uint64_t countFifoPath = 0;
    uint64_t unsupported = 0;

    uint64_t duplicateInodeSizeMismatch = 0;
    uint64_t duplicateInodeAttrMismatch = 0;
    uint64_t duplicateFifoAttrMismatch = 0;
    std::set<std::string> uniqueAttrKeys;

    auto makeAttrKey = [](const FsEntry& e) {
        return e.mode + '\x1f' + e.owner + '\x1f' + e.group;
    };

    for (const auto& e : entries) {
        if (e.type == 'd') {
            ++countDir;
            uniqueAttrKeys.insert(makeAttrKey(e));
            if (e.path.empty())
                ++rootDirectoryCount;
            directories.insert(e.path);
            if (!e.path.empty()) {
                dirChildren[e.parent].push_back(e.name);
                uniqueNames.insert(e.name);
                const bool publicName = isPublicFilenamePath(e);
                namePublicByValue[e.name] = namePublicByValue[e.name] || publicName;
            }
            continue;
        }
        if (e.type == '-') {
            ++countFilePath;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            const bool publicName = isPublicFilenamePath(e);
            namePublicByValue[e.name] = namePublicByValue[e.name] || publicName;
            const bool publicPath = isPublicRegularFilePath(e);
            auto [it, inserted] = filePayloadByInode.emplace(e.inode, e.size);
            if (!inserted && it->second != e.size) {
                ++duplicateInodeSizeMismatch;
            }
            const std::string attrKey = makeAttrKey(e);
            auto [attrIt, attrInserted] = fileAttrKeyByInode.emplace(e.inode, attrKey);
            if (!attrInserted && attrIt->second != attrKey) {
                ++duplicateInodeAttrMismatch;
            }
            ++filePathCountByInode[e.inode];
            filePayloadPublicByInode[e.inode] = filePayloadPublicByInode[e.inode] || publicPath;
            continue;
        }
        if (e.type == 'l') {
            ++countSymlink;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            const bool publicName = isPublicFilenamePath(e);
            namePublicByValue[e.name] = namePublicByValue[e.name] || publicName;
            uniqueSymlinkTargets.insert(e.symlinkTarget);
            ++symlinkPathCountByInode[e.inode];
            uniqueAttrKeys.insert(makeAttrKey(e));
            continue;
        }
        if (e.type == 'p') {
            ++countFifoPath;
            dirChildren[e.parent].push_back(e.name);
            uniqueNames.insert(e.name);
            const bool publicName = isPublicFilenamePath(e);
            namePublicByValue[e.name] = namePublicByValue[e.name] || publicName;
            const std::string attrKey = makeAttrKey(e);
            auto [attrIt, attrInserted] = fifoAttrKeyByInode.emplace(e.inode, attrKey);
            if (!attrInserted && attrIt->second != attrKey) {
                ++duplicateFifoAttrMismatch;
            }
            ++fifoPathCountByInode[e.inode];
            continue;
        }
        ++unsupported;
    }

    for (const auto& [inode, attrKey] : fileAttrKeyByInode) {
        (void)inode;
        uniqueAttrKeys.insert(attrKey);
    }
    for (const auto& [inode, attrKey] : fifoAttrKeyByInode) {
        (void)inode;
        uniqueAttrKeys.insert(attrKey);
    }

    // Ensure every parsed directory has a child vector, including empty dirs.
    for (const auto& d : directories) {
        (void)dirChildren[d];
    }
    for (auto& [directory, names] : dirChildren) {
        (void)directory;
        std::sort(names.begin(), names.end(), [](const std::string& lhs, const std::string& rhs) {
            return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                [](unsigned char left, unsigned char right) { return left < right; });
        });
    }

    uint64_t fileObjectCount = filePayloadByInode.size();
    uint64_t fifoObjectCount = fifoPathCountByInode.size();
    uint64_t hardlinkObjectCount = 0;
    for (const auto& [inode, count] : filePathCountByInode) {
        (void)inode;
        if (count > 1) {
            hardlinkObjectCount += (count - 1);
        }
    }
    uint64_t fifoHardlinkViolatingPaths = 0;
    for (const auto& [inode, count] : fifoPathCountByInode) {
        (void)inode;
        if (count > 1) {
            fifoHardlinkViolatingPaths += (count - 1);
        }
    }
    uint64_t symlinkHardlinkViolatingPaths = 0;
    for (const auto& [inode, count] : symlinkPathCountByInode) {
        (void)inode;
        if (count > 1) {
            symlinkHardlinkViolatingPaths += (count - 1);
        }
    }

    if (parseErrors != 0 ||
        rootDirectoryCount != 1 ||
        unsupported != 0 ||
        duplicateInodeSizeMismatch != 0 ||
        duplicateInodeAttrMismatch != 0 ||
        duplicateFifoAttrMismatch != 0 ||
        symlinkHardlinkViolatingPaths != 0 ||
        fifoHardlinkViolatingPaths != 0) {
        std::cerr << "Input violates femtoFS source-tree mapping policy:\n";
        if (parseErrors != 0)
            std::cerr << "  unparsed input records: " << parseErrors << "\n";
        if (rootDirectoryCount != 1) {
            std::cerr << "  root directory records: " << rootDirectoryCount
                      << " (required exactly 1)\n";
        }
        if (unsupported != 0) {
            std::cerr << "  unsupported source inode kinds: " << unsupported << "\n";
        }
        if (duplicateInodeSizeMismatch != 0) {
            std::cerr << "  regular-file inode size mismatches: " << duplicateInodeSizeMismatch << "\n";
        }
        if (duplicateInodeAttrMismatch != 0) {
            std::cerr << "  regular-file inode attr mismatches: " << duplicateInodeAttrMismatch << "\n";
        }
        if (duplicateFifoAttrMismatch != 0) {
            std::cerr << "  fifo inode attr mismatches: " << duplicateFifoAttrMismatch << "\n";
        }
        if (symlinkHardlinkViolatingPaths != 0) {
            std::cerr << "  symlink hardlink violating paths: " << symlinkHardlinkViolatingPaths << "\n";
        }
        if (fifoHardlinkViolatingPaths != 0) {
            std::cerr << "  fifo hardlink violating paths: " << fifoHardlinkViolatingPaths << "\n";
        }
        return 2;
    }

    const HashSimulationSummary baseline = simulateHashing(directories, dirChildren, smallPrimes);
    const double baselineWeightedMeanSquare = weightedMeanSquare(baseline);
    const double baselineAvgSuccessfulLookupStrcmp = replayLookups(baseline, dirChildren).average;
    const double baselineAvgUnsuccessfulLookupStrcmp = averageUnsuccessfulLookupStrcmp(baseline);
    const double baselineGlobalLoadFactor = globalLoadFactor(baseline);

    struct MixedTrial {
        uint32_t p2 = 0;
        MixedDualHashResult result;
    };

    std::vector<MixedTrial> mixedTrials;
    mixedTrials.clear();
    const MixedTrial* bestMixed = nullptr;

    if (options.dualHashExperiment) {
        constexpr uint32_t kBigPrimeMin = (1u << 8) + 1u; // strictly greater than 2^8
        constexpr uint32_t kBigPrimeMax = (1u << 24);     // strictly less than 2^24

        const auto singleChoicesByDir = buildSingleHashChoiceByDirectoryKey(directories, dirChildren, smallPrimes);
        const size_t sampleCount = std::max<size_t>(1, options.dualHashSamples);
        const auto sampledPrimes = sampleRandomPrimes(
            kBigPrimeMin,
            kBigPrimeMax,
            sampleCount,
            options.dualHashSeed);

        mixedTrials.reserve(sampledPrimes.size());
        for (uint32_t p2 : sampledPrimes) {
            MixedDualHashResult result = simulateHashingMixedDualHash(
                directories,
                dirChildren,
                singleChoicesByDir,
                smallPrimes,
                p2,
                options.dualHashHardThreshold);
            mixedTrials.push_back(MixedTrial{p2, std::move(result)});
        }

        std::sort(mixedTrials.begin(), mixedTrials.end(), [](const MixedTrial& a, const MixedTrial& b) {
            const auto& sa = a.result.summary;
            const auto& sb = b.result.summary;
            if (sa.globalMaxChain != sb.globalMaxChain) {
                return sa.globalMaxChain < sb.globalMaxChain;
            }
            const double wa = weightedMeanSquare(sa);
            const double wb = weightedMeanSquare(sb);
            if (wa != wb) {
                return wa < wb;
            }
            if (sa.totalBuckets != sb.totalBuckets) {
                return sa.totalBuckets < sb.totalBuckets;
            }
            if (a.result.missScore != b.result.missScore) {
                return a.result.missScore < b.result.missScore;
            }
            return a.p2 < b.p2;
        });

        if (!mixedTrials.empty() &&
            mixedTrials.front().result.hardDirectories != 0) {
            bestMixed = &mixedTrials.front();
        }
    }

    const uint64_t dirObjectCount = countDir - 1u; // root descriptor is in header
    const uint64_t symlinkObjectCount = countSymlink;
    const uint64_t metadataObjects = fileObjectCount + hardlinkObjectCount +
        dirObjectCount + symlinkObjectCount + fifoObjectCount;
    const uint64_t attrObjectCount = uniqueAttrKeys.size();
    auto cellCountFor = [&](const HashSimulationSummary& summary) {
        const uint64_t attrReuse = std::min<uint64_t>(
            attrObjectCount, summary.totalEmptyBuckets);
        return metadataObjects + summary.totalBuckets +
               (attrObjectCount - attrReuse);
    };

    HashSimulationSummary selectedSummary = bestMixed == nullptr ?
        baseline : bestMixed->result.summary;
    bool hashBudgetFallback = false;
    if (cellCountFor(selectedSummary) > femtofs::kMaxCellCount) {
        selectedSummary = simulateMinimumHashing(
            directories, dirChildren, smallPrimes);
        hashBudgetFallback = true;
    }
    const bool selectedMixed = bestMixed != nullptr && !hashBudgetFallback;
    const double selectedWeightedMeanSquare = weightedMeanSquare(selectedSummary);
    const LookupStats selectedLookups = replayLookups(
        selectedSummary, dirChildren, selectedMixed ? bestMixed->p2 : 0);
    const double selectedAvgSuccessfulLookupStrcmp = selectedLookups.average;
    const double selectedAvgUnsuccessfulLookupStrcmp = averageUnsuccessfulLookupStrcmp(selectedSummary);
    const double selectedGlobalLoadFactor = globalLoadFactor(selectedSummary);
    const uint64_t filelikeRoleCount = fileObjectCount + symlinkObjectCount + fifoObjectCount;
    const uint64_t dirRoleCount = dirObjectCount;
    const uint64_t hardlinkRoleCount = hardlinkObjectCount;
    const uint64_t bucketRoleCount = selectedSummary.totalBuckets;
    const uint64_t objectRoleCountTotal =
        dirRoleCount + filelikeRoleCount + hardlinkRoleCount + bucketRoleCount;
    const uint64_t attrCellsReused = std::min<uint64_t>(attrObjectCount, selectedSummary.totalEmptyBuckets);
    const uint64_t attrCellsExtended = attrObjectCount - attrCellsReused;

    // Content sizes for packing simulation.
    std::vector<uint32_t> contentSizes;
    std::vector<ClassifiedContent> classifiedContents;
    contentSizes.reserve(filePayloadByInode.size() + uniqueNames.size() + uniqueSymlinkTargets.size());
    classifiedContents.reserve(filePayloadByInode.size() + uniqueNames.size() + uniqueSymlinkTargets.size());

    uint64_t zeroLenFilePayloads = 0;
    uint64_t smallFilePayloads = 0;
    uint64_t largePageAlignedPayloads = 0;
    uint64_t largePartialPayloads = 0;
    uint64_t publicFilePayloads = 0;
    uint64_t privateFilePayloads = 0;
    uint64_t publicFilenameStrings = 0;
    uint64_t privateFilenameStrings = 0;
    uint64_t publicFilenameContentBlobs = 0;
    uint64_t privateFilenameContentBlobs = 0;
    uint64_t privateSymlinkTargetContentBlobs = 0;
    uint64_t publicSmallFilePayloads = 0;
    uint64_t privateSmallFilePayloads = 0;
    uint64_t publicZeroLenFilePayloads = 0;
    uint64_t privateZeroLenFilePayloads = 0;
    uint64_t filesAtMostVmPage = 0;
    uint64_t vmAlignedLargeFiles = 0;
    uint64_t vmAlignedLargeFilesWithTail = 0;
    uint64_t potentiallyUnalignedLargeFiles = 0;
    uint64_t alignedPublicMappableFiles = 0;
    uint64_t payloadEncodingOverflow = 0;
    uint64_t stringEncodingOverflow = 0;

    for (const auto& [inode, size] : filePayloadByInode) {
        const bool isPublic = filePayloadPublicByInode[inode];
        uint32_t encodedSize = 0;
        if (!encodedBlobBytes(size, &encodedSize)) {
            ++payloadEncodingOverflow;
            continue;
        }
        contentSizes.push_back(encodedSize);
        classifiedContents.push_back(ClassifiedContent{encodedSize, isPublic});
        if (isPublic) {
            ++publicFilePayloads;
        } else {
            ++privateFilePayloads;
        }
        if (size == 0) {
            ++zeroLenFilePayloads;
            if (isPublic) {
                ++publicZeroLenFilePayloads;
            } else {
                ++privateZeroLenFilePayloads;
            }
            continue;
        }
        if (size <= vmPageSize) {
            ++filesAtMostVmPage;
        } else if (vmPageSize <= imagePageSize &&
                   encodedSize >= imagePageSize) {
            ++vmAlignedLargeFiles;
            if ((size % vmPageSize) != 0)
                ++vmAlignedLargeFilesWithTail;
        } else {
            ++potentiallyUnalignedLargeFiles;
        }
        if (isPublic && vmPageSize <= imagePageSize &&
            encodedSize >= imagePageSize) {
            ++alignedPublicMappableFiles;
        }

        if (size < imagePageSize) {
            ++smallFilePayloads;
            if (isPublic) {
                ++publicSmallFilePayloads;
            } else {
                ++privateSmallFilePayloads;
            }
        } else if ((size % imagePageSize) == 0) {
            ++largePageAlignedPayloads;
        } else {
            ++largePartialPayloads;
        }
    }

    for (const auto& name : uniqueNames) {
        if (namePublicByValue[name]) {
            ++publicFilenameStrings;
        } else {
            ++privateFilenameStrings;
        }
        uint32_t encodedSize = 0;
        if (!encodedBlobBytes(name.size(), &encodedSize)) {
            ++stringEncodingOverflow;
            continue;
        }
        const bool isPublic = namePublicByValue[name];
        contentSizes.push_back(encodedSize);
        classifiedContents.push_back(ClassifiedContent{encodedSize, isPublic});
        if (isPublic) {
            ++publicFilenameContentBlobs;
        } else {
            ++privateFilenameContentBlobs;
        }
    }

    for (const auto& target : uniqueSymlinkTargets) {
        uint32_t encodedSize = 0;
        if (!encodedBlobBytes(target.size(), &encodedSize)) {
            ++stringEncodingOverflow;
            continue;
        }
        contentSizes.push_back(encodedSize);
        classifiedContents.push_back(ClassifiedContent{encodedSize, false});
        ++privateSymlinkTargetContentBlobs;
    }

    const PackingStats packing = packContents(contentSizes, imagePageSize);
    const VisibilitySplitPackingStats splitPacking =
        packContentsWithVisibilitySplit(classifiedContents, imagePageSize);

    const uint64_t objectTableEntries = metadataObjects + selectedSummary.totalBuckets + attrCellsExtended;
    const uint64_t objectTableBytes = objectTableEntries * femtofs::kCellSize;
    const uint64_t headerBytes = femtofs::kHeaderSize;
    const uint64_t publicOff = alignUp(headerBytes + objectTableBytes,
                                       imagePageSize);
    const uint64_t unsplitImageSize = alignUp(
        publicOff + packing.paddedBytes, imagePageSize);
    const uint64_t privateOff = alignUp(
        publicOff + splitPacking.publicPool.paddedBytes, imagePageSize);
    const uint64_t splitImageBytesEstimate = alignUp(
        privateOff + splitPacking.privatePool.paddedBytes, imagePageSize);
    const uint64_t splitContentRegionBytes = splitImageBytesEstimate - publicOff;
    const uint64_t publicPartSpan = privateOff - publicOff;
    const uint64_t privatePartSpan = splitImageBytesEstimate - privateOff;
    const uint64_t unsplitTotalPadding =
        unsplitImageSize - publicOff - packing.rawBytes;
    const uint64_t splitTotalPadding =
        splitContentRegionBytes - splitPacking.combined.rawBytes;
    const bool vmForcesClean = vmPageSize > imagePageSize;
    const uint64_t cleanAtMostOneCopyFiles =
        filesAtMostVmPage + vmAlignedLargeFiles;
    const uint64_t cleanCopiedPageUpperBoundForThoseFiles =
        filesAtMostVmPage + vmAlignedLargeFilesWithTail;
    const uint64_t publicMappableFilePayloads = publicFilePayloads - publicZeroLenFilePayloads;
    const uint64_t privateMappableFilePayloads = privateFilePayloads - privateZeroLenFilePayloads;
    const uint64_t leakingAlignedPublicCandidates = vmForcesClean ? 0 :
        alignedPublicMappableFiles;
    const uint64_t dirtyShiftedPublicCandidates = vmForcesClean ? 0 :
        publicSmallFilePayloads;
    const bool metadataCellLimitExceeded =
        objectTableEntries == 0 || objectTableEntries > femtofs::kMaxCellCount;
    const bool objectLimitExceeded =
        metadataObjects >= femtofs::kMaxDirectoryEntries;
    const bool imageLimitExceeded = splitImageBytesEstimate >= (1ull << 32);

    const size_t topN = std::min<size_t>(12, selectedSummary.dirs.size());

    std::cout << "Input file: " << inputPath << "\n";
    std::cout << "Parsed lines: " << entries.size() << " (parse errors: " << parseErrors << ")\n\n";

    std::cout << "Format model\n";
    std::cout << "  specification baseline:       0x" << std::hex
              << std::setw(4) << std::setfill('0') << femtofs::kVersion
              << std::dec << std::setfill(' ') << "\n";
    std::cout << "  byte order code:               0 (little-endian)\n";
    std::cout << "  image PAGE_SIZE:               " << imagePageSize << "\n";
    std::cout << "  encoded page-size code:        "
              << pageFormatCode(imagePageSize) << "\n";
    std::cout << "  kernel VM_PAGE_SIZE:           " << vmPageSize << "\n";
    std::cout << "  effective mmap mode forced:    "
              << (vmForcesClean ? "clean" : "none") << "\n";
    if (imagePageSize != femtofs::kPageSize) {
        std::cout << "  compatibility:                future-format experiment, not a valid 0x0100 image model\n";
    }
    std::cout << "\n";

    std::cout << "Filesystem inventory\n";
    std::cout << "  directories (including root): " << countDir << "\n";
    std::cout << "  regular file paths:           " << countFilePath << "\n";
    std::cout << "  symlinks:                     " << countSymlink << "\n";
    std::cout << "  fifo paths:                   " << countFifoPath << "\n";
    std::cout << "  unsupported entry types:      " << unsupported << "\n";
    if (duplicateInodeSizeMismatch != 0) {
        std::cout << "  WARNING: inode size mismatches: " << duplicateInodeSizeMismatch << "\n";
    }
    if (duplicateInodeAttrMismatch != 0) {
        std::cout << "  WARNING: inode attr mismatches: " << duplicateInodeAttrMismatch << "\n";
    }
    if (duplicateFifoAttrMismatch != 0) {
        std::cout << "  WARNING: fifo inode attr mismatches: " << duplicateFifoAttrMismatch << "\n";
    }
    if (fifoHardlinkViolatingPaths != 0) {
        std::cout << "  WARNING: fifo hardlink violating paths: " << fifoHardlinkViolatingPaths << "\n";
    }
    std::cout << "\n";

    std::cout << "femtoFS object model estimate\n";
    std::cout << "  file objects (unique inodes): " << fileObjectCount << "\n";
    std::cout << "  hardlink objects:             " << hardlinkObjectCount << "\n";
    std::cout << "  dir objects (non-root):       " << dirObjectCount << "\n";
    std::cout << "  symlink objects:              " << symlinkObjectCount << "\n";
    std::cout << "  fifo objects:                 " << fifoObjectCount << "\n";
    std::cout << "  non-root objects total:       " << metadataObjects << "\n";
    std::cout << "  visible objects incl. root:   " << metadataObjects + 1u
              << " (limit <= " << femtofs::kMaxDirectoryEntries << ")\n";
    std::cout << "  deduped attr cells incl. root:" << attrObjectCount << "\n";
    std::cout << "  attr cells reusing empties:   " << attrCellsReused << "\n";
    std::cout << "  attr cells extending table:   " << attrCellsExtended << "\n";
    std::cout << "  hash buckets total:           " << selectedSummary.totalBuckets << "\n";
    std::cout << "  object_role total:            " << objectRoleCountTotal << "\n";
    std::cout << "    role=dir:                   " << dirRoleCount << "\n";
    std::cout << "    role=filelike:              " << filelikeRoleCount << "\n";
    std::cout << "    role=hardlink:              " << hardlinkRoleCount << "\n";
    std::cout << "    role=bucket:                " << bucketRoleCount << "\n";
    std::cout << "  metadata table cells total:   " << objectTableEntries
              << " (limit <= " << femtofs::kMaxCellCount << ")\n";
    std::cout << "  metadata table bytes:         " << prettyBytes(objectTableBytes) << "\n";
    std::cout << "  object limit status:          "
              << (objectLimitExceeded ? "EXCEEDED" : "ok") << "\n";
    std::cout << "  metadata cell limit status:   "
              << (metadataCellLimitExceeded ? "EXCEEDED" : "ok") << "\n";
    std::cout << "\n";

    const double avgEmptiesPerDir =
        directories.empty() ? 0.0 : static_cast<double>(selectedSummary.totalEmptyBuckets) / static_cast<double>(directories.size());

    std::cout << "Directory hashing simulation (selected policy)\n";
    if (selectedMixed) {
        std::cout << "  selected policy:              mixed SINGLE/DUAL\n";
        std::cout << "  hash2_base:                   " << bestMixed->p2 << "\n";
        std::cout << "  hard directory rule:          baseline max_chain > " << options.dualHashHardThreshold << "\n";
        std::cout << "  hard directories switched:    " << bestMixed->result.hardDirectories
                  << " / " << directories.size() << "\n";
        std::cout << "  hard directories entry share: " << bestMixed->result.hardEntries
                  << " / " << selectedSummary.totalDirEntries << "\n";
    } else if (hashBudgetFallback) {
        std::cout << "  selected policy:              minimum SINGLE fallback (tablesize=N)\n";
        std::cout << "  hash2_base:                   0\n";
        std::cout << "  reason:                       preferred plan exceeds shared metadata-cell budget\n";
    } else {
        std::cout << "  selected policy:              single-hash baseline\n";
    }
    std::cout << "  ceiling policy:              all sizes in [N, next_prime(2*N)], cap "
              << femtofs::kMaxTableSize << "\n";
    std::cout << "  directories simulated:        " << directories.size() << "\n";
    std::cout << "  total dir entries (N sum):    " << selectedSummary.totalDirEntries << "\n";
    std::cout << "  total buckets:                " << selectedSummary.totalBuckets << "\n";
    std::cout << "  total empty buckets:          " << selectedSummary.totalEmptyBuckets << "\n";
    std::cout << "  avg empty buckets / dir:      " << std::fixed << std::setprecision(2) << avgEmptiesPerDir << "\n";
    std::cout << "  global load factor N/T:       " << std::fixed << std::setprecision(4) << selectedGlobalLoadFactor << "\n";
    std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << selectedWeightedMeanSquare << "\n";
    std::cout << "  avg strcmp per successful lookup: " << std::fixed << std::setprecision(4)
              << selectedAvgSuccessfulLookupStrcmp << "\n";
    std::cout << "  successful lookup comparisons: " << selectedLookups.comparisons << "\n";
    std::cout << "  lookups with one comparison:   " << selectedLookups.oneComparison
              << " / " << selectedSummary.totalDirEntries << "\n";
    std::cout << "  max comparisons per lookup:    " << selectedLookups.maxComparisons << "\n";
    std::cout << "  avg strcmp per unsuccessful lookup: " << std::fixed << std::setprecision(4)
              << selectedAvgUnsuccessfulLookupStrcmp << "\n";
    std::cout << "  perfect-hash dirs (score=1):  " << selectedSummary.dirsPerfect << "\n";
    std::cout << "  dirs hitting ceiling:         " << selectedSummary.dirsHitCeiling << "\n";
    std::cout << "  dirs fallback(score>=1.1):    " << selectedSummary.dirsFallback << "\n";
    std::cout << "  max chain observed:           " << selectedSummary.globalMaxChain << "\n";
    std::cout << "  dirs violating N<2^15:        " << selectedSummary.dirsOverHardLimit << "\n";
    if (selectedMixed) {
        std::cout << "  baseline max chain:           " << baseline.globalMaxChain << "\n";
        std::cout << "  baseline weighted mean-square:" << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
        std::cout << "  baseline avg strcmp success:  " << std::fixed << std::setprecision(4) << baselineAvgSuccessfulLookupStrcmp << "\n";
        std::cout << "  baseline avg strcmp miss:     " << std::fixed << std::setprecision(4) << baselineAvgUnsuccessfulLookupStrcmp << "\n";
    }
    std::cout << "\n";

    if (bestMixed != nullptr) {
        const auto& bestSummary = bestMixed->result.summary;
        const double bestWeightedMeanSquare = weightedMeanSquare(bestSummary);
        const double bestAvgSuccessfulLookupStrcmp = replayLookups(bestSummary, dirChildren, bestMixed->p2).average;
        const double bestGlobalLoadFactor = globalLoadFactor(bestSummary);
        const int64_t deltaBuckets = static_cast<int64_t>(bestSummary.totalBuckets) - static_cast<int64_t>(baseline.totalBuckets);
        const int64_t deltaMetaBytes = deltaBuckets * femtofs::kCellSize;
        const size_t topMixedN = std::min<size_t>(8, bestSummary.dirs.size());

        std::cout << "Mixed SINGLE/DUAL tuning details\n";
        std::cout << "  p1 candidates:                "
                  << femtofs::kSmallPrimes.size()
                  << "-entry SMALL_PRIMES index map\n";
        std::cout << "  hash2_base search range:      (" << (1u << 8) << ", " << (1u << 24) << ")\n";
        std::cout << "  hard directory rule:          baseline max_chain > " << options.dualHashHardThreshold << "\n";
        std::cout << "  sampled random primes:        " << mixedTrials.size() << "\n";
        std::cout << "  RNG seed:                     " << options.dualHashSeed << "\n";
        std::cout << "  best hash2_base:              " << bestMixed->p2 << "\n";
        std::cout << "  hard directories switched:    " << bestMixed->result.hardDirectories
                  << " / " << directories.size() << "\n";
        std::cout << "  hard directories entry share: " << bestMixed->result.hardEntries
                  << " / " << baseline.totalDirEntries << "\n";
        std::cout << "  total buckets:                " << bestSummary.totalBuckets
                  << " (delta " << std::showpos << deltaBuckets << std::noshowpos << ")\n";
        std::cout << "  metadata bytes delta:         " << std::showpos << deltaMetaBytes << " B" << std::noshowpos << "\n";
        std::cout << "  max chain observed:           " << bestSummary.globalMaxChain
                  << " (baseline " << baseline.globalMaxChain << ")\n";
        std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << bestWeightedMeanSquare
                  << " (baseline " << baselineWeightedMeanSquare << ")\n";
        std::cout << "  global load factor N/T:       " << std::fixed << std::setprecision(4) << bestGlobalLoadFactor
                  << " (baseline " << baselineGlobalLoadFactor << ")\n";
        std::cout << "  avg strcmp successful:        " << std::fixed << std::setprecision(4) << bestAvgSuccessfulLookupStrcmp
                  << " (baseline " << baselineAvgSuccessfulLookupStrcmp << ")\n";
        std::cout << "  avg strcmp unsuccessful (uniform anchors): " << std::fixed << std::setprecision(4)
                  << averageUnsuccessfulLookupStrcmp(bestSummary)
                  << " (baseline " << baselineAvgUnsuccessfulLookupStrcmp << ")\n";

        std::cout << "  top random-prime candidates\n";
        std::cout << "    hash2_base | max_chain | weighted_ms | buckets | miss_uniform_anchors\n";
        for (size_t i = 0; i < std::min<size_t>(5, mixedTrials.size()); ++i) {
            const auto& t = mixedTrials[i];
            const auto& s = t.result.summary;
            std::cout << "    " << t.p2
                      << " | " << s.globalMaxChain
                      << " | " << std::fixed << std::setprecision(4) << weightedMeanSquare(s)
                      << " | " << s.totalBuckets
                      << " | " << std::fixed << std::setprecision(4) << averageUnsuccessfulLookupStrcmp(s)
                      << "\n";
        }

        std::cout << "  worst dirs by max_chain (best mixed)\n";
        std::cout << "    path | N | tablesize | p1_index | p1 | score | max_chain\n";
        for (size_t i = 0; i < topMixedN; ++i) {
            const auto& d = bestSummary.dirs[i];
            std::cout << "    " << d.path
                      << " | " << d.n
                      << " | " << d.choice.tablesize
                      << " | " << smallPrimeIndex(d.choice.p)
                      << " | " << d.choice.p
                      << " | " << std::fixed << std::setprecision(4) << d.choice.score
                      << " | " << d.choice.maxChain
                      << "\n";
        }
        std::cout << "\n";
    }

    if (options.budgetedHashExperiment) {
        const auto hashCandidateSets = buildHashCandidateSets(directories, dirChildren, smallPrimes);

        std::vector<uint32_t> budgetsKiB = options.budgetedBudgetsKiB;
        budgetsKiB.push_back(0u);
        std::sort(budgetsKiB.begin(), budgetsKiB.end());
        budgetsKiB.erase(std::unique(budgetsKiB.begin(), budgetsKiB.end()), budgetsKiB.end());

        std::cout << "Budgeted hash tuner (greedy)\n";
        std::cout << "  objective:                   minimize global max chain, then total sum-squares\n";
        std::cout << "  target max chain:            " << options.budgetedTargetMaxChain << "\n";
        std::cout << "  profile | budget_kib | used_buckets | used_bytes | max_chain | weighted_ms | avg_strcmp_ok | avg_strcmp_miss | dirs_changed | upgrades\n";

        std::cout << "  baseline-policy"
                  << " | " << 0
                  << " | " << 0
                  << " | " << 0
                  << " | " << baseline.globalMaxChain
                  << " | " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare
                  << " | " << std::fixed << std::setprecision(4) << baselineAvgSuccessfulLookupStrcmp
                  << " | " << std::fixed << std::setprecision(4) << baselineAvgUnsuccessfulLookupStrcmp
                  << " | " << 0
                  << " | " << 0
                  << "\n";

        for (uint32_t budgetKiB : budgetsKiB) {
            const uint64_t budgetBuckets = static_cast<uint64_t>(budgetKiB) * 64u; // 1 KiB / 16-byte bucket
            const BudgetedHashTuningResult tuned =
                runBudgetedHashTuning(hashCandidateSets, budgetBuckets, options.budgetedTargetMaxChain);

            const double tunedWeightedMeanSquare = weightedMeanSquare(tuned.summary);
            const double tunedAvgSuccessfulLookupStrcmp = replayLookups(tuned.summary, dirChildren).average;
            const double tunedAvgUnsuccessfulLookupStrcmp = averageUnsuccessfulLookupStrcmp(tuned.summary);
            const uint64_t usedBytes = tuned.usedBuckets * femtofs::kCellSize;

            std::cout << "  tuned-budget"
                      << " | " << budgetKiB
                      << " | " << tuned.usedBuckets
                      << " | " << usedBytes
                      << " | " << tuned.summary.globalMaxChain
                      << " | " << std::fixed << std::setprecision(4) << tunedWeightedMeanSquare
                      << " | " << std::fixed << std::setprecision(4) << tunedAvgSuccessfulLookupStrcmp
                      << " | " << std::fixed << std::setprecision(4) << tunedAvgUnsuccessfulLookupStrcmp
                      << " | " << tuned.directoriesChanged
                      << " | " << tuned.upgradesApplied
                      << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "Worst directories by score\n";
    std::cout << "  path | N | tablesize | p1_index | p1 | score | max_chain | empties | hit_ceiling\n";
    for (size_t i = 0; i < topN; ++i) {
        const auto& d = selectedSummary.dirs[i];
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << d.choice.tablesize
                  << " | " << smallPrimeIndex(d.choice.p)
                  << " | " << d.choice.p
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << " | " << d.choice.maxChain
                  << " | " << d.choice.empties
                  << " | " << (d.choice.hitCeiling ? "yes" : "no")
                  << "\n";
    }
    std::cout << "\n";

    std::cout << "Fallback directories (hit ceiling with score >= 1.1)\n";
    std::cout << "  path | N | score\n";
    for (const auto& d : selectedSummary.dirs) {
        if (!(d.choice.hitCeiling && d.choice.score >= 1.1)) {
            continue;
        }
        std::cout << "  " << d.path
                  << " | " << d.n
                  << " | " << std::fixed << std::setprecision(4) << d.choice.score
                  << "\n";
    }
    std::cout << "\n";

    if (options.fixedBaseExperiment) {
        constexpr uint32_t kLargePrimeMin = 1u << 16;
        constexpr uint32_t kLargePrimeMax = 1u << 23;
        constexpr uint32_t kHybridFallbackBase = 1197923u;

        struct FixedBaseTrial {
            uint32_t prime = 0;
            HashSimulationSummary summary;
        };

        const auto sampledPrimes = sampleRandomPrimes(
            kLargePrimeMin,
            kLargePrimeMax,
            options.fixedBaseSamples,
            options.fixedBaseSeed);

        std::vector<FixedBaseTrial> trials;
        trials.reserve(sampledPrimes.size());

        double sumWeightedMeanSquare = 0.0;
        double sumLoadFactor = 0.0;
        uint64_t sumPerfectDirs = 0;
        uint64_t sumFallbackDirs = 0;
        uint64_t sumBuckets = 0;

        for (uint32_t p : sampledPrimes) {
            HashSimulationSummary summary = simulateHashing(directories, dirChildren, {p});
            sumWeightedMeanSquare += weightedMeanSquare(summary);
            sumLoadFactor += globalLoadFactor(summary);
            sumPerfectDirs += summary.dirsPerfect;
            sumFallbackDirs += summary.dirsFallback;
            sumBuckets += summary.totalBuckets;
            trials.push_back(FixedBaseTrial{p, std::move(summary)});
        }

        std::sort(trials.begin(), trials.end(), [](const FixedBaseTrial& a, const FixedBaseTrial& b) {
            const double aw = weightedMeanSquare(a.summary);
            const double bw = weightedMeanSquare(b.summary);
            if (aw != bw) {
                return aw < bw;
            }
            if (a.summary.globalMaxChain != b.summary.globalMaxChain) {
                return a.summary.globalMaxChain < b.summary.globalMaxChain;
            }
            if (a.summary.dirsFallback != b.summary.dirsFallback) {
                return a.summary.dirsFallback < b.summary.dirsFallback;
            }
            return a.prime < b.prime;
        });

        const auto& bestFixed = trials.front();
        const double bestWeightedMeanSquare = weightedMeanSquare(bestFixed.summary);
        const double bestLoadFactor = globalLoadFactor(bestFixed.summary);
        const double avgWeightedMeanSquare =
            sumWeightedMeanSquare / static_cast<double>(trials.size());
        const double avgLoadFactor =
            sumLoadFactor / static_cast<double>(trials.size());
        const double avgPerfectDirs =
            static_cast<double>(sumPerfectDirs) / static_cast<double>(trials.size());
        const double avgFallbackDirs =
            static_cast<double>(sumFallbackDirs) / static_cast<double>(trials.size());
        const double avgBuckets =
            static_cast<double>(sumBuckets) / static_cast<double>(trials.size());

        std::cout << "Fixed-base large-prime experiment\n";
        std::cout << "  sampled primes:               " << trials.size() << "\n";
        std::cout << "  prime range tested:           [" << kLargePrimeMin << ", " << kLargePrimeMax << ")\n";
        std::cout << "  RNG seed:                     " << options.fixedBaseSeed << "\n";
        std::cout << "  best fixed prime:             " << bestFixed.prime << "\n";
        std::cout << "  best weighted mean-square:    " << std::fixed << std::setprecision(4) << bestWeightedMeanSquare << "\n";
        std::cout << "  current weighted mean-square: " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
        std::cout << "  delta vs current:             " << std::showpos << std::fixed << std::setprecision(4)
                  << (bestWeightedMeanSquare - baselineWeightedMeanSquare) << std::noshowpos << "\n";
        std::cout << "  best load factor N/T:         " << std::fixed << std::setprecision(4) << bestLoadFactor << "\n";
        std::cout << "  current load factor N/T:      " << std::fixed << std::setprecision(4) << baselineGlobalLoadFactor << "\n";
        std::cout << "  best perfect-hash dirs:       " << bestFixed.summary.dirsPerfect << "\n";
        std::cout << "  current perfect-hash dirs:    " << baseline.dirsPerfect << "\n";
        std::cout << "  best fallback dirs:           " << bestFixed.summary.dirsFallback << "\n";
        std::cout << "  current fallback dirs:        " << baseline.dirsFallback << "\n";
        std::cout << "  best total buckets:           " << bestFixed.summary.totalBuckets << "\n";
        std::cout << "  current total buckets:        " << baseline.totalBuckets << "\n";
        std::cout << "  best max chain observed:      " << bestFixed.summary.globalMaxChain << "\n";
        std::cout << "  current max chain observed:   " << baseline.globalMaxChain << "\n";
        std::cout << "  average weighted mean-square: " << std::fixed << std::setprecision(4) << avgWeightedMeanSquare << "\n";
        std::cout << "  average load factor N/T:      " << std::fixed << std::setprecision(4) << avgLoadFactor << "\n";
        std::cout << "  average perfect-hash dirs:    " << std::fixed << std::setprecision(2) << avgPerfectDirs << "\n";
        std::cout << "  average fallback dirs:        " << std::fixed << std::setprecision(2) << avgFallbackDirs << "\n";
        std::cout << "  average total buckets:        " << std::fixed << std::setprecision(2) << avgBuckets << "\n";
        std::cout << "\n";

        std::cout << "Best fixed-prime worst directories\n";
        std::cout << "  path | N | tablesize | score | max_chain\n";
        for (size_t i = 0; i < topN; ++i) {
            const auto& d = bestFixed.summary.dirs[i];
            std::cout << "  " << d.path
                      << " | " << d.n
                      << " | " << d.choice.tablesize
                      << " | " << std::fixed << std::setprecision(4) << d.choice.score
                      << " | " << d.choice.maxChain
                      << "\n";
        }
        std::cout << "\n";

        const HashSimulationSummary hybrid =
            simulateHashingWithFixedFallback(directories, dirChildren, smallPrimes, kHybridFallbackBase);
        const double hybridWeightedMeanSquare = weightedMeanSquare(hybrid);
        const double hybridLoadFactor = globalLoadFactor(hybrid);

        std::cout << "Hybrid fixed-fallback experiment\n";
        std::cout << "  fallback base:                " << kHybridFallbackBase << "\n";
        std::cout << "  weighted mean-square chain:   " << std::fixed << std::setprecision(4) << hybridWeightedMeanSquare << "\n";
        std::cout << "  current weighted mean-square: " << std::fixed << std::setprecision(4) << baselineWeightedMeanSquare << "\n";
        std::cout << "  delta vs current:             " << std::showpos << std::fixed << std::setprecision(4)
                  << (hybridWeightedMeanSquare - baselineWeightedMeanSquare) << std::noshowpos << "\n";
        std::cout << "  load factor N/T:              " << std::fixed << std::setprecision(4) << hybridLoadFactor << "\n";
        std::cout << "  current load factor N/T:      " << std::fixed << std::setprecision(4) << baselineGlobalLoadFactor << "\n";
        std::cout << "  perfect-hash dirs:            " << hybrid.dirsPerfect << "\n";
        std::cout << "  current perfect-hash dirs:    " << baseline.dirsPerfect << "\n";
        std::cout << "  dirs using fallback path:     " << hybrid.dirsHitCeiling << "\n";
        std::cout << "  current ceiling-hit dirs:     " << baseline.dirsHitCeiling << "\n";
        std::cout << "  fallback dirs score>=1.1:     " << hybrid.dirsFallback << "\n";
        std::cout << "  current fallback dirs:        " << baseline.dirsFallback << "\n";
        std::cout << "  total buckets:                " << hybrid.totalBuckets << "\n";
        std::cout << "  current total buckets:        " << baseline.totalBuckets << "\n";
        std::cout << "  max chain observed:           " << hybrid.globalMaxChain << "\n";
        std::cout << "  current max chain observed:   " << baseline.globalMaxChain << "\n";
        std::cout << "\n";

        std::cout << "Hybrid fallback worst directories\n";
        std::cout << "  path | N | tablesize | p | score | max_chain | hit_ceiling\n";
        for (size_t i = 0; i < topN; ++i) {
            const auto& d = hybrid.dirs[i];
            std::cout << "  " << d.path
                      << " | " << d.n
                      << " | " << d.choice.tablesize
                      << " | " << d.choice.p
                      << " | " << std::fixed << std::setprecision(4) << d.choice.score
                      << " | " << d.choice.maxChain
                      << " | " << (d.choice.hitCeiling ? "yes" : "no")
                      << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "mmap profile (per canonical regular-file object)\n";
    std::cout << "  image-small (size < PAGE_SIZE):" << smallFilePayloads << "\n";
    std::cout << "  image-page-multiple files:     " << largePageAlignedPayloads << "\n";
    std::cout << "  image-large with tail:         " << largePartialPayloads << "\n";
    std::cout << "  zero-length files:             " << zeroLenFilePayloads << "\n";
    std::cout << "  size <= VM_PAGE_SIZE:          " << filesAtMostVmPage << "\n";
    std::cout << "  VM-aligned larger files:       " << vmAlignedLargeFiles << "\n";
    std::cout << "    with partial VM tail:        " << vmAlignedLargeFilesWithTail << "\n";
    std::cout << "  potentially unaligned larger:  " << potentiallyUnalignedLargeFiles << "\n";
    std::cout << "  clean <=1-copy-page files:     " << cleanAtMostOneCopyFiles << "\n";
    std::cout << "  copied-page upper bound there: " << cleanCopiedPageUpperBoundForThoseFiles << "\n";
    std::cout << "  public nonempty files:         " << publicMappableFilePayloads << "\n";
    std::cout << "  private nonempty (clean-only): " << privateMappableFilePayloads << "\n";
    std::cout << "  leaking guaranteed-aligned:    " << leakingAlignedPublicCandidates << "\n";
    std::cout << "  dirty sub-page candidates:     " << dirtyShiftedPublicCandidates << "\n";
    std::cout << "  private image-small files:     " << privateSmallFilePayloads << "\n";
    if (vmForcesClean)
        std::cout << "  note: VM_PAGE_SIZE > PAGE_SIZE, so leaking/dirty are forced to clean\n";
    std::cout << "\n";

    std::cout << "Content-part packing simulation (image PAGE_SIZE "
              << imagePageSize << ")\n";
    std::cout << "  unique payload objects:       " << filePayloadByInode.size() << "\n";
    std::cout << "  unique filename strings:      " << uniqueNames.size() << "\n";
    std::cout << "  unique symlink targets:       " << uniqueSymlinkTargets.size() << "\n";
    std::cout << "  unique string blobs total:    " << (uniqueNames.size() + uniqueSymlinkTargets.size()) << "\n";
    std::cout << "  packed contents count:        " << packing.contentCount << "\n";
    std::cout << "  stored blob bytes:            " << prettyBytes(packing.rawBytes) << "\n";
    std::cout << "  page-packed tail bytes:       " << prettyBytes(packing.paddedBytes) << "\n";
    std::cout << "  internal packing holes:       " << prettyBytes(packing.paddingBytes)
              << " (" << std::fixed << std::setprecision(4)
              << ((packing.rawBytes == 0) ? 0.0 :
                  (100.0 * static_cast<double>(packing.paddingBytes) / static_cast<double>(packing.rawBytes)))
              << "%)\n";
    std::cout << "  payload blobs overflow-skip:  " << payloadEncodingOverflow << "\n";
    std::cout << "  string blobs overflow-skip:   " << stringEncodingOverflow << "\n";
    std::cout << "\n";

    std::cout << "Visibility-split content-part packing\n";
    std::cout << "  assumption:                   public payload needs world-read file + world-search path; public names need world-read+search path to containing dir\n";
    std::cout << "  public file payload objects:  " << publicFilePayloads << "\n";
    std::cout << "  private file payload objects: " << privateFilePayloads << "\n";
    std::cout << "  public filename strings:      " << publicFilenameStrings << "\n";
    std::cout << "  private filename strings:     " << privateFilenameStrings << "\n";
    std::cout << "  public filename blobs:        " << publicFilenameContentBlobs << "\n";
    std::cout << "  private filename blobs:       " << privateFilenameContentBlobs << "\n";
    std::cout << "  private symlink blobs:        " << privateSymlinkTargetContentBlobs << "\n";
    std::cout << "  public part stored bytes:     " << prettyBytes(splitPacking.publicPool.rawBytes) << "\n";
    std::cout << "  private part stored bytes:    " << prettyBytes(splitPacking.privatePool.rawBytes) << "\n";
    std::cout << "  public packed tail bytes:     " << prettyBytes(splitPacking.publicPool.paddedBytes) << "\n";
    std::cout << "  public part span to boundary: " << prettyBytes(publicPartSpan) << "\n";
    std::cout << "  private packed tail bytes:    " << prettyBytes(splitPacking.privatePool.paddedBytes) << "\n";
    std::cout << "  private part span to EOF:     " << prettyBytes(privatePartSpan) << "\n";
    std::cout << "  packed tails total:           " << prettyBytes(splitPacking.combined.paddedBytes) << "\n";
    std::cout << "  split total padding to EOF:   " << prettyBytes(splitTotalPadding)
              << " (" << std::fixed << std::setprecision(4)
              << ((splitPacking.combined.rawBytes == 0) ? 0.0 :
                  (100.0 * static_cast<double>(splitTotalPadding) /
                   static_cast<double>(splitPacking.combined.rawBytes)))
              << "%)\n";
    std::cout << "  delta total padding:          " << std::showpos
              << static_cast<int64_t>(splitTotalPadding) - static_cast<int64_t>(unsplitTotalPadding)
              << std::noshowpos << " B\n";
    std::cout << "\n";

    std::cout << "Whole-image size estimate\n";
    std::cout << "  header bytes:                 " << headerBytes << "\n";
    std::cout << "  metadata table bytes:         " << objectTableBytes << "\n";
    std::cout << "  public_off:                   " << publicOff << "\n";
    std::cout << "  private_off:                  " << privateOff << "\n";
    std::cout << "  content region incl. gaps:    " << splitContentRegionBytes << "\n";
    std::cout << "  image_size:                   " << prettyBytes(splitImageBytesEstimate)
              << " (" << splitImageBytesEstimate << " bytes)\n";
    std::cout << "  image-size limit status:      "
              << (imageLimitExceeded ? "EXCEEDED" : "ok") << "\n";
    std::cout << "  unsplit comparison image:     " << prettyBytes(unsplitImageSize)
              << " (" << unsplitImageSize << " bytes)\n";
    std::cout << "\n";

    std::cout << "Assumptions\n";
    std::cout << "  - regular-file payload dedup estimated by inode identity (captures hardlinks).\n";
    std::cout << "  - find -ls supplies no device number; inode identity is therefore assumed input-global.\n";
    std::cout << "  - owner/group names stand in for numeric IDs when estimating attribute dedup.\n";
    std::cout << "  - cross-inode byte-identical dedup is unknown from find -ls and not modeled.\n";
    std::cout << "  - therefore cross-visibility promotion of identical payload bytes may be under-modeled.\n";
    std::cout << "  - symlink targets are modeled as private-only blobs outside payload/filename shared dedup domain.\n";
    std::cout << "  - blob storage uses align_up(announced+1, 4) before page-packing simulation.\n";
    std::cout << "  - successful lookup comparisons replay the builder's chains over byte-sorted names.\n";
    std::cout << "  - misses use entry-weighted directories and independent uniform anchors, probing equal anchors once.\n";
    std::cout << "  - find -ls has no content offsets; mmap candidates count only alignment guaranteed by packing rules.\n";

    const bool formatLimitsExceeded = objectLimitExceeded ||
        metadataCellLimitExceeded || imageLimitExceeded ||
        selectedSummary.dirsOverHardLimit != 0 ||
        payloadEncodingOverflow != 0 || stringEncodingOverflow != 0;
    std::cout << "\nStructural format-limit result: "
              << (formatLimitsExceeded ? "INVALID" : "representable") << "\n";

    return formatLimitsExceeded ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "femtofsSim: " << error.what() << '\n';
        return 1;
    }
}

// END File: programs/femtofsSim.cpp
