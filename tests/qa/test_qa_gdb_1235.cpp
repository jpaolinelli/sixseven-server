// =============================================================================
// Adversarial QA tests for GDB-1235.
//
// GDB-1235: QA_HnswIdenticalVectors.TwentyIdenticalWithSmallM was reported as
// order-dependent (10/20 reachable in multi-suite sweeps, 20/20 in isolation).
// Root cause (per implementer handoff, re-verified below): the reverse/
// bidirectional-link maintenance in HnswIndex::insert() evicted a single
// "farthest" neighbor via std::max_element whenever a neighbor list was full.
// With many candidates tied at distance 0 (identical/near-identical vectors),
// std::max_element always resolved the tie to the same slot, so each new
// insert's back-edge perpetually evicted only the *previous* insert's
// back-edge -- earlier occupants of every other slot were never touched.
// This permanently stranded a class of nodes with zero in-edges from the
// reachable component. This was a deterministic structural bug (not actually
// "order-dependent" in the flaky sense) that always reproduces once M is
// small relative to the number of tied candidates -- the original ticket's
// "passes in isolation" report was itself surprising and is retested here.
//
// Fix: HnswIndex::select_neighbors_heuristic prunes the whole grown
// candidate set at once. For strictly-distinct distances it is equivalent to
// keep-M-closest (no behavior change from before). For ties at the eviction
// boundary, it fairly rotates which tied entries survive using the new
// node's monotonically increasing id, so no single node is permanently
// excluded.
// =============================================================================

#include "sixseven/common/platform.h"
#include "sixseven/vector/hnsw_index.h"
#include "sixseven/vector/hnsw_page.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <vector>

using namespace sixseven;

namespace {

class QA1235TempDir {
public:
    QA1235TempDir() {
        path_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb_1235_XXXXXX";
        std::string tmpl = path_.string();
        char* result = sixseven_platform::mkdtemp(tmpl.data());
        EXPECT_NE(result, nullptr);
        path_ = result;
    }
    ~QA1235TempDir() { std::filesystem::remove_all(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct QA1235Fixture {
    QA1235TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    QA1235Fixture() {
        auto db_path = tmp.path() / "qa_gdb1235.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 64);
    }
};

float squared_l2(const std::vector<float>& a, const std::vector<float>& b) {
    float sum = 0.0F;
    for (size_t i = 0; i < a.size(); ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

} // namespace

// =============================================================================
// AC1: identical-vector reachability, small M, deterministic.
// =============================================================================

// Direct re-verification of the ticket's own reported scenario: M=4, N=20
// identical vectors, search(k=20) must return all 20. Run via --gtest_repeat
// at the harness level for determinism; this test itself is also
// self-contained (fresh fixture per run).
TEST(QA_GDB1235, TwentyIdenticalSmallMAllReachable) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 64;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {0.25F, 0.25F, 0.25F, 0.25F};
    for (int i = 0; i < 20; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "insert " << i << ": " << ir.error().message;
    }
    ASSERT_EQ(index.node_count(), 20u);

    auto sr = index.search(vec, 20);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), 20u) << "all 20 identical vectors must be reachable";

    std::set<uint32_t> ids;
    for (const auto& r : sr.value()) {
        ids.insert(r.node_id);
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
    EXPECT_EQ(ids.size(), 20u) << "no duplicate node ids in result set";
}

// Larger N, small M stress: 50 identical vectors at M=4.
TEST(QA_GDB1235, FiftyIdenticalSmallMAllReachable) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 5;
    config.m = 4;
    config.ef_construction = 96;
    config.ef_search = 128;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {-1.0F, 2.0F, 0.0F, 3.5F, -2.5F};
    constexpr uint32_t total = 50;
    for (uint32_t i = 0; i < total; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "insert " << i << ": " << ir.error().message;
    }
    ASSERT_EQ(index.node_count(), total);

    auto sr = index.search(vec, total);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), total) << "all 50 identical vectors must be reachable at M=4";
}

// The OOB boundary case explicitly called out in the handoff: M=1. Layer-0
// cap is m*2 per the implementation's usual doubling, but exercise the
// degenerate single-neighbor-slot path directly regardless of that scaling.
// Must not crash and must produce a correct (if limited) result.
TEST(QA_GDB1235, IdenticalVectorsM1NoCrash) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 3;
    config.m = 1;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {7.0F, 7.0F, 7.0F};
    for (int i = 0; i < 20; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "insert " << i << " with M=1: " << ir.error().message;
    }
    EXPECT_EQ(index.node_count(), 20u);

    // Must not crash; search must return a well-formed (non-empty, no
    // duplicate ids, all distance 0) result even if graph connectivity at
    // M=1 can't guarantee full reachability of all 20 in one search.
    auto sr = index.search(vec, 20);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_GE(sr.value().size(), 1u) << "at least the entry point must be found at M=1";

    std::set<uint32_t> ids;
    for (const auto& r : sr.value()) {
        EXPECT_TRUE(ids.insert(r.node_id).second) << "duplicate node id " << r.node_id;
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
}

// Single insert at M=1 (degenerate: no eviction path exercised at all).
TEST(QA_GDB1235, SingleVectorM1) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 1;
    config.ef_construction = 8;
    config.ef_search = 8;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 2.0F};
    auto ir = index.insert(vec);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(index.node_count(), 1u);

    auto sr = index.search(vec, 1);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 1u);
    EXPECT_FLOAT_EQ(sr.value()[0].distance, 0.0F);
}

// =============================================================================
// AC1 (continued): determinism across repeated runs of the SAME test binary
// invocation via internal repetition (in addition to the harness-level
// --gtest_repeat used during QA). Insert/search cycles are independent per
// call (fresh fixture), so any latent nondeterminism in tie resolution
// (e.g. an unseeded RNG or uninitialized memory influencing the rotation
// offset) would surface as a flaky pass/fail across these iterations.
// =============================================================================

TEST(QA_GDB1235, RepeatedIdenticalVectorRunsAllDeterministic) {
    for (int run = 0; run < 10; ++run) {
        QA1235Fixture fix;
        HnswIndex index(*fix.bpm);

        HnswIndexConfig config;
        config.dimension = 4;
        config.m = 4;
        config.ef_construction = 64;
        config.ef_search = 64;
        ASSERT_TRUE(index.create(config).has_value());

        std::vector<float> vec = {0.5F, 0.5F, 0.5F, 0.5F};
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(index.insert(vec).has_value()) << "run " << run << " insert " << i;
        }

        auto sr = index.search(vec, 20);
        ASSERT_TRUE(sr.has_value()) << "run " << run << ": " << sr.error().message;
        EXPECT_EQ(sr.value().size(), 20u) << "run " << run << " did not reach all 20 nodes";
    }
}

// =============================================================================
// AC2: distinct-distance recall preserved (the v1 regression class).
// =============================================================================

// Brute-force recall@k sanity check on a random distinct dataset: HNSW
// search results for k must match the true k-nearest-neighbors computed by
// brute force, given exact (non-approximate) search parameters (high
// ef_search, small dataset).
TEST(QA_GDB1235, RecallMatchesBruteForceOnDistinctData) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 6;
    config.m = 8;
    config.ef_construction = 128;
    config.ef_search = 256; // High enough to make approximate search exact for this size.
    ASSERT_TRUE(index.create(config).has_value());

    std::mt19937 rng(1235);
    std::uniform_real_distribution<float> dist(-50.0F, 50.0F);

    constexpr int n = 200;
    std::vector<std::vector<float>> vectors;
    vectors.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::vector<float> v(6);
        for (auto& x : v) {
            x = dist(rng);
        }
        vectors.push_back(v);
        ASSERT_TRUE(index.insert(v).has_value()) << "insert " << i;
    }
    ASSERT_EQ(index.node_count(), static_cast<uint32_t>(n));

    // Query with a fresh random point, not necessarily in the dataset.
    std::vector<float> query(6);
    for (auto& x : query) {
        x = dist(rng);
    }

    constexpr int k = 10;
    auto sr = index.search(query, k);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), static_cast<size_t>(k));

    // Brute-force top-k by squared L2 distance.
    std::vector<std::pair<float, int>> brute;
    brute.reserve(n);
    for (int i = 0; i < n; ++i) {
        brute.emplace_back(squared_l2(query, vectors[static_cast<size_t>(i)]), i);
    }
    std::sort(brute.begin(), brute.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::set<uint32_t> hnsw_ids;
    for (const auto& r : sr.value()) {
        hnsw_ids.insert(r.node_id);
    }
    std::set<int> brute_ids;
    for (int i = 0; i < k; ++i) {
        brute_ids.insert(brute[static_cast<size_t>(i)].second);
    }

    int matches = 0;
    for (int id : brute_ids) {
        if (hnsw_ids.count(static_cast<uint32_t>(id)) > 0) {
            ++matches;
        }
    }
    // With ef_search this high on a 200-point dataset, recall should be
    // perfect or very close to it. Flag any drop as a regression signal
    // for the reverse-edge eviction fix affecting ordinary recall.
    EXPECT_GE(matches, k - 1)
        << "recall@" << k << " dropped for distinct-distance data: only " << matches << "/" << k
        << " brute-force neighbors were found by HNSW search";
}

// Direct construction test: full neighbor list with strictly increasing
// distances; a far outlier must not evict a closer neighbor. Complements the
// implementer's own unit test with a different M and dimensionality.
TEST(QA_GDB1235, FarOutlierNeverEvictsCloserNeighborAltConfig) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 3; // layer-0 cap = 6 (typical m*2 doubling)
    config.ef_construction = 64;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> origin = {0.0F, 0.0F};
    ASSERT_TRUE(index.insert(origin).has_value());

    // Distinct, strictly increasing distances: (1,0), (2,0), ..., (6,0).
    for (int i = 1; i <= 6; ++i) {
        std::vector<float> pt = {static_cast<float>(i), 0.0F};
        ASSERT_TRUE(index.insert(pt).has_value());
    }
    EXPECT_EQ(index.node_count(), 7u);

    auto before = index.search(origin, 7);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before.value().size(), 7u);

    // Insert a far outlier -- strictly farther than all existing points.
    std::vector<float> outlier = {1000.0F, 0.0F};
    ASSERT_TRUE(index.insert(outlier).has_value());
    EXPECT_EQ(index.node_count(), 8u);

    auto after = index.search(origin, 7);
    ASSERT_TRUE(after.has_value()) << after.error().message;
    ASSERT_EQ(after.value().size(), 7u)
        << "the 7 closest-to-origin points must remain reachable/closest after inserting a "
           "far outlier";
    for (const auto& r : after.value()) {
        EXPECT_LE(r.distance, 36.0F) << "node " << r.node_id
                                     << " distance " << r.distance
                                     << " -- outlier should not have displaced a closer neighbor";
    }
}

// =============================================================================
// AC3: near-identical / mixed clusters (single-cluster / low-layer scope;
// NOT the multi-cluster multi-layer case tracked separately as GDB-1295).
// =============================================================================

// A single cluster of near-identical (tiny epsilon perturbation) vectors
// plus a handful of clearly-distinct vectors, all inserted interleaved.
// Reachability of the near-identical cluster must hold.
TEST(QA_GDB1235, InterleavedNearIdenticalAndDistinctReachable) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 3;
    config.m = 4;
    config.ef_construction = 64;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    constexpr int cluster_n = 15;
    std::vector<uint32_t> cluster_ids;
    for (int i = 0; i < cluster_n; ++i) {
        float eps = static_cast<float>(i) * 1e-6F;
        std::vector<float> v = {2.0F + eps, 2.0F, 2.0F};
        auto ir = index.insert(v);
        ASSERT_TRUE(ir.has_value());
        cluster_ids.push_back(ir.value());

        // Interleave a clearly distinct vector every few inserts.
        if (i % 3 == 0) {
            std::vector<float> distinct = {
                static_cast<float>(i * 1000), static_cast<float>(i * 1000), 0.0F};
            ASSERT_TRUE(index.insert(distinct).has_value());
        }
    }

    auto sr = index.search(std::vector<float>{2.0F, 2.0F, 2.0F}, cluster_n);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), static_cast<size_t>(cluster_n))
        << "all near-identical cluster members should be reachable even when interleaved with "
           "distinct inserts";

    std::set<uint32_t> found_ids;
    for (const auto& r : sr.value()) {
        found_ids.insert(r.node_id);
    }
    for (uint32_t id : cluster_ids) {
        EXPECT_TRUE(found_ids.count(id) > 0) << "cluster node " << id << " not reachable";
    }
}

// =============================================================================
// Sequencing / boundary adversarial cases.
// =============================================================================

// Zero-radius search (k=1) on identical vectors: must return exactly one
// result at distance 0, never crash or return zero results.
TEST(QA_GDB1235, IdenticalVectorsSearchK1) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {9.0F, 9.0F, 9.0F, 9.0F};
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    auto sr = index.search(vec, 1);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 1u);
    EXPECT_FLOAT_EQ(sr.value()[0].distance, 0.0F);
}

// k larger than node_count: must not crash, must return at most node_count
// results, and every identical vector should still be found since k exceeds
// the corpus size.
TEST(QA_GDB1235, KLargerThanNodeCountIdenticalVectors) {
    QA1235Fixture fix;
    HnswIndex index(*fix.bpm);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {3.0F, 3.0F, 3.0F, 3.0F};
    for (int i = 0; i < 12; ++i) {
        ASSERT_TRUE(index.insert(vec).has_value());
    }

    auto sr = index.search(vec, 1000);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), 12u) << "search(k >> node_count) should return all 12 nodes";
}
