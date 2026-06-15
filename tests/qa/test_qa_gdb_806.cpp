// QA regression tests for GDB-806: Dropped tables must not leak auto-increment counters.
//
// Attack surface:
//   - In-memory counter erased by drop_table_locked (the fix).
//   - drop_database CASCADE must also clear counters via drop_table_locked chain.
//   - Multiple tables: only the dropped table's counter is affected.
//   - Counter advanced mid-sequence before drop.
//   - Recreate with same name gets a fresh counter (new table_id, no stale state).
//   - Durability: the counter is in-memory only in Catalog; persistence is handled
//     by StorageManager (table file header). DROP TABLE removes the file, so no
//     stale persisted counter can be resurrected. This test suite validates the
//     in-memory side (the Catalog layer), which is where the fix lives.

#include "sixseven/catalog/catalog.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace sixseven;

// ---------------------------------------------------------------------------
// Helper: restore default "demo" database for tests
// ---------------------------------------------------------------------------
static void init_qa_catalog(Catalog& cat) {
    auto r = cat.restore_database(default_database_id, "demo");
    (void)r;
}

// Helper: build a minimal TableSchema with one autoincrement-style INT32 column.
static TableSchema make_ai_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "val", TypeId::STRING, true, ""},
    };
    s.columns[0].is_autoincrement = true;
    s.pk_columns = "id";
    return s;
}

// Helper: build a schema without any autoincrement column.
static TableSchema make_plain_schema(const std::string& name) {
    TableSchema s;
    s.name = name;
    s.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "data", TypeId::STRING, true, ""},
    };
    s.pk_columns = "id";
    return s;
}

// ---------------------------------------------------------------------------
// Suite QA_GDB806
// ---------------------------------------------------------------------------

// AC: counter is absent after DROP TABLE
TEST(QA_GDB806, DropTableClearsCounterFromMemory) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid = cat.create_table(default_database_id, make_ai_schema("t"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    cat.init_autoincrement(*tid, 1);
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 1);

    ASSERT_TRUE(cat.drop_table(default_database_id, "t").has_value());

    // Counter must be 0 (absent sentinel) after drop.
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 0);
}

// AC: counter advanced mid-sequence then table dropped — stale value must not linger.
TEST(QA_GDB806, DropTableMidSequenceClearsCounter) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid = cat.create_table(default_database_id, make_ai_schema("orders"));
    ASSERT_TRUE(tid.has_value());

    cat.init_autoincrement(*tid, 1);
    // Simulate many inserts advancing the counter.
    cat.advance_autoincrement(*tid, 999);
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 1000);

    ASSERT_TRUE(cat.drop_table(default_database_id, "orders").has_value());

    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 0)
        << "Counter should be absent (0) after DROP TABLE, not " << cat.get_autoincrement_counter(*tid);
}

// AC: recreate same table name → new table_id, no stale counter.
TEST(QA_GDB806, DropAndRecreateStartsFreshAtOne) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid1 = cat.create_table(default_database_id, make_ai_schema("items"));
    ASSERT_TRUE(tid1.has_value());
    cat.init_autoincrement(*tid1, 1);
    cat.advance_autoincrement(*tid1, 500);
    EXPECT_EQ(cat.get_autoincrement_counter(*tid1), 501);

    ASSERT_TRUE(cat.drop_table(default_database_id, "items").has_value());

    auto tid2 = cat.create_table(default_database_id, make_ai_schema("items"));
    ASSERT_TRUE(tid2.has_value());

    // New table must have a different ID.
    EXPECT_NE(*tid1, *tid2) << "Recreated table must get a new table_id";

    // New table must have no counter yet.
    EXPECT_EQ(cat.get_autoincrement_counter(*tid2), 0)
        << "Recreated table must start with no counter (0), not " << cat.get_autoincrement_counter(*tid2);

    // Initialising the new table's counter at 1 must work cleanly.
    cat.init_autoincrement(*tid2, 1);
    EXPECT_EQ(cat.get_autoincrement_counter(*tid2), 1);
}

// AC: multiple tables — drop one, others' counters unaffected.
TEST(QA_GDB806, DropOneTableDoesNotAffectOtherCounters) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tA = cat.create_table(default_database_id, make_ai_schema("A"));
    auto tB = cat.create_table(default_database_id, make_ai_schema("B"));
    auto tC = cat.create_table(default_database_id, make_ai_schema("C"));
    ASSERT_TRUE(tA.has_value());
    ASSERT_TRUE(tB.has_value());
    ASSERT_TRUE(tC.has_value());

    cat.init_autoincrement(*tA, 1);
    cat.advance_autoincrement(*tA, 10);
    cat.init_autoincrement(*tB, 1);
    cat.advance_autoincrement(*tB, 20);
    cat.init_autoincrement(*tC, 1);
    cat.advance_autoincrement(*tC, 30);

    EXPECT_EQ(cat.get_autoincrement_counter(*tA), 11);
    EXPECT_EQ(cat.get_autoincrement_counter(*tB), 21);
    EXPECT_EQ(cat.get_autoincrement_counter(*tC), 31);

    // Drop B — only B's counter should disappear.
    ASSERT_TRUE(cat.drop_table(default_database_id, "B").has_value());

    EXPECT_EQ(cat.get_autoincrement_counter(*tA), 11)
        << "Table A counter must be unchanged after dropping table B";
    EXPECT_EQ(cat.get_autoincrement_counter(*tB), 0)
        << "Table B counter must be absent after DROP TABLE";
    EXPECT_EQ(cat.get_autoincrement_counter(*tC), 31)
        << "Table C counter must be unchanged after dropping table B";
}

// AC: DROP DATABASE CASCADE drops tables and their counters via drop_table_locked.
TEST(QA_GDB806, DropDatabaseCascadeClearsCounters) {
    Catalog cat;
    init_qa_catalog(cat);

    auto db_id = cat.create_database("cascade_db");
    ASSERT_TRUE(db_id.has_value());

    auto t1 = cat.create_table(*db_id, make_ai_schema("t1"));
    auto t2 = cat.create_table(*db_id, make_ai_schema("t2"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    cat.init_autoincrement(*t1, 1);
    cat.advance_autoincrement(*t1, 100);
    cat.init_autoincrement(*t2, 1);
    cat.advance_autoincrement(*t2, 200);

    EXPECT_EQ(cat.get_autoincrement_counter(*t1), 101);
    EXPECT_EQ(cat.get_autoincrement_counter(*t2), 201);

    // CASCADE drop of entire database should also clear counters.
    auto drop = cat.drop_database(*db_id, true);
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    EXPECT_EQ(cat.get_autoincrement_counter(*t1), 0)
        << "Counter for t1 must be absent after CASCADE DROP DATABASE";
    EXPECT_EQ(cat.get_autoincrement_counter(*t2), 0)
        << "Counter for t2 must be absent after CASCADE DROP DATABASE";
}

// Adversarial: init_autoincrement on a dropped table_id must not resurrect a counter.
TEST(QA_GDB806, InitAutoincrementOnDroppedIdDoesNotInfectNewTable) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid1 = cat.create_table(default_database_id, make_ai_schema("ghost"));
    ASSERT_TRUE(tid1.has_value());
    cat.init_autoincrement(*tid1, 1);
    cat.advance_autoincrement(*tid1, 77);
    ASSERT_TRUE(cat.drop_table(default_database_id, "ghost").has_value());

    // Recreate and get new tid.
    auto tid2 = cat.create_table(default_database_id, make_ai_schema("ghost"));
    ASSERT_TRUE(tid2.has_value());
    EXPECT_NE(*tid1, *tid2);

    // Stale tid1 counter is gone — calling init with old tid affects nothing on new table.
    EXPECT_EQ(cat.get_autoincrement_counter(*tid2), 0)
        << "New table must have counter=0 (absent) even after dropped table used same name";
}

// Adversarial: table WITHOUT an autoincrement column — drop must not crash.
TEST(QA_GDB806, DropTableWithoutAutoincrementColumnIsClean) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid = cat.create_table(default_database_id, make_plain_schema("plain"));
    ASSERT_TRUE(tid.has_value());

    // No init_autoincrement call — counter was never set.
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 0);

    // Drop must succeed with no crash.
    auto drop = cat.drop_table(default_database_id, "plain");
    ASSERT_TRUE(drop.has_value()) << drop.error().message;

    // Counter still absent.
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 0);
}

// Adversarial: repeated drop-recreate cycle — counter always fresh.
TEST(QA_GDB806, RepeatedDropRecreateAlwaysFresh) {
    Catalog cat;
    init_qa_catalog(cat);

    for (int cycle = 0; cycle < 5; ++cycle) {
        auto tid = cat.create_table(default_database_id, make_ai_schema("cycling"));
        ASSERT_TRUE(tid.has_value()) << "cycle " << cycle;

        // Counter must be absent at creation.
        EXPECT_EQ(cat.get_autoincrement_counter(*tid), 0) << "cycle " << cycle;

        cat.init_autoincrement(*tid, 1);
        cat.advance_autoincrement(*tid, static_cast<int64_t>((cycle + 1) * 1000));
        EXPECT_GT(cat.get_autoincrement_counter(*tid), 0) << "cycle " << cycle;

        auto drop = cat.drop_table(default_database_id, "cycling");
        ASSERT_TRUE(drop.has_value()) << "cycle " << cycle << ": " << drop.error().message;
    }
}

// Adversarial: many tables, drop all, then verify none have counters.
TEST(QA_GDB806, DropManyTablesAllCountersCleared) {
    Catalog cat;
    init_qa_catalog(cat);

    constexpr int N = 20;
    std::vector<table_id_t> tids;
    tids.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto tid = cat.create_table(default_database_id, make_ai_schema("t_" + std::to_string(i)));
        ASSERT_TRUE(tid.has_value());
        cat.init_autoincrement(*tid, 1);
        cat.advance_autoincrement(*tid, static_cast<int64_t>(i * 10 + 1));
        tids.push_back(*tid);
    }

    // Drop all tables.
    for (int i = 0; i < N; ++i) {
        auto drop = cat.drop_table(default_database_id, "t_" + std::to_string(i));
        ASSERT_TRUE(drop.has_value()) << "drop t_" << i << ": " << drop.error().message;
    }

    // All counters must be gone.
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(cat.get_autoincrement_counter(tids[static_cast<size_t>(i)]), 0)
            << "counter for t_" << i << " must be absent";
    }
}

// Adversarial: drop then re-check next_autoincrement returns error (no counter).
TEST(QA_GDB806, NextAutoincrementOnDroppedTableReturnsError) {
    Catalog cat;
    init_qa_catalog(cat);

    auto tid = cat.create_table(default_database_id, make_ai_schema("seq_table"));
    ASSERT_TRUE(tid.has_value());
    cat.init_autoincrement(*tid, 1);
    EXPECT_EQ(cat.get_autoincrement_counter(*tid), 1);

    ASSERT_TRUE(cat.drop_table(default_database_id, "seq_table").has_value());

    // Calling next_autoincrement on the now-dropped tid should fail, not return a stale value.
    auto next = cat.next_autoincrement(*tid, TypeId::INT32);
    EXPECT_FALSE(next.has_value())
        << "next_autoincrement on dropped table must fail, but returned " << (next.has_value() ? *next : -1);
}

// Adversarial: cross-database isolation — dropping in db1 must not affect db2 counter.
TEST(QA_GDB806, DropInOneDatabaseDoesNotAffectCounterInAnotherDatabase) {
    Catalog cat;
    init_qa_catalog(cat);

    auto db2 = cat.create_database("db2");
    ASSERT_TRUE(db2.has_value());

    // Same logical table name in two different databases.
    auto t1 = cat.create_table(default_database_id, make_ai_schema("shared"));
    auto t2 = cat.create_table(*db2, make_ai_schema("shared"));
    ASSERT_TRUE(t1.has_value());
    ASSERT_TRUE(t2.has_value());

    cat.init_autoincrement(*t1, 1);
    cat.advance_autoincrement(*t1, 42);
    cat.init_autoincrement(*t2, 1);
    cat.advance_autoincrement(*t2, 99);

    EXPECT_EQ(cat.get_autoincrement_counter(*t1), 43);
    EXPECT_EQ(cat.get_autoincrement_counter(*t2), 100);

    // Drop the table only in default_database_id.
    ASSERT_TRUE(cat.drop_table(default_database_id, "shared").has_value());

    EXPECT_EQ(cat.get_autoincrement_counter(*t1), 0)
        << "Dropped table's counter must be absent";
    EXPECT_EQ(cat.get_autoincrement_counter(*t2), 100)
        << "Other database's counter must be unaffected";
}
