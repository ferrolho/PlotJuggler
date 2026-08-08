// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QLoggingCategory>
#include <string>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/plugin_data_api.h"
#include "pj_plugins/host/service_registry_builder.hpp"

namespace PJ {

/// Diagnostics for host-to-plugin service wiring ("pj.runtime.services").
Q_DECLARE_LOGGING_CATEGORY(lcServices)

/// Log a rejected OPTIONAL service registration. Out-of-line so the logging
/// category has a single definition across the template instantiations below.
void reportServiceRegistrationFailure(std::string_view service_name, const std::string& reason);

namespace detail {

/// The traits expansion `ServiceRegistryBuilder::registerService<Traits>` performs,
/// with the Status kept instead of discarded.
template <class Traits>
[[nodiscard]] Status tryRegisterTraitsService(ServiceRegistryBuilder& registry, typename Traits::Raw service) {
  return registry.tryRegisterService(
      Traits::kName, Traits::kMinVersion, PJ_service_t{service.ctx, static_cast<const void*>(service.vtable)});
}

}  // namespace detail

/// Register a service the host cannot function without — one the SDK plugin
/// bases reach through `require<>()`, or that carries a data path the host has
/// already committed to.
///
/// Rejection means a duplicate name or a null fat pointer: both are host wiring
/// defects, never runtime conditions, so the Status must fail the enclosing
/// operation at the boundary where the cause is still known. Continuing would
/// leave the plugin bound to whichever instance claimed the name first — a wrong
/// service surface indistinguishable from the right one.
///
/// Deliberately does NOT log: the caller owns reporting, and it alone knows
/// which operation failed. Discarding the Status is a bug, hence [[nodiscard]].
template <class Traits>
[[nodiscard]] Status registerRequiredService(ServiceRegistryBuilder& registry, typename Traits::Raw service) {
  return detail::tryRegisterTraitsService<Traits>(registry, service);
}

/// Register a service whose absence is a supported degraded mode — one the SDK
/// plugin bases reach through `get<>()` rather than `require<>()`.
///
/// Warns and continues: nothing upstream would report it otherwise, and no
/// caller should branch on it, so there is no result to retain.
template <class Traits>
void registerOptionalService(ServiceRegistryBuilder& registry, typename Traits::Raw service) {
  if (const Status status = detail::tryRegisterTraitsService<Traits>(registry, service); !status) {
    reportServiceRegistrationFailure(Traits::kName, status.error());
  }
}

}  // namespace PJ
