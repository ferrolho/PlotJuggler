#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared vtable boilerplate for the mock Toolbox plugins used by the
// runtime-catalog tests: an inert stub, differing only in its manifest JSON
// and capabilities. Mirrors mock_data_source_vtable.h so static-registration
// tests can hand a vtable straight to registerStaticToolbox without a DSO.

#include "pj_base/toolbox_protocol.h"

namespace pj_mock {
namespace detail {

inline void* createToolbox() noexcept {
  return reinterpret_cast<void*>(0x1);
}

inline void destroyToolbox(void*) noexcept {}

inline uint64_t toolboxCapabilitiesNone(void*) noexcept {
  return 0;
}

inline bool bindToolbox(void*, PJ_service_registry_t, PJ_error_t*) noexcept {
  return true;
}

inline bool saveToolboxConfig(void*, PJ_string_view_t* out_json, PJ_error_t*) noexcept {
  static constexpr const char* kJson = "{}";
  if (out_json != nullptr) {
    out_json->data = kJson;
    out_json->size = 2;
  }
  return true;
}

inline bool loadToolboxConfig(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

inline PJ_borrowed_dialog_t toolboxDialog(void*) noexcept {
  return PJ_borrowed_dialog_t{nullptr, nullptr};
}

inline void toolboxOnDataChanged(void*) noexcept {}

inline const void* toolboxExtension(void*, PJ_string_view_t) noexcept {
  return nullptr;
}

}  // namespace detail

// capabilities_fn is overridable so a test can model a plugin that advertises
// PJ_TOOLBOX_CAPABILITY_HAS_DIALOG.
inline PJ_toolbox_vtable_t makeMockToolboxVtable(
    const char* manifest_json, uint64_t (*capabilities_fn)(void*) noexcept = detail::toolboxCapabilitiesNone) noexcept {
  return PJ_toolbox_vtable_t{
      PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
      sizeof(PJ_toolbox_vtable_t),
      detail::createToolbox,
      detail::destroyToolbox,
      manifest_json,
      capabilities_fn,
      detail::bindToolbox,
      detail::saveToolboxConfig,
      detail::loadToolboxConfig,
      detail::toolboxDialog,
      detail::toolboxOnDataChanged,
      detail::toolboxExtension,
  };
}

}  // namespace pj_mock
