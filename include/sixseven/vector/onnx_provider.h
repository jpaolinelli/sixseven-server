#pragma once

#include "sixseven/common/result.h"
#include "sixseven/vector/embedding_worker.h"
#include "sixseven/vector/tokenizer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sixseven {

/// Resolved file paths for an ONNX model directory.
///
/// Returned by resolve_onnx_model_paths() after inspecting a path
/// that may be a directory or a direct .onnx file.
struct OnnxModelPaths {
    /// Path to the .onnx model file (always set on success).
    std::string model_path;

    /// Path to tokenizer.json, empty if not found.
    std::string tokenizer_path;
};

/// Abstract interface for ONNX model inference sessions.
///
/// Wraps the ONNX Runtime session to allow mocking in tests.
/// Real implementation uses Ort::Session; tests provide a mock.
class OnnxSession {
public:
    virtual ~OnnxSession() = default;

    /// Run inference with tokenized input and return the embedding vector.
    ///
    /// @param input_ids      Token IDs [seq_len].
    /// @param attention_mask Attention mask [seq_len] (1 = real token, 0 = padding).
    /// @param expected_dim   Expected output embedding dimension.
    /// @return Embedding vector of size expected_dim.
    [[nodiscard]] virtual Result<std::vector<float>> run(const std::vector<int64_t>& input_ids,
                                                         const std::vector<int64_t>& attention_mask,
                                                         size_t expected_dim) = 0;

    /// Run batched inference with N tokenized inputs and return N embedding vectors.
    ///
    /// Rows may have different lengths; implementations are responsible for padding
    /// them into a single [N, max_len] tensor (if needed) and masking padding positions
    /// during pooling. Returns one embedding of size expected_dim per input row.
    ///
    /// The default implementation falls back to N sequential calls to run() — this keeps
    /// existing mocks compiling without modification and provides a correct baseline.
    /// Real implementations (e.g. RealOnnxSession) override this with a single Ort::Session::Run
    /// call over a batched tensor, which amortizes per-invocation ORT overhead across the batch.
    ///
    /// @param input_ids       Per-row token IDs. input_ids.size() == N.
    /// @param attention_mask  Per-row attention masks. attention_mask.size() == N and
    ///                        attention_mask[i].size() == input_ids[i].size() for all i.
    /// @param expected_dim    Expected output embedding dimension (per row).
    /// @return One embedding vector of size expected_dim per input row.
    [[nodiscard]] virtual Result<std::vector<std::vector<float>>>
    run_batch(const std::vector<std::vector<int64_t>>& input_ids,
              const std::vector<std::vector<int64_t>>& attention_mask,
              size_t expected_dim) {
        std::vector<std::vector<float>> out;
        out.reserve(input_ids.size());
        for (size_t i = 0; i < input_ids.size(); ++i) {
            auto r = run(input_ids[i], attention_mask[i], expected_dim);
            if (!r) {
                return tl::unexpected(r.error());
            }
            out.push_back(std::move(*r));
        }
        return ok(std::move(out));
    }

    /// Validate that the model is loaded and functional.
    [[nodiscard]] virtual Result<void> health_check() = 0;
};

/// Create a real ONNX Runtime session from a model file.
///
/// Loads the ONNX model, configures session options (single thread,
/// basic graph optimization — avoids contrib-op fusion diagnostic spam
/// on stderr), and inspects input/output metadata.
///
/// @param model_path Path to the .onnx model file.
/// @return Initialized session, or IO_ERROR if the file cannot be loaded.
[[nodiscard]] Result<std::unique_ptr<OnnxSession>>
create_onnx_session(const std::string& model_path);

/// Resolve a model path that may be a directory or a direct .onnx file.
///
/// If the path is a directory, looks for model.onnx and tokenizer.json
/// inside it. If the path is a .onnx file, uses it directly and checks
/// for tokenizer.json in the same directory.
///
/// @param path Directory containing the model or path to a .onnx file.
/// @return Resolved model and tokenizer paths, or IO_ERROR.
[[nodiscard]] Result<OnnxModelPaths> resolve_onnx_model_paths(const std::string& path);

/// Embedding provider that loads an ONNX model and runs local inference.
///
/// Uses ONNX Runtime to load a transformer-based embedding model and
/// generate embeddings without network access. Supports models with
/// token-based input (input_ids + attention_mask) that produce either
/// direct embeddings [batch, dim] or hidden states [batch, seq, dim]
/// (mean-pooled automatically).
///
/// A simple hash-based tokenizer maps words to integer IDs. This enables
/// the full inference pipeline but does not reproduce the exact tokenization
/// of pretrained models — for production-quality embeddings from pretrained
/// models, a proper tokenizer (BPE/WordPiece) should be used.
///
/// Usage:
/// ```
///   auto session = create_onnx_session("/path/to/model.onnx");
///   OnnxProvider provider("/path/to/model.onnx", 384, std::move(*session));
///   auto vec = provider.embed("hello world");
/// ```
class OnnxProvider : public EmbeddingProvider {
public:
    /// Maximum token sequence length for model input.
    static constexpr size_t MAX_SEQ_LENGTH = 128;

    /// @param model_path Path to the ONNX model file.
    /// @param dim        Expected embedding dimension.
    /// @param session    ONNX inference session (real or mock).
    OnnxProvider(std::string model_path, size_t dim, std::unique_ptr<OnnxSession> session);

    /// @param model_path Path to the ONNX model file.
    /// @param dim        Expected embedding dimension.
    /// @param session    ONNX inference session (real or mock).
    /// @param tokenizer  Tokenizer instance to use for text encoding.
    OnnxProvider(std::string model_path,
                 size_t dim,
                 std::unique_ptr<OnnxSession> session,
                 std::unique_ptr<Tokenizer> tokenizer);

    [[nodiscard]] Result<std::vector<float>> embed(const std::string& text) override;

    [[nodiscard]] Result<std::vector<std::vector<float>>>
    embed_batch(const std::vector<std::string>& texts) override;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] size_t dimension() const override;
    [[nodiscard]] Result<void> health_check() override;

    /// Access the tokenizer used by this provider.
    [[nodiscard]] const Tokenizer& tokenizer() const;

private:
    std::string model_path_;
    size_t dimension_;
    std::unique_ptr<OnnxSession> session_;
    std::unique_ptr<Tokenizer> tokenizer_;
};

/// Create an OnnxProvider from a model path (directory or .onnx file).
///
/// Resolves paths, creates the ONNX session, and loads the tokenizer from
/// tokenizer.json. Fails with INVALID_ARGUMENT if tokenizer.json is absent,
/// cannot be parsed, or uses an unsupported model type (only WordPiece and
/// BPE are supported). Never silently falls back to a hash tokenizer.
///
/// @param path  Directory containing the model or path to a .onnx file.
/// @param dim   Expected embedding dimension.
/// @return Configured OnnxProvider, or error.
[[nodiscard]] Result<std::unique_ptr<OnnxProvider>> create_onnx_provider(const std::string& path,
                                                                         size_t dim);

} // namespace sixseven
