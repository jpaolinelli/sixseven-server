#include "sixseven/catalog/catalog.h"
#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/query_engine.h"
#include "sixseven/executor/storage_manager.h"
#include "sixseven/storage/disk_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_catalog_helpers.h"

using namespace sixseven;

// =============================================================================
// GDB-1203 QA: adversarial coverage for RANK()/DENSE_RANK() tie-handling.
//
// The strengthened dev tests in tests/unit/test_window_sql.cpp
// (RankPartitionByDept, DenseRank) use a seeded dataset with NO ties, so they
// cannot distinguish RANK's "skip after tie" semantics from DENSE_RANK's
// "no skip" semantics. This file closes that gap: every test here includes
// at least one tie in the ORDER BY column and asserts the exact expected
// rank sequence, including the gap (or lack of gap) after the tie.
// =============================================================================

class QA_GDB1203_RankTie : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_catalog(catalog_);
        data_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb1203";
        std::filesystem::remove_all(data_dir_);
        std::filesystem::create_directories(data_dir_);

        storage_ = std::make_unique<StorageManager>(dm_, data_dir_);
        engine_ = std::make_unique<QueryEngine>(catalog_, *storage_);
    }

    void TearDown() override {
        engine_.reset();
        storage_.reset();
        std::filesystem::remove_all(data_dir_);
    }

    QueryResult exec_ok(const std::string& sql) {
        auto result = engine_->execute(sql);
        EXPECT_TRUE(result.has_value()) << "SQL: " << sql << "\n"
                                        << "Error: " << result.error().message;
        return result ? std::move(*result) : QueryResult{};
    }

    DiskManager dm_;
    Catalog catalog_;
    std::filesystem::path data_dir_;
    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<QueryEngine> engine_;
};

// -----------------------------------------------------------------------
// Core semantic test: RANK skips after a tie, DENSE_RANK does not.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, RankSkipsAfterTieDenseRankDoesNot) {
    exec_ok("CREATE TABLE t (id INT PRIMARY KEY, name TEXT, score INT)");
    // Two rows tied at score=100 (highest), one row at score=90, one at 80.
    exec_ok("INSERT INTO t VALUES (1, 'A', 100)");
    exec_ok("INSERT INTO t VALUES (2, 'B', 100)");
    exec_ok("INSERT INTO t VALUES (3, 'C', 90)");
    exec_ok("INSERT INTO t VALUES (4, 'D', 80)");

    auto qr = exec_ok("SELECT name, score, "
                      "RANK() OVER (ORDER BY score DESC) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score DESC) AS drnk "
                      "FROM t");
    ASSERT_EQ(qr.rows.size(), 4u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[2].as_int64();
        drank_by_name[row[0].as_string()] = row[3].as_int64();
    }
    ASSERT_EQ(rank_by_name.size(), 4u);

    // RANK: tied rows A and B both get 1; the next distinct row (C) must
    // SKIP to 3 (not 2), because two rows preceded it. Last row (D) is 4.
    EXPECT_EQ(rank_by_name.at("A"), 1);
    EXPECT_EQ(rank_by_name.at("B"), 1);
    EXPECT_EQ(rank_by_name.at("C"), 3) << "RANK must skip a value after a 2-way tie";
    EXPECT_EQ(rank_by_name.at("D"), 4);

    // DENSE_RANK: tied rows both get 1; the next distinct row gets 2 (NOT 3),
    // since dense rank has no gaps.
    EXPECT_EQ(drank_by_name.at("A"), 1);
    EXPECT_EQ(drank_by_name.at("B"), 1);
    EXPECT_EQ(drank_by_name.at("C"), 2) << "DENSE_RANK must not skip after a tie";
    EXPECT_EQ(drank_by_name.at("D"), 3);
}

// -----------------------------------------------------------------------
// Three-way tie: RANK should skip by 3, DENSE_RANK by 1.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, ThreeWayTieRankSkipsByGroupSize) {
    exec_ok("CREATE TABLE t3 (id INT PRIMARY KEY, name TEXT, score INT)");
    exec_ok("INSERT INTO t3 VALUES (1, 'A', 50)");
    exec_ok("INSERT INTO t3 VALUES (2, 'B', 50)");
    exec_ok("INSERT INTO t3 VALUES (3, 'C', 50)");
    exec_ok("INSERT INTO t3 VALUES (4, 'D', 40)");

    auto qr = exec_ok("SELECT name, "
                      "RANK() OVER (ORDER BY score DESC) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score DESC) AS drnk "
                      "FROM t3");
    ASSERT_EQ(qr.rows.size(), 4u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[1].as_int64();
        drank_by_name[row[0].as_string()] = row[2].as_int64();
    }

    EXPECT_EQ(rank_by_name.at("A"), 1);
    EXPECT_EQ(rank_by_name.at("B"), 1);
    EXPECT_EQ(rank_by_name.at("C"), 1);
    EXPECT_EQ(rank_by_name.at("D"), 4) << "RANK must skip to 4 after a 3-way tie at 1";

    EXPECT_EQ(drank_by_name.at("A"), 1);
    EXPECT_EQ(drank_by_name.at("B"), 1);
    EXPECT_EQ(drank_by_name.at("C"), 1);
    EXPECT_EQ(drank_by_name.at("D"), 2) << "DENSE_RANK must be 2 (no gap) after 3-way tie";
}

// -----------------------------------------------------------------------
// All rows tied: RANK and DENSE_RANK both give 1 to every row.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, AllRowsTiedGiveRankOneEverywhere) {
    exec_ok("CREATE TABLE t_all (id INT PRIMARY KEY, score INT)");
    exec_ok("INSERT INTO t_all VALUES (1, 42)");
    exec_ok("INSERT INTO t_all VALUES (2, 42)");
    exec_ok("INSERT INTO t_all VALUES (3, 42)");
    exec_ok("INSERT INTO t_all VALUES (4, 42)");

    auto qr = exec_ok("SELECT id, "
                      "RANK() OVER (ORDER BY score) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score) AS drnk "
                      "FROM t_all");
    ASSERT_EQ(qr.rows.size(), 4u);
    for (auto& row : qr.rows) {
        EXPECT_EQ(row[1].as_int64(), 1) << "all-tied RANK must be 1 for every row";
        EXPECT_EQ(row[2].as_int64(), 1) << "all-tied DENSE_RANK must be 1 for every row";
    }
}

// -----------------------------------------------------------------------
// Single-row partition: rank must be 1.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, SingleRowGivesRankOne) {
    exec_ok("CREATE TABLE t_single (id INT PRIMARY KEY, score INT)");
    exec_ok("INSERT INTO t_single VALUES (1, 999)");

    auto qr = exec_ok("SELECT id, "
                      "RANK() OVER (ORDER BY score) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score) AS drnk "
                      "FROM t_single");
    ASSERT_EQ(qr.rows.size(), 1u);
    EXPECT_EQ(qr.rows[0][1].as_int64(), 1);
    EXPECT_EQ(qr.rows[0][2].as_int64(), 1);
}

// -----------------------------------------------------------------------
// ORDER BY ASC vs DESC: tie-skip semantics must hold in both directions.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, TieSkipHoldsForAscendingOrder) {
    exec_ok("CREATE TABLE t_asc (id INT PRIMARY KEY, name TEXT, score INT)");
    exec_ok("INSERT INTO t_asc VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO t_asc VALUES (2, 'B', 10)");
    exec_ok("INSERT INTO t_asc VALUES (3, 'C', 20)");

    auto qr = exec_ok("SELECT name, "
                      "RANK() OVER (ORDER BY score ASC) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score ASC) AS drnk "
                      "FROM t_asc");
    ASSERT_EQ(qr.rows.size(), 3u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[1].as_int64();
        drank_by_name[row[0].as_string()] = row[2].as_int64();
    }
    EXPECT_EQ(rank_by_name.at("A"), 1);
    EXPECT_EQ(rank_by_name.at("B"), 1);
    EXPECT_EQ(rank_by_name.at("C"), 3) << "RANK must skip to 3 after ASC tie at 1";

    EXPECT_EQ(drank_by_name.at("A"), 1);
    EXPECT_EQ(drank_by_name.at("B"), 1);
    EXPECT_EQ(drank_by_name.at("C"), 2);
}

// -----------------------------------------------------------------------
// Multiple partitions, each with an internal tie: partitions must not
// leak rank state into each other.
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, MultiplePartitionsEachWithOwnTie) {
    exec_ok("CREATE TABLE t_part (id INT PRIMARY KEY, grp TEXT, name TEXT, score INT)");
    exec_ok("INSERT INTO t_part VALUES (1, 'X', 'A', 5)");
    exec_ok("INSERT INTO t_part VALUES (2, 'X', 'B', 5)");
    exec_ok("INSERT INTO t_part VALUES (3, 'X', 'C', 3)");
    exec_ok("INSERT INTO t_part VALUES (4, 'Y', 'D', 9)");
    exec_ok("INSERT INTO t_part VALUES (5, 'Y', 'E', 9)");
    exec_ok("INSERT INTO t_part VALUES (6, 'Y', 'F', 1)");

    auto qr = exec_ok("SELECT grp, name, "
                      "RANK() OVER (PARTITION BY grp ORDER BY score DESC) AS rnk, "
                      "DENSE_RANK() OVER (PARTITION BY grp ORDER BY score DESC) AS drnk "
                      "FROM t_part");
    ASSERT_EQ(qr.rows.size(), 6u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[1].as_string()] = row[2].as_int64();
        drank_by_name[row[1].as_string()] = row[3].as_int64();
    }

    // Partition X: A/B tied at 5 (rank 1), C at 3 (rank must skip to 3).
    EXPECT_EQ(rank_by_name.at("A"), 1);
    EXPECT_EQ(rank_by_name.at("B"), 1);
    EXPECT_EQ(rank_by_name.at("C"), 3);
    EXPECT_EQ(drank_by_name.at("A"), 1);
    EXPECT_EQ(drank_by_name.at("B"), 1);
    EXPECT_EQ(drank_by_name.at("C"), 2);

    // Partition Y: D/E tied at 9 (rank 1), F at 1 (rank must skip to 3),
    // independent of partition X's ranks.
    EXPECT_EQ(rank_by_name.at("D"), 1);
    EXPECT_EQ(rank_by_name.at("E"), 1);
    EXPECT_EQ(rank_by_name.at("F"), 3);
    EXPECT_EQ(drank_by_name.at("D"), 1);
    EXPECT_EQ(drank_by_name.at("E"), 1);
    EXPECT_EQ(drank_by_name.at("F"), 2);
}

// -----------------------------------------------------------------------
// NULLs in the ORDER BY column: NULLs should form their own peer group,
// and rank/dense_rank must still be self-consistent (no crash, and the
// non-null groups still tie/skip correctly around the NULL group).
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, NullsInOrderByFormOwnPeerGroup) {
    exec_ok("CREATE TABLE t_null (id INT PRIMARY KEY, name TEXT, score INT)");
    exec_ok("INSERT INTO t_null VALUES (1, 'A', 10)");
    exec_ok("INSERT INTO t_null VALUES (2, 'B', NULL)");
    exec_ok("INSERT INTO t_null VALUES (3, 'C', NULL)");
    exec_ok("INSERT INTO t_null VALUES (4, 'D', 10)");

    auto qr = exec_ok("SELECT name, score, "
                      "RANK() OVER (ORDER BY score ASC) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score ASC) AS drnk "
                      "FROM t_null");
    ASSERT_EQ(qr.rows.size(), 4u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[2].as_int64();
        drank_by_name[row[0].as_string()] = row[3].as_int64();
    }

    // B and C (both NULL) must tie with each other.
    EXPECT_EQ(rank_by_name.at("B"), rank_by_name.at("C"))
        << "NULLs in ORDER BY should be peers with each other";
    EXPECT_EQ(drank_by_name.at("B"), drank_by_name.at("C"));

    // A and D (both 10) must tie with each other, and their rank/dense_rank
    // must differ from B/C's group (NULLs are not peers of non-null values).
    EXPECT_EQ(rank_by_name.at("A"), rank_by_name.at("D"));
    EXPECT_EQ(drank_by_name.at("A"), drank_by_name.at("D"));
    EXPECT_NE(rank_by_name.at("A"), rank_by_name.at("B"))
        << "NULL group must not be peers with non-null group";
}

// -----------------------------------------------------------------------
// Adjacent ties in the middle of the sequence (not just at the top/bottom).
// -----------------------------------------------------------------------
TEST_F(QA_GDB1203_RankTie, TieInMiddleOfSequence) {
    exec_ok("CREATE TABLE t_mid (id INT PRIMARY KEY, name TEXT, score INT)");
    exec_ok("INSERT INTO t_mid VALUES (1, 'A', 100)");
    exec_ok("INSERT INTO t_mid VALUES (2, 'B', 90)");
    exec_ok("INSERT INTO t_mid VALUES (3, 'C', 90)");
    exec_ok("INSERT INTO t_mid VALUES (4, 'D', 80)");

    auto qr = exec_ok("SELECT name, "
                      "RANK() OVER (ORDER BY score DESC) AS rnk, "
                      "DENSE_RANK() OVER (ORDER BY score DESC) AS drnk "
                      "FROM t_mid");
    ASSERT_EQ(qr.rows.size(), 4u);

    std::unordered_map<std::string, int64_t> rank_by_name, drank_by_name;
    for (auto& row : qr.rows) {
        rank_by_name[row[0].as_string()] = row[1].as_int64();
        drank_by_name[row[0].as_string()] = row[2].as_int64();
    }

    EXPECT_EQ(rank_by_name.at("A"), 1);
    EXPECT_EQ(rank_by_name.at("B"), 2);
    EXPECT_EQ(rank_by_name.at("C"), 2);
    EXPECT_EQ(rank_by_name.at("D"), 4) << "RANK must skip 3 after mid-sequence tie";

    EXPECT_EQ(drank_by_name.at("A"), 1);
    EXPECT_EQ(drank_by_name.at("B"), 2);
    EXPECT_EQ(drank_by_name.at("C"), 2);
    EXPECT_EQ(drank_by_name.at("D"), 3);
}
