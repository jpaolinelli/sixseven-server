#include "giodb/catalog/catalog.h"
#include "giodb/common/types.h"
#include "giodb/vector/builtin_provider.h"
#include "giodb/vector/embedding_column.h"
#include "giodb/vector/embedding_worker.h"
#include "giodb/vector/http_client.h"
#include "giodb/vector/ollama_provider.h"
#include "giodb/vector/openai_provider.h"
#include "giodb/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace giodb;

// =============================================================================
// Mock HTTP client (reusable)
// =============================================================================

class QA31MockHttpClient : public HttpClient {
public:
    void set_post_response(int status, const std::string& body) {
        post_status_ = status;
        post_body_ = body;
    }

    void set_get_response(int status, const std::string& body) {
        get_status_ = status;
        get_body_ = body;
    }

    void set_network_error(const std::string& message) { network_error_ = message; }

    Result<HttpResponse>
    post(const std::string& url,
         const std::string& body,
         const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_post_url_ = url;
        last_post_body_ = body;
        last_post_headers_ = headers;

        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }

        HttpResponse response;
        response.status_code = post_status_;
        response.body = post_body_;
        response.content_type = "application/json";
        return ok(std::move(response));
    }

    Result<HttpResponse>
    get(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers) override {
        last_get_url_ = url;
        last_get_headers_ = headers;

        if (!network_error_.empty()) {
            auto err = network_error_;
            network_error_.clear();
            return make_error(StatusCode::NETWORK_ERROR, err);
        }

        HttpResponse response;
        response.status_code = get_status_;
        response.body = get_body_;
        response.content_type = "application/json";
        return ok(std::move(response));
    }

    std::string last_post_url_;
    std::string last_post_body_;
    std::vector<std::pair<std::string, std::string>> last_post_headers_;
    std::string last_get_url_;
    std::vector<std::pair<std::string, std::string>> last_get_headers_;

private:
    int post_status_ = 200;
    std::string post_body_;
    int get_status_ = 200;
    std::string get_body_;
    std::string network_error_;
};

// =============================================================================
// Test provider for worker pool tests
// =============================================================================

class QA31TestProvider : public EmbeddingProvider {
public:
    explicit QA31TestProvider(int32_t dimension, bool should_fail = false)
        : dimension_(dimension), should_fail_(should_fail) {}

    Result<std::vector<float>> embed(const std::string& /*text*/) override {
        if (should_fail_) {
            return make_error(StatusCode::INTERNAL_ERROR, "provider failure");
        }
        call_count_.fetch_add(1);
        std::vector<float> vec(static_cast<size_t>(dimension_), 1.0F);
        return ok(std::move(vec));
    }

    Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override {
        if (should_fail_) {
            return make_error(StatusCode::INTERNAL_ERROR, "provider batch failure");
        }
        batch_call_count_.fetch_add(1);
        std::vector<std::vector<float>> result;
        result.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            result.emplace_back(static_cast<size_t>(dimension_), 1.0F);
        }
        return ok(std::move(result));
    }

    std::string name() const override { return "qa31_test"; }
    size_t dimension() const override { return static_cast<size_t>(dimension_); }
    Result<void> health_check() override { return ok(); }

    int call_count() const { return call_count_.load(); }
    int batch_call_count() const { return batch_call_count_.load(); }
    void set_should_fail(bool fail) { should_fail_ = fail; }

private:
    int32_t dimension_;
    bool should_fail_;
    std::atomic<int> call_count_{0};
    std::atomic<int> batch_call_count_{0};
};

// =============================================================================
// Helper: build table schemas
// =============================================================================

static TableSchema make_qa31_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "title", TypeId::STRING, true, ""},
        {2, "body", TypeId::STRING, true, ""},
        {3, "embedding", TypeId::EMBEDDING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

static EmbeddingJob make_qa31_insert_job(table_id_t table_id,
                                         int64_t row_id,
                                         int32_t col_id,
                                         const std::string& text,
                                         const std::string& provider = "qa31_test",
                                         int32_t dim = 128) {
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

static EmbeddingJob make_qa31_update_job(table_id_t table_id,
                                         int64_t row_id,
                                         int32_t col_id,
                                         const std::string& text,
                                         const std::string& provider = "qa31_test",
                                         int32_t dim = 128) {
    EmbeddingJob job;
    job.table_id = table_id;
    job.row_id = row_id;
    job.column_id = col_id;
    job.source_text = text;
    job.provider = provider;
    job.dimension = dim;
    job.type = EmbeddingJob::Type::UPDATE;
    job.retry_count = 0;
    return job;
}

// =============================================================================
// QA_EmbeddingColumn — EmbeddingColumnManager adversarial tests
// =============================================================================

TEST(QA_EmbeddingColumn, NegativeDimensionFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("neg_dim"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = -1;
    def.source_expr = "title";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(*tid, {def});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_EmbeddingColumn, EmptyEmbeddingDefsSucceeds) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("empty_defs"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    auto result = mgr.register_table_embeddings(*tid, {});
    EXPECT_TRUE(result.has_value()) << result.error().message;

    // No embedding columns or indexes should be registered.
    auto embs = catalog.list_embedding_columns(*tid);
    EXPECT_TRUE(embs.empty());
    auto indexes = catalog.list_indexes(*tid);
    EXPECT_TRUE(indexes.empty());
}

TEST(QA_EmbeddingColumn, InsertJobsNoMatchingSourceText) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("no_match"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Pass source_texts with column_id 99 (doesn't match embedding column_id 3).
    auto jobs = mgr.create_insert_jobs(*tid, 1, {{99, "hello"}});
    ASSERT_TRUE(jobs.has_value());
    ASSERT_EQ(jobs->size(), 1u);
    // Job should have empty source_text since no matching column_id was found.
    EXPECT_TRUE((*jobs)[0].source_text.empty());
}

TEST(QA_EmbeddingColumn, UpdateJobsCaseSensitiveSourceExpr) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("case_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "Title"; // Capital T
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Changed column is "title" (lowercase) — source_expr_references is case-sensitive.
    auto jobs = mgr.create_update_jobs(*tid, 42, {"title"}, {{3, "new text"}});
    ASSERT_TRUE(jobs.has_value());
    // "Title" != "title" in the source_expr_references check, so no jobs.
    EXPECT_TRUE(jobs->empty());
}

TEST(QA_EmbeddingColumn, UpdateJobsSourceExprWithExtraWhitespace) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("ws_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "  title  ,  body  "; // Extra whitespace around commas.
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // The implementation trims whitespace, so "title" should match.
    auto jobs = mgr.create_update_jobs(*tid, 42, {"title"}, {{3, "new text"}});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_EQ(jobs->size(), 1u);
}

TEST(QA_EmbeddingColumn, UpdateJobsSourceExprPartialColumnNameNoMatch) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("partial_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "all_titles"; // Contains "title" as substring but not exact match.
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Changed column "title" should NOT match "all_titles".
    auto jobs = mgr.create_update_jobs(*tid, 42, {"title"}, {});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_TRUE(jobs->empty());
}

TEST(QA_EmbeddingColumn, DescribeNonExistentTableReturnsEmpty) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    // Table ID 99999 doesn't exist — describe should return empty, not crash.
    auto infos = mgr.describe_embedding_columns(99999);
    EXPECT_TRUE(infos.empty());
}

TEST(QA_EmbeddingColumn, MakeIndexNameWithSpecialChars) {
    // Verify index name generation with special characters in table/column names.
    auto name = EmbeddingColumnManager::make_index_name("my-table", "my_column");
    EXPECT_EQ(name, "hnsw_my-table_my_column");

    auto empty = EmbeddingColumnManager::make_index_name("", "");
    EXPECT_EQ(empty, "hnsw__");
}

TEST(QA_EmbeddingColumn, RegisterWithVeryLargeDimension) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("large_dim"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 100000; // Very large but valid.
    def.source_expr = "title";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(*tid, {def});
    EXPECT_TRUE(result.has_value()) << result.error().message;

    auto embs = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].dimension, 100000);
}

// =============================================================================
// QA_EmbeddingWorker — Worker pool adversarial tests
// =============================================================================

TEST(QA_EmbeddingWorker, ZeroWorkersEnqueueAndDrain) {
    // Pool with 0 workers — jobs should sit in queue.
    EmbeddingWorkerPool pool({.num_workers = 0});

    // Start should succeed even with 0 workers.
    ASSERT_TRUE(pool.start().has_value());
    EXPECT_TRUE(pool.is_running());

    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 1, 0, "hello")).has_value());
    EXPECT_EQ(pool.pending_count(), 1u);

    // Drain and verify.
    auto drained = pool.drain();
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].source_text, "hello");

    ASSERT_TRUE(pool.stop().has_value());
}

TEST(QA_EmbeddingWorker, NullStoreCallbackCountsAsProcessed) {
    // When store_callback_ is null, embeddings are generated but not stored.
    // The job should still be counted as processed.
    auto provider = std::make_shared<QA31TestProvider>(64);

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa31_test", provider);
    // Deliberately NOT setting store callback.

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 1, 0, "hello")).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && provider->batch_call_count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_GE(provider->batch_call_count(), 1);
    auto s = pool.stats();
    EXPECT_GE(s.jobs_processed, 1u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(QA_EmbeddingWorker, StressTestManyJobs) {
    auto provider = std::make_shared<QA31TestProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 4, .max_batch_size = 16});
    pool.register_provider("qa31_test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue 500 jobs.
    for (int i = 0; i < 500; ++i) {
        auto job = (i % 3 == 0) ? make_qa31_update_job(1, i, 0, "text_" + std::to_string(i))
                                : make_qa31_insert_job(1, i, 0, "text_" + std::to_string(i));
        ASSERT_TRUE(pool.enqueue(std::move(job)).has_value());
    }

    // Wait for all to process.
    for (int i = 0; i < 500 && store_count.load() < 500; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    EXPECT_EQ(store_count.load(), 500);
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 500u);
    EXPECT_EQ(s.jobs_failed, 0u);
}

TEST(QA_EmbeddingWorker, MixedProvidersSomeFailSomeSucceed) {
    auto good_provider = std::make_shared<QA31TestProvider>(64, false);
    auto bad_provider = std::make_shared<QA31TestProvider>(64, true);

    std::atomic<int> store_count{0};

    EmbeddingWorkerConfig config;
    config.num_workers = 1;
    config.max_batch_size = 32;
    config.max_retries = 0; // No retries — fail immediately.
    config.base_backoff = std::chrono::milliseconds(1);

    EmbeddingWorkerPool pool(config);
    pool.register_provider("good", good_provider);
    pool.register_provider("bad", bad_provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());

    // Enqueue 3 good, 2 bad.
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 1, 0, "a", "good", 64)).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 2, 0, "b", "good", 64)).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 3, 0, "c", "bad", 64)).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 4, 0, "d", "bad", 64)).has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 5, 0, "e", "good", 64)).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(pool.stop().has_value());

    // 3 good jobs should succeed, 2 bad should fail.
    EXPECT_EQ(store_count.load(), 3);
    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 3u);
    EXPECT_GE(s.jobs_failed, 2u);
}

TEST(QA_EmbeddingWorker, EnqueueBatchMixed) {
    EmbeddingWorkerPool pool({.num_workers = 0});

    std::vector<EmbeddingJob> jobs;
    // 3 inserts and 2 updates in mixed order.
    jobs.push_back(make_qa31_update_job(1, 1, 0, "u1"));
    jobs.push_back(make_qa31_insert_job(1, 2, 0, "i1"));
    jobs.push_back(make_qa31_update_job(1, 3, 0, "u2"));
    jobs.push_back(make_qa31_insert_job(1, 4, 0, "i2"));
    jobs.push_back(make_qa31_insert_job(1, 5, 0, "i3"));

    ASSERT_TRUE(pool.enqueue_batch(std::move(jobs)).has_value());
    EXPECT_EQ(pool.pending_count(), 5u);

    // Drain and verify priority ordering.
    auto drained = pool.drain();
    ASSERT_EQ(drained.size(), 5u);
    // Inserts first.
    EXPECT_EQ(drained[0].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[1].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[2].type, EmbeddingJob::Type::INSERT);
    // Updates last.
    EXPECT_EQ(drained[3].type, EmbeddingJob::Type::UPDATE);
    EXPECT_EQ(drained[4].type, EmbeddingJob::Type::UPDATE);
}

TEST(QA_EmbeddingWorker, StatsIncludeLatency) {
    auto provider = std::make_shared<QA31TestProvider>(32);
    std::atomic<int> store_count{0};

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa31_test", provider);
    pool.set_store_callback(
        [&](table_id_t, int64_t, int32_t, std::span<const float>) -> Result<void> {
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue(make_qa31_insert_job(1, 1, 0, "hello")).has_value());

    for (int i = 0; i < 100 && store_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    auto s = pool.stats();
    EXPECT_EQ(s.jobs_processed, 1u);
    // total_latency_ms should have been incremented (might be 0 if very fast).
    // Just verify it's a reasonable value.
    EXPECT_GE(s.total_latency_ms, 0u);
}

TEST(QA_EmbeddingWorker, ConcurrentEnqueueAndDrain) {
    EmbeddingWorkerPool pool({.num_workers = 0});

    // Enqueue from multiple threads simultaneously.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&pool, t]() {
            for (int i = 0; i < 50; ++i) {
                auto job = make_qa31_insert_job(1, t * 50 + i, 0, "text");
                auto result = pool.enqueue(std::move(job));
                EXPECT_TRUE(result.has_value());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(pool.pending_count(), 200u);

    auto drained = pool.drain();
    EXPECT_EQ(drained.size(), 200u);
    EXPECT_EQ(pool.pending_count(), 0u);
}

// =============================================================================
// QA_BuiltinProvider — Builtin provider adversarial tests
// =============================================================================

TEST(QA_BuiltinProvider, ZeroDimensionProducesEmptyVector) {
    BuiltinProvider provider(0);

    auto result = provider.embed("hello world");
    // With dimension 0, the hash loop doesn't execute, norm is 0,
    // and normalization is skipped. Should return an empty vector.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
    EXPECT_EQ(provider.dimension(), 0u);
}

TEST(QA_BuiltinProvider, DimensionOneWorks) {
    BuiltinProvider provider(1);

    auto result = provider.embed("hello world");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 1u);
    // Single dimension normalized to unit length → should be +1.0 or -1.0.
    EXPECT_NEAR(std::abs((*result)[0]), 1.0F, 1e-5F);
}

TEST(QA_BuiltinProvider, VeryLongText) {
    BuiltinProvider provider(64);

    // 10,000 word text.
    std::string long_text;
    for (int i = 0; i < 10000; ++i) {
        long_text += "word" + std::to_string(i) + " ";
    }

    auto result = provider.embed(long_text);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);

    // Should still be unit normalized.
    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0F, 1e-5F);
}

TEST(QA_BuiltinProvider, UnicodeText) {
    BuiltinProvider provider(64);

    auto result = provider.embed("hello 世界 🌍 résumé café");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);
}

TEST(QA_BuiltinProvider, PunctuationOnlyText) {
    BuiltinProvider provider(64);

    // Punctuation only — tokenizer should produce no words.
    auto result = provider.embed("!@#$%^&*()");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_BuiltinProvider, NumbersAreTokenized) {
    BuiltinProvider provider(64);

    // Numbers should be treated as tokens.
    auto result = provider.embed("12345");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 64u);
}

TEST(QA_BuiltinProvider, BatchEmbedEmptyList) {
    BuiltinProvider provider(64);

    auto result = provider.embed_batch({});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

TEST(QA_BuiltinProvider, BatchEmbedWithEmptyStringFails) {
    BuiltinProvider provider(64);

    // embed_batch calls embed for each text. If one fails, the whole batch fails.
    auto result = provider.embed_batch({"hello", "", "world"});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_BuiltinProvider, SingleWordText) {
    BuiltinProvider provider(128);

    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 128u);

    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0F, 1e-5F);
}

TEST(QA_BuiltinProvider, RepeatedWordProducesValidVector) {
    BuiltinProvider provider(64);

    // Same word repeated many times.
    auto result = provider.embed("hello hello hello hello hello hello hello hello");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 64u);

    float norm = 0.0F;
    for (float v : *result) {
        norm += v * v;
    }
    EXPECT_NEAR(std::sqrt(norm), 1.0F, 1e-5F);
}

// =============================================================================
// QA_ProviderRegistry — Provider registry adversarial tests
// =============================================================================

TEST(QA_ProviderRegistry, ResolveEmptyNameFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_ProviderRegistry, ResolveSlashOnlyFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("/");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_ProviderRegistry, ResolveLeadingSlashFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("/model");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_ProviderRegistry, ResolveTrailingSlashFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("type/");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_ProviderRegistry, ResolveMultipleSlashesUsesFirst) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "builtin/128/extra" → type="builtin", model="128/extra".
    auto result = registry.resolve("builtin/128/extra");
    // The builtin provider parses "128/extra" as model, tries stoul on it.
    // stoul("128/extra") → 128 (stops at '/').
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->dimension(), 128u);
}

TEST(QA_ProviderRegistry, ResolveBuiltinNonNumericModelUsesDefault) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "builtin/abc" → model="abc", stoul fails, uses default dim 384.
    auto result = registry.resolve("builtin/abc");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ((*result)->dimension(), 384u);
}

TEST(QA_ProviderRegistry, OllamaWithoutBaseUrlFails) {
    Catalog catalog;

    EmbeddingProviderConfig config;
    config.name = "test-ollama";
    config.type = "ollama";
    config.base_url = "";
    config.model = "all-minilm";
    ASSERT_TRUE(catalog.register_embedding_provider(config).has_value());

    ProviderRegistry registry(catalog);
    auto result = registry.resolve("test-ollama");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_ProviderRegistry, ConcurrentResolve) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // Resolve from multiple threads simultaneously — tests cache thread safety.
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            auto result = registry.resolve("builtin/256");
            if (result.has_value()) {
                success_count.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count.load(), 8);
}

// =============================================================================
// QA_OllamaProvider — Ollama provider edge cases
// =============================================================================

TEST(QA_OllamaProvider, EmbedEmptyText) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": [0.1, 0.2, 0.3]})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    // Empty text should still be sent to the API (provider decides if it's valid).
    auto result = provider.embed("");
    // The Ollama provider sends the text as-is; the HTTP client should succeed.
    EXPECT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA_OllamaProvider, EmbedEmptyEmbeddingArray) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    mock->set_post_response(200, R"({"embedding": []})");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(QA_OllamaProvider, Embed429TooManyRequests) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    mock->set_post_response(429, "rate limited");

    OllamaProvider provider("http://localhost:11434", "all-minilm", 3, std::move(mock));

    auto result = provider.embed("test");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

// =============================================================================
// QA_OpenAIProvider — OpenAI provider edge cases
// =============================================================================

TEST(QA_OpenAIProvider, BatchEmbedOutOfOrderIndices) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    // Response with indices out of order.
    mock->set_post_response(200, R"({
        "data": [
            {"embedding": [0.4, 0.5, 0.6], "index": 1},
            {"embedding": [0.1, 0.2, 0.3], "index": 0}
        ]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    // Should be reordered by index: [0] = [0.1,0.2,0.3], [1] = [0.4,0.5,0.6].
    EXPECT_FLOAT_EQ((*result)[0][0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1][0], 0.4F);
}

TEST(QA_OpenAIProvider, BatchEmbedEmptyDataArray) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    mock->set_post_response(200, R"({"data": []})");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed("test");
    // FIX (GDB-237): Provider now correctly rejects empty data array,
    // returning a PARSE_ERROR because no embeddings were received.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_OpenAIProvider, BatchEmbedCountMismatch) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    // Sent 2 texts but only got 1 embedding back.
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    OpenAIProvider provider("sk-test", "text-embedding-3-small", 3, std::move(mock));

    auto result = provider.embed_batch({"hello", "world"});
    // FIX (GDB-237): Provider now correctly detects that embedding at index 1
    // is missing and returns a PARSE_ERROR.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA_OpenAIProvider, EmptyApiKey) {
    auto mock = std::make_unique<QA31MockHttpClient>();
    mock->set_post_response(200, R"({
        "data": [{"embedding": [0.1, 0.2, 0.3], "index": 0}]
    })");

    // Provider with empty API key — should still send the request (validation
    // is done at the registry level).
    OpenAIProvider provider("", "text-embedding-3-small", 3, std::move(mock));
    auto result = provider.embed("test");
    EXPECT_TRUE(result.has_value());
}

// =============================================================================
// QA_EmbeddingColumnEndToEnd — Integration-style adversarial tests
// =============================================================================

TEST(QA_EmbeddingColumnE2E, RegisterThenDropTableCleanup) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("cleanup_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 128;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Verify registration.
    EXPECT_EQ(catalog.list_embedding_columns(*tid).size(), 1u);
    EXPECT_TRUE(catalog.get_index("hnsw_cleanup_test_embedding").has_value());

    // Drop table — should cascade cleanup.
    ASSERT_TRUE(catalog.drop_table(default_database_id, "cleanup_test").has_value());

    // After drop: embedding columns and indexes should be gone.
    EXPECT_TRUE(catalog.list_all_embedding_columns().empty());
    EXPECT_FALSE(catalog.get_index("hnsw_cleanup_test_embedding").has_value());
}

TEST(QA_EmbeddingColumnE2E, FullPipeline) {
    // Simulate the full flow: register embedding → create insert jobs →
    // enqueue → process → verify stored.
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("pipeline_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 32;
    def.source_expr = "title";
    def.provider = "qa31_test";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Create insert jobs.
    auto jobs = mgr.create_insert_jobs(*tid, 1, {{3, "hello world"}});
    ASSERT_TRUE(jobs.has_value());
    ASSERT_EQ(jobs->size(), 1u);

    // Set up worker pool.
    auto provider = std::make_shared<QA31TestProvider>(32);
    std::atomic<int> store_count{0};
    table_id_t stored_table_id = 0;
    int64_t stored_row_id = 0;
    int32_t stored_col_id = 0;
    size_t stored_vec_size = 0;

    EmbeddingWorkerPool pool({.num_workers = 1, .max_batch_size = 32});
    pool.register_provider("qa31_test", provider);
    pool.set_store_callback(
        [&](table_id_t t, int64_t r, int32_t c, std::span<const float> vec) -> Result<void> {
            stored_table_id = t;
            stored_row_id = r;
            stored_col_id = c;
            stored_vec_size = vec.size();
            store_count.fetch_add(1);
            return ok();
        });

    ASSERT_TRUE(pool.start().has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(*jobs)).has_value());

    // Wait for processing.
    for (int i = 0; i < 100 && store_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(pool.stop().has_value());

    // Verify the stored callback was called with correct params.
    EXPECT_EQ(store_count.load(), 1);
    EXPECT_EQ(stored_table_id, *tid);
    EXPECT_EQ(stored_row_id, 1);
    EXPECT_EQ(stored_col_id, 3);
    EXPECT_EQ(stored_vec_size, 32u);
}

TEST(QA_EmbeddingColumnE2E, UpdateThenInsertPriority) {
    // Verify that insert jobs are processed before update jobs.
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_qa31_schema("priority_test"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 32;
    def.source_expr = "title";
    def.provider = "qa31_test";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Create update jobs first, then insert jobs.
    auto update_jobs = mgr.create_update_jobs(*tid, 1, {"title"}, {{3, "updated text"}});
    ASSERT_TRUE(update_jobs.has_value());
    ASSERT_EQ(update_jobs->size(), 1u);
    EXPECT_EQ((*update_jobs)[0].type, EmbeddingJob::Type::UPDATE);

    auto insert_jobs = mgr.create_insert_jobs(*tid, 2, {{3, "new text"}});
    ASSERT_TRUE(insert_jobs.has_value());
    ASSERT_EQ(insert_jobs->size(), 1u);
    EXPECT_EQ((*insert_jobs)[0].type, EmbeddingJob::Type::INSERT);

    // Enqueue to pool and drain — inserts should come before updates.
    EmbeddingWorkerPool pool({.num_workers = 0});
    ASSERT_TRUE(pool.enqueue_batch(std::move(*update_jobs)).has_value());
    ASSERT_TRUE(pool.enqueue_batch(std::move(*insert_jobs)).has_value());

    auto drained = pool.drain();
    ASSERT_EQ(drained.size(), 2u);
    EXPECT_EQ(drained[0].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ(drained[1].type, EmbeddingJob::Type::UPDATE);
}
