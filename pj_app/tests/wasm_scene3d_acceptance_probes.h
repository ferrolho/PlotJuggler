#pragma once

// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// Installs observation-only JavaScript callbacks for the Scene3D browser
// acceptance band. This is linked only in PJ_WASM_ENABLE_INGRESS_PROBE builds.
void installWasmScene3dAcceptanceProbes();

}  // namespace PJ
