#pragma once

// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PluginRuntimeCatalog.h"

namespace pj_app {

// Describes the plugin entry points linked into this application binary.
// Registration itself remains owned by pj_runtime during AppSession startup.
PJ::StaticPluginSet builtInPlugins();

}  // namespace pj_app
