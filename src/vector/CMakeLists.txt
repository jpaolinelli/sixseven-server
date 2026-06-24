# sixseven::vector sources
find_package(httplib CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)

# Use the Windows SDK ICU that ships with the Windows 10/11 SDK.
# The SDK provides icucommon.h / icui18n.h and icu.lib / icuuc.lib / icuin.lib
# in the Universal CRT / UM library directories, which MSVC links automatically
# when targeting Windows 10 RS3+ (1703).  We create an INTERFACE target so the
# include path is visible to consumers.
if(WIN32)
    # Locate the Windows Kit UM include directory that contains icucommon.h.
    find_path(WINSDK_ICU_INCLUDE_DIR
        NAMES icucommon.h
        PATHS
            "$ENV{WindowsSdkDir}Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um"
            "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
            "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um"
        DOC "Windows SDK ICU include directory"
    )
    if(NOT WINSDK_ICU_INCLUDE_DIR)
        # Fallback: try to find via MSVC's default Windows SDK include path.
        # CMake exposes CMAKE_WINDOWS_KITS_10_DIR on recent versions.
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            set(WINSDK_ICU_INCLUDE_DIR
                "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/um"
            )
        endif()
    endif()
    add_library(sixseven_winsdk_icu INTERFACE)
    if(WINSDK_ICU_INCLUDE_DIR)
        target_include_directories(sixseven_winsdk_icu INTERFACE "${WINSDK_ICU_INCLUDE_DIR}")
    endif()
    # icu.lib is the umbrella import lib on Win10 RS3+ (links icuuc + icuin).
    target_link_libraries(sixseven_winsdk_icu INTERFACE icu.lib)
    set(SIXSEVEN_ICU_TARGET sixseven_winsdk_icu)
else()
    find_package(ICU REQUIRED COMPONENTS uc i18n)
    set(SIXSEVEN_ICU_TARGET ICU::uc ICU::i18n)
endif()

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
target_link_libraries(sixseven_vector PUBLIC sixseven_common sixseven_storage sixseven_catalog httplib::httplib onnxruntime::onnxruntime ${SIXSEVEN_ICU_TARGET})
