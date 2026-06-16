/// @file test_gdb_258.cpp
/// @brief Regression tests for GDB-258: ProviderRegistry never instantiated —
///        NEAREST auto-embed and REEMBED fail.
///
/// Verifies that when ProviderRegistry is wired to QueryEngine via
/// set_provider_registry(), the Planner receives it and NEAREST text queries
/// as well as REEMBED commands work correctly.

#include "test_qa_gdb_258_fixture.h"
#include "test_qa_helpers.h"

// ---------------------------------------------------------------------------
// Gap 2 regression: Planner receives provider_registry_ from QueryEngine
// ---------------------------------------------------------------------------

TEST_F(GDB258ProviderRegistryTest, NearestTextQueryFailsWithoutRegistry) {
    // Without calling set_provider_registry, NEAREST with text target must fail
    // with a descriptive error (not a crash).
    create_embedding_table();
    insert_row(1, "AI in 2025", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "Machine learning basics", {0.0F, 1.0F, 0.0F, 0.0F});

    exec_error("SELECT * FROM articles WHERE NEAREST(title_vec, 2) TO 'machine learning'",
               StatusCode::NOT_IMPLEMENTED);
}

TEST_F(GDB258ProviderRegistryTest, NearestTextQuerySucceedsWithRegistry) {
    // Wire provider_registry to engine (the fix for Gap 1 + Gap 2).
    engine_->set_provider_registry(provider_registry_.get());
    create_embedding_table();
    insert_row(1, "AI in 2025", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "Machine learning basics", {0.0F, 1.0F, 0.0F, 0.0F});
    insert_row(3, "Database internals", {0.0F, 0.0F, 1.0F, 0.0F});

    // This would previously fail with "text auto-embedding requires a ProviderRegistry"
    // because the Planner was never given provider_registry_.
    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 2) TO 'machine learning'");
    EXPECT_EQ(qr.rows.size(), 2u);
}

TEST_F(GDB258ProviderRegistryTest, NearestVectorQueryWorksWithoutRegistry) {
    // NEAREST with a literal vector target should work even without a registry,
    // because no text auto-embedding is needed.
    create_embedding_table();
    insert_row(1, "AI in 2025", {1.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "Machine learning basics", {0.0F, 1.0F, 0.0F, 0.0F});

    auto qr = exec_ok("SELECT * FROM articles WHERE NEAREST(title_vec, 2) TO [1.0, 0.0, 0.0, 0.0]");
    EXPECT_EQ(qr.rows.size(), 2u);
}

// ---------------------------------------------------------------------------
// REEMBED regression
// ---------------------------------------------------------------------------

TEST_F(GDB258ProviderRegistryTest, ReembedFailsWithoutRegistry) {
    create_embedding_table();
    insert_row(1, "AI in 2025", {0.0F, 0.0F, 0.0F, 0.0F});

    exec_error("REEMBED TABLE articles", StatusCode::INTERNAL_ERROR);
}

TEST_F(GDB258ProviderRegistryTest, ReembedSucceedsWithRegistry) {
    engine_->set_provider_registry(provider_registry_.get());
    create_embedding_table();
    insert_row(1, "AI in 2025", {0.0F, 0.0F, 0.0F, 0.0F});
    insert_row(2, "Machine learning", {0.0F, 0.0F, 0.0F, 0.0F});

    auto qr = exec_ok("REEMBED TABLE articles");
    EXPECT_EQ(qr.affected_rows, 2);
}

// ---------------------------------------------------------------------------
// EXPLAIN with NEAREST text target
// ---------------------------------------------------------------------------

TEST_F(GDB258ProviderRegistryTest, ExplainNearestTextWithRegistry) {
    // The EXPLAIN path also constructs a Planner — verify it gets the registry.
    engine_->set_provider_registry(provider_registry_.get());
    create_embedding_table();
    insert_row(1, "AI in 2025", {1.0F, 0.0F, 0.0F, 0.0F});

    auto qr =
        exec_ok("EXPLAIN SELECT * FROM articles WHERE NEAREST(title_vec, 1) TO 'search text'");
    EXPECT_FALSE(qr.rows.empty());
}
