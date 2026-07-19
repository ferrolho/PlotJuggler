#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared vtable boilerplate for the mock MessageParser plugins used by the
// runtime-catalog static-registration tests: an inert stub differing only in
// its manifest JSON. The manifest_json pointer is stored, not copied — pass a
// string literal (or anything that outlives the vtable).

#include "pj_base/message_parser_protocol.h"

namespace pj_mock {
namespace parser_detail {

inline void* create() noexcept {
  return reinterpret_cast<void*>(0x1);
}

inline void destroy(void*) noexcept {}

inline bool bind(void*, PJ_service_registry_t, PJ_error_t*) noexcept {
  return true;
}

inline bool bindSchema(void*, PJ_string_view_t, PJ_bytes_view_t, PJ_error_t*) noexcept {
  return true;
}

inline bool saveConfig(void*, PJ_string_view_t* out_json, PJ_error_t*) noexcept {
  static constexpr const char* kJson = "{}";
  if (out_json != nullptr) {
    out_json->data = kJson;
    out_json->size = 2;
  }
  return true;
}

inline bool loadConfig(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

inline bool parse(void*, int64_t, PJ_bytes_view_t, PJ_error_t*) noexcept {
  return true;
}

inline const void* extension(void*, PJ_string_view_t) noexcept {
  return nullptr;
}

}  // namespace parser_detail

inline PJ_message_parser_vtable_t makeMockMessageParserVtable(const char* manifest_json) noexcept {
  return PJ_message_parser_vtable_t{
      PJ_MESSAGE_PARSER_PROTOCOL_VERSION,
      sizeof(PJ_message_parser_vtable_t),
      parser_detail::create,
      parser_detail::destroy,
      manifest_json,
      parser_detail::bind,
      parser_detail::bindSchema,
      parser_detail::saveConfig,
      parser_detail::loadConfig,
      parser_detail::parse,
      parser_detail::extension,
      /*classify_schema=*/nullptr,  // optional tail slot; host treats as kNone
  };
}

}  // namespace pj_mock
