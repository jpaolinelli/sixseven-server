/// @file test_qa_gdb_324.cpp
/// @brief QA adversarial tests for GDB-324: Integrate Tokenizer into OnnxProvider.
///
/// Tests: path resolution edge cases, tokenizer fallback logic, constructor
/// variants, null safety, provider registry integration, and end-to-end
/// tokenizer wiring.

#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/onnx_provider.h"
#include "sixseven/vector/provider_registry.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"
#include "sixseven/vector/wordpiece_tokenizer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sixseven;

namespace {

/// Mock ONNX session that captures inputs and returns configurable embeddings.
class MockOnnxSession324 : public OnnxSession {
public:
    void set_embedding(const std::vector<float>& embedding) {
        embedding_ = embedding;
        should_fail_ = false;
    }

    void set_error(StatusCode code, const std::string& msg) {
        error_code_ = code;
        error_msg_ = msg;
        should_fail_ = true;
    }

    Result<std::vector<float>> run(const std::vector<int64_t>& input_ids,
                                   const std::vector<int64_t>& attention_mask,
                                   size_t expected_dim) override {
        last_input_ids_ = input_ids;
        last_attention_mask_ = attention_mask;
        last_expected_dim_ = expected_dim;
        run_call_count_++;

        if (should_fail_) {
            return make_error(error_code_, error_msg_);
        }

        if (embedding_.size() != expected_dim) {
            return make_error(StatusCode::INTERNAL_ERROR, "dimension mismatch");
        }

        return ok(embedding_);
    }

    Result<void> health_check() override { return ok(); }

    std::vector<int64_t> last_input_ids_;
    std::vector<int64_t> last_attention_mask_;
    size_t last_expected_dim_ = 0;
    int run_call_count_ = 0;

private:
    std::vector<float> embedding_;
    StatusCode error_code_ = StatusCode::INTERNAL_ERROR;
    std::string error_msg_;
    bool should_fail_ = false;
};

/// Write a minimal valid WordPiece tokenizer.json.
void write_wordpiece_json(const std::filesystem::path& path) {
    nlohmann::json doc;
    doc["model"]["type"] = "WordPiece";
    doc["model"]["continuing_subword_prefix"] = "##";
    doc["model"]["vocab"] = {{"[PAD]", 0},
                             {"[UNK]", 100},
                             {"[CLS]", 101},
                             {"[SEP]", 102},
                             {"[MASK]", 103},
                             {"hello", 104},
                             {"world", 105},
                             {"##ing", 106},
                             {"test", 107}};
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 100}, {"content", "[UNK]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}},
                               {{"id", 103}, {"content", "[MASK]"}, {"special", true}}});
    doc["normalizer"]["type"] = "BertNormalizer";
    doc["normalizer"]["lowercase"] = true;
    doc["normalizer"]["strip_accents"] = false;
    doc["pre_tokenizer"]["type"] = "BertPreTokenizer";

    std::ofstream out(path);
    out << doc.dump(2);
}

/// Write a minimal valid BPE tokenizer.json.
void write_bpe_json(const std::filesystem::path& path) {
    nlohmann::json doc;
    doc["model"]["type"] = "BPE";
    doc["model"]["vocab"] = {{"[PAD]", 0},
                             {"[UNK]", 100},
                             {"[CLS]", 101},
                             {"[SEP]", 102},
                             {"h", 200},
                             {"e", 201},
                             {"l", 202},
                             {"o", 203},
                             {"he", 204},
                             {"ll", 205},
                             {"hello", 206}};
    doc["model"]["merges"] = {"h e", "l l", "he ll", "hell o"};
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 100}, {"content", "[UNK]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}}});
    doc["normalizer"]["type"] = "Lowercase";
    doc["normalizer"]["lowercase"] = true;
    doc["pre_tokenizer"]["type"] = "Whitespace";

    std::ofstream out(path);
    out << doc.dump(2);
}

/// Write a Unigram tokenizer.json (unsupported model type).
void write_unigram_json(const std::filesystem::path& path) {
    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = nlohmann::json::array({{{"hello", 0.0}, {"world", 0.0}}});
    doc["added_tokens"] = nlohmann::json::array();

    std::ofstream out(path);
    out << doc.dump(2);
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

} // namespace

// ===========================================================================
// Test fixture with temp directory
// ===========================================================================

class QA_GDB324 : public ::testing::Test {
protected:
    void SetUp() override {
        base_dir_ = std::filesystem::temp_directory_path() / "sixseven_qa_gdb324";
        std::filesystem::remove_all(base_dir_);
        std::filesystem::create_directories(base_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(base_dir_); }

    std::filesystem::path base_dir_;
};

// ===========================================================================
// resolve_onnx_model_paths — boundary & edge cases
// ===========================================================================

TEST_F(QA_GDB324, ResolveEmptyPathFails) {
    auto result = resolve_onnx_model_paths("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, ResolveNonexistentPathFails) {
    auto result = resolve_onnx_model_paths("/nonexistent/path/that/does/not/exist");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, ResolveEmptyDirectoryFails) {
    auto empty_dir = base_dir_ / "empty_model";
    std::filesystem::create_directories(empty_dir);

    auto result = resolve_onnx_model_paths(empty_dir.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, ResolveDirectoryWithOnlyTokenizerFails) {
    // Directory has tokenizer.json but no model.onnx — should fail.
    auto dir = base_dir_ / "tokenizer_only";
    write_file(dir / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths(dir.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, ResolveDirectoryModelOnnxInRootTakesPriorityOverSubdir) {
    // Both root/model.onnx and root/onnx/model.onnx exist.
    // Root should take priority.
    auto dir = base_dir_ / "dual_model";
    write_file(dir / "model.onnx", "root_model");
    write_file(dir / "onnx" / "model.onnx", "subdir_model");

    auto result = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (dir / "model.onnx").string());
}

TEST_F(QA_GDB324, ResolveDirectoryModelInOnnxSubdir) {
    // model.onnx only in onnx/ subdirectory.
    auto dir = base_dir_ / "subdir_only";
    write_file(dir / "onnx" / "model.onnx", "subdir_model");

    auto result = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (dir / "onnx" / "model.onnx").string());
}

TEST_F(QA_GDB324, ResolveDirectoryTokenizerOnlyInRoot) {
    // tokenizer.json is only checked in root, not in onnx/ subdirectory.
    auto dir = base_dir_ / "tokenizer_in_subdir";
    write_file(dir / "model.onnx", "model_data");
    write_file(dir / "onnx" / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // tokenizer.json in onnx/ subdir should NOT be found.
    EXPECT_TRUE(result->tokenizer_path.empty());
}

TEST_F(QA_GDB324, ResolveDirectOnnxFileNoTokenizer) {
    auto model = base_dir_ / "model.onnx";
    write_file(model, "model_data");

    auto result = resolve_onnx_model_paths(model.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, model.string());
    EXPECT_TRUE(result->tokenizer_path.empty());
}

TEST_F(QA_GDB324, ResolveDirectOnnxFileWithTokenizerAlongside) {
    write_file(base_dir_ / "model.onnx", "model_data");
    write_file(base_dir_ / "tokenizer.json", "{}");

    auto result = resolve_onnx_model_paths((base_dir_ / "model.onnx").string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, (base_dir_ / "model.onnx").string());
    EXPECT_EQ(result->tokenizer_path, (base_dir_ / "tokenizer.json").string());
}

TEST_F(QA_GDB324, ResolveNonOnnxFileStillReturnsAsModelPath) {
    // A regular file that's not .onnx — resolve just returns it as model_path.
    auto readme = base_dir_ / "README.txt";
    write_file(readme, "not a model");

    auto result = resolve_onnx_model_paths(readme.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_path, readme.string());
}

TEST_F(QA_GDB324, ResolveDirectoryWithTrailingSlash) {
    auto dir = base_dir_ / "trailing_slash";
    write_file(dir / "model.onnx", "model_data");

    auto path_with_slash = dir.string() + "/";
    auto result = resolve_onnx_model_paths(path_with_slash);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->model_path.empty());
}

// ===========================================================================
// OnnxProvider constructors — backward compatibility & tokenizer wiring
// ===========================================================================

TEST_F(QA_GDB324, ThreeArgConstructorUsesHashTokenizer) {
    auto mock = std::make_unique<MockOnnxSession324>();
    OnnxProvider provider("model.onnx", 384, std::move(mock));

    // Should use HashTokenizer by default.
    EXPECT_EQ(provider.tokenizer().vocab_size(), HashTokenizer::VOCAB_SIZE);
}

TEST_F(QA_GDB324, FourArgConstructorUsesProvidedTokenizer) {
    auto mock = std::make_unique<MockOnnxSession324>();
    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0}, {"[UNK]", 100}, {"[CLS]", 101}, {"[SEP]", 102}, {"test", 200}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    auto tokenizer = std::make_unique<WordPieceTokenizer>(config);

    OnnxProvider provider("model.onnx", 384, std::move(mock), std::move(tokenizer));

    // Should use the provided WordPiece tokenizer (vocab size = 5).
    EXPECT_EQ(provider.tokenizer().vocab_size(), 5u);
}

TEST_F(QA_GDB324, FourArgConstructorWithNullTokenizerFallsBackToHashTokenizer) {
    // When nullptr is passed as the tokenizer argument, the 4-arg constructor
    // must fall back to the same HashTokenizer default used by the 3-arg
    // constructor. This prevents UB on embed() / tokenizer() and keeps
    // behavior identical to the no-tokenizer overload.
    auto mock = std::make_unique<MockOnnxSession324>();
    mock->set_embedding({0.1F, 0.2F, 0.3F});

    OnnxProvider provider("model.onnx", 3, std::move(mock), nullptr);

    // The fallback must be a HashTokenizer — its vocab_size is 30000,
    // which is distinct from the WordPiece vocab_size=5 used in the
    // sibling FourArgConstructorUsesProvidedTokenizer test.
    EXPECT_EQ(provider.tokenizer().vocab_size(), HashTokenizer::VOCAB_SIZE);

    // embed() must succeed end-to-end through the fallback tokenizer.
    // This assertion would crash or produce UB if the null-guard were removed.
    auto result = provider.embed("hello world");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 0.1F);
    EXPECT_FLOAT_EQ((*result)[1], 0.2F);
    EXPECT_FLOAT_EQ((*result)[2], 0.3F);
}

// ===========================================================================
// Tokenizer is actually used in embed()
// ===========================================================================

TEST_F(QA_GDB324, EmbedUsesProvidedWordPieceTokenizer) {
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(384, 0.5F);
    mock_ptr->set_embedding(emb);

    // Build a WordPiece config with known vocab.
    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0},
                    {"[UNK]", 100},
                    {"[CLS]", 101},
                    {"[SEP]", 102},
                    {"[MASK]", 103},
                    {"hello", 200}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    auto tokenizer = std::make_unique<WordPieceTokenizer>(config);

    OnnxProvider provider("model.onnx", 384, std::move(mock), std::move(tokenizer));
    auto result = provider.embed("hello");

    ASSERT_TRUE(result.has_value()) << result.error().message;

    // Verify the WordPiece tokenizer was used: CLS(101), hello(200), SEP(102), PAD(0)...
    ASSERT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
    EXPECT_EQ(mock_ptr->last_input_ids_[0], 101); // CLS
    EXPECT_EQ(mock_ptr->last_input_ids_[1], 200); // "hello" from WP vocab
    EXPECT_EQ(mock_ptr->last_input_ids_[2], 102); // SEP
    // Padding for the rest.
    for (size_t i = 3; i < OnnxProvider::MAX_SEQ_LENGTH; ++i) {
        EXPECT_EQ(mock_ptr->last_input_ids_[i], 0) << "index " << i;
    }
}

TEST_F(QA_GDB324, EmbedUsesProvidedBPETokenizer) {
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(384, 0.5F);
    mock_ptr->set_embedding(emb);

    TokenizerConfig config;
    config.model_type = TokenizerModelType::BPE;
    config.vocab = {{"[PAD]", 0},
                    {"[UNK]", 100},
                    {"[CLS]", 101},
                    {"[SEP]", 102},
                    {"h", 200},
                    {"e", 201},
                    {"l", 202},
                    {"o", 203},
                    {"he", 204},
                    {"ll", 205},
                    {"hello", 206}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.merges = {"h e", "l l", "he ll", "hell o"};
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::WHITESPACE;
    auto tokenizer = std::make_unique<BPETokenizer>(config);

    OnnxProvider provider("model.onnx", 384, std::move(mock), std::move(tokenizer));
    auto result = provider.embed("hello");

    ASSERT_TRUE(result.has_value()) << result.error().message;

    // BPE should tokenize "hello" → [CLS, hello(206), SEP, PAD...]
    ASSERT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
    EXPECT_EQ(mock_ptr->last_input_ids_[0], 101); // CLS
    // BPE merges produce "hello" token 206.
    EXPECT_EQ(mock_ptr->last_input_ids_[1], 206);
    EXPECT_EQ(mock_ptr->last_input_ids_[2], 102); // SEP
}

TEST_F(QA_GDB324, DefaultHashTokenizerProducesDifferentTokensThanWordPiece) {
    // Verify that the default (hash) tokenizer and a WordPiece tokenizer
    // produce different token IDs for the same text, proving the tokenizer
    // is actually wired through.
    HashTokenizer hash_tok;
    auto hash_tokens = hash_tok.encode("hello world", OnnxProvider::MAX_SEQ_LENGTH);

    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0},
                    {"[UNK]", 100},
                    {"[CLS]", 101},
                    {"[SEP]", 102},
                    {"hello", 500},
                    {"world", 501}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    WordPieceTokenizer wp_tok(config);
    auto wp_tokens = wp_tok.encode("hello world", OnnxProvider::MAX_SEQ_LENGTH);

    // Token IDs for "hello" should differ between hash and wordpiece.
    EXPECT_NE(hash_tokens[1], wp_tokens[1]);
}

// ===========================================================================
// Attention mask consistency
// ===========================================================================

TEST_F(QA_GDB324, AttentionMaskConsistentWithCustomTokenizer) {
    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0}, {"[UNK]", 100}, {"[CLS]", 101}, {"[SEP]", 102}, {"hello", 200}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    WordPieceTokenizer tok(config);

    auto tokens = tok.encode("hello", 10);
    auto mask = tok.attention_mask(tokens);

    ASSERT_EQ(tokens.size(), 10u);
    ASSERT_EQ(mask.size(), 10u);

    // Non-padding tokens should have mask=1.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] != 0) {
            EXPECT_EQ(mask[i], 1) << "index " << i;
        } else {
            EXPECT_EQ(mask[i], 0) << "index " << i;
        }
    }
}

// ===========================================================================
// Tokenizer fallback: invalid/corrupt tokenizer.json
// ===========================================================================

TEST_F(QA_GDB324, LoadInvalidJsonTokenizerFails) {
    auto bad_json = base_dir_ / "bad_tokenizer.json";
    write_file(bad_json, "this is not json {{{");

    auto result = load_tokenizer_config(bad_json.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadEmptyJsonObjectTokenizerFails) {
    auto empty_json = base_dir_ / "empty_tokenizer.json";
    write_file(empty_json, "{}");

    auto result = load_tokenizer_config(empty_json.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerMissingVocabFails) {
    nlohmann::json doc;
    doc["model"]["type"] = "WordPiece";
    // Missing vocab.
    auto path = base_dir_ / "no_vocab.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerMissingModelTypeFails) {
    nlohmann::json doc;
    doc["model"]["vocab"] = {{"hello", 1}};
    // Missing model.type.
    auto path = base_dir_ / "no_type.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerArrayFormVocabFails) {
    // write_unigram_json writes model.vocab as a JSON array, not an object.
    // load_tokenizer_config requires vocab to be an object (string->int map);
    // the array form must be rejected with PARSE_ERROR.
    auto unigram_path = base_dir_ / "unigram.json";
    write_unigram_json(unigram_path);

    auto result = load_tokenizer_config(unigram_path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerNonexistentFileFails) {
    auto result = load_tokenizer_config("/nonexistent/tokenizer.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerEmptyFileFails) {
    auto empty = base_dir_ / "empty.json";
    write_file(empty, "");

    auto result = load_tokenizer_config(empty.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

TEST_F(QA_GDB324, LoadTokenizerJsonArrayRootFails) {
    auto arr = base_dir_ / "array_root.json";
    write_file(arr, "[1, 2, 3]");

    auto result = load_tokenizer_config(arr.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Tokenizer config with unsupported model type (Unigram)
// - A proper-object-vocab Unigram config is accepted by load_tokenizer_config
//   (model_type == UNIGRAM), but create_onnx_provider returns INVALID_ARGUMENT
//   for any model type other than WordPiece or BPE (default: case in the
//   switch in src/vector/onnx_provider.cpp).  There is no HashTokenizer
//   fallback on the unsupported-model-type path.
// ===========================================================================

TEST_F(QA_GDB324, LoadUnigramTokenizerConfigSucceeds) {
    // Write a proper Unigram config with valid vocab object.
    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = {{"hello", 1}, {"world", 2}};
    doc["added_tokens"] = nlohmann::json::array();
    auto path = base_dir_ / "unigram_proper.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    // Unigram is a valid model type in the parser.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->model_type, TokenizerModelType::UNIGRAM);
}

TEST_F(QA_GDB324, CreateOnnxProviderUnigramTypeReturnsInvalidArgument) {
    // create_onnx_provider's switch handles WORDPIECE and BPE only; the
    // default: case returns INVALID_ARGUMENT for every other model type.
    // This test drives that branch with a Unigram tokenizer.json whose vocab
    // is a proper object (so load_tokenizer_config succeeds and the switch is
    // reached), paired with a dummy model.onnx file (content is irrelevant —
    // resolve_onnx_model_paths only checks existence, not content).
    auto dir = base_dir_ / "unigram_provider";
    write_file(dir / "model.onnx", "dummy");
    nlohmann::json doc;
    doc["model"]["type"] = "Unigram";
    doc["model"]["vocab"] = {{"hello", 1}, {"world", 2}};
    doc["added_tokens"] = nlohmann::json::array();
    std::ofstream tok_out(dir / "tokenizer.json");
    tok_out << doc.dump();
    tok_out.close();

    auto result = create_onnx_provider(dir.string(), 384);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(result.error().message.find("unsupported tokenizer model type"), std::string::npos);
}

// ===========================================================================
// Provider name format
// ===========================================================================

TEST_F(QA_GDB324, ProviderNameFormatWithDirectoryPath) {
    auto mock = std::make_unique<MockOnnxSession324>();
    OnnxProvider provider("models/all-MiniLM-L6-v2", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx/models/all-MiniLM-L6-v2");
}

TEST_F(QA_GDB324, ProviderNameFormatWithOnnxFilePath) {
    auto mock = std::make_unique<MockOnnxSession324>();
    OnnxProvider provider("path/to/model.onnx", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx/path/to/model.onnx");
}

TEST_F(QA_GDB324, ProviderNameFormatWithAbsoluteDirectoryPath) {
    auto mock = std::make_unique<MockOnnxSession324>();
    OnnxProvider provider("/opt/models/all-MiniLM-L6-v2", 384, std::move(mock));
    EXPECT_EQ(provider.name(), "onnx//opt/models/all-MiniLM-L6-v2");
}

// ===========================================================================
// ProviderRegistry ONNX path with directory style names
// ===========================================================================

TEST_F(QA_GDB324, RegistryOnnxEmptyModelFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    // "onnx/" — should fail validation.
    auto result = registry.resolve("onnx/");
    ASSERT_FALSE(result.has_value());
}

TEST_F(QA_GDB324, RegistryOnnxNonexistentDirectoryFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("onnx/nonexistent/model/dir");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

TEST_F(QA_GDB324, RegistryOnnxNonexistentOnnxFileFails) {
    Catalog catalog;
    ProviderRegistry registry(catalog);

    auto result = registry.resolve("onnx/nonexistent.onnx");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::IO_ERROR);
}

// ===========================================================================
// End-to-end tokenizer loading from directory
// ===========================================================================

TEST_F(QA_GDB324, ResolveAndLoadWordPieceTokenizerFromDirectory) {
    auto dir = base_dir_ / "e2e_wordpiece";
    write_file(dir / "model.onnx", "fake_model");
    write_wordpiece_json(dir / "tokenizer.json");

    auto paths = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;
    ASSERT_FALSE(paths->tokenizer_path.empty());

    auto config = load_tokenizer_config(paths->tokenizer_path);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->model_type, TokenizerModelType::WORDPIECE);

    auto tokenizer = std::make_unique<WordPieceTokenizer>(*config);
    auto tokens = tokenizer->encode("hello world", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    // "hello" and "world" are in vocab.
    EXPECT_EQ(tokens[1], 104); // "hello"
    EXPECT_EQ(tokens[2], 105); // "world"
    EXPECT_EQ(tokens[3], 102); // SEP
}

TEST_F(QA_GDB324, ResolveAndLoadBPETokenizerFromDirectory) {
    auto dir = base_dir_ / "e2e_bpe";
    write_file(dir / "model.onnx", "fake_model");
    write_bpe_json(dir / "tokenizer.json");

    auto paths = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;
    ASSERT_FALSE(paths->tokenizer_path.empty());

    auto config = load_tokenizer_config(paths->tokenizer_path);
    ASSERT_TRUE(config.has_value()) << config.error().message;
    EXPECT_EQ(config->model_type, TokenizerModelType::BPE);

    auto tokenizer = std::make_unique<BPETokenizer>(*config);
    auto tokens = tokenizer->encode("hello", 10);
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0], 101); // CLS
    EXPECT_EQ(tokens[1], 206); // "hello" (merged)
    EXPECT_EQ(tokens[2], 102); // SEP
}

TEST_F(QA_GDB324, NoTokenizerJsonFallsBackToHashTokenizer) {
    // Directory with model.onnx but NO tokenizer.json.
    auto dir = base_dir_ / "no_tokenizer";
    write_file(dir / "model.onnx", "fake_model");

    auto paths = resolve_onnx_model_paths(dir.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;
    EXPECT_TRUE(paths->tokenizer_path.empty());
    // In create_onnx_provider, this would fall back to HashTokenizer.
}

TEST_F(QA_GDB324, DirectOnnxFileWithoutTokenizerFallsBackToHash) {
    // Direct .onnx file with no tokenizer.json alongside.
    auto model = base_dir_ / "solo" / "model.onnx";
    write_file(model, "fake_model");

    auto paths = resolve_onnx_model_paths(model.string());
    ASSERT_TRUE(paths.has_value()) << paths.error().message;
    EXPECT_TRUE(paths->tokenizer_path.empty());
}

// ===========================================================================
// embed_batch with custom tokenizer
// ===========================================================================

TEST_F(QA_GDB324, EmbedBatchUsesCustomTokenizerForAllTexts) {
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(3, 0.5F);
    mock_ptr->set_embedding(emb);

    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0},
                    {"[UNK]", 100},
                    {"[CLS]", 101},
                    {"[SEP]", 102},
                    {"hello", 200},
                    {"world", 201}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    auto tokenizer = std::make_unique<WordPieceTokenizer>(config);

    OnnxProvider provider("model.onnx", 3, std::move(mock), std::move(tokenizer));
    auto result = provider.embed_batch({"hello", "world"});

    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);
    EXPECT_EQ(mock_ptr->run_call_count_, 2);
}

// ===========================================================================
// Stress test: many sequential embeds
// ===========================================================================

TEST_F(QA_GDB324, StressSequentialEmbeddings) {
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    std::vector<float> emb(128, 0.1F);
    mock_ptr->set_embedding(emb);

    TokenizerConfig config;
    config.model_type = TokenizerModelType::WORDPIECE;
    config.vocab = {{"[PAD]", 0}, {"[UNK]", 100}, {"[CLS]", 101}, {"[SEP]", 102}};
    config.special_tokens = {0, 100, 101, 102, 103};
    config.subword_prefix = "##";
    config.normalizer = NormalizerType::LOWERCASE;
    config.pre_tokenizer = PreTokenizerType::PUNCTUATION;
    auto tokenizer = std::make_unique<WordPieceTokenizer>(config);

    OnnxProvider provider("model.onnx", 128, std::move(mock), std::move(tokenizer));

    for (int i = 0; i < 500; ++i) {
        auto result = provider.embed("text number " + std::to_string(i));
        ASSERT_TRUE(result.has_value())
            << "Failed at iteration " << i << ": " << result.error().message;
    }
    EXPECT_EQ(mock_ptr->run_call_count_, 500);
}

// ===========================================================================
// Dimension zero edge case
// ===========================================================================

TEST_F(QA_GDB324, ProviderWithZeroDimension) {
    auto mock = std::make_unique<MockOnnxSession324>();
    mock->set_embedding({});

    OnnxProvider provider("model.onnx", 0, std::move(mock));
    EXPECT_EQ(provider.dimension(), 0u);

    // Embedding with zero dimension — session returns empty vec.
    auto result = provider.embed("hello");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->empty());
}

// ===========================================================================
// Vocab with non-integer IDs in tokenizer.json
// ===========================================================================

TEST_F(QA_GDB324, LoadTokenizerVocabWithFloatIdFails) {
    nlohmann::json doc;
    doc["model"]["type"] = "WordPiece";
    doc["model"]["vocab"] = {{"hello", 1.5}}; // float, not int
    auto path = base_dir_ / "float_vocab.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Merges array with non-string entry
// ===========================================================================

TEST_F(QA_GDB324, LoadTokenizerMergesWithNonStringFails) {
    nlohmann::json doc;
    doc["model"]["type"] = "BPE";
    doc["model"]["vocab"] = {{"hello", 1}};
    doc["model"]["merges"] = {123, "a b"}; // first entry is int, not string
    auto path = base_dir_ / "bad_merges.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::PARSE_ERROR);
}

// ===========================================================================
// Large vocab tokenizer.json
// ===========================================================================

TEST_F(QA_GDB324, LoadTokenizerLargeVocab) {
    nlohmann::json doc;
    doc["model"]["type"] = "WordPiece";
    doc["model"]["continuing_subword_prefix"] = "##";
    nlohmann::json vocab;
    // Build a vocab with 10000 entries.
    vocab["[PAD]"] = 0;
    vocab["[UNK]"] = 100;
    vocab["[CLS]"] = 101;
    vocab["[SEP]"] = 102;
    for (int i = 0; i < 10000; ++i) {
        vocab["token_" + std::to_string(i)] = i + 200;
    }
    doc["model"]["vocab"] = vocab;
    doc["added_tokens"] =
        nlohmann::json::array({{{"id", 0}, {"content", "[PAD]"}, {"special", true}},
                               {{"id", 100}, {"content", "[UNK]"}, {"special", true}},
                               {{"id", 101}, {"content", "[CLS]"}, {"special", true}},
                               {{"id", 102}, {"content", "[SEP]"}, {"special", true}}});
    doc["normalizer"]["type"] = "BertNormalizer";
    doc["normalizer"]["lowercase"] = true;
    doc["pre_tokenizer"]["type"] = "BertPreTokenizer";

    auto path = base_dir_ / "large_vocab.json";
    std::ofstream out(path);
    out << doc.dump();
    out.close();

    auto result = load_tokenizer_config(path.string());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->vocab.size(), 10004u); // 10000 + 4 special tokens
    EXPECT_EQ(result->model_type, TokenizerModelType::WORDPIECE);
}

// ===========================================================================
// Existing tests continue to pass (backward compatibility verification)
// ===========================================================================

TEST_F(QA_GDB324, BackwardCompatThreeArgConstructorEmbedStillWorks) {
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({1.0F, 2.0F, 3.0F});

    // Use the 3-arg constructor (backward compatible path).
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    auto result = provider.embed("backward compatible");
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 3u);
    EXPECT_FLOAT_EQ((*result)[0], 1.0F);
    EXPECT_FLOAT_EQ((*result)[1], 2.0F);
    EXPECT_FLOAT_EQ((*result)[2], 3.0F);

    // Token IDs should be MAX_SEQ_LENGTH from HashTokenizer.
    EXPECT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
}

TEST_F(QA_GDB324, BackwardCompatHealthCheckStillWorks) {
    auto mock = std::make_unique<MockOnnxSession324>();
    OnnxProvider provider("model.onnx", 3, std::move(mock));

    auto result = provider.health_check();
    ASSERT_TRUE(result.has_value()) << result.error().message;
}

// ===========================================================================
// GDB-886: null tokenizer + embed_batch() path
// ===========================================================================

TEST_F(QA_GDB324, GDB886NullTokenizerEmbedBatchSucceeds) {
    // Adversarial: nullptr tokenizer passed to 4-arg ctor; embed_batch must
    // use the fallback HashTokenizer without crashing or dereferencing null.
    auto mock = std::make_unique<MockOnnxSession324>();
    auto* mock_ptr = mock.get();
    mock_ptr->set_embedding({0.5F, 0.6F, 0.7F});

    OnnxProvider provider("model.onnx", 3, std::move(mock), nullptr);

    // Batch of two texts — embed_batch dereferences tokenizer_ for each.
    auto result = provider.embed_batch({"hello world", "adversarial"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 2u);

    // Every row must have the expected dimension.
    EXPECT_EQ((*result)[0].size(), 3u);
    EXPECT_EQ((*result)[1].size(), 3u);

    // Tokenizer path routed through HashTokenizer: MAX_SEQ_LENGTH tokens sent.
    EXPECT_EQ(mock_ptr->last_input_ids_.size(), OnnxProvider::MAX_SEQ_LENGTH);
}

// ===========================================================================
// OnnxModelPaths struct
// ===========================================================================

TEST_F(QA_GDB324, OnnxModelPathsDefaultEmpty) {
    OnnxModelPaths paths;
    EXPECT_TRUE(paths.model_path.empty());
    EXPECT_TRUE(paths.tokenizer_path.empty());
}
