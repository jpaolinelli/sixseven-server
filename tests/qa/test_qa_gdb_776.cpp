#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace sixseven {
namespace {

/// Write JSON to a temp file and return its path.
class TempJsonFile {
public:
    explicit TempJsonFile(const std::string& content)
        : path_(std::filesystem::temp_directory_path() / "sixseven_qa_776_tokenizer.json") {
        std::ofstream out(path_);
        out << content;
    }

    ~TempJsonFile() { std::filesystem::remove(path_); }

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Minimal RoBERTa-style tokenizer fixture helpers
// ---------------------------------------------------------------------------

/// Minimal BPE vocab with RoBERTa-style special tokens.
/// IDs: <s>=0, <pad>=1, </s>=2, <unk>=3, <mask>=4, hello=5, world=6
static constexpr const char* ROBERTA_FIXTURE_JSON = R"({
    "model": {
        "type": "BPE",
        "vocab": {
            "<s>":    0,
            "<pad>":  1,
            "</s>":   2,
            "<unk>":  3,
            "<mask>": 4,
            "hello":  5,
            "world":  6,
            "Ghello": 7,
            "Gworld": 8
        },
        "merges": ["G h", "Gh ello", "G w", "Gw orld"]
    },
    "added_tokens": [
        {"id": 0, "content": "<s>",    "special": true},
        {"id": 1, "content": "<pad>",  "special": true},
        {"id": 2, "content": "</s>",   "special": true},
        {"id": 3, "content": "<unk>",  "special": true},
        {"id": 4, "content": "<mask>", "special": true}
    ],
    "normalizer":    {"type": "Lowercase", "lowercase": false},
    "pre_tokenizer": {"type": "Whitespace"}
})";

/// Same vocab but merges provided as pair-arrays instead of strings.
static constexpr const char* ROBERTA_PAIR_ARRAY_MERGES_JSON = R"({
    "model": {
        "type": "BPE",
        "vocab": {
            "<s>":    0,
            "<pad>":  1,
            "</s>":   2,
            "<unk>":  3,
            "<mask>": 4,
            "hello":  5,
            "world":  6,
            "Ghello": 7,
            "Gworld": 8
        },
        "merges": [["G", "h"], ["Gh", "ello"], ["G", "w"], ["Gw", "orld"]]
    },
    "added_tokens": [
        {"id": 0, "content": "<s>",    "special": true},
        {"id": 1, "content": "<pad>",  "special": true},
        {"id": 2, "content": "</s>",   "special": true},
        {"id": 3, "content": "<unk>",  "special": true},
        {"id": 4, "content": "<mask>", "special": true}
    ],
    "normalizer":    {"type": "Lowercase", "lowercase": false},
    "pre_tokenizer": {"type": "Whitespace"}
})";

// ---------------------------------------------------------------------------
// GDB776: Special token ID loading (AC: loader sets SpecialTokenIds correctly)
// ---------------------------------------------------------------------------

TEST(GDB776, SpecialTokenIdsLoadedFromAngleBracketAddedTokens) {
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->special_tokens.cls, 0) << "<s> must map to cls";
    EXPECT_EQ(result->special_tokens.pad, 1) << "<pad> must map to pad";
    EXPECT_EQ(result->special_tokens.sep, 2) << "</s> must map to sep";
    EXPECT_EQ(result->special_tokens.unk, 3) << "<unk> must map to unk";
    EXPECT_EQ(result->special_tokens.mask, 4) << "<mask> must map to mask";
}

TEST(GDB776, SpecialTokenIdsNotBERTDefaultsWhenAngleBracketVocab) {
    // Regression: BERT defaults are pad=0, unk=100, cls=101, sep=102.
    // A RoBERTa fixture must NOT keep those values.
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_NE(result->special_tokens.unk, 100) << "unk must not be BERT default 100";
    EXPECT_NE(result->special_tokens.cls, 101) << "cls must not be BERT default 101";
    EXPECT_NE(result->special_tokens.sep, 102) << "sep must not be BERT default 102";
}

// ---------------------------------------------------------------------------
// GDB776: encode emits cls at front and sep at end (AC: boundary tokens)
// ---------------------------------------------------------------------------

TEST(GDB776, EncodeEmitsCLSAtFront) {
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto cfg = load_tokenizer_config(f.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    BPETokenizer tok(*cfg);
    auto tokens = tok.encode("hello", 8);

    ASSERT_FALSE(tokens.empty());
    // First token must be cls (<s> = 0).
    EXPECT_EQ(tokens[0], cfg->special_tokens.cls);
}

TEST(GDB776, EncodeEmitsSEPBeforePadding) {
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto cfg = load_tokenizer_config(f.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    BPETokenizer tok(*cfg);
    // max_length=8: [<s>, content..., </s>, <pad>...]
    auto tokens = tok.encode("hello", 8);
    ASSERT_EQ(tokens.size(), 8u);

    // Find the sep token; it should precede any padding.
    bool found_sep = false;
    for (auto t : tokens) {
        if (t == cfg->special_tokens.sep) {
            found_sep = true;
        } else if (t == cfg->special_tokens.pad) {
            EXPECT_TRUE(found_sep) << "pad appeared before sep";
        }
    }
    EXPECT_TRUE(found_sep) << "sep token not emitted";
}

// ---------------------------------------------------------------------------
// GDB776: padding uses <pad> id, not zero (AC: padding uses <pad> id)
// ---------------------------------------------------------------------------

TEST(GDB776, PaddingUsesConfiguredPadId) {
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto cfg = load_tokenizer_config(f.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    ASSERT_EQ(cfg->special_tokens.pad, 1) << "fixture pad id must be 1";

    BPETokenizer tok(*cfg);
    // Short text with long max_length forces padding.
    auto tokens = tok.encode("hello", 16);
    ASSERT_EQ(tokens.size(), 16u);

    int pad_count = 0;
    for (auto t : tokens) {
        if (t == cfg->special_tokens.pad) {
            ++pad_count;
        }
    }
    EXPECT_GT(pad_count, 0) << "expected padding tokens";

    // No token should have value 0 in the padding positions (0 is <s>/cls here,
    // not the pad id).  After the first position (cls) any 0 would mean the old
    // bug is still present.
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == 0) {
            // 0 is <s>/cls -- should not appear after position 0 unless the
            // input happened to produce it as content, which our simple input
            // does not.
            FAIL() << "token ID 0 found at position " << i << " -- old bug: padding with wrong id";
        }
    }
}

// ---------------------------------------------------------------------------
// GDB776: attention mask is 1 for real tokens, 0 only for pads
// ---------------------------------------------------------------------------

TEST(GDB776, AttentionMaskCorrectForRoBERTaTokens) {
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto cfg = load_tokenizer_config(f.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    BPETokenizer tok(*cfg);
    auto tokens = tok.encode("hello", 8);
    auto mask = tok.attention_mask(tokens);

    ASSERT_EQ(mask.size(), tokens.size());

    // All positions before the first pad should be 1.
    bool in_padding = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == cfg->special_tokens.pad) {
            in_padding = true;
        }
        if (in_padding) {
            EXPECT_EQ(mask[i], 0) << "mask[" << i << "] should be 0 (padding)";
        } else {
            EXPECT_EQ(mask[i], 1) << "mask[" << i << "] should be 1 (real token)";
        }
    }
}

TEST(GDB776, AttentionMaskDoesNotZeroTokenWithIdZero) {
    // <s> has id=0 in our fixture. The old code hardcoded pad check against 0,
    // which would have zeroed the cls token in the mask.
    TempJsonFile f(ROBERTA_FIXTURE_JSON);
    auto cfg = load_tokenizer_config(f.path());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
    ASSERT_EQ(cfg->special_tokens.cls, 0) << "fixture cls id must be 0";
    ASSERT_EQ(cfg->special_tokens.pad, 1) << "fixture pad id must be 1";

    BPETokenizer tok(*cfg);
    auto tokens = tok.encode("hello", 8);
    auto mask = tok.attention_mask(tokens);

    // Position 0 is cls (id=0) -- must be 1, not 0.
    ASSERT_FALSE(tokens.empty());
    EXPECT_EQ(tokens[0], 0) << "first token should be cls (id 0)";
    EXPECT_EQ(mask[0], 1) << "cls token (id 0) must not be masked as padding";
}

// ---------------------------------------------------------------------------
// GDB776: merges pair-array format loads successfully (AC: pair-array merges)
// ---------------------------------------------------------------------------

TEST(GDB776, MergesPairArrayFormatLoadsSuccessfully) {
    TempJsonFile f(ROBERTA_PAIR_ARRAY_MERGES_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->merges.size(), 4u);
    // Normalised to "a b" string form.
    EXPECT_EQ(result->merges[0], "G h");
    EXPECT_EQ(result->merges[1], "Gh ello");
}

TEST(GDB776, MergesPairArrayProducesEquivalentTokenization) {
    // String-merges and pair-array-merges fixtures must produce identical
    // token sequences for the same input.
    TempJsonFile f_str(ROBERTA_FIXTURE_JSON);
    TempJsonFile f_arr(ROBERTA_PAIR_ARRAY_MERGES_JSON);

    auto cfg_str = load_tokenizer_config(f_str.path());
    auto cfg_arr = load_tokenizer_config(f_arr.path());
    ASSERT_TRUE(cfg_str.has_value()) << cfg_str.error().message;
    ASSERT_TRUE(cfg_arr.has_value()) << cfg_arr.error().message;

    BPETokenizer tok_str(*cfg_str);
    BPETokenizer tok_arr(*cfg_arr);

    auto tokens_str = tok_str.encode("hello world", 16);
    auto tokens_arr = tok_arr.encode("hello world", 16);
    EXPECT_EQ(tokens_str, tokens_arr);
}

// ---------------------------------------------------------------------------
// GDB776: vocab fallback (no added_tokens) resolves IDs from vocab
// ---------------------------------------------------------------------------

TEST(GDB776, VocabFallbackResolvesRoBERTaSpecialIds) {
    // Tokenizer with no added_tokens section at all.
    static constexpr const char* NO_ADDED_JSON = R"({
        "model": {
            "type": "BPE",
            "vocab": {
                "<s>":    0,
                "<pad>":  1,
                "</s>":   2,
                "<unk>":  3,
                "<mask>": 4,
                "hello":  5
            },
            "merges": []
        },
        "normalizer": {"type": "Lowercase", "lowercase": false},
        "pre_tokenizer": {"type": "Whitespace"}
    })";

    TempJsonFile f(NO_ADDED_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->special_tokens.cls, 0);
    EXPECT_EQ(result->special_tokens.pad, 1);
    EXPECT_EQ(result->special_tokens.sep, 2);
    EXPECT_EQ(result->special_tokens.unk, 3);
    EXPECT_EQ(result->special_tokens.mask, 4);
}

// ---------------------------------------------------------------------------
// GDB776 adversarial: added_tokens IDs take precedence over vocab IDs
// ---------------------------------------------------------------------------

TEST(GDB776, AddedTokensIdsTakePrecedenceOverVocab) {
    // Vocab has <s>=99, but added_tokens says <s>=0.
    // The loader must use the added_tokens id (0), not the vocab id (99).
    // This isolates the added_tokens angle-bracket code path from the vocab fallback.
    static constexpr const char* CONFLICT_JSON = R"({
        "model": {
            "type": "BPE",
            "vocab": {
                "<s>":   99,
                "<pad>": 98,
                "</s>":  97
            },
            "merges": []
        },
        "added_tokens": [
            {"id": 0, "content": "<s>",   "special": true},
            {"id": 1, "content": "<pad>", "special": true},
            {"id": 2, "content": "</s>",  "special": true}
        ],
        "normalizer":    {"type": "Lowercase", "lowercase": false},
        "pre_tokenizer": {"type": "Whitespace"}
    })";

    TempJsonFile f(CONFLICT_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // added_tokens ids must win over vocab ids.
    EXPECT_EQ(result->special_tokens.cls, 0) << "added_tokens <s>=0 must override vocab <s>=99";
    EXPECT_EQ(result->special_tokens.pad, 1) << "added_tokens <pad>=1 must override vocab <pad>=98";
    EXPECT_EQ(result->special_tokens.sep, 2) << "added_tokens </s>=2 must override vocab </s>=97";
}

// ---------------------------------------------------------------------------
// GDB776 adversarial: non-integer id in added_tokens is silently skipped
// ---------------------------------------------------------------------------

TEST(GDB776, NonIntegerAddedTokenIdIsSkipped) {
    // A malformed added_tokens entry with a string id must be silently ignored,
    // and the loader must still succeed (falling back to vocab or defaults).
    static constexpr const char* BAD_ID_JSON = R"({
        "model": {
            "type": "BPE",
            "vocab": {"<s>": 0, "<pad>": 1, "</s>": 2},
            "merges": []
        },
        "added_tokens": [
            {"id": "not-an-int", "content": "<s>", "special": true},
            {"id": 1, "content": "<pad>", "special": true},
            {"id": 2, "content": "</s>",  "special": true}
        ],
        "normalizer":    {"type": "Lowercase", "lowercase": false},
        "pre_tokenizer": {"type": "Whitespace"}
    })";

    TempJsonFile f(BAD_ID_JSON);
    auto result = load_tokenizer_config(f.path());
    // Must not error — bad entry is skipped, load succeeds.
    ASSERT_TRUE(result.has_value()) << result.error().message;
    // <s> was skipped (non-integer id); vocab fallback should still resolve it.
    EXPECT_EQ(result->special_tokens.cls, 0)
        << "vocab fallback must resolve <s>=0 after bad added_tokens entry";
    // Others from added_tokens should be set correctly.
    EXPECT_EQ(result->special_tokens.pad, 1);
    EXPECT_EQ(result->special_tokens.sep, 2);
}

// ---------------------------------------------------------------------------
// GDB776 adversarial: partial resolution — <s> set but </s> absent
// ---------------------------------------------------------------------------

TEST(GDB776, PartialAngleBracketResolutionLeavesOtherRolesAtDefault) {
    // Only <s> is in added_tokens; </s> is absent entirely (not in vocab either).
    // cls must be set; sep must keep the BERT default (102).
    static constexpr const char* PARTIAL_JSON = R"({
        "model": {
            "type": "BPE",
            "vocab": {"<s>": 0, "hello": 5},
            "merges": []
        },
        "added_tokens": [
            {"id": 0, "content": "<s>", "special": true}
        ],
        "normalizer":    {"type": "Lowercase", "lowercase": false},
        "pre_tokenizer": {"type": "Whitespace"}
    })";

    TempJsonFile f(PARTIAL_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    EXPECT_EQ(result->special_tokens.cls, 0) << "<s> must set cls=0";
    // </s> not in added_tokens or vocab → stays at BERT default.
    EXPECT_EQ(result->special_tokens.sep, 102) << "sep must remain at BERT default 102";
}

// ---------------------------------------------------------------------------
// GDB776 adversarial: bracket style wins when file has both styles for same role
// ---------------------------------------------------------------------------

TEST(GDB776, BracketTokenBeatsAngleBracketForSameRole) {
    // A hypothetical file with both [CLS] and <s> in added_tokens.
    // [CLS] is processed first (listed first), so cls should be 101, not 0.
    static constexpr const char* DUAL_JSON = R"({
        "model": {
            "type": "BPE",
            "vocab": {"[CLS]": 101, "<s>": 0},
            "merges": []
        },
        "added_tokens": [
            {"id": 101, "content": "[CLS]", "special": true},
            {"id": 0,   "content": "<s>",   "special": true}
        ],
        "normalizer":    {"type": "Lowercase", "lowercase": false},
        "pre_tokenizer": {"type": "Whitespace"}
    })";

    TempJsonFile f(DUAL_JSON);
    auto result = load_tokenizer_config(f.path());
    ASSERT_TRUE(result.has_value()) << result.error().message;

    // [CLS]=101 is set by the bracket branch; <s>=0 is in else-if, so cls stays 101.
    EXPECT_EQ(result->special_tokens.cls, 101)
        << "[CLS] bracket entry must win over subsequent <s> angle-bracket entry";
}

} // namespace
} // namespace sixseven