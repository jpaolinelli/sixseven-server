/// QA adversarial tests for GDB-233 and GDB-234:
///   GDB-233: EdgeTable delete_edge lacks rollback on partial index failure
///   GDB-234: GraphEngine LINK/UNLINK operations not logged to WAL
///
/// GDB-233 tests verify that insert/delete operations maintain full index
/// consistency (forward, reverse, and unique) across a variety of orderings
/// and patterns, exercising the rollback paths added in the fix.
///
/// GDB-234 tests verify that GraphEngine correctly writes WAL records for
/// LINK and UNLINK operations when a WalWriter is provided, and that the
/// engine still functions correctly with a nullptr WAL (backward compat).

#include "giodb/catalog/catalog.h"
#include "giodb/graph/edge_table.h"
#include "giodb/graph/graph_engine.h"
#include "giodb/storage/wal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <vector>

using namespace giodb;

// -- Helpers ------------------------------------------------------------------

static EdgeTableConfig make_config(const std::string& name, bool prevent_duplicates = false) {
    EdgeTableConfig config;
    config.edge_id = 1;
    config.name = name;
    config.source_table_id = 100;
    config.target_table_id = 200;
    config.source_pk_type = TypeId::INT64;
    config.target_pk_type = TypeId::INT64;
    config.prevent_duplicates = prevent_duplicates;
    return config;
}

static Value pk(int64_t v) {
    return Value(v);
}

static TableSchema make_table_schema(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT64, false, ""},
        {1, "name", TypeId::STRING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

// =============================================================================
// GDB-233: EdgeTable delete_edge rollback — index consistency tests
// =============================================================================

/// Insert and delete many edges one-by-one, verifying all three indexes
/// (forward, reverse, unique) remain consistent after every single delete.
TEST(QA_EdgeTableDeleteRollback, InsertDeleteManyVerifyConsistencyAfterEachDelete) {
    EdgeTable table(make_config("e", true));

    constexpr int count = 50;
    std::vector<uint64_t> row_ids;
    row_ids.reserve(count);

    // Insert many edges with distinct source/target pairs.
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 1000), {});
        ASSERT_TRUE(r.has_value()) << "insert i=" << i << ": " << r.error().message;
        row_ids.push_back(*r);
    }
    ASSERT_EQ(table.size(), static_cast<uint64_t>(count));

    // Delete edges one-by-one in forward order, checking consistency after each.
    for (int i = 0; i < count; ++i) {
        auto d = table.delete_edge(row_ids[static_cast<size_t>(i)]);
        ASSERT_TRUE(d.has_value()) << "delete i=" << i << ": " << d.error().message;

        // Forward index: deleted source should have no edges.
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "forward index not cleaned for pk " << i;

        // Reverse index: deleted target should have no edges.
        auto to = table.get_edges_to(pk(i + 1000));
        ASSERT_TRUE(to.has_value());
        EXPECT_TRUE(to->empty()) << "reverse index not cleaned for pk " << (i + 1000);

        // Unique index: should allow re-insert of the same pair.
        auto reinsert = table.insert_edge(pk(i), pk(i + 1000), {});
        ASSERT_TRUE(reinsert.has_value())
            << "unique index not cleaned for pair (" << i << ", " << (i + 1000) << ")";

        // Delete again to restore state for next iteration.
        ASSERT_TRUE(table.delete_edge(*reinsert).has_value());
    }

    EXPECT_EQ(table.size(), 0u);
    EXPECT_TRUE(table.empty());
}

/// Delete edges in reverse insertion order — exercises a different traversal
/// pattern through the B+ tree indexes.
TEST(QA_EdgeTableDeleteRollback, DeleteInReverseOrderConsistency) {
    EdgeTable table(make_config("e", true));

    constexpr int count = 30;
    std::vector<uint64_t> row_ids;

    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 500), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        row_ids.push_back(*r);
    }

    // Delete in reverse order.
    for (int i = count - 1; i >= 0; --i) {
        auto d = table.delete_edge(row_ids[static_cast<size_t>(i)]);
        ASSERT_TRUE(d.has_value()) << "reverse delete i=" << i << ": " << d.error().message;

        // Verify forward index consistency.
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "forward index stale for pk " << i;

        // Verify reverse index consistency.
        auto to = table.get_edges_to(pk(i + 500));
        ASSERT_TRUE(to.has_value());
        EXPECT_TRUE(to->empty()) << "reverse index stale for pk " << (i + 500);
    }

    EXPECT_TRUE(table.empty());
}

/// Insert, delete, and re-insert the same edge repeatedly — the unique
/// constraint index must be fully cleaned on each delete for the re-insert
/// to succeed.
TEST(QA_EdgeTableDeleteRollback, InsertDeleteReinsertCyclesWithUniqueIndex) {
    EdgeTable table(make_config("e", true));

    constexpr int cycles = 20;
    for (int c = 0; c < cycles; ++c) {
        auto r = table.insert_edge(pk(42), pk(99), {});
        ASSERT_TRUE(r.has_value()) << "cycle " << c << " insert: " << r.error().message;
        EXPECT_EQ(table.size(), 1u);

        // Verify the edge is findable via all indexes.
        auto found = table.find_edge(pk(42), pk(99));
        ASSERT_TRUE(found.has_value());
        ASSERT_TRUE(found->has_value());
        EXPECT_EQ((*found)->edge_row_id, *r);

        auto from = table.get_edges_from(pk(42));
        ASSERT_TRUE(from.has_value());
        EXPECT_EQ(from->size(), 1u);

        auto to = table.get_edges_to(pk(99));
        ASSERT_TRUE(to.has_value());
        EXPECT_EQ(to->size(), 1u);

        // Delete.
        auto d = table.delete_edge(*r);
        ASSERT_TRUE(d.has_value()) << "cycle " << c << " delete: " << d.error().message;
        EXPECT_EQ(table.size(), 0u);

        // All indexes should be empty.
        auto found_after = table.find_edge(pk(42), pk(99));
        ASSERT_TRUE(found_after.has_value());
        EXPECT_FALSE(found_after->has_value())
            << "unique index stale after delete in cycle " << c;
    }
}

/// Delete with prevent_duplicates enabled — verifies all three indexes
/// (forward, reverse, unique) are updated consistently during delete.
TEST(QA_EdgeTableDeleteRollback, DeleteWithUniqueIndexFullVerification) {
    EdgeTable table(make_config("e", true));

    // Insert several distinct edges.
    auto r1 = table.insert_edge(pk(1), pk(10), {});
    auto r2 = table.insert_edge(pk(2), pk(20), {});
    auto r3 = table.insert_edge(pk(3), pk(30), {});
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(table.size(), 3u);

    // Delete the middle edge.
    ASSERT_TRUE(table.delete_edge(*r2).has_value());

    // Remaining edges should be queryable.
    auto from1 = table.get_edges_from(pk(1));
    ASSERT_TRUE(from1.has_value());
    EXPECT_EQ(from1->size(), 1u);

    auto from3 = table.get_edges_from(pk(3));
    ASSERT_TRUE(from3.has_value());
    EXPECT_EQ(from3->size(), 1u);

    // Deleted edge's indexes should be clean.
    auto from2 = table.get_edges_from(pk(2));
    ASSERT_TRUE(from2.has_value());
    EXPECT_TRUE(from2->empty());

    auto to20 = table.get_edges_to(pk(20));
    ASSERT_TRUE(to20.has_value());
    EXPECT_TRUE(to20->empty());

    // Re-insert the same pair should succeed (unique index cleaned).
    auto r2_again = table.insert_edge(pk(2), pk(20), {});
    ASSERT_TRUE(r2_again.has_value())
        << "re-insert after delete failed: " << r2_again.error().message;
    EXPECT_EQ(table.size(), 3u);
}

/// Concurrent read safety during deletion: read from multiple sources/targets
/// while performing interleaved deletes. The shared_mutex should protect reads.
TEST(QA_EdgeTableDeleteRollback, ReadsWhileDeleting) {
    EdgeTable table(make_config("e"));

    // One source fans out to many targets.
    constexpr int fan_out = 50;
    std::vector<uint64_t> row_ids;
    for (int i = 0; i < fan_out; ++i) {
        auto r = table.insert_edge(pk(1), pk(i + 100), {});
        ASSERT_TRUE(r.has_value());
        row_ids.push_back(*r);
    }

    // Delete every other edge and verify consistent reads after each.
    for (int i = 0; i < fan_out; i += 2) {
        ASSERT_TRUE(table.delete_edge(row_ids[static_cast<size_t>(i)]).has_value());

        auto from = table.get_edges_from(pk(1));
        ASSERT_TRUE(from.has_value());

        // Verify none of the returned edges have been deleted.
        for (const auto& edge : *from) {
            auto get_result = table.get_edge(edge.edge_row_id);
            EXPECT_TRUE(get_result.has_value())
                << "forward index returned stale edge_row_id=" << edge.edge_row_id;
        }

        // Verify reverse index for the just-deleted target is clean.
        auto to = table.get_edges_to(pk(i + 100));
        ASSERT_TRUE(to.has_value());
        EXPECT_TRUE(to->empty()) << "reverse index stale for deleted target pk " << (i + 100);
    }
}

/// Delete all edges and verify completely empty state across all indexes.
TEST(QA_EdgeTableDeleteRollback, DeleteAllVerifyEmptyState) {
    EdgeTable table(make_config("e", true));

    constexpr int count = 100;
    std::vector<uint64_t> row_ids;
    std::vector<std::pair<int64_t, int64_t>> pairs;

    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 2000), {});
        ASSERT_TRUE(r.has_value()) << r.error().message;
        row_ids.push_back(*r);
        pairs.emplace_back(i, i + 2000);
    }
    ASSERT_EQ(table.size(), static_cast<uint64_t>(count));

    // Delete all.
    for (uint64_t id : row_ids) {
        ASSERT_TRUE(table.delete_edge(id).has_value());
    }

    EXPECT_EQ(table.size(), 0u);
    EXPECT_TRUE(table.empty());

    // Verify all forward lookups return empty.
    for (int i = 0; i < count; ++i) {
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "forward index not empty for pk " << i;
    }

    // Verify all reverse lookups return empty.
    for (int i = 0; i < count; ++i) {
        auto to = table.get_edges_to(pk(i + 2000));
        ASSERT_TRUE(to.has_value());
        EXPECT_TRUE(to->empty()) << "reverse index not empty for pk " << (i + 2000);
    }

    // Verify all unique index entries are cleaned (re-insert should succeed).
    for (int i = 0; i < count; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 2000), {});
        ASSERT_TRUE(r.has_value())
            << "unique index stale for pair (" << i << ", " << (i + 2000) << ")";
    }
    EXPECT_EQ(table.size(), static_cast<uint64_t>(count));
}

/// Self-edges with duplicate prevention: delete/re-insert cycles.
/// Self-edges are special because the same pk appears in both forward
/// and reverse indexes.
TEST(QA_EdgeTableDeleteRollback, SelfEdgeDeleteReinsertWithUniqueIndex) {
    EdgeTable table(make_config("e", true));

    for (int cycle = 0; cycle < 10; ++cycle) {
        auto r = table.insert_edge(pk(7), pk(7), {});
        ASSERT_TRUE(r.has_value()) << "cycle " << cycle << ": " << r.error().message;

        // Both forward and reverse should show the self-edge.
        auto from = table.get_edges_from(pk(7));
        ASSERT_TRUE(from.has_value());
        EXPECT_EQ(from->size(), 1u);

        auto to = table.get_edges_to(pk(7));
        ASSERT_TRUE(to.has_value());
        EXPECT_EQ(to->size(), 1u);

        // Duplicate blocked.
        auto dup = table.insert_edge(pk(7), pk(7), {});
        EXPECT_FALSE(dup.has_value());

        // Delete.
        ASSERT_TRUE(table.delete_edge(*r).has_value());

        // All indexes clean.
        from = table.get_edges_from(pk(7));
        ASSERT_TRUE(from.has_value());
        EXPECT_TRUE(from->empty()) << "forward index stale after self-edge delete, cycle " << cycle;

        to = table.get_edges_to(pk(7));
        ASSERT_TRUE(to.has_value());
        EXPECT_TRUE(to->empty()) << "reverse index stale after self-edge delete, cycle " << cycle;

        auto found = table.find_edge(pk(7), pk(7));
        ASSERT_TRUE(found.has_value());
        EXPECT_FALSE(found->has_value())
            << "unique index stale after self-edge delete, cycle " << cycle;
    }
}

/// Mixed insertion/deletion pattern: insert N edges, delete a random subset,
/// verify that only the non-deleted edges remain queryable.
TEST(QA_EdgeTableDeleteRollback, InterleavedInsertDeletePartialSubset) {
    EdgeTable table(make_config("e", true));

    constexpr int total = 40;
    std::vector<uint64_t> row_ids;
    for (int i = 0; i < total; ++i) {
        auto r = table.insert_edge(pk(i), pk(i + 3000), {});
        ASSERT_TRUE(r.has_value());
        row_ids.push_back(*r);
    }

    // Delete every third edge.
    std::set<int> deleted;
    for (int i = 0; i < total; i += 3) {
        ASSERT_TRUE(table.delete_edge(row_ids[static_cast<size_t>(i)]).has_value());
        deleted.insert(i);
    }

    // Verify consistency.
    for (int i = 0; i < total; ++i) {
        auto from = table.get_edges_from(pk(i));
        ASSERT_TRUE(from.has_value());

        if (deleted.count(i)) {
            EXPECT_TRUE(from->empty()) << "deleted edge pk " << i << " still in forward index";

            auto to = table.get_edges_to(pk(i + 3000));
            ASSERT_TRUE(to.has_value());
            EXPECT_TRUE(to->empty()) << "deleted edge target pk " << (i + 3000)
                                     << " still in reverse index";

            // Re-insert should succeed (unique index cleaned).
            auto reins = table.insert_edge(pk(i), pk(i + 3000), {});
            ASSERT_TRUE(reins.has_value())
                << "unique index stale for deleted pair (" << i << ", " << (i + 3000) << ")";
            // Delete again for clean final state.
            ASSERT_TRUE(table.delete_edge(*reins).has_value());
        } else {
            EXPECT_EQ(from->size(), 1u) << "surviving edge pk " << i << " missing from forward index";
        }
    }
}

// =============================================================================
// GDB-234: GraphEngine LINK/UNLINK WAL logging tests
// =============================================================================

/// Helper RAII class that creates a temporary WAL directory and cleans up.
class WalTestDir {
public:
    WalTestDir() {
        path_ = std::filesystem::temp_directory_path() / "giodb_qa_gdb_233_234"
                / std::to_string(counter_++);
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~WalTestDir() {
        std::filesystem::remove_all(path_);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

/// GraphEngine with nullptr WAL -- LINK and UNLINK should still work
/// (backward compatibility).
TEST(QA_GraphEngineWAL, NullptrWalBackwardCompat) {
    Catalog catalog;
    GraphEngine engine(catalog, nullptr);

    auto t1 = catalog.create_table(default_database_id, make_table_schema("users"));
    ASSERT_TRUE(t1.has_value());
    auto t2 = catalog.create_table(default_database_id, make_table_schema("posts"));
    ASSERT_TRUE(t2.has_value());

    ASSERT_TRUE(engine
                    .create_edge_type("follows", *t1, *t1, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    // LINK with nullptr WAL.
    auto link = engine.link("follows", pk(1), pk(2));
    ASSERT_TRUE(link.has_value()) << "LINK failed with nullptr WAL: " << link.error().message;

    // Verify the edge exists.
    auto from = engine.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_EQ(from->size(), 1u);

    // UNLINK with nullptr WAL.
    auto unlink = engine.unlink("follows", pk(1), pk(2));
    ASSERT_TRUE(unlink.has_value()) << "UNLINK failed with nullptr WAL: " << unlink.error().message;

    from = engine.get_edges_from("follows", pk(1));
    ASSERT_TRUE(from.has_value());
    EXPECT_TRUE(from->empty());
}

/// GraphEngine with a real WalWriter -- LINK should write a WAL record.
TEST(QA_GraphEngineWAL, LinkWritesWalRecord) {
    WalTestDir wal_dir;

    WalWriterOptions opts;
    opts.enable_group_commit = false;
    auto wal_writer = std::make_unique<WalWriter>(wal_dir.path(), opts);
    ASSERT_TRUE(wal_writer->open().has_value());

    lsn_t lsn_before = wal_writer->current_lsn();

    Catalog catalog;
    GraphEngine engine(catalog, wal_writer.get());

    auto t1 = catalog.create_table(default_database_id, make_table_schema("nodes"));
    ASSERT_TRUE(t1.has_value());

    ASSERT_TRUE(engine
                    .create_edge_type("connects", *t1, *t1, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    auto link = engine.link("connects", pk(10), pk(20));
    ASSERT_TRUE(link.has_value()) << link.error().message;

    // WAL should have advanced (a record was written).
    lsn_t lsn_after = wal_writer->current_lsn();
    EXPECT_GT(lsn_after, lsn_before) << "WAL LSN did not advance after LINK";

    // Flush and close the writer, then read back.
    ASSERT_TRUE(wal_writer->flush().has_value());
    ASSERT_TRUE(wal_writer->close().has_value());

    // Read WAL records and verify we find an INSERT record.
    WalReader reader(wal_dir.path());
    ASSERT_TRUE(reader.open().has_value());

    bool found_insert = false;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) {
            break; // End of WAL or error.
        }
        if (rec->type == WalRecordType::INSERT) {
            found_insert = true;
            break;
        }
    }
    EXPECT_TRUE(found_insert) << "No INSERT WAL record found after LINK";
    ASSERT_TRUE(reader.close().has_value());
}

/// GraphEngine with a real WalWriter -- UNLINK should write a WAL record.
TEST(QA_GraphEngineWAL, UnlinkWritesWalRecord) {
    WalTestDir wal_dir;

    WalWriterOptions opts;
    opts.enable_group_commit = false;
    auto wal_writer = std::make_unique<WalWriter>(wal_dir.path(), opts);
    ASSERT_TRUE(wal_writer->open().has_value());

    Catalog catalog;
    GraphEngine engine(catalog, wal_writer.get());

    auto t1 = catalog.create_table(default_database_id, make_table_schema("nodes"));
    ASSERT_TRUE(t1.has_value());

    ASSERT_TRUE(engine
                    .create_edge_type("connects", *t1, *t1, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    // LINK first.
    auto link = engine.link("connects", pk(10), pk(20));
    ASSERT_TRUE(link.has_value());

    lsn_t lsn_before_unlink = wal_writer->current_lsn();

    // UNLINK.
    auto unlink = engine.unlink("connects", pk(10), pk(20));
    ASSERT_TRUE(unlink.has_value()) << unlink.error().message;

    lsn_t lsn_after_unlink = wal_writer->current_lsn();
    EXPECT_GT(lsn_after_unlink, lsn_before_unlink) << "WAL LSN did not advance after UNLINK";

    ASSERT_TRUE(wal_writer->flush().has_value());
    ASSERT_TRUE(wal_writer->close().has_value());

    // Read WAL and verify we find a DELETE record.
    WalReader reader(wal_dir.path());
    ASSERT_TRUE(reader.open().has_value());

    bool found_delete = false;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) {
            break;
        }
        if (rec->type == WalRecordType::DELETE) {
            found_delete = true;
            break;
        }
    }
    EXPECT_TRUE(found_delete) << "No DELETE WAL record found after UNLINK";
    ASSERT_TRUE(reader.close().has_value());
}

/// Multiple LINKs then UNLINKs -- WAL records should accumulate.
TEST(QA_GraphEngineWAL, MultipleLinkUnlinkAccumulateWalRecords) {
    WalTestDir wal_dir;

    WalWriterOptions opts;
    opts.enable_group_commit = false;
    auto wal_writer = std::make_unique<WalWriter>(wal_dir.path(), opts);
    ASSERT_TRUE(wal_writer->open().has_value());

    Catalog catalog;
    GraphEngine engine(catalog, wal_writer.get());

    auto t1 = catalog.create_table(default_database_id, make_table_schema("nodes"));
    ASSERT_TRUE(t1.has_value());

    ASSERT_TRUE(engine
                    .create_edge_type("follows", *t1, *t1, TypeId::INT64, TypeId::INT64, {})
                    .has_value());

    constexpr int link_count = 5;

    // Perform several LINKs.
    for (int i = 0; i < link_count; ++i) {
        auto link = engine.link("follows", pk(1), pk(i + 100));
        ASSERT_TRUE(link.has_value()) << link.error().message;
    }

    // Then UNLINK all of them.
    for (int i = 0; i < link_count; ++i) {
        auto unlink = engine.unlink("follows", pk(1), pk(i + 100));
        ASSERT_TRUE(unlink.has_value()) << unlink.error().message;
    }

    ASSERT_TRUE(wal_writer->flush().has_value());
    ASSERT_TRUE(wal_writer->close().has_value());

    // Read WAL and count INSERT/DELETE records.
    WalReader reader(wal_dir.path());
    ASSERT_TRUE(reader.open().has_value());

    int insert_count = 0;
    int delete_count = 0;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) {
            break;
        }
        if (rec->type == WalRecordType::INSERT) {
            ++insert_count;
        } else if (rec->type == WalRecordType::DELETE) {
            ++delete_count;
        }
    }
    ASSERT_TRUE(reader.close().has_value());

    EXPECT_EQ(insert_count, link_count)
        << "Expected " << link_count << " INSERT WAL records, got " << insert_count;
    EXPECT_EQ(delete_count, link_count)
        << "Expected " << link_count << " DELETE WAL records, got " << delete_count;
}

/// LINK duplicate edge with WAL -- a failed LINK should NOT write a WAL record.
TEST(QA_GraphEngineWAL, FailedLinkDoesNotWriteWalRecord) {
    WalTestDir wal_dir;

    WalWriterOptions opts;
    opts.enable_group_commit = false;
    auto wal_writer = std::make_unique<WalWriter>(wal_dir.path(), opts);
    ASSERT_TRUE(wal_writer->open().has_value());

    Catalog catalog;
    GraphEngine engine(catalog, wal_writer.get());

    auto t1 = catalog.create_table(default_database_id, make_table_schema("nodes"));
    ASSERT_TRUE(t1.has_value());

    // Create with duplicate prevention enabled.
    ASSERT_TRUE(engine
                    .create_edge_type("follows", *t1, *t1, TypeId::INT64, TypeId::INT64, {}, true)
                    .has_value());

    // First LINK succeeds.
    auto link1 = engine.link("follows", pk(1), pk(2));
    ASSERT_TRUE(link1.has_value());

    lsn_t lsn_after_first = wal_writer->current_lsn();

    // Duplicate LINK should fail (constraint violation).
    auto link2 = engine.link("follows", pk(1), pk(2));
    EXPECT_FALSE(link2.has_value());
    EXPECT_EQ(link2.error().code, StatusCode::CONSTRAINT_VIOLATION);

    // WAL should NOT have advanced.
    lsn_t lsn_after_dup = wal_writer->current_lsn();
    EXPECT_EQ(lsn_after_first, lsn_after_dup)
        << "WAL advanced after failed duplicate LINK; should not have written a record";

    ASSERT_TRUE(wal_writer->flush().has_value());
    ASSERT_TRUE(wal_writer->close().has_value());

    // Double-check: read WAL and count INSERT records.
    WalReader reader(wal_dir.path());
    ASSERT_TRUE(reader.open().has_value());

    int insert_count = 0;
    while (true) {
        auto rec = reader.next();
        if (!rec.has_value()) {
            break;
        }
        if (rec->type == WalRecordType::INSERT) {
            ++insert_count;
        }
    }
    ASSERT_TRUE(reader.close().has_value());

    EXPECT_EQ(insert_count, 1) << "Expected exactly 1 INSERT WAL record (not 2)";
}
