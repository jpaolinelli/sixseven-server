/// @file test_qa_gdb_757.cpp
/// @brief GDB-757: create_onnx_provider must fail with a clear error instead of
/// silently falling back to the hash tokenizer when tokenizer.json is missing,
/// corrupt, or uses an unsupported model type.

#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {
namespace {

class GDB757Fixture : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "sixseven_test_gdb757";
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    std::filesystem::path write_file(const std::string& name, const std::string& content) {
        auto p = tmp_dir_ / name;
        std::ofstream out(p);
        out << content;
        return p;
    }

    std::filesystem::path tmp_dir_;
};

// ---------------------------------------------------------------------------
// AC (a): Missing tokenizer.json -> IO_ERROR from load_tokenizer_config.
// This maps to the "failed to load tokenizer" error branch in
// create_onnx_provider (which now returns make_error instead of hash fallback).
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_MissingTokenizerJsonFailsLoad) {
    auto missing = tmp_dir_ / "nonexistent_tokenizer.json";
    auto result = load_tokenizer_config(missing.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR) << "error: " << result.error().message;
}

// ---------------------------------------------------------------------------
// AC (b): Corrupt tokenizer.json -> PARSE_ERROR from load_tokenizer_config.
// Maps to the same "failed to load tokenizer" error branch.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_CorruptTokenizerJsonFailsLoad) {
    auto tok_path = write_file("tokenizer_corrupt.json", "{ this is not valid json !!!}}}");
    auto result = load_tokenizer_config(tok_path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR) << "error: " << result.error().message;
}

TEST_F(GDB757Fixture, GDB757_TokenizerJsonMissingModelKeyFailsLoad) {
    auto tok_path = write_file("tokenizer_no_model.json", R"({"added_tokens":[]})");
    auto result = load_tokenizer_config(tok_path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR) << "error: " << result.error().message;
}

// ---------------------------------------------------------------------------
// AC (c): Unigram tokenizer.json is parsed successfully as UNIGRAM model type
// but is NOT WordPiece or BPE, so it hits the default: case which must now
// produce INVALID_ARGUMENT (not a silent hash fallback).
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_UnigramTokenizerJsonParsedAsUnigram) {
    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = nlohmann::json::object();
    doc["added_tokens"] = nlohmann::json::array();

    auto tok_path = write_file("tokenizer_unigram.json", doc.dump());
    auto result = load_tokenizer_config(tok_path.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::UNIGRAM);
    // Must not be WORDPIECE or BPE (the only two supported types).
    EXPECT_NE(result->model_type, TokenizerModelType::WORDPIECE);
    EXPECT_NE(result->model_type, TokenizerModelType::BPE);
}

TEST_F(GDB757Fixture, GDB757_UnigramTypeIsUnsupported) {
    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = nlohmann::json::object();
    doc["added_tokens"] = nlohmann::json::array();

    auto tok_path = write_file("tokenizer_unigram2.json", doc.dump());
    auto config = load_tokenizer_config(tok_path.string());
    ASSERT_TRUE(config.has_value()) << config.error().message;

    // Confirm this type hits the default: branch (unsupported).
    bool is_supported = (config->model_type == TokenizerModelType::WORDPIECE ||
                         config->model_type == TokenizerModelType::BPE);
    EXPECT_FALSE(is_supported) << "UNIGRAM must not be treated as a supported tokenizer type";
}

// ---------------------------------------------------------------------------
// Regression: the explicit/opt-in HashTokenizer path (single-arg ctor) still
// works.  This ctor is used by unit tests and must not be removed.
// It is NOT reachable from create_onnx_provider.
// ---------------------------------------------------------------------------

namespace {
class MinimalMockSession : public OnnxSession {
public:
    Result<std::vector<float>> run(const std::vector<int64_t>& /*input_ids*/,
                                   const std::vector<int64_t>& /*attention_mask*/,
                                   size_t expected_dim) override {
        return ok(std::vector<float>(expected_dim, 0.5F));
    }
    Result<void> health_check() override { return ok(); }
};
} // namespace

TEST(GDB757, OptInHashTokenizerCtorStillWorks) {
    // Direct construction bypasses create_onnx_provider; HashTokenizer is
    // the explicit default.  This must keep working.
    auto session = std::make_unique<MinimalMockSession>();
    OnnxProvider provider("fake_model.onnx", 2, std::move(session));
    EXPECT_EQ(provider.tokenizer().vocab_size(), HashTokenizer::VOCAB_SIZE);
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 2u);
}

// ---------------------------------------------------------------------------
// Regression: WordPiece and BPE tokenizer.json still load successfully.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_WordPieceTokenizerJsonLoadsSuccessfully) {
    nlohmann::json doc;
    doc["model"]["type"] = "WordPiece";
    doc["model"]["continuing_subword_prefix"] = "##";
    doc["model"]["vocab"] = {
        {"[PAD]", 0}, {"[CLS]", 101}, {"[SEP]", 102}, {"hello", 104}, {"world", 105}};
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}}});
    doc["normalizer"]["type"] = "BertNormalizer";
    doc["normalizer"]["lowercase"] = true;
    doc["pre_tokenizer"]["type"] = "BertPreTokenizer";

    auto tok_path = write_file("tokenizer_wp.json", doc.dump());
    auto result = load_tokenizer_config(tok_path.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::WORDPIECE);
}

TEST_F(GDB757Fixture, GDB757_BPETokenizerJsonLoadsSuccessfully) {
    nlohmann::json doc;
    doc["model"]["type"] = "BPE";
    doc["model"]["vocab"] = {{"[PAD]", 0}, {"hello", 1}};
    doc["model"]["merges"] = nlohmann::json::array();
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}}});
    doc["normalizer"]["type"] = "Lowercase";
    doc["normalizer"]["lowercase"] = true;
    doc["pre_tokenizer"]["type"] = "Whitespace";

    auto tok_path = write_file("tokenizer_bpe.json", doc.dump());
    auto result = load_tokenizer_config(tok_path.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::BPE);
}

// ---------------------------------------------------------------------------
// create_onnx_provider with model dir + no tokenizer.json must return an
// INVALID_ARGUMENT error from the tokenizer branch.  Because tokenizer
// validation now runs BEFORE session creation, a real model file is not
// required for this test — the tokenizer check fires first.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_CreateProviderWithoutTokenizerJsonReturnsError) {
    auto model_dir = tmp_dir_ / "model_no_tok";
    std::filesystem::create_directories(model_dir);
    {
        std::ofstream out(model_dir / "model.onnx");
        out << "this is not a real onnx model";
    }
    // No tokenizer.json alongside.
    auto result = create_onnx_provider(model_dir.string(), 384);
    ASSERT_FALSE(result.has_value())
        << "REGRESSION: create_onnx_provider succeeded with no tokenizer.json";
    // Tokenizer validation fires before session load, so the error is INVALID_ARGUMENT
    // (not IO_ERROR from a failed ONNX session).
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
        << "error: " << result.error().message;
}

// ---------------------------------------------------------------------------
// create_onnx_provider with a corrupt tokenizer.json must return an error from
// the tokenizer branch (INVALID_ARGUMENT wrapping the PARSE_ERROR).  Session
// load is never attempted because tokenizer validation runs first.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_CreateProviderWithCorruptTokenizerReturnsError) {
    auto model_dir = tmp_dir_ / "model_corrupt_tok";
    std::filesystem::create_directories(model_dir);
    {
        std::ofstream out(model_dir / "model.onnx");
        out << "not a real model";
    }
    // Write a corrupt tokenizer.json alongside.
    write_file("model_corrupt_tok/tokenizer.json", "{{{ not valid json");

    auto result = create_onnx_provider(model_dir.string(), 384);
    ASSERT_FALSE(result.has_value())
        << "REGRESSION: create_onnx_provider succeeded with corrupt tokenizer.json";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
        << "error: " << result.error().message;
    // The message must mention the tokenizer path.
    EXPECT_NE(result.error().message.find("tokenizer"), std::string::npos)
        << "error message does not mention 'tokenizer': " << result.error().message;
}

// ---------------------------------------------------------------------------
// create_onnx_provider with a Unigram tokenizer.json must return
// INVALID_ARGUMENT (unsupported model type).  Session load is never attempted.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_CreateProviderWithUnigramTokenizerReturnsError) {
    auto model_dir = tmp_dir_ / "model_unigram_tok";
    std::filesystem::create_directories(model_dir);
    {
        std::ofstream out(model_dir / "model.onnx");
        out << "not a real model";
    }

    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = {{"hello", 1}};
    doc["added_tokens"] = nlohmann::json::array();
    write_file("model_unigram_tok/tokenizer.json", doc.dump());

    auto result = create_onnx_provider(model_dir.string(), 384);
    ASSERT_FALSE(result.has_value())
        << "REGRESSION: create_onnx_provider succeeded with Unigram tokenizer";
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT)
        << "error: " << result.error().message;
    EXPECT_NE(result.error().message.find("unsupported"), std::string::npos)
        << "error message does not mention 'unsupported': " << result.error().message;
}

// ---------------------------------------------------------------------------
// Mutation-resistance: load_tokenizer_config on a non-existent path must fail.
// If someone re-introduces the silent fallback, the hash tokenizer swallows
// the error and returns success -- this assertion detects that.
// ---------------------------------------------------------------------------

TEST_F(GDB757Fixture, GDB757_MissingTokenizerMessageSubstring) {
    auto result = load_tokenizer_config("/definitely/does/not/exist/tokenizer.json");
    ASSERT_FALSE(result.has_value())
        << "REGRESSION: load_tokenizer_config returned success for a non-existent "
           "file -- silent hash fallback may have been re-introduced";
}

} // namespace
} // namespace sixseven