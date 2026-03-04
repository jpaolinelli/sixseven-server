# giodb::vector sources
find_package(httplib CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)

add_library(giodb_vector STATIC
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
    onnx_provider.cpp
    provider_registry.cpp
)
target_include_directories(giodb_vector PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(giodb_vector PUBLIC giodb_common giodb_storage giodb_catalog httplib::httplib onnxruntime::onnxruntime)
