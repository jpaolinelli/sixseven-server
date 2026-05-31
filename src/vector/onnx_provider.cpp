#include "sixseven/vector/onnx_provider.h"

#include "sixseven/common/logging.h"
#include "sixseven/vector/bpe_tokenizer.h"
#include "sixseven/vector/tokenizer_json_loader.h"
#include "sixseven/vector/wordpiece_tokenizer.h"

#include <onnxruntime/onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <sstream>

namespace sixseven {

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

    [[nodiscard]] Result<std::vector<std::vector<float>>>
    run_batch(const std::vector<std::vector<int64_t>>& input_ids,
              const std::vector<std::vector<int64_t>>& attention_mask,
              size_t expected_dim) override {
        if (input_ids.size() != attention_mask.size()) {
            return make_error(StatusCode::INVALID_ARGUMENT,
                              "run_batch: input_ids and attention_mask size mismatch");
        }
        if (input_ids.empty()) {
            return ok(std::vector<std::vector<float>>{});
        }

        try {
            const auto batch_size = static_cast<int64_t>(input_ids.size());

            // Determine the padded sequence length for this batch.
            size_t max_len = 0;
            for (const auto& row : input_ids) {
                max_len = std::max(max_len, row.size());
            }
            if (max_len == 0) {
                return make_error(StatusCode::INVALID_ARGUMENT,
                                  "run_batch: all input rows are empty");
            }
            const auto seq_len = static_cast<int64_t>(max_len);
            const size_t total_elems = static_cast<size_t>(batch_size) * max_len;

            // Flatten into batched tensors, padding shorter rows with zeros.
            // Pad token id = 0 (BERT/MiniLM [PAD]); mask bit = 0 on padding positions
            // so mean-pooling ignores them regardless of the exact pad id value.
            std::vector<int64_t> ids_flat(total_elems, 0);
            std::vector<int64_t> mask_flat(total_elems, 0);
            for (size_t i = 0; i < input_ids.size(); ++i) {
                if (attention_mask[i].size() != input_ids[i].size()) {
                    return make_error(StatusCode::INVALID_ARGUMENT,
                                      "run_batch: row " + std::to_string(i) +
                                          " attention_mask/input_ids length mismatch");
                }
                const size_t row_len = input_ids[i].size();
                const size_t base = i * max_len;
                std::copy_n(input_ids[i].data(), row_len, ids_flat.data() + base);
                std::copy_n(attention_mask[i].data(), row_len, mask_flat.data() + base);
            }

            std::array<int64_t, 2> input_shape = {batch_size, seq_len};

            auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            // Build input tensors.
            std::vector<Ort::Value> input_values;
            input_values.push_back(Ort::Value::CreateTensor<int64_t>(memory_info,
                                                                     ids_flat.data(),
                                                                     ids_flat.size(),
                                                                     input_shape.data(),
                                                                     input_shape.size()));

            input_values.push_back(Ort::Value::CreateTensor<int64_t>(memory_info,
                                                                     mask_flat.data(),
                                                                     mask_flat.size(),
                                                                     input_shape.data(),
                                                                     input_shape.size()));

            // If the model expects token_type_ids, provide zeros of the same shape.
            std::vector<int64_t> token_type_ids;
            bool needs_token_types = false;
            for (const auto& name : input_names_) {
                if (name == "token_type_ids") {
                    needs_token_types = true;
                    break;
                }
            }
            if (needs_token_types) {
                token_type_ids.assign(total_elems, 0);
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

            // Single batched Run — this is the whole point of the override.
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

            auto& output = output_values[0];
            auto output_info = output.GetTensorTypeAndShapeInfo();
            auto output_shape = output_info.GetShape();
            const float* output_data = output.GetTensorData<float>();

            std::vector<std::vector<float>> results;
            results.reserve(input_ids.size());

            if (output_shape.size() == 3) {
                // [batch, out_seq_len, hidden_dim] — mean-pool each row over its
                // non-padding positions using the ORIGINAL (unpadded) attention_mask.
                if (output_shape[0] != batch_size) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "ONNX batched output batch dim mismatch: expected " +
                                          std::to_string(batch_size) + ", got " +
                                          std::to_string(output_shape[0]));
                }
                const auto out_seq_len = output_shape[1];
                const auto hidden_dim = output_shape[2];
                const size_t row_floats =
                    static_cast<size_t>(out_seq_len) * static_cast<size_t>(hidden_dim);

                for (size_t i = 0; i < input_ids.size(); ++i) {
                    const float* row_data = output_data + i * row_floats;
                    // Use the per-row unpadded attention_mask so mean_pool counts only
                    // the real tokens of row i — identical semantics to single-row run().
                    results.push_back(mean_pool(row_data, attention_mask[i], out_seq_len,
                                                hidden_dim));
                }
            } else if (output_shape.size() == 2) {
                // [batch, dim] — direct per-row embedding.
                if (output_shape[0] != batch_size) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "ONNX batched output batch dim mismatch: expected " +
                                          std::to_string(batch_size) + ", got " +
                                          std::to_string(output_shape[0]));
                }
                const auto dim = output_shape[1];
                for (size_t i = 0; i < input_ids.size(); ++i) {
                    const float* row_data = output_data + i * dim;
                    results.emplace_back(row_data, row_data + dim);
                }
            } else {
                return make_error(StatusCode::INTERNAL_ERROR,
                                  "unexpected ONNX output shape: expected rank 2 or 3, got " +
                                      std::to_string(output_shape.size()));
            }

            for (size_t i = 0; i < results.size(); ++i) {
                if (results[i].size() != expected_dim) {
                    return make_error(StatusCode::INTERNAL_ERROR,
                                      "ONNX batched dimension mismatch at row " +
                                          std::to_string(i) + ": expected " +
                                          std::to_string(expected_dim) + ", got " +
                                          std::to_string(results[i].size()));
                }
            }

            return ok(std::move(results));
        } catch (const Ort::Exception& e) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "ONNX batched inference failed: " + std::string(e.what()));
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
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "sixseven_onnx");

        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        // NOTE: deliberately using BASIC (constant folding, redundant-node elimination,
        // layout optimization) rather than EXTENDED. ORT_ENABLE_EXTENDED turns on contrib-op
        // fusion (MultiHeadAttention, etc.) whose graph optimizer prints a multi-page fusion
        // trace directly to stderr during session creation, bypassing the Ort::Env log severity
        // filter above. BASIC retains the high-win optimizations without the diagnostic spam.
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);

        // Use filesystem::path::c_str() — returns wchar_t* on Windows (which
        // Ort::Session expects) and char* on POSIX.
        std::filesystem::path fs_model_path(model_path);
        auto session = std::make_unique<Ort::Session>(env, fs_model_path.c_str(), options);

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

        SIXSEVEN_LOG_INFO("loaded ONNX model '{}': {} inputs, {} outputs",
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
// resolve_onnx_model_paths — directory vs file auto-discovery
// ---------------------------------------------------------------------------

Result<OnnxModelPaths> resolve_onnx_model_paths(const std::string& path) {
    namespace fs = std::filesystem;

    if (!fs::exists(path)) {
        return make_error(StatusCode::IO_ERROR, "path not found: " + path);
    }

    OnnxModelPaths result;

    if (fs::is_directory(path)) {
        // Look for model.ort (ORT format) or model.onnx inside the directory.
        auto model_file = fs::path(path) / "model.ort";
        if (!fs::exists(model_file)) {
            model_file = fs::path(path) / "model.onnx";
        }
        if (!fs::exists(model_file)) {
            // Try onnx/model.onnx subdirectory.
            model_file = fs::path(path) / "onnx" / "model.onnx";
        }
        if (!fs::exists(model_file)) {
            return make_error(StatusCode::IO_ERROR,
                              "no model.ort or model.onnx found in directory: " + path);
        }
        result.model_path = model_file.string();

        // Check for tokenizer.json in the directory root.
        auto tokenizer_file = fs::path(path) / "tokenizer.json";
        if (fs::exists(tokenizer_file)) {
            result.tokenizer_path = tokenizer_file.string();
        }
    } else {
        // Direct .onnx file path.
        result.model_path = path;

        // Check for tokenizer.json alongside the model file.
        auto tokenizer_file = fs::path(path).parent_path() / "tokenizer.json";
        if (fs::exists(tokenizer_file)) {
            result.tokenizer_path = tokenizer_file.string();
        }
    }

    return ok(std::move(result));
}

// ---------------------------------------------------------------------------
// create_onnx_provider — high-level factory
// ---------------------------------------------------------------------------

Result<std::unique_ptr<OnnxProvider>> create_onnx_provider(const std::string& path, size_t dim) {
    auto paths = resolve_onnx_model_paths(path);
    if (!paths.has_value()) {
        return tl::unexpected(paths.error());
    }

    auto session = create_onnx_session(paths->model_path);
    if (!session.has_value()) {
        return tl::unexpected(session.error());
    }

    std::unique_ptr<Tokenizer> tokenizer;

    if (!paths->tokenizer_path.empty()) {
        auto config = load_tokenizer_config(paths->tokenizer_path);
        if (!config.has_value()) {
            SIXSEVEN_LOG_WARN("failed to load tokenizer from '{}': {}; falling back to hash tokenizer",
                           paths->tokenizer_path,
                           config.error().message);
            tokenizer = std::make_unique<HashTokenizer>(OnnxProvider::MAX_SEQ_LENGTH);
        } else {
            switch (config->model_type) {
            case TokenizerModelType::WORDPIECE:
                tokenizer = std::make_unique<WordPieceTokenizer>(*config);
                SIXSEVEN_LOG_INFO("loaded WordPiece tokenizer from '{}'", paths->tokenizer_path);
                break;
            case TokenizerModelType::BPE:
                tokenizer = std::make_unique<BPETokenizer>(*config);
                SIXSEVEN_LOG_INFO("loaded BPE tokenizer from '{}'", paths->tokenizer_path);
                break;
            default:
                SIXSEVEN_LOG_WARN("unsupported tokenizer model type in '{}'; "
                               "falling back to hash tokenizer",
                               paths->tokenizer_path);
                tokenizer = std::make_unique<HashTokenizer>(OnnxProvider::MAX_SEQ_LENGTH);
                break;
            }
        }
    } else {
        SIXSEVEN_LOG_WARN("no tokenizer.json found for model '{}'; using hash tokenizer",
                       paths->model_path);
        tokenizer = std::make_unique<HashTokenizer>(OnnxProvider::MAX_SEQ_LENGTH);
    }

    return ok(std::make_unique<OnnxProvider>(path, dim, std::move(*session), std::move(tokenizer)));
}

// ---------------------------------------------------------------------------
// OnnxProvider
// ---------------------------------------------------------------------------

OnnxProvider::OnnxProvider(std::string model_path, size_t dim, std::unique_ptr<OnnxSession> session)
    : model_path_(std::move(model_path)), dimension_(dim), session_(std::move(session)),
      tokenizer_(std::make_unique<HashTokenizer>(MAX_SEQ_LENGTH)) {}

OnnxProvider::OnnxProvider(std::string model_path,
                           size_t dim,
                           std::unique_ptr<OnnxSession> session,
                           std::unique_ptr<Tokenizer> tokenizer)
    : model_path_(std::move(model_path)), dimension_(dim), session_(std::move(session)),
      tokenizer_(std::move(tokenizer)) {}

Result<std::vector<float>> OnnxProvider::embed(const std::string& text) {
    if (text.empty()) {
        return make_error(StatusCode::INVALID_ARGUMENT, "cannot embed empty text");
    }

    auto tokens = tokenizer_->encode(text, MAX_SEQ_LENGTH);
    auto mask = tokenizer_->attention_mask(tokens);

    return session_->run(tokens, mask, dimension_);
}

Result<std::vector<std::vector<float>>>
OnnxProvider::embed_batch(const std::vector<std::string>& texts) {
    if (texts.empty()) {
        return ok(std::vector<std::vector<float>>{});
    }

    // Tokenize all texts up front, then dispatch a single batched run. The worker pool
    // (EmbeddingWorkerPool::dequeue_batch) already aggregates up to max_batch_size jobs
    // into this call, so N is typically 1..32. The RealOnnxSession::run_batch override
    // performs a single ORT session Run over a [N, max_len] tensor, which is where the
    // actual speedup comes from — previously this function looped calling embed() per
    // text and paid N× ORT invocation overhead for no batching win.
    std::vector<std::vector<int64_t>> all_ids;
    std::vector<std::vector<int64_t>> all_masks;
    all_ids.reserve(texts.size());
    all_masks.reserve(texts.size());

    for (const auto& text : texts) {
        if (text.empty()) {
            return make_error(StatusCode::INVALID_ARGUMENT, "cannot embed empty text");
        }
        auto tokens = tokenizer_->encode(text, MAX_SEQ_LENGTH);
        auto mask = tokenizer_->attention_mask(tokens);
        all_ids.push_back(std::move(tokens));
        all_masks.push_back(std::move(mask));
    }

    return session_->run_batch(all_ids, all_masks, dimension_);
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

const Tokenizer& OnnxProvider::tokenizer() const {
    return *tokenizer_;
}

} // namespace sixseven
