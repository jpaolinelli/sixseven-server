#include "sixseven/vector/embedding_column.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace sixseven;

// -- Helper: build a table schema with an EMBEDDING column --------------------

static TableSchema make_schema_with_embedding(const std::string& name) {
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

static TableSchema make_schema_multi_embedding(const std::string& name) {
    TableSchema schema;
    schema.name = name;
    schema.columns = {
        {0, "id", TypeId::INT32, false, ""},
        {1, "title", TypeId::STRING, true, ""},
        {2, "body", TypeId::STRING, true, ""},
        {3, "title_vec", TypeId::EMBEDDING, true, ""},
        {4, "body_vec", TypeId::EMBEDDING, true, ""},
    };
    schema.pk_columns = "id";
    return schema;
}

// -- register_table_embeddings ------------------------------------------------

TEST(EmbeddingColumn, RegisterSingleEmbedding) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    EmbeddingColumnDef def;
    def.table_id = *tid;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(*tid, {def});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify embedding column was registered in catalog.
    auto embs = catalog.list_embedding_columns(*tid);
    ASSERT_EQ(embs.size(), 1u);
    EXPECT_EQ(embs[0].column_id, 3);
    EXPECT_EQ(embs[0].dimension, 384);
    EXPECT_EQ(embs[0].source_expr, "title");
    EXPECT_EQ(embs[0].provider, "openai");

    // Verify companion HNSW index was auto-created.
    auto indexes = catalog.list_indexes(*tid);
    ASSERT_EQ(indexes.size(), 1u);
    EXPECT_EQ(indexes[0].name, "hnsw_docs_embedding");
    EXPECT_EQ(indexes[0].index_type, "hnsw");
    EXPECT_EQ(indexes[0].columns, "embedding");
}

TEST(EmbeddingColumn, RegisterMultipleEmbeddings) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_multi_embedding("articles"));
    ASSERT_TRUE(tid.has_value()) << tid.error().message;

    EmbeddingColumnDef def1;
    def1.column_id = 3;
    def1.dimension = 384;
    def1.source_expr = "title";
    def1.provider = "openai";

    EmbeddingColumnDef def2;
    def2.column_id = 4;
    def2.dimension = 768;
    def2.source_expr = "body";
    def2.provider = "cohere";

    auto result = mgr.register_table_embeddings(*tid, {def1, def2});
    ASSERT_TRUE(result.has_value()) << result.error().message;

    auto embs = catalog.list_embedding_columns(*tid);
    EXPECT_EQ(embs.size(), 2u);

    auto indexes = catalog.list_indexes(*tid);
    EXPECT_EQ(indexes.size(), 2u);

    // Verify both HNSW indexes were created.
    auto idx1 = catalog.get_index("hnsw_articles_title_vec");
    ASSERT_TRUE(idx1.has_value()) << idx1.error().message;
    EXPECT_EQ(idx1->index_type, "hnsw");

    auto idx2 = catalog.get_index("hnsw_articles_body_vec");
    ASSERT_TRUE(idx2.has_value()) << idx2.error().message;
    EXPECT_EQ(idx2->index_type, "hnsw");
}

TEST(EmbeddingColumn, RegisterInvalidDimensionFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 0;
    def.source_expr = "title";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(*tid, {def});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(EmbeddingColumn, RegisterEmptySourceExprFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(*tid, {def});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(EmbeddingColumn, RegisterEmptyProviderFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "";

    auto result = mgr.register_table_embeddings(*tid, {def});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(EmbeddingColumn, RegisterNonexistentTableFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    EmbeddingColumnDef def;
    def.column_id = 0;
    def.dimension = 128;
    def.source_expr = "title";
    def.provider = "openai";

    auto result = mgr.register_table_embeddings(999, {def});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NOT_FOUND);
}

TEST(EmbeddingColumn, RegisterDuplicateColumnFails) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";

    auto first = mgr.register_table_embeddings(*tid, {def});
    ASSERT_TRUE(first.has_value()) << first.error().message;

    auto second = mgr.register_table_embeddings(*tid, {def});
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, StatusCode::ALREADY_EXISTS);
}

// -- create_insert_jobs -------------------------------------------------------

TEST(EmbeddingColumn, CreateInsertJobsSingleColumn) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    auto jobs = mgr.create_insert_jobs(*tid, 42, {{3, "Hello world"}});
    ASSERT_TRUE(jobs.has_value()) << jobs.error().message;
    ASSERT_EQ(jobs->size(), 1u);

    EXPECT_EQ((*jobs)[0].table_id, *tid);
    EXPECT_EQ((*jobs)[0].row_id, 42);
    EXPECT_EQ((*jobs)[0].column_id, 3);
    EXPECT_EQ((*jobs)[0].source_text, "Hello world");
    EXPECT_EQ((*jobs)[0].provider, "openai");
    EXPECT_EQ((*jobs)[0].dimension, 384);
    EXPECT_EQ((*jobs)[0].type, EmbeddingJob::Type::INSERT);
    EXPECT_EQ((*jobs)[0].retry_count, 0);
}

TEST(EmbeddingColumn, CreateInsertJobsMultipleColumns) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_multi_embedding("articles"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def1;
    def1.column_id = 3;
    def1.dimension = 384;
    def1.source_expr = "title";
    def1.provider = "openai";

    EmbeddingColumnDef def2;
    def2.column_id = 4;
    def2.dimension = 768;
    def2.source_expr = "body";
    def2.provider = "cohere";

    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def1, def2}).has_value());

    auto jobs = mgr.create_insert_jobs(*tid, 1, {{3, "Title text"}, {4, "Body content"}});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_EQ(jobs->size(), 2u);

    EXPECT_EQ((*jobs)[0].column_id, 3);
    EXPECT_EQ((*jobs)[0].source_text, "Title text");
    EXPECT_EQ((*jobs)[1].column_id, 4);
    EXPECT_EQ((*jobs)[1].source_text, "Body content");
}

TEST(EmbeddingColumn, CreateInsertJobsNoEmbeddingColumns) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    TableSchema schema;
    schema.name = "plain";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    auto tid = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(tid.has_value());

    auto jobs = mgr.create_insert_jobs(*tid, 1, {});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_TRUE(jobs->empty());
}

// -- create_update_jobs -------------------------------------------------------

TEST(EmbeddingColumn, CreateUpdateJobsSourceColumnChanged) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Update the "title" column — should trigger re-embedding.
    auto jobs = mgr.create_update_jobs(*tid, 42, {"title"}, {{3, "Updated title"}});
    ASSERT_TRUE(jobs.has_value());
    ASSERT_EQ(jobs->size(), 1u);
    EXPECT_EQ((*jobs)[0].type, EmbeddingJob::Type::UPDATE);
    EXPECT_EQ((*jobs)[0].source_text, "Updated title");
}

TEST(EmbeddingColumn, CreateUpdateJobsUnrelatedColumnChanged) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Update "body" column — should NOT trigger re-embedding since
    // the embedding source is "title".
    auto jobs = mgr.create_update_jobs(*tid, 42, {"body"}, {});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_TRUE(jobs->empty());
}

TEST(EmbeddingColumn, CreateUpdateJobsMultiSourceExpr) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title, body";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Update "body" — should trigger because source_expr includes "body".
    auto jobs = mgr.create_update_jobs(*tid, 42, {"body"}, {{3, "Updated body"}});
    ASSERT_TRUE(jobs.has_value());
    ASSERT_EQ(jobs->size(), 1u);
    EXPECT_EQ((*jobs)[0].type, EmbeddingJob::Type::UPDATE);
}

TEST(EmbeddingColumn, CreateUpdateJobsNoEmbeddingColumns) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    TableSchema schema;
    schema.name = "plain";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    auto tid = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(tid.has_value());

    auto jobs = mgr.create_update_jobs(*tid, 1, {"id"}, {});
    ASSERT_TRUE(jobs.has_value());
    EXPECT_TRUE(jobs->empty());
}

// -- describe_embedding_columns -----------------------------------------------

TEST(EmbeddingColumn, DescribeEmbeddingColumns) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    auto infos = mgr.describe_embedding_columns(*tid);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].column_name, "embedding");
    EXPECT_EQ(infos[0].dimension, 384);
    EXPECT_EQ(infos[0].source_expr, "title");
    EXPECT_EQ(infos[0].provider, "openai");
    EXPECT_EQ(infos[0].index_name, "hnsw_docs_embedding");
}

TEST(EmbeddingColumn, DescribeEmbeddingColumnsEmpty) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    TableSchema schema;
    schema.name = "plain";
    schema.columns = {{0, "id", TypeId::INT32, false, ""}};
    auto tid = catalog.create_table(default_database_id, schema);
    ASSERT_TRUE(tid.has_value());

    auto infos = mgr.describe_embedding_columns(*tid);
    EXPECT_TRUE(infos.empty());
}

TEST(EmbeddingColumn, DescribeMultipleEmbeddingColumns) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_multi_embedding("articles"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def1;
    def1.column_id = 3;
    def1.dimension = 384;
    def1.source_expr = "title";
    def1.provider = "openai";

    EmbeddingColumnDef def2;
    def2.column_id = 4;
    def2.dimension = 768;
    def2.source_expr = "body";
    def2.provider = "cohere";

    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def1, def2}).has_value());

    auto infos = mgr.describe_embedding_columns(*tid);
    ASSERT_EQ(infos.size(), 2u);
    EXPECT_EQ(infos[0].column_name, "title_vec");
    EXPECT_EQ(infos[0].dimension, 384);
    EXPECT_EQ(infos[0].provider, "openai");
    EXPECT_EQ(infos[1].column_name, "body_vec");
    EXPECT_EQ(infos[1].dimension, 768);
    EXPECT_EQ(infos[1].provider, "cohere");
}

// -- make_index_name ----------------------------------------------------------

TEST(EmbeddingColumn, MakeIndexName) {
    EXPECT_EQ(EmbeddingColumnManager::make_index_name("docs", "embedding"), "hnsw_docs_embedding");
    EXPECT_EQ(EmbeddingColumnManager::make_index_name("articles", "title_vec"),
              "hnsw_articles_title_vec");
}

// -- Drop table cascades to indexes created by embedding manager ---------------

TEST(EmbeddingColumn, DropTableCascadesToEmbeddingIndexes) {
    Catalog catalog;
    EmbeddingColumnManager mgr(catalog);

    auto tid = catalog.create_table(default_database_id, make_schema_with_embedding("docs"));
    ASSERT_TRUE(tid.has_value());

    EmbeddingColumnDef def;
    def.column_id = 3;
    def.dimension = 384;
    def.source_expr = "title";
    def.provider = "openai";
    ASSERT_TRUE(mgr.register_table_embeddings(*tid, {def}).has_value());

    // Verify index exists.
    ASSERT_TRUE(catalog.get_index("hnsw_docs_embedding").has_value());

    // Drop the table.
    ASSERT_TRUE(catalog.drop_table(default_database_id, "docs").has_value());

    // Both embedding columns and HNSW index should be gone.
    EXPECT_TRUE(catalog.list_all_embedding_columns().empty());
    EXPECT_FALSE(catalog.get_index("hnsw_docs_embedding").has_value());
}
