#pragma once

#include "giodb/common/result.h"
#include "giodb/vector/embedding_worker.h"

#include <string>
#include <vector>

namespace giodb {

/// Local embedding provider that generates embeddings without network access.
///
/// Uses a hash-projection approach to convert text into fixed-dimension
/// vectors. This is a deterministic, fast, purely-local embedding method
/// that does not require model files or network connectivity.
///
/// Algorithm:
///   1. Tokenize: split text into lowercase words on whitespace/punctuation.
///   2. Hash-project: for each word, use a seeded hash to compute a
///      contribution to each dimension of the output vector.
///   3. Accumulate: sum contributions across all words.
///   4. Normalize: L2-normalize the result to unit length.
///
/// Properties:
///   - Deterministic: same input always produces the same output.
///   - Word-overlap similarity: texts sharing words produce similar vectors.
///   - No dependencies: no model files, no network, no external libraries.
///   - Configurable dimension: any positive integer dimension is supported.
///
/// This is not a neural embedding model — it won't capture semantic meaning
/// the way models like all-MiniLM or text-embedding-3-small do. But it is
/// useful for testing, prototyping, and scenarios where network access is
/// unavailable.
///
/// Usage:
/// ```
///   BuiltinProvider provider(384);
///   auto vec = provider.embed("hello world");
/// ```
class BuiltinProvider : public EmbeddingProvider {
public:
    /// @param dim Embedding dimension to produce.
    explicit BuiltinProvider(size_t dim);

    [[nodiscard]] Result<std::vector<float>> embed(const std::string& text) override;

    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] size_t dimension() const override;
    [[nodiscard]] Result<void> health_check() override;

private:
    /// Hash a word into a contribution for each dimension.
    void hash_word_into(const std::string& word, std::vector<float>& vec) const;

    /// Tokenize text into lowercase words.
    static std::vector<std::string> tokenize(const std::string& text);

    size_t dimension_;
};

} // namespace giodb
