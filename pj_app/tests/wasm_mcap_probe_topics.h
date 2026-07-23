#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Topic names owned by the WASM MCAP probe parser's acceptance fixtures. Host
// code that needs a fixture-specific hook references these constants so no
// test-domain name is hardcoded in production translation units.
inline constexpr char kWasmProbeBulkTopic[] = "/bulk/data";
