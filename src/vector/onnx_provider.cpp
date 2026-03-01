#include "giodb/vector/onnx_provider.h"

#include "giodb/common/logging.h"

#include <onnxruntime/onnxruntime_cxx_api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <numeric>
#include <sstream>

namespace giodb {

// ---------------------------------------------------------------------------
// RealOnnxSession — wraps Ort::Session for actual ONNX Runtime inference
// ---------------------------------------------------------------------------

class RealOnnxSession : public OnnxSession {
public:
    explicit RealOnnxSession(Ort::Env env,
                             std::unique_ptr<Ort::Session> session,
                             std::vector<std::string> input_names,
                             std::vector<std::string> output_names)
        : env_(std::move(env)), session_(std::move(session)), input_names_(std::move(input_names)),
          output_names_(std::move(output_names)) {}

    [[nodiscard]] Result<std::vector<float>> run(const std::vector<int64_t>& input_ids,
                                                 const std::vector<int64_t>& attention_mask,
                                                 size_t expected_dim) override {

        try {
            const auto seq_len = static_cast<int64_t>(input_ids.size());
            const int64_t batch_size = 1;
            std::array<int64_t, 2> input_shape = {batch_size, seq_len};

            auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            // Build input tensors.
            std::vector<Ort::Value> input_values;
            input_values.push_back(
                Ort::Value::CreateTensor<int64_t>(memory_info,
                                                  const_cast<int64_t*>(input_ids.data()),
                                                  input_ids.size(),
                                                  input_shape.data(),
                                                  input_shape.size()));

            input_values.push_back(
                Ort::Value::CreateTensor<int64_t>(memory_info,
                                                  const_cast<int64_t*>(attention_mask.data()),
                                                  attention_mask.size(),
                                                  input_shape.data(),
                                                  input_shape.size()));

            // If model expects token_type_ids, provide zeros.
            std::vector<int64_t> token_type_ids;
            bool needs_token_types = false;
            for (const auto& name : input_names_) {
                if (name == "token_type_ids") {
                    needs_token_types = true;
                    break;
                }
            }
            if (needs_token_types) {
                token_type_ids.assign(input_ids.size(), 0);
                input_values.push_back(Ort::Value::CreateTensor<int64_t>(memory_info,
                                                                         token_type_ids.data(),
                                                                         token_type_ids.size(),
                                                                         input_shape.data(),
                                                                         input_shape.size()));
            }

            // Prepare name pointers.
            std::vector<const char*> in_names;
            in_names.reserve(input_names_.size());
            for (const auto& n : input_names_) {
                in_names.push_back(n.c_str());
            }

            std::vector<const char*> out_names;
            out_names.reserve(output_names_.size());
            for (const auto& n : output_names_) {
                out_names.push_back(n.c_str());
            }

            // Run inference.
            auto output_values = session_->Run(Ort::RunOptions{nullptr},
                                               in_names.data(),
                                               input_values.data(),
                                               input_values.size(),
                                               out_names.data(),
                                               out_names.size());

            if (output_values.empty()) {
                return make_error(StatusCode::INTERNAL_ERROR,
                                  "ONNX model produced no output tensors");
            }

            // Extract embedding from output tensor.
            auto& output = output_values[0];
            auto output_info = output.GetTensorTypeAndShapeInfo();
            auto output_shape = output_info.GetShape();
            const float* output_data = output.GetTensorData<float>();

            std::vector<float> embedding;

            if (output_shape.size() == 3) {
                // [batch, seq_len, hidden_dim] — mean-pool over non-padding tokens.
                auto out_seq_len = output_shape[1];
                auto hidden_dim = output_shape[2];
                embedding = mean_pool(output_data, attention_mask, out_seq_len, hidden_dim);
            } else if (output_shape.size() == 2) {
                // [batch, dim] — direct embedding.
                auto dim = output_shape[1];
                embedding.assign(output_data, output_data + dim);
            } else {
                return make_error(StatusCode::INTERNAL_ERROR,
                                  "unexpected ONNX output shape: expected rank 2 or 3, got " +
                                      std::to_string(output_shape.size()));
            }

            if (embedding.size() != expected_dim) {
                return make_error(StatusCode::INTERNAL_ERROR,
                                  "ONNX dimension mismatch: expected " +
                                      std::to_string(expected_dim) + ", got " +
                                      std::to_string(embedding.size()));
            }

            return ok(std::move(embedding));
        } catch (const Ort::Exception& e) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "ONNX inference failed: " + std::string(e.what()));
        }
    }

    [[nodiscard]] Result<void> health_check() override {
        // Verify the session is loaded and can describe its inputs/outputs.
        if (!session_) {
            return make_error(StatusCode::INTERNAL_ERROR, "ONNX session is not initialized");
        }
        if (session_->GetInputCount() == 0) {
            return make_error(StatusCode::INTERNAL_ERROR, "ONNX model has no inputs");
        }
        if (session_->GetOutputCount() == 0) {
            return make_error(StatusCode::INTERNAL_ERROR, "ONNX model has no outputs");
        }
        return ok();
    }

private:
    /// Mean-pool hidden states over non-padding positions.
    static std::vector<float> mean_pool(const float* data,
                                        const std::vector<int64_t>& attention_mask,
                                        int64_t seq_len,
                                        int64_t hidden_dim) {
        std::vector<float> result(static_cast<size_t>(hidden_dim), 0.0F);
        float count = 0.0F;

        for (int64_t s = 0; s < seq_len; ++s) {
            if (s < static_cast<int64_t>(attention_mask.size()) &&
                attention_mask[static_cast<size_t>(s)] != 0) {
                for (int64_t d = 0; d < hidden_dim; ++d) {
                    result[static_cast<size_t>(d)] += data[s * hidden_dim + d];
                }
                count += 1.0F;
            }
        }

        if (count > 0.0F) {
            for (auto& v : result) {
                v /= count;
            }
        }

        return result;
    }

    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
};

// ---------------------------------------------------------------------------
// create_onnx_session — factory for RealOnnxSession
// ---------------------------------------------------------------------------

Result<std::unique_ptr<OnnxSession>> create_onnx_session(const std::string& model_path) {
    // Validate the model file exists.
    if (!std::filesystem::exists(model_path)) {
        return make_error(StatusCode::IO_ERROR, "ONNX model file not found: " + model_path);
    }

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "giodb_onnx");

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        auto session = std::make_unique<Ort::Session>(env, model_path.c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;

        // Extract input names.
        std::vector<std::string> input_names;
        for (size_t i = 0; i < session->GetInputCount(); ++i) {
            auto name = session->GetInputNameAllocated(i, allocator);
            input_names.emplace_back(name.get());
        }

        // Extract output names.
        std::vector<std::string> output_names;
        for (size_t i = 0; i < session->GetOutputCount(); ++i) {
            auto name = session->GetOutputNameAllocated(i, allocator);
            output_names.emplace_back(name.get());
        }

        GIODB_LOG_INFO("loaded ONNX model '{}': {} inputs, {} outputs",
                       model_path,
                       input_names.size(),
                       output_names.size());

        return ok(std::unique_ptr<OnnxSession>(std::make_unique<RealOnnxSession>(
            std::move(env), std::move(session), std::move(input_names), std::move(output_names))));
    } catch (const Ort::Exception& e) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to load ONNX model '" + model_path +
                              "': " + std::string(e.what()));
    }
}

// ---------------------------------------------------------------------------
// OnnxProvider
// ---------------------------------------------------------------------------

OnnxProvider::OnnxProvider(std::string model_path, size_t dim, std::unique_ptr<OnnxSession> session)
    : model_path_(std::move(model_path)), dimension_(dim), session_(std::move(session)) {}

Result<std::vector<float>> OnnxProvider::embed(const std::string& text) {
    if (text.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot embed empty text");
    }

    auto tokens = tokenize(text, MAX_SEQ_LENGTH);
    auto mask = make_attention_mask(tokens);

    return session_->run(tokens, mask, dimension_);
}

Result<std::vector<std::vector<float>>>
OnnxProvider::embed_batch(const std::vector<std::string>& texts) {
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

std::string OnnxProvider::name() const {
    return "onnx/" + model_path_;
}

size_t OnnxProvider::dimension() const {
    return dimension_;
}

Result<void> OnnxProvider::health_check() {
    return session_->health_check();
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

std::vector<int64_t> OnnxProvider::tokenize(const std::string& text, size_t max_length) {
    // Simple hash-based tokenizer.
    // 1. Split into lowercase words on whitespace/punctuation.
    // 2. Hash each word to a stable integer in [104, 30103].
    //    (Avoids special token IDs: PAD=0, UNK=100, CLS=101, SEP=102, MASK=103.)
    // 3. Add CLS at start, SEP at end, pad with 0.

    constexpr int64_t CLS_TOKEN = 101;
    constexpr int64_t SEP_TOKEN = 102;
    constexpr int64_t PAD_TOKEN = 0;
    constexpr int64_t VOCAB_OFFSET = 104;
    constexpr int64_t VOCAB_SIZE = 30000;

    std::vector<int64_t> tokens;
    tokens.push_back(CLS_TOKEN);

    std::string current_word;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            if (!current_word.empty()) {
                if (tokens.size() < max_length - 1) {
                    auto hash = std::hash<std::string>{}(current_word);
                    tokens.push_back(static_cast<int64_t>(hash % VOCAB_SIZE) + VOCAB_OFFSET);
                }
                current_word.clear();
            }
        }
    }
    // Flush the last word.
    if (!current_word.empty() && tokens.size() < max_length - 1) {
        auto hash = std::hash<std::string>{}(current_word);
        tokens.push_back(static_cast<int64_t>(hash % VOCAB_SIZE) + VOCAB_OFFSET);
    }

    tokens.push_back(SEP_TOKEN);

    // Pad to max_length.
    while (tokens.size() < max_length) {
        tokens.push_back(PAD_TOKEN);
    }

    // Truncate if somehow exceeded (shouldn't happen with the checks above).
    if (tokens.size() > max_length) {
        tokens.resize(max_length);
        tokens.back() = SEP_TOKEN;
    }

    return tokens;
}

std::vector<int64_t> OnnxProvider::make_attention_mask(const std::vector<int64_t>& tokens) {
    std::vector<int64_t> mask;
    mask.reserve(tokens.size());
    for (int64_t token : tokens) {
        mask.push_back(token != 0 ? 1 : 0);
    }
    return mask;
}

} // namespace giodb
