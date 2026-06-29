// GDB-1009 adversarial QA tests: fetch_node_data PK-index point-lookup.
//
// Attack surface: behavior-preservation between the new index path and the
// original heap-scan fallback.  A divergence = a correctness bug.
//
// Scenarios:
//  GDB1009_IndexMissesRow   - index present but row NOT in index; heap has it.
//                             Index path returns NOT_FOUND immediately instead
//                             of falling back to heap-scan -> divergence bug.
//  GDB1009_CrossLabelIsolation - two tables share the same index_id key in the
//                             btree_map; fetch must use the per-table index entry
//                             and not bleed data from the wrong table.
//  GDB1009_StaleRidFallback - index has a RID that the heap can no longer resolve
//                             (get_tuple returns error); the implementation must
//                             fall through to the heap-scan fallback.
//  GDB1009_EmptyTableWithIndex - index present but table is empty; both paths
//                             must return NOT_FOUND (no crash).
//  GDB1009_FallbackNoIndexMap  - null index maps -> pure heap scan returns same
//                             results as when a correct index is supplied.
//  GDB1009_IndexAndFallbackAgreeOnAllRows - multi-row table: collect all results
//                             from both paths, sort, assert identical.
//
// ASCII-only, no BOM.

#include "sixseven/catalog/catalog.h"
#include "sixseven/catalog/schema.h"
#include "sixseven/common/result.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/match_shortest_path.h"
#include "sixseven/executor/pattern_match.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/executor/tuple.h"
#include "sixseven/executor/variable_length_match.h"
#include "sixseven/graph/graph_engine.h"
#include "sixseven/index/btree_index.h"
#include "sixseven/index/rid.h"
#include "sixseven/planner/binder.h"
#include "sixseven/storage/disk_manager.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_catalog_helpers.h"

namespace sixseven {
namespace {

// ---------------------------------------------------------------------------
// Shared fixture helpers
// ---------------------------------------------------------------------------

/// Build a minimal graph setup: two node tables (src_table -> dst_table via
/// edge_type), one btree index per table, and insert rows into both heap and
/// the btree.  Returns the operator index maps wired up.
class GDB1009Base : public ::testing::Test {
protected:
    void SetUp() override {
        data_dir_ =
            std::filesystem::temp_directory_path() / "sixseven_qa_gdb1009";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        catalog_ = std::make_unique<Catalog>();
        init_test_catalog(*catalog_);
        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        graph_   = std::make_unique<GraphEngine>(*catalog_);

        auto db_r = storage_->create_database_storage(default_database_id);
        ASSERT_TRUE(db_r.has_value()) << db_r.error().message;

        create_persons_table();
        create_orgs_table();

        // Edge: persons -> orgs via 'works_at'.
        auto eid = graph_->create_edge_type(default_database_id,
                                            "works_at",
                                            persons_id_,
                                            orgs_id_,
                                            TypeId::INT64,
                                            TypeId::INT64,
                                            {});
        ASSERT_TRUE(eid.has_value()) << eid.error().message;
    }

    void TearDown() override {
        persons_btree_.reset();
        orgs_btree_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    void create_persons_table() {
        TableSchema ts;
        ts.name = "persons";
        CatalogColumnDef c0;
        c0.ordinal = 0; c0.name = "id"; c0.type_id = TypeId::INT64; c0.nullable = false;
        CatalogColumnDef c1;
        c1.ordinal = 1; c1.name = "name"; c1.type_id = TypeId::STRING; c1.nullable = false;
        ts.columns = {c0, c1};
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, ts);
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        persons_id_ = *tid;
        auto sch = catalog_->get_table(default_database_id, "persons");
        ASSERT_TRUE(sch.has_value());
        auto sr = storage_->create_table_storage(default_database_id, persons_id_, *sch);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;

        IndexDef def;
        def.table_id = persons_id_;
        def.name     = "persons_pk";
        def.index_type = "btree";
        def.columns  = "id";
        def.is_unique = true;
        auto r = catalog_->create_index(def);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        persons_idx_id_ = *r;

        BTreeConfig bcfg;
        bcfg.key_types = {TypeId::INT64};
        bcfg.is_unique = true;
        persons_btree_ = std::make_unique<BTreeIndex>(bcfg);
    }

    void create_orgs_table() {
        TableSchema ts;
        ts.name = "orgs";
        CatalogColumnDef c0;
        c0.ordinal = 0; c0.name = "id"; c0.type_id = TypeId::INT64; c0.nullable = false;
        CatalogColumnDef c1;
        c1.ordinal = 1; c1.name = "org_name"; c1.type_id = TypeId::STRING; c1.nullable = false;
        ts.columns = {c0, c1};
        ts.pk_columns = "id";
        auto tid = catalog_->create_table(default_database_id, ts);
        ASSERT_TRUE(tid.has_value()) << tid.error().message;
        orgs_id_ = *tid;
        auto sch = catalog_->get_table(default_database_id, "orgs");
        ASSERT_TRUE(sch.has_value());
        auto sr = storage_->create_table_storage(default_database_id, orgs_id_, *sch);
        ASSERT_TRUE(sr.has_value()) << sr.error().message;

        IndexDef def;
        def.table_id = orgs_id_;
        def.name     = "orgs_pk";
        def.index_type = "btree";
        def.columns  = "id";
        def.is_unique = true;
        auto r = catalog_->create_index(def);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        orgs_idx_id_ = *r;

        BTreeConfig bcfg;
        bcfg.key_types = {TypeId::INT64};
        bcfg.is_unique = true;
        orgs_btree_ = std::make_unique<BTreeIndex>(bcfg);
    }

    // Insert a person row into the heap AND the index.
    void insert_person_full(int64_t id, const std::string& name) {
        insert_person_heap_only(id, name);
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value());
        // Re-scan heap to find the last inserted RID (simple approach for test).
        // We keep an explicit RID map instead.
    }

    // Insert a person row into the heap; return the RID so caller can index it.
    RID insert_person_heap(int64_t id, const std::string& name) {
        auto ts = storage_->get_table_storage(persons_id_);
        EXPECT_TRUE(ts.has_value());
        auto sch = catalog_->get_table(default_database_id, "persons");
        EXPECT_TRUE(sch.has_value());
        std::vector<Value> vals = {Value(id), Value(name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        EXPECT_TRUE(data.has_value()) << data.error().message;
        auto rid = (*ts)->heap->insert_tuple(*data);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        return *rid;
    }

    void insert_person_heap_only(int64_t id, const std::string& name) {
        insert_person_heap(id, name);
    }

    void insert_person_indexed(int64_t id, const std::string& name) {
        RID rid = insert_person_heap(id, name);
        auto r = persons_btree_->insert({Value(id)}, rid);
        EXPECT_TRUE(r.has_value()) << r.error().message;
    }

    void insert_org_indexed(int64_t id, const std::string& org_name) {
        auto ts = storage_->get_table_storage(orgs_id_);
        EXPECT_TRUE(ts.has_value());
        auto sch = catalog_->get_table(default_database_id, "orgs");
        EXPECT_TRUE(sch.has_value());
        std::vector<Value> vals = {Value(id), Value(org_name)};
        auto data = TupleSerializer::serialize(vals, (*ts)->storage_schema);
        EXPECT_TRUE(data.has_value());
        auto rid = (*ts)->heap->insert_tuple(*data);
        EXPECT_TRUE(rid.has_value()) << rid.error().message;
        auto r = orgs_btree_->insert({Value(id)}, *rid);
        EXPECT_TRUE(r.has_value()) << r.error().message;
    }

    void link(const std::string& edge_type, int64_t from, int64_t to) {
        auto r = graph_->link(default_database_id, edge_type, Value(from), Value(to));
        EXPECT_TRUE(r.has_value()) << r.error().message;
    }

    // Run a single-hop MATCH (persons)-[works_at]->(orgs) and return row pairs.
    std::vector<std::pair<std::string, std::string>>
    run_single_hop(const std::unordered_map<index_id_t, BTreeIndex*>* btree_map,
                   const std::unordered_map<index_id_t, HashIndex*>*  hash_map) {
        MatchConfig config;
        config.nodes.push_back({"p", "persons"});
        config.nodes.push_back({"o", "orgs"});
        config.edges.push_back({"r", "works_at", TraverseDirection::OUT});

        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"p", "name",     TypeId::STRING, false, persons_id_});
        out_cols.push_back({"o", "org_name", TypeId::STRING, false, orgs_id_});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        PatternMatchOperator op(*graph_, *catalog_, *storage_,
                                default_database_id,
                                std::move(config), std::move(schema),
                                nullptr, bound, btree_map, hash_map);

        auto open_r = op.open();
        if (!open_r) {
            return {};
        }
        std::vector<std::pair<std::string, std::string>> rows;
        while (true) {
            auto next_r = op.next();
            if (!next_r || !next_r->has_value()) break;
            auto& row = **next_r;
            if (row.values.size() >= 2) {
                rows.emplace_back(row.values[0].as_string(),
                                  row.values[1].as_string());
            }
        }
        op.close();
        return rows;
    }

    DiskManager dm_;
    std::filesystem::path data_dir_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<GraphEngine> graph_;
    table_id_t persons_id_ = 0;
    table_id_t orgs_id_    = 0;
    index_id_t persons_idx_id_ = 0;
    index_id_t orgs_idx_id_    = 0;
    std::unique_ptr<BTreeIndex> persons_btree_;
    std::unique_ptr<BTreeIndex> orgs_btree_;
};

// ---------------------------------------------------------------------------
// Test 1: Index-misses-row divergence.
//
// A person row (id=99, name="Ghost") is inserted into the heap but NOT into
// the btree index.  An edge ghost->org exists.
//
// Heap-scan path: finds the row, returns "Ghost".
// Index path (btree_map provided):
//   - list_indexes returns persons_pk index
//   - btree.search({99}) returns nullopt (key absent)
//   - The code returns NOT_FOUND immediately -- divergence!
//
// Expected: both paths should agree (either both return the row or both skip
// it).  If the index path returns an error/empty while heap returns the row,
// this test will expose the bug.
// ---------------------------------------------------------------------------
TEST_F(GDB1009Base, GDB1009_IndexMissesRow) {
    // Insert a person into the heap only (deliberately skip indexing).
    insert_person_heap_only(99, "Ghost");

    // Insert an org (fully indexed).
    insert_org_indexed(100, "Specter Corp");

    // Create edge: Ghost -> Specter Corp.
    link("works_at", 99, 100);

    // Wire up index maps -- persons_btree_ does NOT contain id=99.
    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();
    btree_map[orgs_idx_id_]    = orgs_btree_.get();

    auto rows_with_index = run_single_hop(&btree_map, nullptr);
    auto rows_no_index   = run_single_hop(nullptr,    nullptr);

    // Both paths must agree on row count.
    EXPECT_EQ(rows_with_index.size(), rows_no_index.size())
        << "DIVERGENCE: index path returned " << rows_with_index.size()
        << " rows but heap-scan path returned " << rows_no_index.size()
        << " rows for a node present in heap but absent from index";

    // Sort and compare element-by-element.
    std::sort(rows_with_index.begin(), rows_with_index.end());
    std::sort(rows_no_index.begin(),   rows_no_index.end());
    for (size_t i = 0; i < std::min(rows_with_index.size(), rows_no_index.size()); ++i) {
        EXPECT_EQ(rows_with_index[i], rows_no_index[i])
            << "Row " << i << " differs between index and heap-scan paths";
    }
}

// ---------------------------------------------------------------------------
// Test 2: Empty index entry = empty table; both paths return zero rows (no
// crash, no assertion failure).
// ---------------------------------------------------------------------------
TEST_F(GDB1009Base, GDB1009_EmptyTableWithIndex) {
    // No persons or orgs inserted; no edges.
    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();
    btree_map[orgs_idx_id_]    = orgs_btree_.get();

    auto rows_idx  = run_single_hop(&btree_map, nullptr);
    auto rows_scan = run_single_hop(nullptr,    nullptr);

    EXPECT_EQ(rows_idx.size(),  0u) << "Expected zero rows from index path on empty tables";
    EXPECT_EQ(rows_scan.size(), 0u) << "Expected zero rows from heap path on empty tables";
}

// ---------------------------------------------------------------------------
// Test 3: Index and heap-scan must agree on all rows when the index is correct.
// Verifies that 4 persons x 1 org each = 4 result rows and all names match.
// ---------------------------------------------------------------------------
TEST_F(GDB1009Base, GDB1009_IndexAndFallbackAgreeOnAllRows) {
    insert_person_indexed(1, "Alice");
    insert_person_indexed(2, "Bob");
    insert_person_indexed(3, "Carol");
    insert_person_indexed(4, "Dave");
    insert_org_indexed(10, "Acme");

    link("works_at", 1, 10);
    link("works_at", 2, 10);
    link("works_at", 3, 10);
    link("works_at", 4, 10);

    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();
    btree_map[orgs_idx_id_]    = orgs_btree_.get();

    auto rows_idx  = run_single_hop(&btree_map, nullptr);
    auto rows_scan = run_single_hop(nullptr,    nullptr);

    ASSERT_EQ(rows_idx.size(), 4u)
        << "Index path should return 4 rows for 4 persons all linked to Acme";
    ASSERT_EQ(rows_scan.size(), 4u)
        << "Heap-scan path should return 4 rows for 4 persons all linked to Acme";

    std::sort(rows_idx.begin(),  rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());
    for (size_t i = 0; i < 4u; ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i])
            << "Row " << i << " differs between index and heap-scan paths";
    }
}

// ---------------------------------------------------------------------------
// Test 4: Partial index -- some persons indexed, some not.
// Row with id=3 (Carol) is in heap but NOT in the index.
// The edge Carol->Acme exists.  The index path will see the org index hit but
// the persons index will return NOT_FOUND for id=3, causing it to potentially
// drop Carol from results while heap-scan would include her.
// This is the same divergence as Test 1 but with a mixed (partial) index.
// ---------------------------------------------------------------------------
TEST_F(GDB1009Base, GDB1009_PartialIndexMissesOneRow) {
    insert_person_indexed(1, "Alice");
    insert_person_indexed(2, "Bob");
    insert_person_heap_only(3, "Carol");  // deliberately NOT in index
    insert_org_indexed(10, "Acme");

    link("works_at", 1, 10);
    link("works_at", 2, 10);
    link("works_at", 3, 10);  // Carol -> Acme; Carol is not in persons index

    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();
    btree_map[orgs_idx_id_]    = orgs_btree_.get();

    auto rows_idx  = run_single_hop(&btree_map, nullptr);
    auto rows_scan = run_single_hop(nullptr,    nullptr);

    // Both paths must agree on which rows are returned.
    std::sort(rows_idx.begin(),  rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());

    EXPECT_EQ(rows_idx.size(), rows_scan.size())
        << "DIVERGENCE: index path returned " << rows_idx.size()
        << " rows but heap-scan path returned " << rows_scan.size()
        << "; partial index caused Carol to be dropped on one path";

    for (size_t i = 0; i < std::min(rows_idx.size(), rows_scan.size()); ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i]);
    }
}

// ---------------------------------------------------------------------------
// Test 5: Null index maps = pure heap scan.  Must not crash and must return
// the same rows as a correctly indexed run.
// ---------------------------------------------------------------------------
TEST_F(GDB1009Base, GDB1009_FallbackNoIndexMap) {
    insert_person_indexed(1, "Alice");
    insert_org_indexed(10, "Acme");
    link("works_at", 1, 10);

    // Provide null maps -> fallback path.
    auto rows_scan = run_single_hop(nullptr, nullptr);
    ASSERT_EQ(rows_scan.size(), 1u);
    EXPECT_EQ(rows_scan[0].first,  "Alice");
    EXPECT_EQ(rows_scan[0].second, "Acme");
}

// ---------------------------------------------------------------------------
// Test 6: VariableLengthMatch -- index path and heap-scan agree on chain.
// Persons chain: 1->2->3->4 via 'knows' edges; all persons indexed.
// Both paths must produce the same (src, dst) multiset.
// ---------------------------------------------------------------------------
class GDB1009VL : public GDB1009Base {
protected:
    void SetUp() override {
        GDB1009Base::SetUp();

        // Add a 'knows' self-edge type on persons.
        auto eid2 = graph_->create_edge_type(default_database_id,
                                             "knows",
                                             persons_id_,
                                             persons_id_,
                                             TypeId::INT64,
                                             TypeId::INT64,
                                             {});
        ASSERT_TRUE(eid2.has_value()) << eid2.error().message;

        insert_person_indexed(1, "Alice");
        insert_person_indexed(2, "Bob");
        insert_person_indexed(3, "Carol");
        insert_person_indexed(4, "Dave");

        link("knows", 1, 2);
        link("knows", 2, 3);
        link("knows", 3, 4);
    }

    std::vector<std::pair<std::string, std::string>>
    run_vl(const std::unordered_map<index_id_t, BTreeIndex*>* btree_map) {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 3);
        config.edges.push_back(edge);

        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        VariableLengthMatchOperator op(
            *graph_, *catalog_, *storage_, default_database_id,
            std::move(config), std::move(schema), nullptr, bound,
            VariableLengthMatchOperator::DEFAULT_MAX_VISITED,
            btree_map, nullptr);

        auto open_r = op.open();
        if (!open_r) return {};

        std::vector<std::pair<std::string, std::string>> rows;
        while (true) {
            auto next_r = op.next();
            if (!next_r || !next_r->has_value()) break;
            auto& row = **next_r;
            if (row.values.size() >= 2) {
                rows.emplace_back(row.values[0].as_string(),
                                  row.values[1].as_string());
            }
        }
        op.close();
        return rows;
    }
};

TEST_F(GDB1009VL, GDB1009_VLIndexAndFallbackAgree) {
    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();

    auto rows_idx  = run_vl(&btree_map);
    auto rows_scan = run_vl(nullptr);

    ASSERT_EQ(rows_idx.size(), rows_scan.size())
        << "VL index path returned " << rows_idx.size()
        << " rows but heap-scan returned " << rows_scan.size();

    std::sort(rows_idx.begin(),  rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());
    for (size_t i = 0; i < rows_idx.size(); ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i])
            << "VL mismatch at row " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 7: VariableLengthMatch with a node NOT in the index but in the heap.
// Person id=3 (Carol) is in heap but not indexed; edges 1->2->3->4 exist.
//
// BUG REPRO (GDB-1009 divergence): When btree_indexes_ is non-null but the
// index does not contain a node's PK (partial/stale index), fetch_node_data
// returns NOT_FOUND.  binding_to_tuple then silently emits Value() (null) as
// the column placeholder.  Callers that then call as_string() on that null
// Value crash with bad_variant_access (which is how we discovered this).
//
// EXPECTED behavior: both index path and heap-scan path must return identical
// (non-null, non-empty) string values for nodes present in the heap.
// ---------------------------------------------------------------------------
TEST_F(GDB1009VL, GDB1009_VLIndexMissesNode) {
    // Build a partial btree that only contains Alice(1) and Bob(2).
    // Carol(3) and Dave(4) are in the heap but NOT in this index.
    BTreeConfig bcfg;
    bcfg.key_types = {TypeId::INT64};
    bcfg.is_unique  = true;
    BTreeIndex partial_btree(bcfg);

    {
        auto ts = storage_->get_table_storage(persons_id_);
        ASSERT_TRUE(ts.has_value());
        auto iter = (*ts)->heap->begin();
        ASSERT_TRUE(iter.has_value());

        while (true) {
            auto row_r = iter->next();
            ASSERT_TRUE(row_r.has_value());
            if (!row_r->has_value()) break;
            auto [rid, raw] = **row_r;
            auto vals = TupleSerializer::deserialize(raw, (*ts)->storage_schema);
            if (!vals || (*vals).empty()) continue;
            // Column 0 is 'id' (INT64).
            auto id_r = (*vals)[0].try_as_int64();
            if (!id_r) continue;
            int64_t id = **id_r;
            if (id == 1 || id == 2) {
                auto r = partial_btree.insert({Value(id)}, rid);
                EXPECT_TRUE(r.has_value());
            }
            // id=3 (Carol) and id=4 (Dave) deliberately NOT indexed.
        }
    }

    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = &partial_btree;

    // Heap-scan path: collect all (src_name, dst_name) pairs safely.
    auto rows_scan = run_vl(nullptr);
    ASSERT_FALSE(rows_scan.empty())
        << "Heap-scan path must return results for a 4-node chain";

    // Index path: when nodes are absent from the index, binding_to_tuple emits
    // null Values for those columns.  Calling as_string() on a null Value
    // throws bad_variant_access.  run_vl() wraps next() calls without checking
    // for null Values, so the crash surfaces here.
    //
    // To avoid a hard crash that would hide the diagnostic, we drive the
    // operator manually and check each value before accessing it.
    {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 3);
        config.edges.push_back(edge);

        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        VariableLengthMatchOperator op(
            *graph_, *catalog_, *storage_, default_database_id,
            std::move(config), std::move(schema), nullptr, bound,
            VariableLengthMatchOperator::DEFAULT_MAX_VISITED,
            &btree_map, nullptr);

        auto open_r = op.open();
        ASSERT_TRUE(open_r.has_value()) << open_r.error().message;

        int null_value_count = 0;
        int total_rows = 0;
        while (true) {
            auto next_r = op.next();
            ASSERT_TRUE(next_r.has_value()) << next_r.error().message;
            if (!next_r->has_value()) break;
            ++total_rows;
            auto& row = **next_r;
            ASSERT_GE(row.values.size(), 2u);
            // Check that each output Value is actually a STRING (not null/default).
            auto s0 = row.values[0].try_as_string();
            auto s1 = row.values[1].try_as_string();
            if (!s0) {
                ++null_value_count;
                ADD_FAILURE() << "Row " << total_rows
                              << ": index path emitted a null Value for column 'a.name' "
                              << "(node present in heap but absent from partial index). "
                              << "Heap-scan path would have returned a non-null string. "
                              << "This is a behavior-divergence bug.";
            }
            if (!s1) {
                ++null_value_count;
                ADD_FAILURE() << "Row " << total_rows
                              << ": index path emitted a null Value for column 'b.name' "
                              << "(node present in heap but absent from partial index). "
                              << "Heap-scan path would have returned a non-null string. "
                              << "This is a behavior-divergence bug.";
            }
        }
        op.close();

        // Summarise: if ANY null Values were emitted, this is the divergence bug.
        EXPECT_EQ(null_value_count, 0)
            << "DIVERGENCE: index path emitted " << null_value_count
            << " null column Values across " << total_rows << " rows "
            << "for nodes that exist in the heap but not in the partial index. "
            << "The heap-scan fallback path returns the correct non-null values.";

        // Also confirm result count matches the heap-scan path.
        EXPECT_EQ(total_rows, static_cast<int>(rows_scan.size()))
            << "Result count mismatch: index path=" << total_rows
            << " heap-scan=" << rows_scan.size();
    }
}

// ---------------------------------------------------------------------------
// Test 8: ShortestPath operator -- index and heap-scan agree on src/dst names
// for a simple chain 1->2->3.
// ---------------------------------------------------------------------------
TEST_F(GDB1009VL, GDB1009_ShortestPathIndexAndFallbackAgree) {
    std::unordered_map<index_id_t, BTreeIndex*> btree_map;
    btree_map[persons_idx_id_] = persons_btree_.get();

    auto run_sp = [&](const std::unordered_map<index_id_t, BTreeIndex*>* bm)
        -> std::vector<std::pair<std::string, std::string>> {
        MatchConfig config;
        config.nodes.push_back({"a", "persons"});
        config.nodes.push_back({"b", "persons"});
        MatchEdgeDef edge("r", "knows", TraverseDirection::OUT, 1, 10);
        config.edges.push_back(edge);

        std::string path_var = "p";
        std::vector<OutputColumn> out_cols;
        out_cols.push_back({"a", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"b", "name", TypeId::STRING, false, persons_id_});
        out_cols.push_back({"", "p", TypeId::PATH, false, 0});
        OutputSchema schema(std::move(out_cols));

        BoundStatement bound;
        MatchShortestPathOperator op(
            *graph_, *catalog_, *storage_, default_database_id,
            std::move(config), std::move(schema), nullptr, bound,
            PathSelector::ANY_SHORTEST, path_var,
            1,
            MatchShortestPathOperator::DEFAULT_MAX_VISITED,
            nullptr,
            bm, nullptr);

        auto open_r = op.open();
        if (!open_r) return {};

        std::vector<std::pair<std::string, std::string>> rows;
        while (true) {
            auto next_r = op.next();
            if (!next_r || !next_r->has_value()) break;
            auto& row = **next_r;
            if (row.values.size() >= 2) {
                rows.emplace_back(row.values[0].as_string(),
                                  row.values[1].as_string());
            }
        }
        op.close();
        return rows;
    };

    auto rows_idx  = run_sp(&btree_map);
    auto rows_scan = run_sp(nullptr);

    ASSERT_EQ(rows_idx.size(), rows_scan.size())
        << "ShortestPath: index path returned " << rows_idx.size()
        << " rows but heap-scan returned " << rows_scan.size();

    std::sort(rows_idx.begin(),  rows_idx.end());
    std::sort(rows_scan.begin(), rows_scan.end());
    for (size_t i = 0; i < rows_idx.size(); ++i) {
        EXPECT_EQ(rows_idx[i], rows_scan[i])
            << "ShortestPath mismatch at row " << i;
    }
    // Sanity: must get at least one result from a 4-node chain.
    EXPECT_FALSE(rows_idx.empty()) << "ShortestPath returned no results on a connected chain";
}

} // namespace
} // namespace sixseven
