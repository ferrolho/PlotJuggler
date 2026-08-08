// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ServiceRegistration.h"

#include <QString>

namespace PJ {

Q_LOGGING_CATEGORY(lcServices, "pj.runtime.services")

void reportServiceRegistrationFailure(std::string_view service_name, const std::string& reason) {
  qCWarning(lcServices) << "[service-registry] rejected service="
                        << QString::fromUtf8(service_name.data(), static_cast<qsizetype>(service_name.size()))
                        << "reason=" << QString::fromStdString(reason);
}

}  // namespace PJ
