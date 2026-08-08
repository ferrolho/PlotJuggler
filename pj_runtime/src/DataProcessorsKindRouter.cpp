// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DataProcessorsKindRouter.h"

#include <string>
#include <string_view>

#include "pj_base/sdk/plugin_data_api.hpp"  // sdk::fillError, sdk::toStringView
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ServiceRegistration.h"

namespace PJ {

namespace {

constexpr std::string_view kDomain = "data_processors.router";

/// Fetch one backend's plugin-namespaced id list into `out` (owned copies — the
/// backend's views die on its next vtable call). Returns false on backend error.
bool appendIds(const PJ_data_processors_host_t& backend, std::vector<std::string>& out, PJ_error_t* out_error) {
  uint64_t count = 0;
  if (!backend.vtable->list_data_processor_ids(backend.ctx, nullptr, 0, &count, out_error)) {
    return false;
  }
  if (count == 0) {
    return true;
  }
  std::vector<PJ_string_view_t> views(count);
  if (!backend.vtable->list_data_processor_ids(backend.ctx, views.data(), count, &count, out_error)) {
    return false;
  }
  for (uint64_t i = 0; i < count && i < views.size(); ++i) {
    out.emplace_back(views[i].data, views[i].size);
  }
  return true;
}

}  // namespace

DataProcessorsKindRouter::DataProcessorsKindRouter(
    PJ_data_processors_host_t markers, PJ_data_processors_host_t transforms)
    : markers_(markers),
      transforms_(transforms),
      vtable_{
          .protocol_version = 1,
          .struct_size = sizeof(PJ_data_processors_host_vtable_t),
          .create_data_processor = &DataProcessorsKindRouter::onCreate,
          .remove_data_processor = &DataProcessorsKindRouter::onRemove,
          .list_data_processor_ids = &DataProcessorsKindRouter::onList,
          .data_processor_config = &DataProcessorsKindRouter::onConfig,
          .validate_data_processor_script = &DataProcessorsKindRouter::onValidate,
      },
      raw_{this, &vtable_} {}

Status DataProcessorsKindRouter::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::DataProcessorsHostService>(registry, raw_);
}

const PJ_data_processors_host_t& DataProcessorsKindRouter::backendFor(PJ_string_view_t kind) const noexcept {
  return sdk::toStringView(kind) == "markers" ? markers_ : transforms_;
}

const PJ_data_processors_host_t* DataProcessorsKindRouter::ownerOf(PJ_string_view_t id) const noexcept {
  PJ_string_view_t recipe{};
  PJ_error_t ignore{};
  if (transforms_.vtable->data_processor_config(transforms_.ctx, id, &recipe, &ignore)) {
    return &transforms_;
  }
  ignore = PJ_error_t{};
  if (markers_.vtable->data_processor_config(markers_.ctx, id, &recipe, &ignore)) {
    return &markers_;
  }
  return nullptr;
}

bool DataProcessorsKindRouter::onCreate(
    void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
    uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
    PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
    uint64_t* out_topics_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsKindRouter*>(ctx);
  if (self == nullptr) {
    return false;
  }
  const PJ_data_processors_host_t& backend = self->backendFor(kind);
  // The two backends share one id namespace here: reject an id already live in the
  // OTHER backend, or create would spawn a second node and make remove/config
  // ambiguous (the H4 cross-kind collision). Same-kind ids fall through to the
  // backend's own upsert.
  if (const PJ_data_processors_host_t* owner = self->ownerOf(id); owner != nullptr && owner != &backend) {
    sdk::fillError(out_error, 1, kDomain, "data processor id already exists with a different kind");
    return false;
  }
  return backend.vtable->create_data_processor(
      backend.ctx, id, kind, language, inputs, input_count, outputs, output_count, script, params_json, flags,
      out_topics, out_topics_capacity, out_topics_count, out_error);
}

bool DataProcessorsKindRouter::onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsKindRouter*>(ctx);
  if (self == nullptr) {
    return false;
  }
  // Route to the id's real owner so its real error surfaces (the old
  // probe-transforms-first swallowed a marker id's true error).
  const PJ_data_processors_host_t* owner = self->ownerOf(id);
  if (owner == nullptr) {
    sdk::fillError(out_error, 1, kDomain, "unknown data processor id");
    return false;
  }
  return owner->vtable->remove_data_processor(owner->ctx, id, out_error);
}

bool DataProcessorsKindRouter::onList(
    void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsKindRouter*>(ctx);
  if (self == nullptr) {
    return false;
  }
  try {
    std::vector<std::string> ids;
    if (!appendIds(self->transforms_, ids, out_error) || !appendIds(self->markers_, ids, out_error)) {
      return false;
    }
    self->list_storage_ = std::move(ids);
    self->list_views_.clear();
    self->list_views_.reserve(self->list_storage_.size());
    for (const std::string& s : self->list_storage_) {
      self->list_views_.push_back(PJ_string_view_t{s.data(), s.size()});
    }
    if (out_count != nullptr) {
      *out_count = self->list_views_.size();
    }
    if (out_ids != nullptr) {
      const uint64_t n = capacity < self->list_views_.size() ? capacity : self->list_views_.size();
      for (uint64_t i = 0; i < n; ++i) {
        out_ids[i] = self->list_views_[i];
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool DataProcessorsKindRouter::onConfig(
    void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsKindRouter*>(ctx);
  if (self == nullptr) {
    return false;
  }
  // Route to the id's real owner (the old probe-transforms-first eclipsed a marker id).
  const PJ_data_processors_host_t* owner = self->ownerOf(id);
  if (owner == nullptr) {
    sdk::fillError(out_error, 1, kDomain, "unknown data processor id");
    return false;
  }
  return owner->vtable->data_processor_config(owner->ctx, id, out_recipe_json, out_error);
}

bool DataProcessorsKindRouter::onValidate(
    void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script, PJ_string_view_t params_json,
    PJ_error_t* out_error) noexcept {
  auto* self = static_cast<DataProcessorsKindRouter*>(ctx);
  if (self == nullptr) {
    return false;
  }
  const PJ_data_processors_host_t& backend = self->backendFor(kind);
  return backend.vtable->validate_data_processor_script(backend.ctx, kind, language, script, params_json, out_error);
}

}  // namespace PJ
