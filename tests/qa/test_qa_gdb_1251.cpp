/// @file test_qa_gdb_1251.cpp
/// @brief QA regression tests for GDB-1251 (AC6).
///
/// WITHIN TRAVERSE must validate that the graph traversal actually reaches the
/// table that owns the EMBEDDING column referenced by NEAREST. Before the fix,
/// reachable PKs were intersected with the vector table's heap by raw numeric
/// equality, so a heterogeneous edge that reached the WRONG table (e.g.
/// follows readers->readers while NEAREST is on books) silently produced a
/// garbage scope from cross-table PK collisions.
///
/// These tests cover the acceptance criteria:
///  - AC1: error when the traversal reaches the wrong table (canonical repro).
///  - AC2: heterogeneous traversal ENDING at the vector table is allowed, and
///         a start-node PK collision does not leak into scope.
///  - AC3: OUT / IN / BOTH direction matrix across homogeneous (allowed) and
///         heterogeneous-reaching-wrong-table (error) edges.
///  - AC4: the traversal start key is coerced against the START table's PK
///         type, not the vector table's PK type (STRING-PK regression).

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/status.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/index_manager.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/vector/embedding_column.h"
#include "sixseven/vector/provider_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

namespace {

// =============================================================================
// Fixture
// =============================================================================

class QA_GDB1251 : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1251";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        init_test_catalog(catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_engine_ = std::make_unique<GraphEngine>(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_, graph_engine_.get());
        provider_registry_ = std::make_unique<ProviderRegistry>(catalog_);
    }

    void TearDown() override {
        engine_.reset();
        graph_engine_.reset();
        storage_.reset();
        provider_registry_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        ASSERT_TRUE(result.has_value()) << "SQL failed: " << sql << "\n" << result.error().message;
    }

    void register_embedding(table_id_t table_id,
                            int32_t col_id,
                            int32_t dim,
                            const std::string& source,
                            const std::string& provider) {
        EmbeddingColumnDef emb_def;
        emb_def.table_id = table_id;
        emb_def.column_id = col_id;
        emb_def.dimension = dim;
        emb_def.source_expr = source;
        emb_def.provider = provider;
        auto reg = catalog_.register_embedding_column(emb_def);
        ASSERT_TRUE(reg.has_value()) << reg.error().message;

        if (catalog_.get_embedding_provider(provider).has_value()) {
            return;
        }
        EmbeddingProviderConfig prov_config;
        prov_config.name = provider;
        prov_config.type = "builtin";
        prov_config.dimension = dim;
        auto prov_reg = catalog_.register_embedding_provider(prov_config);
        ASSERT_TRUE(prov_reg.has_value()) << prov_reg.error().message;
    }

    /// books(id INT PK, title, description_vec EMBEDDING),
    /// readers(id INT PK, name),
    /// edges: reads(readers->books), follows(readers->readers),
    /// similar_to(books->books).
    void setup_schema() {
        exec_ok(
            "CREATE TABLE books (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
        exec_ok("CREATE TABLE readers (id INT PRIMARY KEY, name VARCHAR)");

        auto books = catalog_.get_table(default_database_id, "books");
        ASSERT_TRUE(books.has_value());
        register_embedding(books->table_id, 2, 4, "title", "builtin/4");
        engine_->set_provider_registry(provider_registry_.get());

        exec_ok("CREATE EDGE TYPE reads FROM readers TO books");
        exec_ok("CREATE EDGE TYPE follows FROM readers TO readers");
        exec_ok("CREATE EDGE TYPE similar_to FROM books TO books");
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_engine_;
    std::unique_ptr<QueryEngine> engine_;
    std::unique_ptr<ProviderRegistry> provider_registry_;
};

} // namespace

// =============================================================================
// AC1: traversal reaching the WRONG table is rejected (canonical repro).
// =============================================================================

TEST_F(QA_GDB1251, AC1_FollowsReachesReadersButNearestOnBooks_Errors) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value()) << "follows reaches readers, NEAREST is on books";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    const std::string& msg = result.error().message;
    EXPECT_NE(msg.find("follows"), std::string::npos) << msg;
    EXPECT_NE(msg.find("readers"), std::string::npos) << msg;
    EXPECT_NE(msg.find("books"), std::string::npos) << msg;
}

// =============================================================================
// AC2: heterogeneous edge ENDING at the vector table is allowed; the start-node
// PK collision must not leak into scope.
// =============================================================================

TEST_F(QA_GDB1251, AC2_ReadsEndingAtBooks_AllowedAndStartCollisionExcluded) {
    setup_schema();
    exec_ok("INSERT INTO readers VALUES (1, 'alice')");
    // Book id 1 numerically collides with reader 1's id but is NOT reached.
    exec_ok("INSERT INTO books VALUES (1, 'collision', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (10, 'reached_a', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (11, 'reached_b', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (12, 'unreached', [0.0, 0.0, 1.0, 0.0])");

    exec_ok("LINK readers(1) TO books(10) VIA reads");
    exec_ok("LINK readers(1) TO books(11) VIA reads");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM readers(1) DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    std::sort(ids.begin(), ids.end());
    ASSERT_EQ(ids.size(), 2u) << "expected exactly the reached books {10, 11}";
    EXPECT_EQ(ids[0], 10);
    EXPECT_EQ(ids[1], 11);
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 1), 0)
        << "start-node PK collision (book id 1) leaked into scope";
    EXPECT_EQ(std::count(ids.begin(), ids.end(), 12), 0) << "unreached book leaked into scope";
}

// =============================================================================
// AC3: direction matrix.
// =============================================================================

// reads DIRECTION IN from a book reaches readers -> error.
TEST_F(QA_GDB1251, AC3_ReadsDirectionIn_ReachesReaders_Errors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result = engine_->execute("SELECT title FROM books "
                                   "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                                   "WITHIN TRAVERSE reads FROM books(1) DIRECTION IN MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("readers"), std::string::npos) << result.error().message;
}

// similar_to DIRECTION IN (books->books) reaches books -> allowed.
TEST_F(QA_GDB1251, AC3_SimilarToDirectionIn_ReachesBooks_Allowed) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE similar_to FROM books(2) DIRECTION IN MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(result->rows.size(), 1u);
}

// reads DIRECTION BOTH (heterogeneous) -> error.
TEST_F(QA_GDB1251, AC3_ReadsDirectionBoth_Heterogeneous_Errors) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'b', [1.0, 0.0, 0.0, 0.0])");

    auto result =
        engine_->execute("SELECT title FROM books "
                         "WHERE NEAREST(description_vec, 3) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads FROM books(1) DIRECTION BOTH MAX_DEPTH 2");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// similar_to DIRECTION BOTH (homogeneous) -> allowed.
TEST_F(QA_GDB1251, AC3_SimilarToDirectionBoth_Homogeneous_Allowed) {
    setup_schema();
    exec_ok("INSERT INTO books VALUES (1, 'a', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books VALUES (2, 'b', [0.9, 0.1, 0.0, 0.0])");
    exec_ok("LINK books(1) TO books(2) VIA similar_to");

    auto result =
        engine_->execute("SELECT id FROM books "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE similar_to FROM books(1) DIRECTION BOTH MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_GE(result->rows.size(), 1u);
}

// =============================================================================
// AC4: start key is coerced against the START table's PK type (STRING), not the
// vector table's PK type (INT).
// =============================================================================

TEST_F(QA_GDB1251, AC4_StringPkStartKey_CoercedAgainstStartTable) {
    exec_ok("CREATE TABLE books2 (id INT PRIMARY KEY, title VARCHAR, description_vec EMBEDDING)");
    exec_ok("CREATE TABLE readers2 (id VARCHAR PRIMARY KEY, name VARCHAR)");

    auto books = catalog_.get_table(default_database_id, "books2");
    ASSERT_TRUE(books.has_value());
    register_embedding(books->table_id, 2, 4, "title", "builtin/4");
    engine_->set_provider_registry(provider_registry_.get());

    exec_ok("CREATE EDGE TYPE reads2 FROM readers2 TO books2");
    exec_ok("INSERT INTO readers2 VALUES ('alice', 'Alice')");
    exec_ok("INSERT INTO books2 VALUES (10, 'reached', [1.0, 0.0, 0.0, 0.0])");
    exec_ok("INSERT INTO books2 VALUES (11, 'unreached', [0.0, 1.0, 0.0, 0.0])");
    exec_ok("LINK readers2('alice') TO books2(10) VIA reads2");

    auto result =
        engine_->execute("SELECT id FROM books2 "
                         "WHERE NEAREST(description_vec, 5) TO [1.0, 0.0, 0.0, 0.0] "
                         "WITHIN TRAVERSE reads2 FROM readers2('alice') DIRECTION OUT MAX_DEPTH 1");
    ASSERT_TRUE(result.has_value()) << result.error().message;

    std::vector<int32_t> ids;
    for (const auto& row : result->rows) {
        ASSERT_FALSE(row.empty());
        ids.push_back(row[0].as_int32());
    }
    ASSERT_EQ(ids.size(), 1u) << "expected exactly the reached book {10}";
    EXPECT_EQ(ids[0], 10);
}
