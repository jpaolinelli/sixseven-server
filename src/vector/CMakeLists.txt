# sixseven::vector sources
find_package(httplib CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)

add_library(sixseven_vector STATIC
    backfill_manager.cpp
    bpe_tokenizer.cpp
    distance.cpp
    hnsw_page.cpp
    hnsw_index.cpp
    embedding_column.cpp
    embedding_worker.cpp
    http_client.cpp
    ollama_provider.cpp
    openai_provider.cpp
    builtin_provider.cpp
    pre_tokenizer.cpp
    text_normalizer.cpp
    tokenizer.cpp
    tokenizer_json_loader.cpp
    wordpiece_tokenizer.cpp
    onnx_provider.cpp
    provider_registry.cpp
)
target_include_directories(sixseven_vector PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(sixseven_vector PUBLIC sixseven_common sixseven_storage sixseven_catalog httplib::httplib onnxruntime::onnxruntime ICU::uc ICU::i18n)
