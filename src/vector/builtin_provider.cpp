#include "sixseven/vector/builtin_provider.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace sixseven {

BuiltinProvider::BuiltinProvider(size_t dim) : dimension_(dim) {}

Result<std::vector<float>> BuiltinProvider::embed(const std::string& text) {
    if (text.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot embed empty text");
    }

    auto words = tokenize(text);
    if (words.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT,
                          "text contains no words after tokenization");
    }

    std::vector<float> vec(dimension_, 0.0F);

    for (const auto& word : words) {
        hash_word_into(word, vec);
    }

    // L2-normalize to unit length.
    float norm = 0.0F;
    for (float v : vec) {
        norm += v * v;
    }
    norm = std::sqrt(norm);

    if (norm > 1e-9F) {
        for (float& v : vec) {
            v /= norm;
        }
    }

    return ok(std::move(vec));
}

Result<std::vector<std::vector<float>>>
BuiltinProvider::embed_batch(const std::vector<std::string>& texts) {
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    for (const auto& text : texts) {
        auto result = embed(text);
        if (!result.has_value()) {
            return tl::unexpected(result.error());
        }
        results.push_back(std::move(*result));
    }

    return ok(std::move(results));
}

std::string BuiltinProvider::name() const {
    return "builtin/" + std::to_string(dimension_);
}

size_t BuiltinProvider::dimension() const {
    return dimension_;
}

Result<void> BuiltinProvider::health_check() {
    // The built-in provider is always available (no network dependency).
    return ok();
}

void BuiltinProvider::hash_word_into(const std::string& word, std::vector<float>& vec) const {
    // Use FNV-1a hash with per-dimension seeds to project each word
    // into a pseudo-random contribution for each dimension.
    //
    // FNV-1a constants for 64-bit:
    constexpr uint64_t fnv_offset = 14695981039346656037ULL;
    constexpr uint64_t fnv_prime = 1099511628211ULL;

    for (size_t d = 0; d < dimension_; ++d) {
        // Seed the hash with the dimension index.
        uint64_t hash = fnv_offset ^ (d * 2654435761ULL);

        // Hash each byte of the word.
        for (unsigned char c : word) {
            hash ^= static_cast<uint64_t>(c);
            hash *= fnv_prime;
        }

        // Convert to float in [-1, 1] range.
        // Use top bits for better distribution.
        auto bits = static_cast<int32_t>(hash >> 33);
        float contribution = static_cast<float>(bits) / static_cast<float>(INT32_MAX >> 1);

        vec[d] += contribution;
    }
}

std::vector<std::string> BuiltinProvider::tokenize(const std::string& text) {
    std::vector<std::string> words;
    std::string current_word;

    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (!current_word.empty()) {
                words.push_back(std::move(current_word));
                current_word.clear();
            }
        }
    }

    if (!current_word.empty()) {
        words.push_back(std::move(current_word));
    }

    return words;
}

} // namespace sixseven
