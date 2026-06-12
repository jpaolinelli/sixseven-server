#include "sixseven/vector/tokenizer_json_loader.h"

#include "sixseven/common/logging.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace sixseven {

namespace {

/// Map a JSON model type string to TokenizerModelType.
Result<TokenizerModelType> parse_model_type(const std::string& type_str) {
    if (type_str == "WordPiece") {
        return ok(TokenizerModelType::WORDPIECE);
    }
    if (type_str == "BPE") {
        return ok(TokenizerModelType::BPE);
    }
    if (type_str == "Unigram") {
        return ok(TokenizerModelType::UNIGRAM);
    }
    return make_error(StatusCode::INVALID_ARGUMENT, "unsupported model type: " + type_str);
}

/// Map a JSON normalizer type string to NormalizerType.
NormalizerType parse_normalizer_type(const std::string& type_str, bool lowercase) {
    if (type_str == "BertNormalizer") {
        return NormalizerType::BERT;
    }
    if (type_str == "Lowercase") {
        return lowercase ? NormalizerType::LOWERCASE : NormalizerType::NONE;
    }
    if (type_str == "NFC" || type_str == "NFKC") {
        return NormalizerType::NFC;
    }
    return NormalizerType::NONE;
}

/// Map a JSON pre-tokenizer type string to PreTokenizerType.
PreTokenizerType parse_pre_tokenizer_type(const std::string& type_str) {
    if (type_str == "BertPreTokenizer" || type_str == "Punctuation") {
        return PreTokenizerType::PUNCTUATION;
    }
    if (type_str == "Whitespace" || type_str == "WhitespaceSplit") {
        return PreTokenizerType::WHITESPACE;
    }
    return PreTokenizerType::NONE;
}

} // namespace

Result<TokenizerConfig> load_tokenizer_config(const std::string& path) {
    // Read the file.
    std::ifstream file(path);
    if (!file.is_open()) {
        return make_error(StatusCode::IO_ERROR, "cannot open tokenizer file: " + path);
    }

    // Parse JSON.
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(file);
    } catch (const nlohmann::json::parse_error& e) {
        return make_error(StatusCode::PARSE_ERROR,
                          "malformed JSON in tokenizer file: " + std::string(e.what()));
    }

    // Validate top-level structure.
    if (!doc.is_object()) {
        return make_error(StatusCode::PARSE_ERROR, "tokenizer file root must be a JSON object");
    }
    if (!doc.contains("model") || !doc["model"].is_object()) {
        return make_error(StatusCode::PARSE_ERROR,
                          "tokenizer file missing required 'model' object");
    }

    const auto& model = doc["model"];

    // --- model.type (required) ---
    if (!model.contains("type") || !model["type"].is_string()) {
        return make_error(StatusCode::PARSE_ERROR, "model missing required 'type' string field");
    }
    auto model_type = parse_model_type(model["type"].get<std::string>());
    if (!model_type) {
        return make_error(model_type.error().code, model_type.error().message);
    }

    TokenizerConfig config;
    config.model_type = *model_type;

    // --- model.vocab (required) ---
    if (!model.contains("vocab") || !model["vocab"].is_object()) {
        return make_error(StatusCode::PARSE_ERROR, "model missing required 'vocab' object field");
    }
    const auto& vocab_json = model["vocab"];
    config.vocab.reserve(vocab_json.size());
    for (const auto& [token, id] : vocab_json.items()) {
        if (!id.is_number_integer()) {
            return make_error(StatusCode::PARSE_ERROR,
                              "vocab entry '" + token + "' has non-integer ID");
        }
        config.vocab[token] = id.get<int64_t>();
    }

    // --- model.continuing_subword_prefix (optional) ---
    if (model.contains("continuing_subword_prefix") &&
        model["continuing_subword_prefix"].is_string()) {
        config.subword_prefix = model["continuing_subword_prefix"].get<std::string>();
    }

    // --- model.merges (optional, for BPE) ---
    if (model.contains("merges") && model["merges"].is_array()) {
        const auto& merges_json = model["merges"];
        config.merges.reserve(merges_json.size());
        for (const auto& merge : merges_json) {
            if (merge.is_string()) {
                // Common format: "a b"
                config.merges.push_back(merge.get<std::string>());
            } else if (merge.is_array() && merge.size() == 2 && merge[0].is_string() &&
                       merge[1].is_string()) {
                // Newer pair-array format: ["a", "b"] -- normalise to "a b" string.
                config.merges.push_back(merge[0].get<std::string>() + " " +
                                        merge[1].get<std::string>());
            } else {
                return make_error(StatusCode::PARSE_ERROR,
                                  "model.merges contains invalid entry (expected string or "
                                  "two-element string array)");
            }
        }
    }

    // --- added_tokens (optional) ---
    // Tracks which special token roles were resolved from added_tokens so we can
    // skip the vocab-fallback pass for those roles.
    bool pad_set = false;
    bool unk_set = false;
    bool cls_set = false;
    bool sep_set = false;
    bool mask_set = false;

    if (doc.contains("added_tokens") && doc["added_tokens"].is_array()) {
        for (const auto& token : doc["added_tokens"]) {
            if (!token.is_object() || !token.contains("id") || !token.contains("content")) {
                continue;
            }
            if (!token["id"].is_number_integer() || !token["content"].is_string()) {
                continue;
            }
            const auto id = token["id"].get<int64_t>();
            const auto content = token["content"].get<std::string>();

            // BERT-style bracket tokens.
            if (content == "[PAD]") {
                config.special_tokens.pad = id;
                pad_set = true;
            } else if (content == "[UNK]") {
                config.special_tokens.unk = id;
                unk_set = true;
            } else if (content == "[CLS]") {
                config.special_tokens.cls = id;
                cls_set = true;
            } else if (content == "[SEP]") {
                config.special_tokens.sep = id;
                sep_set = true;
            } else if (content == "[MASK]") {
                config.special_tokens.mask = id;
                mask_set = true;
            }
            // RoBERTa/GPT-style angle-bracket tokens.
            // Mapping: <s> = CLS/BOS, </s> = SEP/EOS, <pad> = PAD,
            //          <unk> = UNK, <mask> = MASK.
            // Only set a role if it has not already been resolved by a
            // bracket-style entry earlier in the added_tokens list.
            else if (content == "<s>" && !cls_set) {
                config.special_tokens.cls = id;
                cls_set = true;
            } else if (content == "</s>" && !sep_set) {
                config.special_tokens.sep = id;
                sep_set = true;
            } else if (content == "<pad>" && !pad_set) {
                config.special_tokens.pad = id;
                pad_set = true;
            } else if (content == "<unk>" && !unk_set) {
                config.special_tokens.unk = id;
                unk_set = true;
            } else if (content == "<mask>" && !mask_set) {
                config.special_tokens.mask = id;
                mask_set = true;
            }
        }
    }

    // --- vocab-based fallback for special token IDs ---
    // If a role was not resolved from added_tokens, look up the canonical token
    // string in the vocab and use its ID.  This handles tokenizer.json files that
    // omit added_tokens or use non-standard special-token layouts.
    if (!pad_set) {
        auto it = config.vocab.find("[PAD]");
        if (it == config.vocab.end()) {
            it = config.vocab.find("<pad>");
        }
        if (it != config.vocab.end()) {
            config.special_tokens.pad = it->second;
            pad_set = true;
        }
    }
    if (!unk_set) {
        auto it = config.vocab.find("[UNK]");
        if (it == config.vocab.end()) {
            it = config.vocab.find("<unk>");
        }
        if (it != config.vocab.end()) {
            config.special_tokens.unk = it->second;
            unk_set = true;
        }
    }
    if (!cls_set) {
        auto it = config.vocab.find("[CLS]");
        if (it == config.vocab.end()) {
            it = config.vocab.find("<s>");
        }
        if (it != config.vocab.end()) {
            config.special_tokens.cls = it->second;
            cls_set = true;
        }
    }
    if (!sep_set) {
        auto it = config.vocab.find("[SEP]");
        if (it == config.vocab.end()) {
            it = config.vocab.find("</s>");
        }
        if (it != config.vocab.end()) {
            config.special_tokens.sep = it->second;
            sep_set = true;
        }
    }
    if (!mask_set) {
        auto it = config.vocab.find("[MASK]");
        if (it == config.vocab.end()) {
            it = config.vocab.find("<mask>");
        }
        if (it != config.vocab.end()) {
            config.special_tokens.mask = it->second;
            mask_set = true;
        }
    }

    // Suppress unused-variable warnings for flags in non-debug builds.
    (void)pad_set;
    (void)unk_set;
    (void)cls_set;
    (void)sep_set;
    (void)mask_set;

    // --- normalizer (optional) ---
    if (doc.contains("normalizer") && doc["normalizer"].is_object()) {
        const auto& norm = doc["normalizer"];
        bool lowercase = true;
        bool strip_accents = false;

        if (norm.contains("lowercase") && norm["lowercase"].is_boolean()) {
            lowercase = norm["lowercase"].get<bool>();
        }
        if (norm.contains("strip_accents") && norm["strip_accents"].is_boolean()) {
            strip_accents = norm["strip_accents"].get<bool>();
        }

        config.normalizer_lowercase = lowercase;
        config.normalizer_strip_accents = strip_accents;

        if (norm.contains("type") && norm["type"].is_string()) {
            config.normalizer = parse_normalizer_type(norm["type"].get<std::string>(), lowercase);
        } else {
            // No type field: infer from the lowercase flag.
            config.normalizer = lowercase ? NormalizerType::LOWERCASE : NormalizerType::NONE;
        }
    }

    // --- pre_tokenizer (optional) ---
    if (doc.contains("pre_tokenizer") && doc["pre_tokenizer"].is_object()) {
        const auto& pre_tok = doc["pre_tokenizer"];
        if (pre_tok.contains("type") && pre_tok["type"].is_string()) {
            config.pre_tokenizer = parse_pre_tokenizer_type(pre_tok["type"].get<std::string>());
        }
    }

    SIXSEVEN_LOG_DEBUG("loaded tokenizer config: vocab_size={}, model_type={}",
                       config.vocab.size(),
                       static_cast<int>(config.model_type));

    return ok(std::move(config));
}

} // namespace sixseven