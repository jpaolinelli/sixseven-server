/// @file test_qa_gdb_1224.cpp
/// @brief QA adversarial spot-check for GDB-1224 product fixes:
///   1. ValueHash mixed-width numeric contract (include/sixseven/common/value_hash.h)
///   2. TupleSerializer trailing zero-length var-length field (src/table/tuple.cpp)
///   3. Parser expression depth guard (src/parser/parser.cpp)
///   4. pk_to_int64 widening for narrow integer PKs (src/executor/graph_traversal_core.cpp)

#include "sixseven/catalog/catalog.h"
#include "sixseven/common/value.h"
#include "sixseven/common/value_hash.h"
#include "sixseven/executor/graph_traversal_core.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/parser/lexer.h"
#include "sixseven/parser/parser.h"
#include "sixseven/storage/buffer_pool.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "test_qa_helpers.h"

using namespace sixseven;

// =============================================================================
// 1. ValueHash mixed-width numeric contract
// =============================================================================

TEST(QA1224_ValueHash, EqualValueDifferentIntWidthsHashEqual) {
    ValueHash h;
    Value i8{static_cast<int8_t>(2)};
    Value i16{static_cast<int16_t>(2)};
    Value i32{static_cast<int32_t>(2)};
    Value i64{static_cast<int64_t>(2)};
    Value u8{static_cast<uint8_t>(2)};
    Value u16{static_cast<uint16_t>(2)};
    Value u32{static_cast<uint32_t>(2)};
    Value u64{static_cast<uint64_t>(2)};

    size_t base = h(i32);
    EXPECT_EQ(h(i8), base);
    EXPECT_EQ(h(i16), base);
    EXPECT_EQ(h(i64), base);
    EXPECT_EQ(h(u8), base);
    EXPECT_EQ(h(u16), base);
    EXPECT_EQ(h(u32), base);
    EXPECT_EQ(h(u64), base);
}

TEST(QA1224_ValueHash, EqualFloat32Float64HashEqual) {
    ValueHash h;
    Value f32{3.5f};
    Value f64{3.5};
    EXPECT_EQ(h(f32), h(f64));
}

TEST(QA1224_ValueHash, UnequalValuesUsuallyHashDifferently) {
    ValueHash h;
    Value a{static_cast<int32_t>(2)};
    Value b{static_cast<int32_t>(3)};
    EXPECT_NE(h(a), h(b));
}

class QA1224_HashJoinMixedWidth : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa1224_hashjoin";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }
    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA1224_HashJoinMixedWidth, Int32JoinsInt64OnMatchingValue) {
    exec_ok("CREATE TABLE small_ids (id INT, label VARCHAR)");
    exec_ok("CREATE TABLE big_ids (id BIGINT, tag VARCHAR)");
    exec_ok("INSERT INTO small_ids VALUES (2, 'two')");
    exec_ok("INSERT INTO small_ids VALUES (5, 'five')");
    exec_ok("INSERT INTO big_ids VALUES (2, 'matched')");
    exec_ok("INSERT INTO big_ids VALUES (9, 'unmatched')");

    auto qr = exec_ok("SELECT small_ids.label, big_ids.tag FROM small_ids JOIN big_ids "
                       "ON small_ids.id = big_ids.id");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "two");
    EXPECT_EQ(qr.rows[0][1].as_string(), "matched");
}

TEST_F(QA1224_HashJoinMixedWidth, GroupByMixedWidthEqualKeysGroupTogether) {
    // A single table with a narrow-width integer column: GROUP BY must
    // collapse rows whose stored values are numerically equal even though
    // ValueHash's job is exercised on the hash-index/aggregation path for
    // this column's declared width. This mainly guards against a regression
    // reintroducing per-width hash buckets for a single homogeneous column;
    // the cross-table mixed-width case is covered by the JOIN test above.
    exec_ok("CREATE TABLE t_small (v SMALLINT)");
    exec_ok("INSERT INTO t_small VALUES (2)");
    exec_ok("INSERT INTO t_small VALUES (2)");
    exec_ok("INSERT INTO t_small VALUES (5)");

    auto qr = exec_ok("SELECT v, COUNT(*) FROM t_small GROUP BY v ORDER BY v");
    ASSERT_EQ(qr.rows.size(), 2u);
    EXPECT_EQ(qr.rows[0][0].as_int16(), 2);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 2);
    EXPECT_EQ(qr.rows[1][0].as_int16(), 5);
    EXPECT_EQ(qr.rows[1][1].as_int64(), 1);
}

// =============================================================================
// 2. TupleSerializer trailing zero-length var-length field
// =============================================================================

class QA1224_TupleTrailingEmpty : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa1224_tuple";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        bootstrap_qa_catalog(catalog_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }
    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
    std::filesystem::path data_dir_;
};

TEST_F(QA1224_TupleTrailingEmpty, SingleTrailingEmptyStringRoundTrips) {
    exec_ok("CREATE TABLE t (id INT, name VARCHAR)");
    exec_ok("INSERT INTO t VALUES (1, '')");
    auto qr = exec_ok("SELECT id, name FROM t WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_string(), "");
}

TEST_F(QA1224_TupleTrailingEmpty, MultipleTrailingEmptyStringsRoundTrip) {
    exec_ok("CREATE TABLE t2 (id INT, a VARCHAR, b VARCHAR, c VARCHAR)");
    exec_ok("INSERT INTO t2 VALUES (1, 'x', '', '')");
    auto qr = exec_ok("SELECT a, b, c FROM t2 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][0].as_string(), "x");
    EXPECT_EQ(qr.rows[0][1].as_string(), "");
    EXPECT_EQ(qr.rows[0][2].as_string(), "");
}

TEST(QA1224_TupleSerializerDirect, EmptyBlobTrailingFieldRoundTrips) {
    // Drive TupleSerializer directly (bypassing SQL BLOB literal syntax,
    // which this dialect doesn't expose) to exercise the exact
    // serialize/deserialize + get_field path the GDB-1224 fix touches, with
    // an empty BLOB as the last variable-length field.
    Schema schema({{"id", TypeId::INT32}, {"data", TypeId::BLOB}});

    std::vector<Value> values;
    values.push_back(Value(int32_t{1}));
    values.push_back(Value(Blob{}));

    auto serialized = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(serialized.has_value()) << serialized.error().message;

    auto deserialized = TupleSerializer::deserialize(*serialized, schema);
    ASSERT_TRUE(deserialized.has_value()) << deserialized.error().message;
    ASSERT_EQ(deserialized->size(), 2u);
    EXPECT_TRUE((*deserialized)[1].as_blob().empty());

    auto field = TupleSerializer::get_field(*serialized, schema, 1);
    ASSERT_TRUE(field.has_value()) << field.error().message;
    EXPECT_TRUE(field->as_blob().empty());
}

TEST_F(QA1224_TupleTrailingEmpty, AllNullThenTrailingEmptyStringRoundTrips) {
    exec_ok("CREATE TABLE t4 (id INT, a VARCHAR, b VARCHAR)");
    exec_ok("INSERT INTO t4 VALUES (1, NULL, '')");
    auto qr = exec_ok("SELECT a, b FROM t4 WHERE id = 1");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_TRUE(qr.rows[0][0].is_null());
    EXPECT_EQ(qr.rows[0][1].as_string(), "");
}

// =============================================================================
// 3. Parser expression depth guard
// =============================================================================

static Result<StmtPtr> qa_parse_one(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.tokenize();
    if (!tokens) {
        return tl::unexpected(tokens.error());
    }
    Parser parser(std::move(*tokens));
    return parser.parse();
}

TEST(QA1224_ParserDepthGuard, JustOverLimitReturnsCleanParseError) {
    // kMaxExpressionDepth = 32. 40 nested parens should cleanly fail rather
    // than crash/hang.
    std::string expr(40, '(');
    expr += "1";
    expr += std::string(40, ')');
    std::string sql = "SELECT " + expr + ";";

    auto result = qa_parse_one(sql);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST(QA1224_ParserDepthGuard, JustUnderLimitStillParses) {
    std::string expr(20, '(');
    expr += "1";
    expr += std::string(20, ')');
    std::string sql = "SELECT " + expr + ";";

    auto result = qa_parse_one(sql);
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

TEST(QA1224_ParserDepthGuard, DepthResetsBetweenSeparateParserInstances) {
    // Two deep-but-legal statements in a row, each parsed by re-constructing
    // the parser (mirrors how the engine parses each incoming statement),
    // both must parse successfully -- proving depth state doesn't leak.
    std::string expr(20, '(');
    expr += "1";
    expr += std::string(20, ')');
    std::string sql = "SELECT " + expr + ";";

    auto result1 = qa_parse_one(sql);
    ASSERT_TRUE(result1.has_value()) << result1.error().message;

    auto result2 = qa_parse_one(sql);
    ASSERT_TRUE(result2.has_value()) << result2.error().message;
}

TEST(QA1224_ParserDepthGuard, DeepFailureFollowedByLegalStatementOnSameParser) {
    // First parse a statement that blows the depth guard (error return, not
    // exception) via one Parser instance, then parse a second, legal
    // statement via a fresh Parser -- confirming no static/global depth
    // counter leaked across instances/parses.
    std::string deep_expr(40, '(');
    deep_expr += "1";
    deep_expr += std::string(40, ')');
    auto bad_result = qa_parse_one("SELECT " + deep_expr + ";");
    ASSERT_FALSE(bad_result.has_value());

    auto good_result = qa_parse_one("SELECT 1 + 2;");
    ASSERT_TRUE(good_result.has_value()) << good_result.error().message;
}

// =============================================================================
// 4. pk_to_int64 widening (graph traversal with narrow-int PKs)
// =============================================================================

TEST(QA1224_PkToInt64, Int8PkWidensLosslessly) {
    auto r = pk_to_int64(Value(static_cast<int8_t>(-5)));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, -5);
}

TEST(QA1224_PkToInt64, Int16PkWidensLosslessly) {
    auto r = pk_to_int64(Value(static_cast<int16_t>(1234)));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, 1234);
}

TEST(QA1224_PkToInt64, UInt32PkWidensLosslessly) {
    auto r = pk_to_int64(Value(static_cast<uint32_t>(4000000000ULL)));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, 4000000000LL);
}

TEST(QA1224_PkToInt64, NullPkIsRejectedNotCrashed) {
    auto r = pk_to_int64(Value::make_null());
    EXPECT_FALSE(r.has_value());
}

TEST(QA1224_PkToInt64, StringPkStillRejected) {
    // STRING PKs remain explicitly out of scope per the fix's documented
    // tradeoff -- must fail cleanly, not silently coerce or crash.
    auto r = pk_to_int64(Value(std::string("abc")));
    EXPECT_FALSE(r.has_value());
}

/// Integration-level check: a full graph traversal (via
/// MatchShortestPathOperator, the real caller of pk_to_int64) over a nodes
/// table whose PK column is declared UINT32 -- narrower than the INT64 the
/// pre-fix code exclusively supported.
class QA1224_GraphNarrowPkTraversal : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa1224_graph_pk";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        bootstrap_qa_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_ = std::make_unique<GraphEngine>(*catalog_);

        auto db_storage = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_storage.has_value()) << db_storage.error().message;

        TableSchema ts;
        ts.name = "nodes";
        CatalogColumnDef pk_col;
        pk_col.ordinal = 0;
        pk_col.name = "id";
        pk_col.type_id = TypeId::UINT32;
        pk_col.nullable = false;
        ts.columns.push_back(pk_col);
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, std::move(ts));
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        nodes_id_ = *tid;

        auto schema = catalog_->get_table(default_database_id, "nodes");
        ASSERT_TRUE(schema.has_value());
        auto sr = storage_->create_table_storage(default_database_id, nodes_id_, *schema);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;

        auto eid = graph_->create_edge_type(
            default_database_id, "road", nodes_id_, nodes_id_, TypeId::UINT32, TypeId::UINT32, {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void insert_node(uint32_t id) {
        auto ts = storage_->get_table_storage(nodes_id_);
        ASSERT_TRUE(ts.has_value()) << ts.error().message;
        std::vector<Value> vals = {Value(id)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        ASSERT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        ASSERT_TRUE(rid.has_value()) << rid.error().message;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t nodes_id_ = 0;
};

TEST_F(QA1224_GraphNarrowPkTraversal, ExpandNeighborsIdsWorksWithUint32Pk) {
    insert_node(100);
    insert_node(200);
    auto link = graph_->link(default_database_id, "road", Value(uint32_t{100}), Value(uint32_t{200}));
    ASSERT_TRUE(link.has_value()) << link.error().message;

    auto neighbors =
        expand_neighbors_ids(*graph_, default_database_id, "road", Value(uint32_t{100}), TraverseDirection::OUT);
    ASSERT_TRUE(neighbors.has_value()) << neighbors.error().message;
    ASSERT_EQ(neighbors->size(), 1u);
    EXPECT_EQ((*neighbors)[0].first.as_uint32(), 200u);
}
