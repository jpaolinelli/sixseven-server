/// @file test_qa_gdb_282.cpp
/// QA regression tests for GDB-282: Wire INSERT path to trigger async EMBEDDING
/// column generation.
///
/// These tests verify the end-to-end INSERT → embedding generation → storage
/// pipeline with adversarial edge cases, boundary values, and stress scenarios.

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/builtin_provider.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// Test fixture
// =============================================================================

class QA_GDB282 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb282";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);

        pool_ = std::make_unique<EmbeddingWorkerPool>(
            EmbeddingWorkerConfig{.num_workers = 1,
                                  .max_batch_size = 32,
                                  .max_retries = 2,
                                  .base_backoff = std::chrono::milliseconds{10},
                                  .max_backoff = std::chrono::milliseconds{50}});
        pool_->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));
        pool_->register_provider("builtin/8", std::make_shared<BuiltinProvider>(8));
        pool_->register_provider("builtin/16", std::make_shared<BuiltinProvider>(16));

        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
        engine_->set_provider_registry(&provider_registry_);
        engine_->set_embedding_worker_pool(pool_.get());

        auto start = pool_->start();
        ASSERT_TRUE(start.has_value()) << start.error().message;
    }

    void TearDown() override {
        if (pool_ && pool_->is_running()) {
            auto stop = pool_->stop();
            EXPECT_TRUE(stop.has_value());
        }
        engine_.reset();
        pool_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    void exec_fail(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_FALSE(result.has_value());
    }

    bool wait_for_pool(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pool_->pending_count() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }

    DiskManager dm_;
    Catalog catalog_;
    ProviderRegistry provider_registry_{catalog_};
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<EmbeddingWorkerPool> pool_;
    std::unique_ptr<QueryEngine> engine_;
};

// =============================================================================
// AC1: INSERT triggers embedding generation for EMBEDDING columns
// =============================================================================

TEST_F(QA_GDB282, AC1_SingleRowInsertGeneratesEmbedding) {
    exec_ok("CREATE TABLE t1 (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t1 (id, title) VALUES (1, 'hello world')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title, vec FROM t1 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "hello world");
    EXPECT_FALSE(qr.rows[0][2].is_null());
    EXPECT_EQ(qr.rows[0][2].type_id(), TypeId::EMBEDDING);
    EXPECT_EQ(qr.rows[0][2].as_embedding().size(), 4u);
}

TEST_F(QA_GDB282, AC1_MultiRowInsertGeneratesEmbeddings) {
    exec_ok("CREATE TABLE t2 (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");
    exec_ok("INSERT INTO t2 (id, body) VALUES "
            "(1, 'first'), (2, 'second'), (3, 'third'), (4, 'fourth'), (5, 'fifth')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t2");
    ASSERT_EQ(qr.rows.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_FALSE(qr.rows[i][1].is_null()) << "row " << i << " embedding is null";
        EXPECT_EQ(qr.rows[i][1].as_embedding().size(), 4u);
    }
}

// =============================================================================
// AC2: Multiple EMBEDDING columns on same table
// =============================================================================

TEST_F(QA_GDB282, AC2_TwoEmbeddingColumnsDifferentDimensions) {
    exec_ok("CREATE TABLE t3 ("
            "  id INT, title TEXT, body TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4'),"
            "  body_vec EMBEDDING(8, body, 'builtin/8')"
            ")");

    exec_ok("INSERT INTO t3 (id, title, body) VALUES (1, 'Title Text', 'Body Text Content')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT title_vec, body_vec FROM t3 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);

    // title_vec: dimension 4
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);

    // body_vec: dimension 8
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 8u);
}

TEST_F(QA_GDB282, AC2_ThreeEmbeddingColumnsSameSource) {
    // Two embedding columns sourcing from the same text column.
    exec_ok("CREATE TABLE t4 ("
            "  id INT, text TEXT,"
            "  vec4 EMBEDDING(4, text, 'builtin/4'),"
            "  vec8 EMBEDDING(8, text, 'builtin/8'),"
            "  vec16 EMBEDDING(16, text, 'builtin/16')"
            ")");

    exec_ok("INSERT INTO t4 (id, text) VALUES (1, 'same source text')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT vec4, vec8, vec16 FROM t4 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 8u);
    EXPECT_EQ(qr.rows[0][2].as_embedding().size(), 16u);
}

// =============================================================================
// AC3: NULL source text leaves embedding NULL
// =============================================================================

TEST_F(QA_GDB282, AC3_NullSourceTextLeavesEmbeddingNull) {
    exec_ok("CREATE TABLE t5 (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t5 (id, title) VALUES (1, NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t5 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

TEST_F(QA_GDB282, AC3_MixedNullAndNonNull_ValidRowsOnly) {
    // Verify that rows with non-null source text get embeddings when
    // NOT batched with null-source rows. Separate from the batch poisoning
    // bug test (BUG_BatchPoisoningNullSourceLosesValidEmbeddings).
    exec_ok("CREATE TABLE t6 (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // Insert valid-text rows first, wait for processing.
    exec_ok("INSERT INTO t6 (id, title) VALUES (1, 'has text')");
    exec_ok("INSERT INTO t6 (id, title) VALUES (3, 'also text')");
    ASSERT_TRUE(wait_for_pool());

    // Insert null-source rows separately.
    exec_ok("INSERT INTO t6 (id, title) VALUES (2, NULL)");
    exec_ok("INSERT INTO t6 (id, title) VALUES (4, NULL)");
    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t6 ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 4u);

    ASSERT_FALSE(qr.rows[0][1].is_null()) << "row 1 should have embedding";
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
    EXPECT_TRUE(qr.rows[1][1].is_null()) << "row 2 should be null (NULL source)";
    ASSERT_FALSE(qr.rows[2][1].is_null()) << "row 3 should have embedding";
    EXPECT_EQ(qr.rows[2][1].as_embedding().size(), 4u);
    EXPECT_TRUE(qr.rows[3][1].is_null()) << "row 4 should be null (NULL source)";
}

// =============================================================================
// AC4: Tables without EMBEDDING columns are unaffected
// =============================================================================

TEST_F(QA_GDB282, AC4_TableWithoutEmbeddingColumnsNormal) {
    exec_ok("CREATE TABLE plain (id INT, name TEXT, value FLOAT)");
    auto qr = exec_ok("INSERT INTO plain VALUES (1, 'test', 3.14)");
    EXPECT_EQ(qr.affected_rows, 1);
    EXPECT_EQ(pool_->pending_count(), 0u);

    auto sel = exec_ok("SELECT * FROM plain");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0][0].as_int32(), 1);
    EXPECT_EQ(sel.rows[0][1].as_string(), "test");
}

// =============================================================================
// Adversarial: Column ordering
// =============================================================================

TEST_F(QA_GDB282, ReorderedColumnInsertGeneratesEmbedding) {
    // INSERT with columns in a different order than the schema.
    exec_ok("CREATE TABLE t_reorder ("
            "  id INT, title TEXT, body TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Columns specified in reverse order: body, title, id.
    exec_ok("INSERT INTO t_reorder (body, title, id) VALUES ('body text', 'title text', 1)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title, title_vec FROM t_reorder WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "title text");
    // The embedding should still be generated from 'title', not 'body'.
    EXPECT_FALSE(qr.rows[0][2].is_null());
    EXPECT_EQ(qr.rows[0][2].as_embedding().size(), 4u);
}

TEST_F(QA_GDB282, ReorderedColumnDifferentEmbeddingColumns) {
    // Two embedding columns, insert with columns reordered.
    exec_ok("CREATE TABLE t_reorder2 ("
            "  id INT, title TEXT, body TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4'),"
            "  body_vec EMBEDDING(8, body, 'builtin/8')"
            ")");

    exec_ok(
        "INSERT INTO t_reorder2 (body, id, title) VALUES ('body content', 42, 'title content')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title_vec, body_vec FROM t_reorder2");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 42);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
    EXPECT_FALSE(qr.rows[0][2].is_null());
    EXPECT_EQ(qr.rows[0][2].as_embedding().size(), 8u);
}

// =============================================================================
// Adversarial: Empty string source text
// =============================================================================

TEST_F(QA_GDB282, EmptyStringSourceLeavesEmbeddingNull) {
    // An empty string '' is not NULL, but the builtin provider rejects it.
    exec_ok("CREATE TABLE t_empty (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_empty (id, title) VALUES (1, '')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_empty WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Empty string → BuiltinProvider returns error → embedding stays null after retries exhaust.
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

// =============================================================================
// Adversarial: Whitespace-only and punctuation-only source text
// =============================================================================

TEST_F(QA_GDB282, WhitespaceOnlySourceLeavesEmbeddingNull) {
    exec_ok("CREATE TABLE t_ws (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_ws (id, title) VALUES (1, '     ')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_ws WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Whitespace tokenizes to zero words → provider returns error.
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

TEST_F(QA_GDB282, PunctuationOnlySourceLeavesEmbeddingNull) {
    exec_ok("CREATE TABLE t_punct (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_punct (id, title) VALUES (1, '!!! ??? ...')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_punct WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][1].is_null());
}

// =============================================================================
// Adversarial: Boundary text lengths
// =============================================================================

TEST_F(QA_GDB282, SingleCharacterSourceGeneratesEmbedding) {
    exec_ok("CREATE TABLE t_char (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_char (id, title) VALUES (1, 'a')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT vec FROM t_char WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);
}

TEST_F(QA_GDB282, LongSourceTextGeneratesEmbedding) {
    exec_ok("CREATE TABLE t_long (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");

    // Build a 500-character string (fits in a page but is still sizable).
    std::string long_text(500, 'x');
    for (size_t i = 50; i < long_text.size(); i += 50) {
        long_text[i] = ' ';
    }
    exec_ok("INSERT INTO t_long (id, body) VALUES (1, '" + long_text + "')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT vec FROM t_long WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Deterministic embedding output
// =============================================================================

TEST_F(QA_GDB282, SameInputProducesSameEmbedding) {
    exec_ok("CREATE TABLE t_det (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_det (id, title) VALUES (1, 'deterministic test')");
    exec_ok("INSERT INTO t_det (id, title) VALUES (2, 'deterministic test')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_det ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_FALSE(qr.rows[0][1].is_null());
    ASSERT_FALSE(qr.rows[1][1].is_null());

    auto& emb1 = qr.rows[0][1].as_embedding();
    auto& emb2 = qr.rows[1][1].as_embedding();
    ASSERT_EQ(emb1.size(), emb2.size());
    for (size_t i = 0; i < emb1.size(); ++i) {
        EXPECT_FLOAT_EQ(emb1[i], emb2[i]) << "dimension " << i << " differs";
    }
}

TEST_F(QA_GDB282, DifferentInputProducesDifferentEmbedding) {
    exec_ok("CREATE TABLE t_diff (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_diff (id, title) VALUES (1, 'apple')");
    exec_ok("INSERT INTO t_diff (id, title) VALUES (2, 'banana')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_diff ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 2u);
    ASSERT_FALSE(qr.rows[0][1].is_null());
    ASSERT_FALSE(qr.rows[1][1].is_null());

    auto& emb1 = qr.rows[0][1].as_embedding();
    auto& emb2 = qr.rows[1][1].as_embedding();
    bool all_equal = true;
    for (size_t i = 0; i < emb1.size() && i < emb2.size(); ++i) {
        if (emb1[i] != emb2[i]) {
            all_equal = false;
            break;
        }
    }
    EXPECT_FALSE(all_equal) << "different inputs should produce different embeddings";
}

// =============================================================================
// Adversarial: Multiple separate INSERTs accumulate correctly
// =============================================================================

TEST_F(QA_GDB282, SequentialInsertsAllGetEmbeddings) {
    exec_ok("CREATE TABLE t_seq (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    for (int i = 1; i <= 10; ++i) {
        exec_ok("INSERT INTO t_seq (id, title) VALUES (" + std::to_string(i) + ", 'text " +
                std::to_string(i) + "')");
    }

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_seq ORDER BY id");
    ASSERT_EQ(qr.rows.size(), 10u);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_FALSE(qr.rows[i][1].is_null()) << "row " << (i + 1) << " embedding is null";
        EXPECT_EQ(qr.rows[i][1].as_embedding().size(), 4u);
    }
}

// =============================================================================
// Adversarial: Stress — large batch insert
// =============================================================================

TEST_F(QA_GDB282, StressLargeBatchInsert) {
    exec_ok("CREATE TABLE t_stress (id INT, body TEXT, vec EMBEDDING(4, body, 'builtin/4'))");

    // 30-row multi-value INSERT. Uses short text to avoid page overflow
    // (GDB-212: update_tuple corruption on full pages).
    std::string sql = "INSERT INTO t_stress (id, body) VALUES ";
    for (int i = 1; i <= 30; ++i) {
        if (i > 1)
            sql += ", ";
        sql += "(" + std::to_string(i) + ", 'doc" + std::to_string(i) + "')";
    }
    exec_ok(sql);

    ASSERT_TRUE(wait_for_pool(std::chrono::milliseconds{10000}));

    auto qr = exec_ok("SELECT COUNT(*) FROM t_stress WHERE vec IS NOT NULL");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int64(), 30);
}

// =============================================================================
// Adversarial: Worker pool stats tracking
// =============================================================================

TEST_F(QA_GDB282, StatsReflectProcessedJobs) {
    exec_ok("CREATE TABLE t_stats (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    auto before = pool_->stats();

    exec_ok("INSERT INTO t_stats (id, title) VALUES (1, 'stat test 1'), (2, 'stat test 2')");

    ASSERT_TRUE(wait_for_pool());

    auto after = pool_->stats();
    // Two rows → two embedding jobs.
    EXPECT_GE(after.jobs_processed, before.jobs_processed + 2);
}

// =============================================================================
// Adversarial: Source column with special characters
// =============================================================================

TEST_F(QA_GDB282, SourceWithSpecialCharacters) {
    exec_ok("CREATE TABLE t_special (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // Text with unicode-like characters, mixed alphanumeric/special.
    exec_ok("INSERT INTO t_special (id, title) VALUES (1, 'hello-world_test 123!@#$% foo')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT vec FROM t_special WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    // Should still extract words: hello, world, test, 123, foo → valid embedding.
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Omitted source column defaults to NULL
// =============================================================================

TEST_F(QA_GDB282, OmittedSourceColumnDefaultsToNull) {
    exec_ok("CREATE TABLE t_omit ("
            "  id INT, title TEXT, body TEXT,"
            "  vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Insert without the 'title' column — it defaults to NULL.
    exec_ok("INSERT INTO t_omit (id, body) VALUES (1, 'some body text')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, title, vec FROM t_omit WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][1].is_null()) << "title should be NULL";
    EXPECT_TRUE(qr.rows[0][2].is_null()) << "vec should be NULL (no source text)";
}

// =============================================================================
// Adversarial: Insert with explicit NULL in VALUES for embedding column
// =============================================================================

TEST_F(QA_GDB282, ExplicitNullForEmbeddingColumnDoesNotCrash) {
    exec_ok("CREATE TABLE t_expnull ("
            "  id INT, title TEXT,"
            "  vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    // Explicitly provide NULL for the embedding column itself.
    exec_ok("INSERT INTO t_expnull (id, title, vec) VALUES (1, 'hello', NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_expnull WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    // The embedding should be generated from 'title', overwriting the explicit NULL.
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Multiple inserts with same ID (no unique constraint)
// =============================================================================

TEST_F(QA_GDB282, DuplicateIdRowsBothGetEmbeddings) {
    exec_ok("CREATE TABLE t_dup (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    exec_ok("INSERT INTO t_dup (id, title) VALUES (1, 'first version')");
    exec_ok("INSERT INTO t_dup (id, title) VALUES (1, 'second version')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT title, vec FROM t_dup WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_FALSE(qr.rows[i][1].is_null()) << "row " << i << " embedding is null";
        EXPECT_EQ(qr.rows[i][1].as_embedding().size(), 4u);
    }
}

// =============================================================================
// Adversarial: Insert with all columns specified in schema order
// =============================================================================

TEST_F(QA_GDB282, InsertAllColumnsSchemaOrder) {
    exec_ok("CREATE TABLE t_full (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    // Provide all columns including the embedding column explicitly as NULL.
    exec_ok("INSERT INTO t_full VALUES (1, 'full insert', NULL)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT vec FROM t_full WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][0].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Embedding column at different schema positions
// =============================================================================

TEST_F(QA_GDB282, EmbeddingColumnFirstPosition) {
    // Embedding column is the first column in the schema.
    exec_ok("CREATE TABLE t_first ("
            "  vec EMBEDDING(4, title, 'builtin/4'),"
            "  id INT,"
            "  title TEXT"
            ")");

    exec_ok("INSERT INTO t_first (id, title) VALUES (1, 'position test')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec FROM t_first WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
}

TEST_F(QA_GDB282, EmbeddingColumnMiddlePosition) {
    exec_ok("CREATE TABLE t_mid ("
            "  id INT,"
            "  vec EMBEDDING(4, title, 'builtin/4'),"
            "  title TEXT,"
            "  extra INT"
            ")");

    exec_ok("INSERT INTO t_mid (id, title, extra) VALUES (1, 'middle test', 42)");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, vec, extra FROM t_mid WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
    EXPECT_EQ(qr.rows[0][2].as_int32(), 42);
}

// =============================================================================
// Adversarial: Unregistered provider
// =============================================================================

TEST_F(QA_GDB282, UnregisteredProviderSilentlyFails) {
    // The 'builtin/999' provider is not registered in the pool.
    exec_ok("CREATE TABLE t_noprov ("
            "  id INT, title TEXT,"
            "  vec EMBEDDING(4, title, 'builtin/999')"
            ")");

    // INSERT should succeed (embedding is async best-effort).
    auto qr = exec_ok("INSERT INTO t_noprov (id, title) VALUES (1, 'no provider')");
    EXPECT_EQ(qr.affected_rows, 1);

    ASSERT_TRUE(wait_for_pool());

    // Embedding stays null because provider doesn't exist.
    auto sel = exec_ok("SELECT id, vec FROM t_noprov WHERE id = 1");
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_TRUE(sel.rows[0][1].is_null());
}

// =============================================================================
// Adversarial: Affected row count correctness
// =============================================================================

TEST_F(QA_GDB282, AffectedRowCountCorrectWithEmbedding) {
    exec_ok("CREATE TABLE t_count (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");

    auto qr1 = exec_ok("INSERT INTO t_count (id, title) VALUES (1, 'one')");
    EXPECT_EQ(qr1.affected_rows, 1);

    auto qr3 = exec_ok("INSERT INTO t_count (id, title) VALUES "
                       "(2, 'two'), (3, 'three'), (4, 'four')");
    EXPECT_EQ(qr3.affected_rows, 3);
}

// =============================================================================
// Adversarial: Verify embedding is stored in the correct column
// =============================================================================

TEST_F(QA_GDB282, EmbeddingStoredInCorrectColumn) {
    // Ensure embedding is stored in the right column, not in adjacent columns.
    exec_ok("CREATE TABLE t_correct ("
            "  id INT, before_col TEXT, title TEXT, after_col TEXT,"
            "  title_vec EMBEDDING(4, title, 'builtin/4')"
            ")");

    exec_ok("INSERT INTO t_correct (id, before_col, title, after_col) "
            "VALUES (1, 'before', 'target text', 'after')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT id, before_col, title, after_col, title_vec FROM t_correct");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_int32(), 1);
    EXPECT_EQ(qr.rows[0][1].as_string(), "before");
    EXPECT_EQ(qr.rows[0][2].as_string(), "target text");
    EXPECT_EQ(qr.rows[0][3].as_string(), "after");
    EXPECT_FALSE(qr.rows[0][4].is_null());
    EXPECT_EQ(qr.rows[0][4].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Source text with embedded quotes
// =============================================================================

TEST_F(QA_GDB282, SourceTextWithEscapedQuotes) {
    exec_ok("CREATE TABLE t_quote (id INT, title TEXT, vec EMBEDDING(4, title, 'builtin/4'))");
    exec_ok("INSERT INTO t_quote (id, title) VALUES (1, 'it''s a test')");

    ASSERT_TRUE(wait_for_pool());

    auto qr = exec_ok("SELECT title, vec FROM t_quote WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "it's a test");
    EXPECT_FALSE(qr.rows[0][1].is_null());
    EXPECT_EQ(qr.rows[0][1].as_embedding().size(), 4u);
}

// =============================================================================
// Adversarial: Pool not started (embedding_pool_ is set but pool never started)
// =============================================================================

TEST_F(QA_GDB282, PoolNotStartedStillInsertsSuccessfully) {
    // Create a fresh engine with a non-started pool.
    auto pool2 = std::make_unique<EmbeddingWorkerPool>(
        EmbeddingWorkerConfig{.num_workers = 1, .max_batch_size = 32});
    pool2->register_provider("builtin/4", std::make_shared<BuiltinProvider>(4));
    // Intentionally NOT calling pool2->start().

    auto engine2 = std::make_unique<QueryEngine>(catalog_, *storage_);
    engine2->set_provider_registry(&provider_registry_);
    engine2->set_embedding_worker_pool(pool2.get());

    auto cr = engine2->execute("CREATE TABLE t_nostart (id INT, title TEXT, "
                               "vec EMBEDDING(4, title, 'builtin/4'))");
    ASSERT_TRUE(cr.has_value()) << cr.error().message;

    // INSERT should still succeed — jobs are enqueued but not processed.
    auto ir = engine2->execute("INSERT INTO t_nostart (id, title) VALUES (1, 'test')");
    ASSERT_TRUE(ir.has_value()) << ir.error().message;
    EXPECT_EQ(ir->affected_rows, 1);

    // Jobs are pending in the queue.
    EXPECT_GE(pool2->pending_count(), 1u);

    engine2.reset();
    pool2.reset();
}

// NOTE: The batch-poisoning repro (mixed NULL/valid multi-row INSERT) is
// covered canonically by TEST_F(QA_GDB297, MixedNullValidBatchDoesNotPoisonValidEmbeddings)
// in test_qa_gdb_297.cpp — that file is the regression suite for the GDB-297
// fix and expands on the scenario extensively. Duplicate removed per GDB-864.
