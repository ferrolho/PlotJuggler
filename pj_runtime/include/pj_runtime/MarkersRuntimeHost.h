#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/types.hpp"
#include "pj_runtime/MarkerService.h"

namespace PJ {

class ServiceRegistryBuilder;

/// Host-side bridge that exposes the unified `pj.data_processors.v1` SDK service to a
/// plugin and delegates to `MarkerService` (the object engine; this bridge serves only
/// kind="markers" — kind="transform" is routed to the DerivedEngine bridge, owned by
/// the transform-editor work).
/// Implements the C ABI vtable (trampolines recover `this` from ctx), namespaces
/// every node id under the calling plugin (so list/remove/config only ever
/// see/affect THAT plugin's nodes), and stamps each submitted recipe with the
/// dataset the host says is active.
///
/// Lifetime: the bridge is independent of the nodes it created — destroying it
/// tears down only this object; the nodes keep living in `MarkerService` (the
/// host owns the script + runtime), so a plugin DSO unload never kills a running
/// node. `service` must outlive this host. Not movable: the vtable fat pointer
/// stores `this`.
///
/// NOTE: the class/file name is kept as `MarkersRuntimeHost` to bound churn; it
/// bridges the kind="markers" path of the unified data-processors service. A rename is
/// a cosmetic follow-up.
class MarkersRuntimeHost {
 public:
  /// `plugin_id` namespaces submitted ids. `active_dataset` supplies the dataset a
  /// newly submitted generator targets, given its declared input series keys — host
  /// policy (the wire carries no dataset, only series keys). Both `service` and
  /// whatever `active_dataset` reads must outlive this.
  MarkersRuntimeHost(
      MarkerService& service, std::string plugin_id,
      std::function<DatasetId(const std::vector<std::string>& inputs)> active_dataset);

  MarkersRuntimeHost(const MarkersRuntimeHost&) = delete;
  MarkersRuntimeHost& operator=(const MarkersRuntimeHost&) = delete;

  /// Register `DataProcessorsHostService` into the builder used to bind the plugin.
  void registerServices(ServiceRegistryBuilder& registry);

  /// The fat pointer this registers — exposed for direct binding in tests.
  [[nodiscard]] PJ_data_processors_host_t raw() const noexcept {
    return data_processors_;
  }

 private:
  static bool onCreate(
      void* ctx, PJ_string_view_t id, PJ_string_view_t kind, PJ_string_view_t language, const PJ_string_view_t* inputs,
      uint64_t input_count, const PJ_string_view_t* outputs, uint64_t output_count, PJ_string_view_t script,
      PJ_string_view_t params_json, uint32_t flags, PJ_string_view_t* out_topics, uint64_t out_topics_capacity,
      uint64_t* out_topics_count, PJ_error_t* out_error) noexcept;
  static bool onRemove(void* ctx, PJ_string_view_t id, PJ_error_t* out_error) noexcept;
  static bool onList(
      void* ctx, PJ_string_view_t* out_ids, uint64_t capacity, uint64_t* out_count, PJ_error_t* out_error) noexcept;
  static bool onConfig(
      void* ctx, PJ_string_view_t id, PJ_string_view_t* out_recipe_json, PJ_error_t* out_error) noexcept;
  static bool onValidate(
      void* ctx, PJ_string_view_t kind, PJ_string_view_t language, PJ_string_view_t script,
      PJ_string_view_t params_json, PJ_error_t* out_error) noexcept;

  /// Namespaced storage key for a plugin-local id: `plugin_id_ + "/" + local`.
  [[nodiscard]] std::string makeKey(std::string_view local_id) const;

  MarkerService& service_;
  std::string plugin_id_;
  std::function<DatasetId(const std::vector<std::string>& inputs)> active_dataset_;
  PJ_data_processors_host_vtable_t vtable_;
  PJ_data_processors_host_t data_processors_;
  std::vector<std::string> list_storage_;           ///< backs onList's borrowed views until the next call
  std::string config_storage_;                      ///< backs onConfig's borrowed view until the next call
  std::vector<std::string> create_topics_storage_;  ///< backs onCreate's borrowed out_topics until the next call
};

}  // namespace PJ
