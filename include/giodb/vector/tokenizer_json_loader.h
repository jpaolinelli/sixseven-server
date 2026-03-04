#pragma once

#include "giodb/common/result.h"
#include "giodb/vector/tokenizer.h"

#include <string>

namespace giodb {

/// Load a TokenizerConfig from a Hugging Face tokenizer.json file.
///
/// Parses model type, vocabulary, special tokens, normalizer settings,
/// pre-tokenizer type, subword prefix, and BPE merge rules.
///
/// @param path  Filesystem path to a tokenizer.json file.
/// @return Populated TokenizerConfig, or an error on parse failure.
[[nodiscard]] Result<TokenizerConfig> load_tokenizer_config(const std::string& path);

} // namespace giodb
