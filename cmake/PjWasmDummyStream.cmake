# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_DUMMY_STREAM)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_dummy_stream_dir data_stream_dummy
    dummy_stream.cpp manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_dummy_stream STATIC
    "${_pj_dummy_stream_dir}/dummy_stream.cpp")
target_compile_features(pj_wasm_dummy_stream PRIVATE cxx_std_20)
target_include_directories(pj_wasm_dummy_stream PRIVATE
    "${_pj_dummy_stream_dir}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pj_wasm_dummy_stream PRIVATE
    plotjuggler_sdk::plugin_sdk)

pj_embed_manifest(pj_wasm_dummy_stream
    MANIFEST_FILE "${_pj_dummy_stream_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/dummy_manifest.hpp"
    VAR_NAME kDummyManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_dummy_stream)
