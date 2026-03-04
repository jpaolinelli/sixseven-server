// =============================================================================
// Adversarial QA tests for GDB-235, GDB-236, GDB-237 bug fixes.
//
// GDB-235: HNSW Insert — Bidirectional connections dropped when distances are
//          equal. Fix: Changed < to <= in neighbor eviction comparison.
//
// GDB-236: EmbeddingWorkerPool queue not persisted — jobs lost on restart.
//          Fix: Added EmbeddingJobPersistence interface with persist/remove/load
//          callbacks.
//
// GDB-237: OpenAIProvider returns wrong-dimension vectors when API returns
//          fewer embeddings. Fix: Added `received` tracking vector and
//          validation that all indices got embeddings.
// =============================================================================

#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/hnsw_index.h"
#include "giodb/vector/hnsw_page.h"
#include "giodb/vector/http_client.h"
#include "giodb/vector/openai_provider.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb_235_237_XXXXXX";
        std::string tmpl = path_.string();
        char* result = mkdtemp(tmpl.data());
        EXPECT_NE(result, nullptr);
        path_ = result;
    }

    ~TempDir() { std::filesystem::remove_all(path_); }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct SmallFixture {
    TempDir tmp;
    DiskManager disk_manager;
    FileId file_id = 0;
    std::unique_ptr<BufferPoolManager> bpm;

    SmallFixture() {
        auto db_path = tmp.path() / "qa_gdb235.db";
        auto fid = disk_manager.create_file(db_path);
        EXPECT_TRUE(fid.has_value());
        file_id = fid.value();
        bpm = std::make_unique<BufferPoolManager>(disk_manager, file_id, 64);
    }
};

} // namespace

// =============================================================================
// Mock HTTP Client for GDB-237 tests
// =============================================================================

class QA237MockHttpClient : public HttpClient {
public:
    void set_post_response(int status, const std::string& body) {
        post_status_ = status;
        post_body_ = body;
    }

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        (void)url;
        (void)body;
        (void)headers;
        HttpResponse response;
        response.status_code = post_status_;
        response.body = post_body_;
        response.content_type = "application/json";
        return ok(std::move(response));
    }

    Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers) override {
        (void)url;
        (void)headers;
        HttpResponse response;
        response.status_code = 200;
        response.body = "{}";
        return ok(std::move(response));
    }

private:
    int post_status_ = 200;
    std::string post_body_;
};

// =============================================================================
// Test Provider for GDB-236 tests
// =============================================================================

class QA236TestProvider : public EmbeddingProvider {
public:
    explicit QA236TestProvider(size_t dim) : dim_(dim) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        call_count_.fetch_add(1);
        std::vector<float> v(dim_, 1.0F);
        return ok(std::move(v));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        batch_call_count_.fetch_add(1);
        std::vector<std::vector<float>> result;
        result.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            result.emplace_back(dim_, 1.0F);
        }
        return ok(std::move(result));
    }

    std::string name() const override { return "qa236_test"; }
    size_t dimension() const override { return dim_; }
    Result<void> health_check() override { return ok(); }

    int call_count() const { return call_count_.load(); }
    int batch_call_count() const { return batch_call_count_.load(); }

private:
    size_t dim_;
    std::atomic<int> call_count_{0};
    std::atomic<int> batch_call_count_{0};
};

// =============================================================================
// Failing provider for max-retry persistence tests
// =============================================================================

class QA236FailingProvider : public EmbeddingProvider {
public:
    explicit QA236FailingProvider(size_t dim) : dim_(dim) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        return make_error(StatusCode::INTERNAL_ERROR, "always fails");
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& /*texts*/) override {
        return make_error(StatusCode::INTERNAL_ERROR, "batch always fails");
    }

    std::string name() const override { return "qa236_failing"; }
    size_t dimension() const override { return dim_; }
    Result<void> health_check() override { return ok(); }

private:
    size_t dim_;
};

// =============================================================================
// Helper: create an EmbeddingJob for GDB-236 tests
// =============================================================================

static EmbeddingJob make_qa236_job(table_id_t table_id,
                                   int64_t row_id,
                                   int32_t col_id,
                                   const std::string& text,
                                   const std::string& provider = "qa236_test",
                                   int32_t dim = 32) {
    EmbeddingJob job;
    job.table_id = table_id;
    job.row_id = row_id;
    job.column_id = col_id;
    job.source_text = text;
    job.provider = provider;
    job.dimension = dim;
    job.type = EmbeddingJob::Type::INSERT;
    job.retry_count = 0;
    return job;
}

// =============================================================================
// GDB-235: HNSW Insert — Bidirectional connections with equal distances
// =============================================================================

// Test 1: Insert 10 identical vectors, search for k=10 — all 10 should be found.
// This directly exercises the bug fix: with <, only one of the equal-distance
// nodes would be kept; with <=, all are retained.
TEST(QA_HnswIdenticalVectors, TenIdenticalSearchForAll) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 3;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {1.0F, 1.0F, 1.0F};
    for (int i = 0; i < 10; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
        EXPECT_EQ(ir.value(), static_cast<uint32_t>(i));
    }

    EXPECT_EQ(index.node_count(), 10u);

    // All 10 identical vectors should be reachable and have distance 0.
    auto sr = index.search(vec, 10);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 10u) << "Expected all 10 identical vectors to be reachable";
    for (const auto& r : sr.value()) {
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
}

// Test 2: Insert 20 identical vectors with M=4, search for k=20 — all should be
// reachable despite the low neighbor limit forcing eviction decisions.
TEST(QA_HnswIdenticalVectors, TwentyIdenticalWithSmallM) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 4;
    config.m = 4;
    config.ef_construction = 64;
    config.ef_search = 64;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {0.5F, 0.5F, 0.5F, 0.5F};
    for (int i = 0; i < 20; ++i) {
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
    }

    EXPECT_EQ(index.node_count(), 20u);

    auto sr = index.search(vec, 20);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), 20u)
        << "All 20 identical vectors should be reachable even with M=4";

    // Verify all distances are 0.
    for (const auto& r : sr.value()) {
        EXPECT_FLOAT_EQ(r.distance, 0.0F);
    }
}

// Test 3: Mix of identical and unique vectors — all identical ones should be
// found together.
TEST(QA_HnswIdenticalVectors, MixedIdenticalAndUnique) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 8;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert 5 identical vectors at (1, 1).
    std::vector<float> identical = {1.0F, 1.0F};
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.insert(identical).has_value());
    }

    // Insert 5 unique vectors far away from (1, 1).
    for (int i = 0; i < 5; ++i) {
        std::vector<float> unique = {static_cast<float>(i * 100), static_cast<float>(i * 100)};
        ASSERT_TRUE(index.insert(unique).has_value());
    }

    EXPECT_EQ(index.node_count(), 10u);

    // Search for the identical vector — should get all 5 at distance 0.
    auto sr = index.search(identical, 5);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    ASSERT_EQ(sr.value().size(), 5u);

    for (const auto& r : sr.value()) {
        EXPECT_FLOAT_EQ(r.distance, 0.0F)
            << "Node " << r.node_id << " should be one of the identical vectors";
    }

    // Collect the node IDs — should be {0, 1, 2, 3, 4}.
    std::set<uint32_t> ids;
    for (const auto& r : sr.value()) {
        ids.insert(r.node_id);
    }
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(ids.count(i) > 0) << "Identical node " << i << " missing from results";
    }
}

// Test 4: Insert 2 identical vectors at distance 0 — both should have
// bidirectional connections to each other.
TEST(QA_HnswIdenticalVectors, TwoIdenticalBidirectionalConnections) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 16;
    config.ef_search = 16;
    ASSERT_TRUE(index.create(config).has_value());

    std::vector<float> vec = {3.0F, 4.0F};
    auto ir0 = index.insert(vec);
    ASSERT_TRUE(ir0.has_value());
    EXPECT_EQ(ir0.value(), 0u);

    auto ir1 = index.insert(vec);
    ASSERT_TRUE(ir1.has_value());
    EXPECT_EQ(ir1.value(), 1u);

    // Read both nodes and verify bidirectional connections on layer 0.
    auto loc0 = index.node_location(0);
    ASSERT_TRUE(loc0.has_value());
    auto node0 = index.read_node(loc0.value());
    ASSERT_TRUE(node0.has_value());

    auto loc1 = index.node_location(1);
    ASSERT_TRUE(loc1.has_value());
    auto node1 = index.read_node(loc1.value());
    ASSERT_TRUE(node1.has_value());

    // Node 0 should have node 1 as a neighbor on layer 0.
    bool node0_has_node1 = false;
    if (!node0.value().neighbors.empty()) {
        for (const auto& neighbor : node0.value().neighbors[0]) {
            if (neighbor.node_id == 1) {
                node0_has_node1 = true;
                EXPECT_FLOAT_EQ(neighbor.distance, 0.0F);
            }
        }
    }
    EXPECT_TRUE(node0_has_node1)
        << "Node 0 should have node 1 as a neighbor (bidirectional connection)";

    // Node 1 should have node 0 as a neighbor on layer 0.
    bool node1_has_node0 = false;
    if (!node1.value().neighbors.empty()) {
        for (const auto& neighbor : node1.value().neighbors[0]) {
            if (neighbor.node_id == 0) {
                node1_has_node0 = true;
                EXPECT_FLOAT_EQ(neighbor.distance, 0.0F);
            }
        }
    }
    EXPECT_TRUE(node1_has_node0)
        << "Node 1 should have node 0 as a neighbor (bidirectional connection)";
}

// Test 5: Insert vectors with very close but not identical distances —
// connectivity should still be maintained.
TEST(QA_HnswIdenticalVectors, VeryCloseDistancesMaintainConnectivity) {
    SmallFixture fix;
    HnswIndex index(*fix.bpm, nullptr);

    HnswIndexConfig config;
    config.dimension = 2;
    config.m = 4;
    config.ef_construction = 32;
    config.ef_search = 32;
    ASSERT_TRUE(index.create(config).has_value());

    // Insert vectors that are extremely close to each other (within float
    // epsilon) to stress the <= comparison boundary.
    float base = 1.0F;
    for (int i = 0; i < 10; ++i) {
        // Tiny perturbations: 1e-7 apart.
        float offset = static_cast<float>(i) * 1e-7F;
        std::vector<float> vec = {base + offset, base};
        auto ir = index.insert(vec);
        ASSERT_TRUE(ir.has_value()) << "Insert " << i << ": " << ir.error().message;
    }

    EXPECT_EQ(index.node_count(), 10u);

    // Search for all 10 — they should all be reachable since they are in a
    // tiny cluster.
    std::vector<float> query = {base, base};
    auto sr = index.search(query, 10);
    ASSERT_TRUE(sr.has_value()) << sr.error().message;
    EXPECT_EQ(sr.value().size(), 10u) << "All 10 tightly-clustered vectors should be reachable";

    // Distances should all be very small.
    for (const auto& r : sr.value()) {
        EXPECT_LT(r.distance, 1e-6F)
            << "Node " << r.node_id << " distance " << r.distance << " too large";
    }
}

// =============================================================================
// GDB-236: EmbeddingWorkerPool persistence — jobs survive restart
// =============================================================================

// Test 1: Set persistence callbacks, enqueue jobs, verify persist is called.
TEST(QA_EmbeddingPersistence, PersistCalledOnEnqueue) {
    std::atomic<int> persist_count{0};
    std::vector<EmbeddingJob> persisted_jobs;
    std::mutex mu;

    EmbeddingWorkerPool pool({.num_workers = 0});

    EmbeddingJobPersistence persistence;
    persistence.persist = [&](const EmbeddingJob& job) -> Result<void> {
        persist_count.fetch_add(1);
        std::lock_guard lock(mu);
        persisted_jobs.push_back(job);
        return ok();
    };
    persistence.remove = [](table_id_t, int64_t, int32_t) -> Result<void> { return ok(); };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };

    pool.set_persistence(std::move(persistence));

    // Enqueue 3 individual jobs.
    ASSERT_TRUE(pool.enqueue(make_qa236_job(1, 1, 0, "text1")).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa236_job(1, 2, 0, "text2")).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa236_job(1, 3, 0, "text3")).has_value());

    EXPECT_EQ(persist_count.load(), 3);

    std::lock_guard lock(mu);
    ASSERT_EQ(persisted_jobs.size(), 3u);
    EXPECT_EQ(persisted_jobs[0].row_id, 1);
    EXPECT_EQ(persisted_jobs[1].row_id, 2);
    EXPECT_EQ(persisted_jobs[2].row_id, 3);
}

// Test 2: Set persistence, process a job, verify remove is called after success.
TEST(QA_EmbeddingPersistence, RemoveCalledAfterSuccess) {
    std::atomic<int> remove_count{0};
    std::vector<std::tuple<table_id_t, int64_t, int32_t>> removed_keys;
    std::mutex mu;

    auto provider = std::make_shared<QA236TestProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 32;
    config.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("qa236_test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    EmbeddingJobPersistence persistence;
    persistence.persist = [](const EmbeddingJob&) -> Result<void> { return ok(); };
    persistence.remove = [&](table_id_t t, int64_t r, int32_t c) -> Result<void> {
        remove_count.fetch_add(1);
        std::lock_guard lock(mu);
        removed_keys.emplace_back(t, r, c);
        return ok();
    };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };

    pool.set_persistence(std::move(persistence));
    ASSERT_TRUE(pool.start().has_value());

    ASSERT_TRUE(pool.enqueue(make_qa236_job(10, 42, 3, "hello world")).has_value());

    // Wait for processing.
    for (int i = 0; i < 200 && store_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_GE(store_count.load(), 1);
    EXPECT_GE(remove_count.load(), 1);

    std::lock_guard lock(mu);
    ASSERT_GE(removed_keys.size(), 1u);
    EXPECT_EQ(std::get<0>(removed_keys[0]), 10u);
    EXPECT_EQ(std::get<1>(removed_keys[0]), 42);
    EXPECT_EQ(std::get<2>(removed_keys[0]), 3);
}

// Test 3: Set persistence, start pool, verify load is called and jobs are
// replayed from persistent storage.
TEST(QA_EmbeddingPersistence, LoadCalledOnStartAndJobsReplayed) {
    std::atomic<bool> load_called{false};
    auto provider = std::make_shared<QA236TestProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 32;
    config.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("qa236_test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    // Pre-populate persistent storage with 2 jobs to simulate a restart.
    EmbeddingJobPersistence persistence;
    persistence.persist = [](const EmbeddingJob&) -> Result<void> { return ok(); };
    persistence.remove = [](table_id_t, int64_t, int32_t) -> Result<void> { return ok(); };
    persistence.load = [&]() -> Result<std::vector<EmbeddingJob>> {
        load_called.store(true);
        std::vector<EmbeddingJob> jobs;
        jobs.push_back(make_qa236_job(1, 100, 0, "persisted_job_1"));
        jobs.push_back(make_qa236_job(1, 200, 0, "persisted_job_2"));
        return ok(std::move(jobs));
    };

    pool.set_persistence(std::move(persistence));
    ASSERT_TRUE(pool.start().has_value());

    EXPECT_TRUE(load_called.load()) << "load() should be called during start()";

    // Wait for the 2 loaded jobs to be processed.
    for (int i = 0; i < 200 && store_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 2) << "Both persisted jobs should have been processed";
}

// Test 4: Enqueue without persistence set — still works (backward compat).
TEST(QA_EmbeddingPersistence, NoPersistenceStillWorks) {
    auto provider = std::make_shared<QA236TestProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa236_test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });
    // Deliberately NOT setting persistence.

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_qa236_job(1, 1, 0, "no persist")).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa236_job(1, 2, 0, "no persist 2")).has_value());

    for (int i = 0; i < 200 && store_count.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 2);
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 2u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

// Test 5: Batch enqueue with persistence — all jobs get persisted.
TEST(QA_EmbeddingPersistence, BatchEnqueuePersistsAll) {
    std::atomic<int> persist_count{0};

    EmbeddingWorkerPool pool({.num_workers = 0});

    EmbeddingJobPersistence persistence;
    persistence.persist = [&](const EmbeddingJob&) -> Result<void> {
        persist_count.fetch_add(1);
        return ok();
    };
    persistence.remove = [](table_id_t, int64_t, int32_t) -> Result<void> { return ok(); };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };

    pool.set_persistence(std::move(persistence));

    std::vector<EmbeddingJob> batch;
    for (int i = 0; i < 7; ++i) {
        batch.push_back(make_qa236_job(1, i, 0, "batch_" + std::to_string(i)));
    }

    ASSERT_TRUE(pool.enqueue_batch(std::move(batch)).has_value());

    EXPECT_EQ(persist_count.load(), 7) << "All 7 batch jobs should have been persisted";
    EXPECT_EQ(pool.pending_count(), 7u);
}

// Test 6: Failed job exceeds max retries — verify remove is called for
// permanent failure cleanup.
TEST(QA_EmbeddingPersistence, MaxRetriesTriggersRemove) {
    std::atomic<int> remove_count{0};

    auto failing_provider = std::make_shared<QA236FailingProvider>(32);

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 1;
    config.max_retries = 0; // Fail immediately, no retries.
    config.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("qa236_failing", failing_provider);

    EmbeddingJobPersistence persistence;
    persistence.persist = [](const EmbeddingJob&) -> Result<void> { return ok(); };
    persistence.remove = [&](table_id_t, int64_t, int32_t) -> Result<void> {
        remove_count.fetch_add(1);
        return ok();
    };
    persistence.load = []() -> Result<std::vector<EmbeddingJob>> {
        return ok(std::vector<EmbeddingJob>{});
    };

    pool.set_persistence(std::move(persistence));
    ASSERT_TRUE(pool.start().has_value());

    auto job = make_qa236_job(1, 99, 5, "will fail", "qa236_failing", 32);
    ASSERT_TRUE(pool.enqueue(std::move(job)).has_value());

    // Wait for the job to fail and be removed.
    for (int i = 0; i < 200 && remove_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_GE(remove_count.load(), 1)
        << "Permanently failed job should be removed from persistent storage";

    auto s = pool.stats();
    EXPECT_GE(s.jobs_failed, 1u);
}

// =============================================================================
// GDB-237: OpenAIProvider — wrong-dimension vectors on partial responses
// =============================================================================

// Test 1: Empty data array in response — should error, not return zero-dim
// vectors. This was the core GDB-237 bug: empty data array was silently accepted.
TEST(QA_OpenAIValidation, EmptyDataArrayErrors) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test text");
    ASSERT_FALSE(result.has_value()) << "Empty data array should produce an error";
    // The provider should detect that no embeddings were received.
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// Test 2: Fewer embeddings than requested (1 of 2) — should error because
// index 1 is missing.
TEST(QA_OpenAIValidation, FewerEmbeddingsThanRequested) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_FALSE(result.has_value())
        << "Should error when API returns fewer embeddings than requested";
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// Test 3: Correct response — should succeed (normal path, regression guard).
TEST(QA_OpenAIValidation, CorrectResponseSucceeds) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 1}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);

    // Verify correct ordering by index.
    EXPECT_FLOAT_EQ((*result)[0][0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[0][1], 0.2F);
    EXPECT_FLOAT_EQ((*result)[0][2], 0.3F);
    EXPECT_FLOAT_EQ((*result)[1][0], 0.4F);
    EXPECT_FLOAT_EQ((*result)[1][1], 0.5F);
    EXPECT_FLOAT_EQ((*result)[1][2], 0.6F);
}

// Test 4: Missing index field in response entry — should default to index 0,
// not crash. When only one text is requested, this is valid behavior.
TEST(QA_OpenAIValidation, MissingIndexFieldDefaultsToZero) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    // Single entry with no "index" field — defaults to index 0.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.7, 0.8, 0.9]}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    // Single text — index 0 default should work.
    auto result = provider.embed("single text");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.7F);
    EXPECT_FLOAT_EQ((*result)[1], 0.8F);
    EXPECT_FLOAT_EQ((*result)[2], 0.9F);
}

// Test 5: Duplicate index in response — second one should overwrite the first,
// but all other indices must still be present. If not all are present, it should
// error.
TEST(QA_OpenAIValidation, DuplicateIndexOverwritesButMissingIndicesError) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    // Two entries with index 0, but index 1 is missing.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 0},
            {"embedding": [0.4, 0.5, 0.6], "index": 0}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    // Index 1 was never received, so validation should catch this.
    // NOTE (Low finding): The duplicate index causes push_back to accumulate
    // values (6 instead of 3), triggering INVALID_ARGUMENT (dimension mismatch)
    // before the received[] validation can fire with PARSE_ERROR. The error is
    // still returned (not silent corruption), just via a different pathway.
    ASSERT_FALSE(result.has_value())
        << "Duplicate index 0 with missing index 1 should produce an error";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// Test 6: Out-of-range index — should error because the index exceeds
// the number of requested texts.
TEST(QA_OpenAIValidation, OutOfRangeIndexErrors) {
    auto mock = std::make_unique<QA237MockHttpClient>();
    // Index 5 is out of range for a single-text request.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.1, 0.2, 0.3], "index": 5}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("single text");
    ASSERT_FALSE(result.has_value()) << "Out-of-range index should produce an error";
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}
